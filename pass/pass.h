#pragma once
#include "llvm/IR/PassManager.h"

namespace llvm {

class TinycoveragePass : public PassInfoMixin<TinycoveragePass> {
  public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
    static constexpr bool isRequired() { return true; }

  private:
    GlobalVariable *CreateSection(Module &M, Type *Ty, size_t N, StringRef SectionName, Constant *Initializer) const;
    Constant *AddFunctionNameVar(Module &M, Function &F) const;

    void InsertCallbackInvocation(Module &M) const;

    void instrumentFunction(Module &M, Function &F);
    void InstrumentBlock(Module &M, Function &F, BasicBlock &BB, size_t Idx, GlobalVariable *Array);

    Type *IntptrTy, *Int8Ty, *Int1Ty;
    const DataLayout *DataLayout;
    FunctionAnalysisManager *FAM;
    SmallVector<GlobalValue *> Globals;
};
}
