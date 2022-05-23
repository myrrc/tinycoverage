#include "tinycoverage.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>

constexpr size_t report_file_size_upper_limit = 200 * 1024;

constexpr uint32_t MagicTestEntry = 0xfefefeaa;
constexpr uint32_t MagicNamesIndicesStart = 0xfefefeab;
constexpr uint32_t MagicNamesStart = 0xfefefeac;

size_t bb_count;
bool *counters;
char **func_names;

int report_file_fd;
uint32_t *report_file_ptr;
uint32_t *report_file_ptr_pos;

void write(uint32_t value) {
    *report_file_ptr_pos = value;
    ++report_file_ptr_pos;
}

void write(std::string_view s) {
    write((s.size() / 4) + 1);

    char *ptr = reinterpret_cast<char *>(report_file_ptr_pos);
    ptr = std::copy_n(s.data(), s.size(), ptr);
    ptr = std::copy_n("\0\0\0\0", 4 - s.size() % 4, ptr);
    report_file_ptr_pos = reinterpret_cast<uint32_t *>(ptr);
}

void tinycoverage_test_finished() {
    write(MagicTestEntry);

    for (size_t bb_index = 0; bb_index < bb_count; ++bb_index)
        if (counters[bb_index]) {
            write(bb_index);
            counters[bb_index] = false;
        }
}

void emit_func_names_set() {
    write(MagicNamesIndicesStart);

    std::unordered_map<char *, size_t> name2index;

    for (size_t i = 0; i < bb_count; ++i)
        if (const auto it = name2index.find(func_names[i]); it == name2index.end()) {
            const size_t index = name2index.size();
            name2index[func_names[i]] = index;
            write(index);
        } else {
            write(it->second);
        }

    write(MagicNamesStart);

    for (auto [name_ptr, index] : name2index) {
        write(index);
        write(name_ptr);
    }
}

int tinycoverage_shut_down() {
    emit_func_names_set();

    if (msync(report_file_ptr, report_file_size_upper_limit, MS_SYNC) == -1)
        return -1;
    if (munmap(report_file_ptr, report_file_size_upper_limit) == -1)
        return -1;

    const size_t file_real_size = (report_file_ptr_pos + 1 - report_file_ptr) * sizeof(uint32_t);

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
    report_file_ptr_pos = report_file_ptr;
    return 0;
}

extern "C" void __tinycoverage_init(bool *cnt_start, bool *cnt_end, char **names_start) {
    counters = cnt_start;
    func_names = names_start;
    bb_count = cnt_end - cnt_start;
}
