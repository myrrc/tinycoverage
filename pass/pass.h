#pragma once
#include "llvm/IR/PassManager.h"

namespace llvm {

// todo check target triple
// todo store in a single section (linux uses less than 64 bytes for addresses)

constexpr char CountersSection[] = "__tinycoverage_counters";
constexpr char FuncNamesSection[] = "__tinycoverage_func_names";
constexpr char CallbackName[] = "__tinycoverage_init";

class TinycoveragePass : public PassInfoMixin<TinycoveragePass> {
  public:
    TinycoveragePass() = default;
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
    static constexpr bool isRequired() { return true; }

  private:
    GlobalVariable *CreateSection(Module &M, Type *Ty, size_t N, StringRef SectionName, Constant *Initializer) const;
    Constant *AddFunctionNameVar(Module &M, Function &F) const;

    void InsertCallbackInvocation(Module &M) const;

    void instrumentFunction(Module &M, Function &F);
    void InjectCoverageAtBlock(Module &M, Function &F, BasicBlock &BB, size_t Idx, GlobalVariable *Array);

    Type *IntptrTy, *Int8Ty, *Int1Ty, *Int8PtrTy;
    const DataLayout *DataLayout;
    FunctionAnalysisManager *FAM;
    SmallVector<GlobalValue *, 20> GlobalsToAppendToCompilerUsed;
};
}
