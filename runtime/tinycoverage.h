#pragma once
extern "C" void __tinycoverage_init(bool * start, bool * end);

namespace tinycoverage {
void test_finished();
int shut_down();
};
