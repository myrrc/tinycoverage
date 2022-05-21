#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

// linux only, ELF only

using namespace llvm;

const char SanCovModuleCtorBoolFlagName[] = "sancov.module_ctor_bool_flag";
static constexpr uint64_t SanCtorAndDtorPriority = 2;

const char TinyCoverageCallback[] = "__tinycoverage_init";

const char TinycoverageSectionName[] = "tinycoverage";
const char TinycoverageSectionNamePrefixed[] = "__tinycoverage";
const char TinycoverageSectionStartName[] = "__start___tinycoverage";
const char TinycoverageSectionStopName[] = "__stop___tinycoverage";

static cl::opt<bool> Enabled("tinycoverage", cl::desc("Instrument each BB"),
                             cl::Hidden, cl::init(false));

namespace {

class TinycoveragePass : public ModulePass {
    void instrumentFunction(Module &M, Function &F);
    void InjectCoverageAtBlock(Module &M, Function &F, BasicBlock &BB,
                               size_t Idx, GlobalVariable *FunctionBoolArray);
    void CreateInitCallsForSection(Module &M);

    Type *IntptrTy, *Int8Ty, *Int1Ty;
    const DataLayout *DataLayout;

    SmallVector<GlobalValue *, 20> GlobalsToAppendToCompilerUsed;

    const DominatorTree *DTCallback(Function &F) {
        return &this->getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
    };

    const PostDominatorTree *PDTCallback(Function &F) {
        return &this->getAnalysis<PostDominatorTreeWrapperPass>(F)
                    .getPostDomTree();
    };

    static inline char ID = 0;

  public:
    TinycoveragePass() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
        if (!Enabled) {
            return false;
        }

        DataLayout = &M.getDataLayout();

        IRBuilder<> IRB(M.getContext());

        IntptrTy = IRB.getIntNTy(DataLayout->getPointerSizeInBits());
        Int8Ty = IRB.getInt8Ty();
        Int1Ty = IRB.getInt1Ty();

        GlobalsToAppendToCompilerUsed.clear();

        for (Function &F : M)
            instrumentFunction(M, F);

        CreateInitCallsForSection(M);

        appendToCompilerUsed(M, GlobalsToAppendToCompilerUsed);

        return true;
    }

    StringRef getPassName() const final { return "ModuleTinyCoverage"; }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
        AU.addRequired<DominatorTreeWrapperPass>();
        AU.addRequired<PostDominatorTreeWrapperPass>();
    }
};

} // namespace

void TinycoveragePass::CreateInitCallsForSection(Module &M) {
    Type *Ty = Int1Ty;
    Type *PtrTy = PointerType::getUnqual(Ty);

    const GlobalValue::LinkageTypes Linkage =
        GlobalVariable::ExternalWeakLinkage;

    GlobalVariable *SecStart = new GlobalVariable(
        M, Ty, false, Linkage, nullptr, TinycoverageSectionStartName);

    SecStart->setVisibility(GlobalValue::HiddenVisibility);

    GlobalVariable *SecEnd = new GlobalVariable(M, Ty, false, Linkage, nullptr,
                                                TinycoverageSectionStopName);

    SecEnd->setVisibility(GlobalValue::HiddenVisibility);

    const char *CtorName = SanCovModuleCtorBoolFlagName;

    Function *CtorFunc =
        createSanitizerCtorAndInitFunctions(M, CtorName, TinyCoverageCallback,
                                            {PtrTy, PtrTy}, {SecStart, SecEnd})
            .first;

    assert(CtorFunc->getName() == CtorName);

    CtorFunc->setComdat(M.getOrInsertComdat(CtorName));
    appendToGlobalCtors(M, CtorFunc, SanCtorAndDtorPriority, CtorFunc);
}

// True if block has successors and it dominates all of them.
static bool isFullDominator(const BasicBlock *BB, const DominatorTree *DT) {
    if (succ_empty(BB))
        return false;

    return llvm::all_of(successors(BB), [&](const BasicBlock *SUCC) {
        return DT->dominates(BB, SUCC);
    });
}

// True if block has predecessors and it postdominates all of them.
static bool isFullPostDominator(const BasicBlock *BB,
                                const PostDominatorTree *PDT) {
    if (pred_empty(BB))
        return false;

    return llvm::all_of(predecessors(BB), [&](const BasicBlock *PRED) {
        return PDT->dominates(BB, PRED);
    });
}

static bool shouldInstrumentBlock(const Function &F, const BasicBlock *BB,
                                  const DominatorTree *DT,
                                  const PostDominatorTree *PDT) {
    // Don't insert coverage for blocks containing nothing but unreachable: we
    // will never call __sanitizer_cov() for them, so counting them in
    // NumberOfInstrumentedBlocks() might complicate calculation of code
    // coverage percentage. Also, unreachable instructions frequently have no
    // debug locations.
    if (isa<UnreachableInst>(BB->getFirstNonPHIOrDbgOrLifetime()))
        return false;

    // Don't insert coverage into blocks without a valid insertion point
    // (catchswitch blocks).
    if (BB->getFirstInsertionPt() == BB->end())
        return false;

    if (&F.getEntryBlock() == BB)
        return true;

    // Do not instrument full dominators, or full post-dominators with multiple
    // predecessors.
    return !isFullDominator(BB, DT) &&
           !(isFullPostDominator(BB, PDT) && !BB->getSinglePredecessor());
}

// Returns true iff From->To is a backedge.
// A twist here is that we treat From->To as a backedge if
//   * To dominates From or
//   * To->UniqueSuccessor dominates From
static bool IsBackEdge(BasicBlock *From, BasicBlock *To,
                       const DominatorTree *DT) {
    if (DT->dominates(To, From))
        return true;
    if (auto Next = To->getUniqueSuccessor())
        if (DT->dominates(Next, From))
            return true;
    return false;
}

void TinycoveragePass::instrumentFunction(Module &M, Function &F) {
    if (F.empty() || F.getName().find(".module_ctor") != std::string::npos ||
        F.getName().startswith("__sanitizer_") ||
        F.getName().startswith("__tinycoverage_") ||
        F.getLinkage() == GlobalValue::AvailableExternallyLinkage ||
        isa<UnreachableInst>(F.getEntryBlock().getTerminator()))
        return;

    SmallVector<BasicBlock *, 16> BlocksToInstrument;

    const DominatorTree *DT = DTCallback(F);
    const PostDominatorTree *PDT = PDTCallback(F);

    for (BasicBlock &BB : F)
        if (shouldInstrumentBlock(F, &BB, DT, PDT))
            BlocksToInstrument.push_back(&BB);

    if (BlocksToInstrument.empty())
        return;

    // Create section for functions

    ArrayType *ArrayTy = ArrayType::get(Int1Ty, BlocksToInstrument.size());

    auto Array =
        new GlobalVariable(M, ArrayTy, false, GlobalVariable::PrivateLinkage,
                           Constant::getNullValue(ArrayTy), "__sancov_gen_");

    Array->setSection(TinycoverageSectionNamePrefixed);
    Array->setAlignment(
        Align(DataLayout->getTypeStoreSize(Int1Ty).getFixedSize()));

    GlobalsToAppendToCompilerUsed.push_back(Array);

    for (size_t i = 0, N = BlocksToInstrument.size(); i < N; i++)
        InjectCoverageAtBlock(M, F, *BlocksToInstrument[i], i, Array);
}

void SetNoSanitizeMetadata(Module &M, Instruction *I) {
    I->setMetadata(I->getModule()->getMDKindID("nosanitize"),
                   MDNode::get(M.getContext(), None));
}

void TinycoveragePass::InjectCoverageAtBlock(
    Module &M, Function &F, BasicBlock &BB, size_t Idx,
    GlobalVariable *FunctionBoolArray) {
    BasicBlock::iterator IP = BB.getFirstInsertionPt();
    const bool IsEntryBB = &BB == &F.getEntryBlock();

    DebugLoc EntryLoc;

    if (IsEntryBB) {
        if (auto SP = F.getSubprogram())
            EntryLoc =
                DILocation::get(SP->getContext(), SP->getScopeLine(), 0, SP);
        // Keep static allocas and llvm.localescape calls in the entry block.
        // Even if we aren't splitting the block, it's nice for allocas to be
        // before calls.
        IP = PrepareToSplitEntryBlock(BB, IP);
    }

    InstrumentationIRBuilder IRB(&*IP);

    if (EntryLoc)
        IRB.SetCurrentDebugLocation(EntryLoc);

    auto FlagPtr = IRB.CreateGEP(
        FunctionBoolArray->getValueType(), FunctionBoolArray,
        {ConstantInt::get(IntptrTy, 0), ConstantInt::get(IntptrTy, Idx)});
    auto Load = IRB.CreateLoad(Int1Ty, FlagPtr);
    auto ThenTerm =
        SplitBlockAndInsertIfThen(IRB.CreateIsNull(Load), &*IP, false);
    IRBuilder<> ThenIRB(ThenTerm);
    auto Store = ThenIRB.CreateStore(ConstantInt::getTrue(Int1Ty), FlagPtr);

    SetNoSanitizeMetadata(Load);
    SetNoSanitizeMetadata(Store);
}
