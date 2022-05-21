#pragma once
extern "C" void __sanitizer_cov_bool_flag_init(bool * start, bool * end);

namespace tinycoverage {
void test_finished();
int shut_down();
};
