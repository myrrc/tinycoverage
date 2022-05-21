#include "tinycoverage.h"
#include <cstdlib>
#include <cstdint>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

constexpr const char report_path[] = "/home/myrrc/tinycoverage/report";
constexpr size_t report_file_size_upper_limit = 200 * 1024 * 1024;

constexpr uint32_t MagicTestEntry = 0xfefefefe;

bool* table;
size_t bb_count;
int report_file_fd;
uint32_t* report_file_ptr;
size_t report_file_pos{0};

void (*init_abort_callback)() = abort;

constexpr bool exchange(bool& obj, bool target)
{
    bool out = obj;
    obj = target;
    return out;
}

namespace tinycoverage
{
void test_finished()
{
    report_file_ptr[report_file_pos++] = MagicTestEntry;

    const size_t count = bb_count;

    for (size_t bb_index = 0; bb_index < count; ++bb_index)
        if (exchange(table[bb_index], false))
            report_file_ptr[report_file_pos++] = bb_index;
}

int shut_down()
{
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

void init()
{
    report_file_fd = open(report_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (report_file_fd == -1)
        init_abort_callback();

    if (ftruncate(report_file_fd, report_file_size_upper_limit) == -1)
        init_abort_callback();

    void* const ptr = mmap(nullptr, report_file_size_upper_limit, PROT_WRITE, MAP_SHARED, report_file_fd, 0);
    if (ptr == MAP_FAILED)
        init_abort_callback();

    report_file_ptr = static_cast<uint32_t*>(ptr);
}
};  // namespace tinycoverage

extern "C" void __tinycoverage_init(bool* start, bool* end)
{
    table = start;
    bb_count = end - start;

    for (; start != end; ++start)
        *start = false;

    tinycoverage::init();
}
