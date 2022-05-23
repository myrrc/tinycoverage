#include "pass.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <unordered_set>

using namespace llvm;

constexpr StringRef CountersSection = "__tinycoverage_counters";
constexpr StringRef FuncNamesSection = "__tinycoverage_func_names";
constexpr StringRef CallbackName = "__tinycoverage_init";

constexpr StringRef CountersSectionStart = "__start___tinycoverage_counters";
constexpr StringRef CountersSectionStop = "__stop___tinycoverage_counters";
constexpr StringRef FuncNamesSectionStart = "__start___tinycoverage_func_names";

constexpr StringRef CtorName = "tinycoverage.module_ctor";

// TODO check target triple for Linux and endianess.
// TODO store in a single section (linux uses less than 64 bytes for addresses)
// TODO do not duplicate function names, use .debug_str (hard)
// TODO support GCOV regex to include/exclude source files
// TODO think about blocks deduplication

void TinycoveragePass::InsertCallbackInvocation(Module &M) const {
    Type *const Int1PtrTy = PointerType::getUnqual(Int1Ty);
    Type *const Int8PtrTy = PointerType::getUnqual(Int8Ty);
    Type *const Int8PtrPtrTy = PointerType::getUnqual(Int8PtrTy);

    constexpr auto Linkage = GlobalVariable::ExternalWeakLinkage;

    GlobalVariable *const CountersStart = new GlobalVariable(
        M, Int1Ty, false, Linkage, nullptr, CountersSectionStart);

    GlobalVariable *const CountersStop = new GlobalVariable(
        M, Int1Ty, false, Linkage, nullptr, CountersSectionStop);

    GlobalVariable *const FuncNamesStart = new GlobalVariable(
        M, Int8PtrTy, false, Linkage, nullptr, FuncNamesSectionStart);

    constexpr auto Hidden = GlobalValue::HiddenVisibility;
    CountersStart->setVisibility(Hidden);
    CountersStop->setVisibility(Hidden);
    FuncNamesStart->setVisibility(Hidden);

    const SmallVector<Type *, 3> ArgTypes = {Int1PtrTy, Int1PtrTy, Int8PtrPtrTy};
    const SmallVector<Value *, 3> Args = {CountersStart, CountersStop, FuncNamesStart};

    const auto [CtorFunc, _] = createSanitizerCtorAndInitFunctions(M, CtorName, CallbackName, ArgTypes, Args);
    CtorFunc->setComdat(M.getOrInsertComdat(CtorName));

    constexpr uint64_t CtorPriority = 2;
    appendToGlobalCtors(M, CtorFunc, CtorPriority, CtorFunc);
}

PreservedAnalyses TinycoveragePass::run(Module &M, ModuleAnalysisManager &MAM) {
    FAM = &MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
    DataLayout = &M.getDataLayout();

    IRBuilder<> builder(M.getContext());

    IntptrTy = builder.getIntNTy(DataLayout->getPointerSizeInBits());
    Int8Ty = builder.getInt8Ty();
    Int1Ty = builder.getInt1Ty();

    for (Function &F : M)
        instrumentFunction(M, F);

    InsertCallbackInvocation(M);

    appendToCompilerUsed(M, Globals);

    return PreservedAnalyses::none();
}

bool shouldInstrumentBlock(const Function &F, const BasicBlock &BB, const DominatorTree &DT,
                           const PostDominatorTree &PDT) {
    if (isa<UnreachableInst>(BB.getFirstNonPHIOrDbgOrLifetime())
        || BB.getFirstInsertionPt() == BB.end())
        return false;

    if (&F.getEntryBlock() == &BB)
        return true;

    const auto isFullDominator = [&] {
        if (succ_empty(&BB))
            return false;

        return all_of(successors(&BB), [&](const BasicBlock *SUCC) { return DT.dominates(&BB, SUCC); });
    };

    const auto isFullPostDominator = [&] {
        if (pred_empty(&BB))
            return false;

        return all_of(predecessors(&BB), [&](const BasicBlock *PRED) { return PDT.dominates(&BB, PRED); });
    };

    return !isFullDominator() && !(isFullPostDominator() && !BB.getSinglePredecessor());
}

GlobalVariable *TinycoveragePass::CreateSection(
    Module &M, Type *Ty, size_t N, StringRef SectionName,
    Constant *Initializer) const {

    ArrayType *const ArrayTy = ArrayType::get(Ty, N);
    GlobalVariable *const Array = new GlobalVariable(
        M, ArrayTy, false, GlobalVariable::PrivateLinkage, Initializer);

    Array->setSection(SectionName);
    Array->setAlignment(Align(DataLayout->getTypeStoreSize(Ty).getFixedSize()));

    return Array;
}

Constant *TinycoveragePass::AddFunctionNameVar(Module &M, Function &F) const {
    const StringRef Name = F.getName();

    SmallVector<Constant *> NameVector(Name.size() + 1);

    for (size_t i = 0; i < Name.size(); i++)
        NameVector[i] = ConstantInt::get(Int8Ty, Name[i]);

    NameVector[Name.size()] = ConstantInt::get(Int8Ty, 0);

    ArrayType *StringTy = ArrayType::get(Int8Ty, NameVector.size());

    // Multiple basic blocks may point to same function names.
    // In order to merge them in the resulting binary, we need LinkOnceAny.
    // Unused can be discarded.
    GlobalVariable *Variable = new GlobalVariable(
        M, StringTy, true, GlobalValue::LinkOnceAnyLinkage,
        ConstantArray::get(StringTy, NameVector),
        Name);

    return ConstantExpr::getBitCast(Variable, PointerType::getUnqual(Int8Ty));
}

void EmitNotes(const Function &F, const BasicBlock &BB, size_t Idx, raw_fd_ostream &out) {
    out << F.getSubprogram()->getFilename() << " " << F.getName() << " " << Idx << " ";

    std::unordered_set<unsigned int> lineset = {F.getSubprogram()->getLine()};

    for (const Instruction &I : BB)
        if (const DebugLoc &loc = I.getDebugLoc(); loc && loc.getLine() > 0)
            lineset.insert(loc.getLine());

    for (auto line : lineset)
        out << line << " ";

    out << "\n";
}

void TinycoveragePass::instrumentFunction(Module &M, Function &F) {
    if (F.empty()
        || F.getName().find(".module_ctor") != std::string::npos
        || F.getName().find("tinycoverage") != std::string::npos
        || F.getName().startswith("__sanitizer_")
        || F.getLinkage() == GlobalValue::AvailableExternallyLinkage
        || isa<UnreachableInst>(F.getEntryBlock().getTerminator()))
        return;

    SmallVector<BasicBlock *, 16> BlocksToInstrument;

    const DominatorTree &DT = FAM->getResult<DominatorTreeAnalysis>(F);
    const PostDominatorTree &PDT = FAM->getResult<PostDominatorTreeAnalysis>(F);

    for (BasicBlock &BB : F)
        if (shouldInstrumentBlock(F, BB, DT, PDT))
            BlocksToInstrument.push_back(&BB);

    if (BlocksToInstrument.empty())
        return;

    const size_t N = BlocksToInstrument.size();

    GlobalVariable *const Counters = CreateSection(
        M, Int1Ty, N, CountersSection, Constant::getNullValue(Int1Ty));
    Globals.push_back(Counters);

    const std::string NotesFile = std::string(M.getSourceFileName()) + ".tcno";
    std::error_code EC;

    // fix this (each .cpp file maps to multiple module files),
    // so multiple files are output in each module
    raw_fd_ostream out(NotesFile, EC, sys::fs::OF_Append);

    if (EC) {
        M.getContext().emitError(Twine("failed to open coverage notes file for writing: ") + EC.message());
        return;
    }

    for (size_t i = 0; i < N; i++) {
        BasicBlock &BB = *BlocksToInstrument[i];
        InstrumentBlock(M, F, BB, i, Counters);
        EmitNotes(F, BB, i, out);
    }

    // Here we could get address of function's name in DWARF's .debug_str, but it's too hard for me.
    Constant *const FuncPtr = AddFunctionNameVar(M, F);
    Type *const FuncNameTy = PointerType::getUnqual(Int8Ty);

    GlobalVariable *const FuncNames = CreateSection(
        M, FuncNameTy, N, FuncNamesSection, FuncPtr);

    Globals.push_back(FuncNames);
}

struct InstrumentationIRBuilder : IRBuilder<> {
    InstrumentationIRBuilder(Instruction *IP) : IRBuilder<>(IP) {
        if (getCurrentDebugLocation())
            return;
        if (DISubprogram *SP = IP->getFunction()->getSubprogram())
            SetCurrentDebugLocation(DILocation::get(SP->getContext(), 0, 0, SP));
    }
};

void TinycoveragePass::InstrumentBlock(Module &M, Function &F, BasicBlock &BB, size_t Idx,
                                       GlobalVariable *Array) {
    BasicBlock::iterator IP = BB.getFirstInsertionPt();
    const bool IsEntryBB = &BB == &F.getEntryBlock();

    DebugLoc EntryLoc;

    if (IsEntryBB) {
        if (DISubprogram *SP = F.getSubprogram()) {
            EntryLoc = DILocation::get(SP->getContext(), SP->getScopeLine(), 0, SP);
        }

        IP = PrepareToSplitEntryBlock(BB, IP);
    }

    InstrumentationIRBuilder IRB(&*IP);

    if (EntryLoc)
        IRB.SetCurrentDebugLocation(EntryLoc);

    const SmallVector<Value *, 2> Idxs = {ConstantInt::get(IntptrTy, 0), ConstantInt::get(IntptrTy, Idx)};
    Value *const FlagPtr = IRB.CreateGEP(Array->getValueType(), Array, Idxs);
    LoadInst *const Load = IRB.CreateLoad(Int1Ty, FlagPtr);
    Instruction *const ThenTerm = SplitBlockAndInsertIfThen(IRB.CreateIsNull(Load), &*IP, false);

    IRBuilder<> ThenIRB(ThenTerm);
    StoreInst *const Store = ThenIRB.CreateStore(ConstantInt::getTrue(Int1Ty), FlagPtr);

    auto SetNoSanitizeMetadata = [&M](Instruction *I) {
        I->setMetadata(I->getModule()->getMDKindID("nosanitize"), MDNode::get(M.getContext(), None));
    };

    SetNoSanitizeMetadata(Load);
    SetNoSanitizeMetadata(Store);
}
