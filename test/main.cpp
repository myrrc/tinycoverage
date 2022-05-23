#include "../runtime/tinycoverage.h"
#include "bar/bar.h"
#include "foo.h"

int main(int argc, char **) {
    tinycoverage_init("/home/myrrc/tinycoverage/report");

    if (argc > 1) {
        inline_func_in_src();
    } else {
        normal_func_in_src();
    }

    template_func_in_src<int>();

    tinycoverage_test_finished();

    if (argc == 1) {
        inline_func_in_src();
    } else {
        normal_func_in_src();
    }

    template_func_in_src<bool>();

    tinycoverage_test_finished();
    tinycoverage_shut_down();
}
