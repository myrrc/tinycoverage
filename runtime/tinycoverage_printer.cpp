#include "tinycoverage.h"
#include <cstdio>

size_t bb_count;
bool *counters;
char **func_names;

void tinycoverage_test_finished() {
    for (size_t i = 0; i < bb_count; ++i) {
        printf("%s %d\n", func_names[i], counters[i]);
        counters[i] = false;
    }
}

int tinycoverage_shut_down() { return 0; }
int tinycoverage_init(const char *) { return 0; }

extern "C" void __tinycoverage_init(bool *cnt_start, bool *cnt_end, char** names_start) {
    counters = cnt_start;
    func_names = names_start;
    bb_count = cnt_end - cnt_start;
}
