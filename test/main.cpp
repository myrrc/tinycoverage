#include "../runtime/tinycoverage.h"
#include "bar/bar.h"
#include "foo.h"

int main(int argc, char **) {
    tinycoverage_init("/home/myrrc/tinycoverage/report");

    if (argc > 1) {
        foo();
    } else {
        bar();
    }

    tinycoverage_test_finished();

    if (argc == 1) {
        foo();
    } else {
        bar();
    }

    tinycoverage_test_finished();
    tinycoverage_shut_down();
}
