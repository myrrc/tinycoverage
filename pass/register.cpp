#include "pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

PassPluginLibraryInfo getTinycoveragePluginInfo() {
    const auto callback = [](PassBuilder &PB) {
        PB.registerPipelineEarlySimplificationEPCallback([&](ModulePassManager &MPM, auto) {
            MPM.addPass(TinycoveragePass());
            return true;
        });
    };

    return {LLVM_PLUGIN_API_VERSION, "tinycoverage", "0.0.1", callback};
};

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return getTinycoveragePluginInfo();
}
