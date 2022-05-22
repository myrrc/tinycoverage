#pragma once
extern "C" void __tinycoverage_counters_init(bool *start, bool *end);
extern "C" void __tinycoverage_func_names_init(char **start, char **end);

int tinycoverage_init(const char *report_file_name);
int tinycoverage_shut_down();
void tinycoverage_test_finished();
