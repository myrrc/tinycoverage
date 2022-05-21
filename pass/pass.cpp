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

void runBye(Function &F) {
    if (F.empty())
        return;

    errs() << "Bye: ";
    errs().write_escaped(F.getName()) << '\n';
}

struct LegacyBye : public ModulePass {
    static inline char ID = 0;

    LegacyBye() : ModulePass(ID) {}

    bool runOnModule(Module &M) final {
        if (!Wave) {
            return false;
        }

        for (Function &F : M)
            runBye(F);

        return true;
    }
};
} // namespace

static RegisterPass<LegacyBye> X("goodbye", "Good Bye World Pass",
                                 false /* Only looks at CFG */,
                                 false /* Analysis Pass */);

/* Legacy PM Registration */
static llvm::RegisterStandardPasses RegisterBye(
    llvm::PassManagerBuilder::EP_EarlyAsPossible,
    [](const llvm::PassManagerBuilder &Builder,
       llvm::legacy::PassManagerBase &PM) { PM.add(new LegacyBye()); });
