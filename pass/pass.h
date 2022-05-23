#pragma once
#include "llvm/IR/PassManager.h"

namespace llvm {

class TinycoveragePass : public PassInfoMixin<TinycoveragePass> {
  public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
    static constexpr bool isRequired() { return true; }

  private:
    Type *IntptrTy, *Int8Ty, *Int1Ty;
    const DataLayout *DataLayout;
    FunctionAnalysisManager *FAM;
    SmallVector<GlobalValue *> Globals;

    using BBInfo = SmallVector<int>; // lineset
    using FuncInfo = SmallVector<BBInfo>;

    // [source file name -> [function name -> function info]]
    StringMap<StringMap<FuncInfo>> ModuleInfo;

    GlobalVariable *createSection(Module &M, Type *Ty, size_t N, StringRef SectionName, Constant *Init) const;
    Constant *addFunctionNameVar(Module &M, Function &F) const;

    void insertCallbackInvocation(Module &M) const;

    void instrumentFunction(Module &M, Function &F);
    void instrumentBlock(Module &M, Function &F, BasicBlock &BB, size_t Idx, GlobalVariable *Array);
    void collectBBInfo(const Function &F, const BasicBlock &BB, BBInfo &BBI);
    void emitModuleInfo(const Module&M) const;
};
}
