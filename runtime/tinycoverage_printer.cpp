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

extern "C" void __tinycoverage_counters_init(bool *start, bool *end) {
    counters = start;
    bb_count = end - start;
}

extern "C" void __tinycoverage_func_names_init(char **start, char **) {
    func_names = start;
}
