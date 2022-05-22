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

static cl::opt<bool> Enabled("tinycoverage", cl::desc("Instrument each BB"), cl::Hidden, cl::init(false));

namespace {

constexpr char CountersSection[] = "__tinycoverage_counters";
constexpr char FuncNamesSection[] = "__tinycoverage_func_names";

class TinycoveragePass : public ModulePass {
    GlobalVariable *CreateSection(Module &M, Type *Ty, size_t N, StringRef SectionName, Constant *Initializer) const;
    Constant *AddFunctionNameVar(Module &M, Function &F) const;

    void instrumentFunction(Module &M, Function &F);
    void InjectCoverageAtBlock(Module &M, Function &F, BasicBlock &BB, size_t Idx, GlobalVariable *Array);

    Type *IntptrTy, *Int8Ty, *Int1Ty;
    const DataLayout *DataLayout;

    SmallVector<GlobalValue *, 20> GlobalsToAppendToCompilerUsed;

    const DominatorTree *DTCallback(Function &F) {
        return &this->getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
    };

    const PostDominatorTree *PDTCallback(Function &F) {
        return &this->getAnalysis<PostDominatorTreeWrapperPass>(F).getPostDomTree();
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
}

void CreateCallbackCall(Module &M, Type *ItemTy, StringRef SectionName) {
    constexpr auto Linkage = GlobalVariable::ExternalWeakLinkage;

    const Twine SectionStart = "__start_" + SectionName;
    const Twine SectionStop = "__stop_" + SectionName;

    GlobalVariable *const Start = new GlobalVariable(M, ItemTy, false, Linkage, nullptr, SectionStart);
    GlobalVariable *const End = new GlobalVariable(M, ItemTy, false, Linkage, nullptr, SectionStop);

    Start->setVisibility(GlobalValue::HiddenVisibility);
    End->setVisibility(GlobalValue::HiddenVisibility);

    Type *const PtrTy = PointerType::getUnqual(ItemTy);

    const std::string CallbackName = SectionName.str() + "_init";
    const std::string CtorName = "tinycoverage.module_ctor_" + SectionName.str();

    Function *const CtorFunc
        = createSanitizerCtorAndInitFunctions(M, CtorName, CallbackName, {PtrTy, PtrTy}, {Start, End}).first;

    assert(CtorFunc->getName() == CtorName);

    CtorFunc->setComdat(M.getOrInsertComdat(CtorName));

    constexpr uint64_t CtorPriority = 2;
    appendToGlobalCtors(M, CtorFunc, CtorPriority, CtorFunc);
}

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

    CreateCallbackCall(M, Int1Ty, CountersSection);
    CreateCallbackCall(M, PointerType::getUnqual(Int8Ty), FuncNamesSection);

    appendToCompilerUsed(M, GlobalsToAppendToCompilerUsed);

    return true;
}

static bool isFullDominator(const BasicBlock *BB, const DominatorTree *DT) {
    if (succ_empty(BB))
        return false;

    return all_of(successors(BB), [&](const BasicBlock *SUCC) { return DT->dominates(BB, SUCC); });
}

static bool isFullPostDominator(const BasicBlock *BB, const PostDominatorTree *PDT) {
    if (pred_empty(BB))
        return false;

    return all_of(predecessors(BB), [&](const BasicBlock *PRED) { return PDT->dominates(BB, PRED); });
}

static bool shouldInstrumentBlock(const Function &F, const BasicBlock *BB, const DominatorTree *DT,
                                  const PostDominatorTree *PDT) {
    if (isa<UnreachableInst>(BB->getFirstNonPHIOrDbgOrLifetime())
        || BB->getFirstInsertionPt() == BB->end())
        return false;

    if (&F.getEntryBlock() == BB)
        return true;

    return !isFullDominator(BB, DT) && !(isFullPostDominator(BB, PDT) && !BB->getSinglePredecessor());
}

GlobalVariable *TinycoveragePass::CreateSection(
    Module &M, Type *Ty, size_t N, StringRef SectionName,
    Constant *Initializer) const {
    ArrayType *const Type = ArrayType::get(Ty, N);

    GlobalVariable *const Array = new GlobalVariable(M, Type, false, GlobalVariable::PrivateLinkage, Initializer);

    Array->setSection(SectionName);
    Array->setAlignment(Align(DataLayout->getTypeStoreSize(Ty).getFixedSize()));

    return Array;
}

Constant *TinycoveragePass::AddFunctionNameVar(Module &M, Function &F) const {
    const StringRef Name = F.getSubprogram()->getName();

    SmallVector<Constant *> NameVector(Name.size() + 1);

    for (size_t i = 0; i < Name.size(); i++)
        NameVector[i] = ConstantInt::get(Int8Ty, Name[i]);

    NameVector[Name.size()] = ConstantInt::get(Int8Ty, 0);

    ArrayType *StringTy = ArrayType::get(Int8Ty, NameVector.size());

    GlobalVariable *Variable = new GlobalVariable(
        M, StringTy, true, GlobalValue::PrivateLinkage,
        ConstantArray::get(StringTy, NameVector));

    return ConstantExpr::getBitCast(Variable, PointerType::getUnqual(Int8Ty));
}

void TinycoveragePass::instrumentFunction(Module &M, Function &F) {
    if (F.empty()
        || F.getName().find(".module_ctor") != std::string::npos
        || F.getName().startswith("__sanitizer_")
        || F.getName().startswith("__tinycoverage_")
        || F.getLinkage() == GlobalValue::AvailableExternallyLinkage
        || isa<UnreachableInst>(F.getEntryBlock().getTerminator()))
        return;

    SmallVector<BasicBlock *, 16> BlocksToInstrument;

    const DominatorTree *const DT = DTCallback(F);
    const PostDominatorTree *const PDT = PDTCallback(F);

    for (BasicBlock &BB : F)
        if (shouldInstrumentBlock(F, &BB, DT, PDT))
            BlocksToInstrument.push_back(&BB);

    if (BlocksToInstrument.empty())
        return;

    const size_t N = BlocksToInstrument.size();

    GlobalVariable *const Counters = CreateSection(M, Int1Ty, N, CountersSection,
                                                   Constant::getNullValue(Int1Ty));

    GlobalsToAppendToCompilerUsed.push_back(Counters);

    const std::string notesFile = std::string(M.getName()) + ".tcno";
    std::error_code ec;

    raw_fd_ostream out(notesFile, ec, sys::fs::OF_Append);

    if (ec) {
        M.getContext().emitError(Twine("failed to open coverage notes file for writing: ") + ec.message());
        return;
    }

    SmallVector<Constant *> FuncNamesArray;

    for (size_t i = 0; i < N; i++) {
        BasicBlock &BB = *BlocksToInstrument[i];

        InjectCoverageAtBlock(M, F, BB, i, Counters);

        static size_t counter = 0;
        const size_t global_index = counter++;

        out << F.getSubprogram()->getFilename() << " " << F.getName() << " " << global_index << " ";

        std::unordered_set<unsigned int> lineset;

        for (const Instruction &I : *BlocksToInstrument[i])
            if (const DebugLoc &loc = I.getDebugLoc(); loc && loc.getLine() > 0)
                lineset.insert(loc.getLine());

        for (auto line : lineset)
            out << line << " ";

        out << "\n";
        out.flush();
    }

    // Here we could get address of function's name in DWARF's .debug_str, but it's too hard for me.
    Constant *FuncPtr = AddFunctionNameVar(M, F);

    GlobalVariable *const FuncNames = CreateSection(
        M, PointerType::getUnqual(Int8Ty), N, FuncNamesSection, FuncPtr);

    GlobalsToAppendToCompilerUsed.push_back(FuncNames);
}

struct InstrumentationIRBuilder : IRBuilder<> {
    InstrumentationIRBuilder(Instruction *IP) : IRBuilder<>(IP) {
        if (getCurrentDebugLocation())
            return;
        if (DISubprogram *SP = IP->getFunction()->getSubprogram())
            SetCurrentDebugLocation(DILocation::get(SP->getContext(), 0, 0, SP));
    }
};

void TinycoveragePass::InjectCoverageAtBlock(Module &M, Function &F, BasicBlock &BB, size_t Idx,
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

    auto FlagPtr
        = IRB.CreateGEP(Array->getValueType(), Array, {ConstantInt::get(IntptrTy, 0), ConstantInt::get(IntptrTy, Idx)});
    auto Load = IRB.CreateLoad(Int1Ty, FlagPtr);
    auto ThenTerm = SplitBlockAndInsertIfThen(IRB.CreateIsNull(Load), &*IP, false);

    IRBuilder<> ThenIRB(ThenTerm);
    auto Store = ThenIRB.CreateStore(ConstantInt::getTrue(Int1Ty), FlagPtr);

    auto SetNoSanitizeMetadata = [&M](Instruction *I) {
        I->setMetadata(I->getModule()->getMDKindID("nosanitize"), MDNode::get(M.getContext(), None));
    };

    SetNoSanitizeMetadata(Load);
    SetNoSanitizeMetadata(Store);
}

static RegisterPass<TinycoveragePass> RegisterPass("tinycoverage", "Tinycoverage", false, false);

static RegisterStandardPasses RegisterBye(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                          [](const PassManagerBuilder &, legacy::PassManagerBase &PM) {
                                              PM.add(new TinycoveragePass());
                                          });
