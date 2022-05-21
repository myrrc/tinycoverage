#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

using namespace llvm;

static cl::opt<bool> Wave("wave-goodbye", cl::init(false),
                          cl::desc("wave good bye"));

namespace {

struct LegacyBye : public ModulePass {
    static inline char ID = 0;

    LegacyBye() : ModulePass(ID) {}

    bool runOnModule(Module &M) final {
        if (!Wave) {
            return false;
        }

        errs() << "Module: " << M.getName() << "\n";

        for (Function &F : M) {
            if (F.empty())
                continue;

            errs() << "Bye: " << F.getName() << '\n';
        }

        return true;
    }
};
} // namespace

static RegisterPass<LegacyBye>
    X("tinycoverage",
      "Pass that instruments basic blocks and emits block info to files for "
      "each translation unit",
      false /* Only looks at CFG */, false /* Analysis Pass */);

static RegisterStandardPasses
    RegisterBye(PassManagerBuilder::EP_EnabledOnOptLevel0,
                [](const PassManagerBuilder &, legacy::PassManagerBase &PM) {
                    PM.add(new LegacyBye());
                });
