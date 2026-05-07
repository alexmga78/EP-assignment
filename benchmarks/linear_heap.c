/* linear_heap — sequential read+write walk over a malloc'd array.
 *
 * Expected tracer output:
 *   * IP histogram dominated by this binary (the loop body), with a small
 *     contribution from libc (malloc, printf, ...).
 *   * ADDR histogram concentrated on the "[heap]" object, evenly distributed
 *     across all kBucketsPerObject buckets — because the walk is linear and
 *     the heap allocation is contiguous.
 *
 * Print the buffer's start address on stdout so the reviewer can manually
 * verify perf samples land in the expected range.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    size_t n = 64ull * 1024 * 1024 / sizeof(unsigned long);   /* 64 MiB */
    int passes = 4;
    if (argc > 1) n = (size_t)strtoull(argv[1], NULL, 10);
    if (argc > 2) passes = atoi(argv[2]);

    unsigned long* buf = (unsigned long*)malloc(n * sizeof(*buf));
    if (!buf) { perror("malloc"); return 1; }

    fprintf(stdout, "linear_heap: buf=%p len=%zu bytes pid=%d\n",
            (void*)buf, n * sizeof(*buf), (int)getpid());
    fflush(stdout);

    /* Touch every page once to commit physical memory before sampling kicks in. */
    memset(buf, 0, n * sizeof(*buf));

    unsigned long acc = 0;
    for (int p = 0; p < passes; ++p) {
        for (size_t i = 0; i < n; ++i) {
            acc       += buf[i];      /* read  */
            buf[i]     = i + p;       /* write */
        }
    }

    fprintf(stdout, "linear_heap: done, acc=%lu\n", acc);
    free(buf);
    return 0;
}
