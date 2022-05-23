#include "pass.h"
#include "../common/magic.h"
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

void TinycoveragePass::insertCallbackInvocation(Module &M) const {
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

    const auto [CtorFunc, _] = createSanitizerCtorAndInitFunctions(
        M, CtorName, CallbackName, ArgTypes, Args);
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

    insertCallbackInvocation(M);
    appendToCompilerUsed(M, Globals);

    emitModuleInfo(M);

    return PreservedAnalyses::none();
}

void TinycoveragePass::emitModuleInfo(const Module &M) const {
    const SmallString<100> NotesFile = {M.getSourceFileName(), ".tcno"};
    std::error_code EC;

    // fix this (each .cpp file maps to multiple module files),
    // so multiple files are output in each module
    raw_fd_ostream Out(NotesFile, EC, sys::fs::OF_Append);

    if (EC) {
        M.getContext().emitError("failed to open coverage notes file for writing");
        return;
    }

    auto writeHex = [&](uint32_t t) {
        char buf[4];
        buf[0] = t;
        buf[1] = t >> 8;
        buf[2] = t >> 16;
        buf[3] = t >> 24;
        Out.write(buf, 4);
    };

    auto writeStr = [&](StringRef str) {
        writeHex((str.size() / 4) + 1);
        Out.write(str.data(), str.size());
        Out.write_zeros(4 - str.size() % 4);
    };

    writeHex(MagicEntry);
    writeHex(ModuleInfo.size());

    for (auto ModuleIt = ModuleInfo.begin(); ModuleIt != ModuleInfo.end(); ++ModuleIt) {
        const auto &FuncInfo = ModuleIt->getValue();

        writeStr(ModuleIt->getKey());
        writeHex(FuncInfo.size());

        for (auto FuncIt = FuncInfo.begin(); FuncIt != FuncInfo.end(); ++FuncIt) {
            const auto &Blocks = FuncIt->getValue();

            writeStr(FuncIt->getKey());
            writeHex(Blocks.size());

            for (const BBInfo &lineset : Blocks) {
                writeHex(lineset.size());

                for (int line : lineset)
                    writeHex(line);
            }
        }
    }
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

GlobalVariable *TinycoveragePass::createSection(
    Module &M, Type *Ty, size_t N, StringRef SectionName,
    Constant *Initializer) const {

    ArrayType *const ArrayTy = ArrayType::get(Ty, N);
    GlobalVariable *const Array = new GlobalVariable(
        M, ArrayTy, false, GlobalVariable::PrivateLinkage, Initializer);

    Array->setSection(SectionName);
    Array->setAlignment(Align(DataLayout->getTypeStoreSize(Ty).getFixedSize()));

    return Array;
}

Constant *TinycoveragePass::addFunctionNameVar(Module &M, Function &F) const {
    const StringRef Name = F.getName();

    SmallVector<Constant *> NameVector(Name.size() + 1);

    for (size_t i = 0; i < Name.size(); i++)
        NameVector[i] = ConstantInt::get(Int8Ty, Name[i]);

    NameVector[Name.size()] = ConstantInt::get(Int8Ty, 0);

    ArrayType *const StringTy = ArrayType::get(Int8Ty, NameVector.size());

    // Multiple basic blocks may point to same function names.
    // In order to merge them in the resulting binary, we need LinkOnceAny.
    // Unused can be discarded.
    GlobalVariable *const Variable = new GlobalVariable(
        M, StringTy, true, GlobalValue::LinkOnceAnyLinkage,
        ConstantArray::get(StringTy, NameVector),
        Name);

    return ConstantExpr::getBitCast(Variable, PointerType::getUnqual(Int8Ty));
}

void TinycoveragePass::collectBBInfo(const Function &F, const BasicBlock &BB, BBInfo &BBI) {
    std::unordered_set<unsigned int> lineset = {F.getSubprogram()->getLine()};

    for (const Instruction &I : BB)
        if (const DebugLoc &loc = I.getDebugLoc(); loc && loc.getLine() > 0)
            lineset.insert(loc.getLine());

    BBI.append(lineset.begin(), lineset.end());
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

    GlobalVariable *const Counters = createSection(
        M, Int1Ty, N, CountersSection, Constant::getNullValue(Int1Ty));
    Globals.push_back(Counters);

    const StringRef SourceFileName = F.getSubprogram()->getFilename();
    FuncInfo &FuncInfo = ModuleInfo[SourceFileName][F.getName()];

    FuncInfo.resize(N);

    for (size_t i = 0; i < N; i++) {
        BasicBlock &BB = *BlocksToInstrument[i];
        instrumentBlock(M, F, BB, i, Counters);
        collectBBInfo(F, BB, FuncInfo[i]);
    }

    // Here we could get address of function's name in DWARF's .debug_str, but it's too hard for me.
    Constant *const FuncPtr = addFunctionNameVar(M, F);
    Type *const FuncNameTy = PointerType::getUnqual(Int8Ty);

    GlobalVariable *const FuncNames = createSection(
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

void TinycoveragePass::instrumentBlock(Module &M, Function &F, BasicBlock &BB, size_t Idx,
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
