#include "tinycoverage.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

constexpr size_t report_file_size_upper_limit = 200 * 1024;

constexpr uint32_t MagicTestEntry = 0xfefefefe;

size_t bb_count;
bool *counters;
char **func_names;

int report_file_fd;
uint32_t *report_file_ptr;
size_t report_file_pos{0};

void tinycoverage_test_finished() {
    report_file_ptr[report_file_pos++] = MagicTestEntry;

    for (size_t bb_index = 0; bb_index < bb_count; ++bb_index)
        if (counters[bb_index]) {
            report_file_ptr[report_file_pos++] = bb_index;
            counters[bb_index] = false;
        }
}

int tinycoverage_shut_down() {
    if (msync(report_file_ptr, report_file_size_upper_limit, MS_SYNC) == -1)
        return -1;
    if (munmap(report_file_ptr, report_file_size_upper_limit) == -1)
        return -1;

    const size_t file_real_size = ++report_file_pos * sizeof(uint32_t);

    if (ftruncate(report_file_fd, file_real_size) == -1)
        return -1;
    if (close(report_file_fd) == -1)
        return -1;

    return 0;
}

int tinycoverage_init(const char *report_file_name) {
    report_file_fd = open(report_file_name, O_RDWR | O_CREAT | O_TRUNC, 0666);

    if (report_file_fd == -1)
        return -1;

    if (ftruncate(report_file_fd, report_file_size_upper_limit) == -1)
        return -1;

    void *const ptr = mmap(nullptr, report_file_size_upper_limit, PROT_WRITE,
                           MAP_SHARED, report_file_fd, 0);
    if (ptr == MAP_FAILED)
        return -1;

    report_file_ptr = static_cast<uint32_t *>(ptr);
    return 0;
}

extern "C" void __tinycoverage_counters_init(bool *start, bool *end) {
    counters = start;
    bb_count = end - start;
}

extern "C" void __tinycoverage_func_names_init(char **start, char **) {
    func_names = start;
}
