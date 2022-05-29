#include "pass.h"
#include "../common/magic.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Transforms/Instrumentation.h"
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

// TODO support GCOV regex to include/exclude source files

PreservedAnalyses TinycoveragePass::run(Module &M, ModuleAnalysisManager &MAM) {
    const DataLayout &Layout = M.getDataLayout();

    FAM = &MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

    IRBuilder Builder(M.getContext());

    IntptrTy = Builder.getIntPtrTy(Layout);
    CounterTy = Builder.getInt1Ty();
    FuncNameTy = Builder.getInt8PtrTy();

    CountersAlign = Align(Layout.getTypeStoreSize(CounterTy).getFixedSize());
    FuncNamesAlign = Align(Layout.getTypeStoreSize(FuncNameTy).getFixedSize());

    const NamedMDNode &CUNode = *M.getNamedMetadata("llvm.dbg.cu");

    for (size_t i = 0; i != CUNode.getNumOperands(); ++i)
        if (const DICompileUnit &CU = *cast<DICompileUnit>(CUNode.getOperand(i)); !CU.getDWOId()) {
            for (Function &F : M)
                instrumentFunction(M, F);

            emitCUInfo(M, CU);
        }

    insertCallbackInvocation(M);
    appendToCompilerUsed(M, Globals);

    return PreservedAnalyses::none();
}

void TinycoveragePass::insertCallbackInvocation(Module &M) const {
    constexpr auto Linkage = GlobalVariable::ExternalWeakLinkage;

    GlobalVariable *const CountersStart = new GlobalVariable(
        M, CounterTy, true, Linkage, nullptr, CountersSectionStart);

    GlobalVariable *const CountersStop = new GlobalVariable(
        M, CounterTy, true, Linkage, nullptr, CountersSectionStop);

    GlobalVariable *const FuncNamesStart = new GlobalVariable(
        M, FuncNameTy, true, Linkage, nullptr, FuncNamesSectionStart);

    constexpr auto Hidden = GlobalValue::HiddenVisibility;
    CountersStart->setVisibility(Hidden);
    CountersStop->setVisibility(Hidden);
    FuncNamesStart->setVisibility(Hidden);

    Type *const CounterPtrTy = PointerType::getUnqual(CounterTy);
    Type *const FuncNamePtrTy = PointerType::getUnqual(FuncNameTy);

    const SmallVector<Type *, 3> ArgTypes = {CounterPtrTy, CounterPtrTy, FuncNamePtrTy};
    const SmallVector<Value *, 3> Args = {CountersStart, CountersStop, FuncNamesStart};

    const auto [CtorFunc, _] = createSanitizerCtorAndInitFunctions(
        M, CtorName, CallbackName, ArgTypes, Args);
    CtorFunc->setComdat(M.getOrInsertComdat(CtorName));

    constexpr uint64_t CtorPriority = 2;
    appendToGlobalCtors(M, CtorFunc, CtorPriority, CtorFunc);
}

void writeHex(raw_fd_ostream &Out, uint32_t t) {
    char buf[4];
    buf[0] = t;
    buf[1] = t >> 8;
    buf[2] = t >> 16;
    buf[3] = t >> 24;
    Out.write(buf, 4);
};

void writeStr(raw_fd_ostream &Out, StringRef str) {
    writeHex(Out, (str.size() / 4) + 1);
    Out.write(str.data(), str.size());
    Out.write_zeros(4 - str.size() % 4);
};

void TinycoveragePass::emitCUInfo(const Module &M, const DICompileUnit &CU) const {
    const SmallString<128> Filename = {CU.getFilename(), ".tcno"};
    const StringRef NotesFile = sys::path::filename(Filename);

    std::error_code EC;
    raw_fd_ostream Out(NotesFile, EC);

    if (EC) {
        M.getContext().emitError("failed to open coverage notes file for writing");
        return;
    }

    for (auto ModuleIt = ModuleInfo.begin(); ModuleIt != ModuleInfo.end(); ++ModuleIt) {
        const auto &FuncInfo = ModuleIt->getValue();

        writeHex(Out, MagicEntry);
        writeStr(Out, ModuleIt->getKey()); // source file name
        writeHex(Out, FuncInfo.size());

        for (auto FuncIt = FuncInfo.begin(); FuncIt != FuncInfo.end(); ++FuncIt) {
            const auto &Blocks = FuncIt->getValue();

            writeStr(Out, FuncIt->getKey()); // func name
            writeHex(Out, Blocks.size());

            for (const BBInfo &lineset : Blocks) {
                writeHex(Out, lineset.size());

                for (int line : lineset)
                    writeHex(Out, line);
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

void TinycoveragePass::collectBBInfo(const Function &F, const BasicBlock &BB, BBInfo &BBI) {
    std::unordered_set<unsigned int> lineset;

    for (const Instruction &I : BB)
        if (!isa<DbgInfoIntrinsic>(&I))
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

    SmallVector<BasicBlock *> BlocksToInstrument;

    const DominatorTree &DT = FAM->getResult<DominatorTreeAnalysis>(F);
    const PostDominatorTree &PDT = FAM->getResult<PostDominatorTreeAnalysis>(F);

    for (BasicBlock &BB : F)
        if (shouldInstrumentBlock(F, BB, DT, PDT))
            BlocksToInstrument.push_back(&BB);

    if (BlocksToInstrument.empty())
        return;

    const size_t N = BlocksToInstrument.size();

    constexpr auto LinkAny = GlobalVariable::LinkOnceAnyLinkage;

    ArrayType *const CountersArrTy = ArrayType::get(CounterTy, N);
    Constant *const CountersInit = Constant::getNullValue(CounterTy);
    GlobalVariable *const Counters = new GlobalVariable(M, CountersArrTy, false, LinkAny, CountersInit);
    Counters->setSection(CountersSection);
    Counters->setAlignment(CountersAlign);
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
    Constant *const FNInit = createPrivateGlobalForString(M, F.getName(), true);

    ArrayType *const FNArrayTy = ArrayType::get(FuncNameTy, N);
    GlobalVariable *const FuncNames = new GlobalVariable(M, FNArrayTy, false, LinkAny, FNInit);
    FuncNames->setSection(FuncNamesSection);
    FuncNames->setAlignment(FuncNamesAlign);
    Globals.push_back(FuncNames);
}

void TinycoveragePass::instrumentBlock(Module &M, Function &F, BasicBlock &BB, size_t Idx,
                                       GlobalVariable *Array) {
    IRBuilder Builder(&*BB.getFirstInsertionPt());

    const SmallVector<Value *, 2> Idxs = {ConstantInt::get(IntptrTy, 0), ConstantInt::get(IntptrTy, Idx)};
    Value *const Item = Builder.CreateGEP(Array->getValueType(), Array, Idxs);
    LoadInst *const Load = Builder.CreateLoad(CounterTy, Item);
    StoreInst *const Store = Builder.CreateStore(ConstantInt::getTrue(CounterTy), Item);

    auto SetNoSanitizeMetadata = [&M](Instruction *I) {
        I->setMetadata(I->getModule()->getMDKindID("nosanitize"), MDNode::get(M.getContext(), None));
    };

    SetNoSanitizeMetadata(Load);
    SetNoSanitizeMetadata(Store);
}
