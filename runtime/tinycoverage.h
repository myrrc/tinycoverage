#pragma once
extern "C" void __tinycoverage_init(bool *cnt_start, bool *cnt_end, char** names_start);

int tinycoverage_init(const char *report_file_name);
int tinycoverage_shut_down();
void tinycoverage_test_finished();
