#include "test.h"

#include "core/sysmem.h"

TEST(sysmem_reports_free_and_total_memory) {
    const SystemMemory m = systemMemory();
    // Any machine that can run the tests has a few hundred MiB free and more in all.
    CHECK(m.freeBytes > (size_t(64) << 20));
    CHECK(m.totalBytes >= m.freeBytes);
    CHECK(m.totalBytes < (size_t(1) << 50));
}

TEST(sysmem_free_helper_matches_the_report) {
    const size_t a = freeHostBytes();
    const SystemMemory m = systemMemory();
    // Free memory moves a little between two calls, never by half of the machine.
    const size_t diff = a > m.freeBytes ? a - m.freeBytes : m.freeBytes - a;
    CHECK(diff < m.totalBytes / 2);
}
