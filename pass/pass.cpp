#include "unordered_set"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

static cl::opt<bool> Enabled("tinycoverage", cl::desc("Instrument each BB"),
                             cl::Hidden, cl::init(false));

namespace {

constexpr char CtorName[] = "tinycoverage.module_ctor";
constexpr char CallbackName[] = "__tinycoverage_init";
constexpr char SectionName[] = "tinycoverage";
constexpr char SectionVariableName[] = "__tinycoverage_gen_";

constexpr char SectionNamePrefixed[] = "__tinycoverage";
constexpr char SectionStartName[] = "__start___tinycoverage";
constexpr char SectionStopName[] = "__stop___tinycoverage";

class TinycoveragePass : public ModulePass {
    GlobalVariable *CreateSection(Module &M, size_t N) const;
    void instrumentFunction(Module &M, Function &F);
    void InjectCoverageAtBlock(Module &M, Function &F, BasicBlock &BB,
                               size_t Idx, GlobalVariable *Array);
    void CreateCallbackCall(Module &M);

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

  public:
    static inline char ID = 0;

    TinycoveragePass() : ModulePass(ID) {}

    bool runOnModule(Module &M) final;

    void getAnalysisUsage(AnalysisUsage &AU) const final {
        AU.addRequired<DominatorTreeWrapperPass>();
        AU.addRequired<PostDominatorTreeWrapperPass>();
    }
};
} // namespace

bool TinycoveragePass::runOnModule(Module &M) {
    if (!Enabled) {
        return false;
    }

    DataLayout = &M.getDataLayout();

    IRBuilder<> builder(M.getContext());

    IntptrTy = builder.getIntNTy(DataLayout->getPointerSizeInBits());
    Int8Ty = builder.getInt8Ty();
    Int1Ty = builder.getInt1Ty();

    GlobalsToAppendToCompilerUsed.clear();

    for (Function &F : M)
        instrumentFunction(M, F);

    CreateCallbackCall(M);

    appendToCompilerUsed(M, GlobalsToAppendToCompilerUsed);

    return true;
}

void TinycoveragePass::CreateCallbackCall(Module &M) {
    const auto Linkage = GlobalVariable::ExternalWeakLinkage;

    GlobalVariable *const Start = new GlobalVariable(M, Int1Ty, false, Linkage,
                                                     nullptr, SectionStartName);

    GlobalVariable *const End =
        new GlobalVariable(M, Int1Ty, false, Linkage, nullptr, SectionStopName);

    Start->setVisibility(GlobalValue::HiddenVisibility);
    End->setVisibility(GlobalValue::HiddenVisibility);

    Type *const PtrTy = PointerType::getUnqual(Int1Ty);

    Function *const CtorFunc =
        createSanitizerCtorAndInitFunctions(M, CtorName, CallbackName,
                                            {PtrTy, PtrTy}, {Start, End})
            .first;

    assert(CtorFunc->getName() == CtorName);

    CtorFunc->setComdat(M.getOrInsertComdat(CtorName));

    constexpr uint64_t CtorPriority = 2;
    appendToGlobalCtors(M, CtorFunc, CtorPriority, CtorFunc);
}

static bool isFullDominator(const BasicBlock *BB, const DominatorTree *DT) {
    if (succ_empty(BB))
        return false;

    return llvm::all_of(successors(BB), [&](const BasicBlock *SUCC) {
        return DT->dominates(BB, SUCC);
    });
}

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
    if (isa<UnreachableInst>(BB->getFirstNonPHIOrDbgOrLifetime()) ||
        BB->getFirstInsertionPt() == BB->end())
        return false;

    if (&F.getEntryBlock() == BB)
        return true;

    return !isFullDominator(BB, DT) &&
           !(isFullPostDominator(BB, PDT) && !BB->getSinglePredecessor());
}

GlobalVariable *TinycoveragePass::CreateSection(Module &M, size_t N) const {
    ArrayType *const Type = ArrayType::get(Int1Ty, N);

    GlobalVariable *const Array =
        new GlobalVariable(M, Type, false, GlobalVariable::PrivateLinkage,
                           Constant::getNullValue(Type), SectionVariableName);

    Array->setSection(SectionNamePrefixed);
    Array->setAlignment(
        Align(DataLayout->getTypeStoreSize(Int1Ty).getFixedSize()));

    return Array;
}

void TinycoveragePass::instrumentFunction(Module &M, Function &F) {
    if (F.empty() || F.getName().find(".module_ctor") != std::string::npos ||
        F.getName().startswith("__sanitizer_") ||
        F.getName().startswith("__tinycoverage_") ||
        F.getLinkage() == GlobalValue::AvailableExternallyLinkage ||
        isa<UnreachableInst>(F.getEntryBlock().getTerminator()))
        return;

    SmallVector<BasicBlock *, 16> BlocksToInstrument;

    const DominatorTree *const DT = DTCallback(F);
    const PostDominatorTree *const PDT = PDTCallback(F);

    for (BasicBlock &BB : F)
        if (shouldInstrumentBlock(F, &BB, DT, PDT))
            BlocksToInstrument.push_back(&BB);

    if (BlocksToInstrument.empty())
        return;

    GlobalVariable *const Array = CreateSection(M, BlocksToInstrument.size());
    GlobalsToAppendToCompilerUsed.push_back(Array);

    const std::string notesFile = std::string(M.getName()) + ".tcno";
    std::error_code ec;

    raw_fd_ostream out(notesFile, ec, sys::fs::OF_Append);

    if (ec) {
        M.getContext().emitError(
            Twine("failed to open coverage notes file for writing: ") +
            ec.message());
        return;
    }

    for (size_t i = 0, N = BlocksToInstrument.size(); i < N; i++) {
        InjectCoverageAtBlock(M, F, *BlocksToInstrument[i], i, Array);

        static size_t counter = 0;
        const size_t global_index = counter++;

        errs() << global_index << "\n";

        out << F.getSubprogram()->getFilename() << " " << F.getName() << " "
            << global_index << " ";

        std::unordered_set<unsigned int> lineset;

        for (const Instruction &I : *BlocksToInstrument[i])
            if (const DebugLoc &loc = I.getDebugLoc(); loc && loc.getLine() > 0)
                lineset.insert(loc.getLine());

        for (auto line : lineset)
            out << line << " ";

        out << "\n";
        out.flush();
    }
}

struct InstrumentationIRBuilder : IRBuilder<> {
    static void ensureDebugInfo(IRBuilder<> &IRB, const Function &F) {
        if (IRB.getCurrentDebugLocation())
            return;
        if (DISubprogram *SP = F.getSubprogram())
            IRB.SetCurrentDebugLocation(
                DILocation::get(SP->getContext(), 0, 0, SP));
    }

    explicit InstrumentationIRBuilder(Instruction *IP) : IRBuilder<>(IP) {
        ensureDebugInfo(*this, *IP->getFunction());
    }
};

void TinycoveragePass::InjectCoverageAtBlock(Module &M, Function &F,
                                             BasicBlock &BB, size_t Idx,
                                             GlobalVariable *Array) {
    BasicBlock::iterator IP = BB.getFirstInsertionPt();
    const bool IsEntryBB = &BB == &F.getEntryBlock();

    DebugLoc EntryLoc;

    if (IsEntryBB) {
        if (DISubprogram *SP = F.getSubprogram()) {
            EntryLoc =
                DILocation::get(SP->getContext(), SP->getScopeLine(), 0, SP);
        }

        IP = PrepareToSplitEntryBlock(BB, IP);
    }

    InstrumentationIRBuilder IRB(&*IP);

    if (EntryLoc)
        IRB.SetCurrentDebugLocation(EntryLoc);

    auto FlagPtr = IRB.CreateGEP(
        Array->getValueType(), Array,
        {ConstantInt::get(IntptrTy, 0), ConstantInt::get(IntptrTy, Idx)});
    auto Load = IRB.CreateLoad(Int1Ty, FlagPtr);
    auto ThenTerm =
        SplitBlockAndInsertIfThen(IRB.CreateIsNull(Load), &*IP, false);

    IRBuilder<> ThenIRB(ThenTerm);
    auto Store = ThenIRB.CreateStore(ConstantInt::getTrue(Int1Ty), FlagPtr);

    auto SetNoSanitizeMetadata = [&M](Instruction *I) {
        I->setMetadata(I->getModule()->getMDKindID("nosanitize"),
                       MDNode::get(M.getContext(), None));
    };

    SetNoSanitizeMetadata(Load);
    SetNoSanitizeMetadata(Store);
}

static RegisterPass<TinycoveragePass> RegisterPass("tinycoverage", "Tinycoverage", false,
                                        false);

static RegisterStandardPasses
    RegisterBye(PassManagerBuilder::EP_EnabledOnOptLevel0,
                [](const PassManagerBuilder &, legacy::PassManagerBase &PM) {
                    PM.add(new TinycoveragePass());
                });
