#pragma once
#include "llvm/IR/PassManager.h"

namespace llvm {
struct TinycoveragePass : public PassInfoMixin<TinycoveragePass> {
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
    static constexpr bool isRequired() { return true; }

  private:
    FunctionAnalysisManager *FAM;
    Type *CounterTy, *FuncNameTy, *IntptrTy;
    Align CountersAlign, FuncNamesAlign;
    SmallVector<GlobalValue *> Globals;

    using BBInfo = SmallVector<int>; // lineset
    using FuncInfo = SmallVector<BBInfo>;

    // [source file name -> [function name -> function info]]
    StringMap<StringMap<FuncInfo>> ModuleInfo;

    void insertCallbackInvocation(Module &M) const;
    void instrumentFunction(Module &M, Function &F);
    void instrumentBlock(Module &M, Function &F, BasicBlock &BB, size_t Idx, GlobalVariable *Array);
    void collectBBInfo(const Function &F, const BasicBlock &BB, BBInfo &BBI);
    void emitCUInfo(const Module &M, const DICompileUnit& CU) const;
};
}
