#pragma once
extern "C" void __tinycoverage_init(bool * start, bool * end);

namespace tinycoverage {
void test_finished();
int shut_down();
};

//extern "C" void __tinycoverage_test_finished();
//extern "C" void __tinycoverage_shut_down();
