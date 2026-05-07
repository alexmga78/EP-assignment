/* random_heap — uniform random reads/writes over a malloc'd array.
 *
 * Expected tracer output:
 *   * ADDR histogram on "[heap]" should be approximately *flat* across all
 *     kBucketsPerObject buckets — this is the sanity check for sampling
 *     uniformity (the documentation question "what guarantee do we have
 *     that the sampling is uniform?").
 *
 * A simple xorshift64 keeps libc out of the inner loop so the IP histogram
 * pins to this binary, not libc's rand().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static inline unsigned long xorshift64(unsigned long* s) {
    unsigned long x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

int main(int argc, char** argv) {
    size_t n = 8ull * 1024 * 1024 / sizeof(unsigned long);    /* 8 MiB */
    long iters = 50 * 1000 * 1000;
    if (argc > 1) n     = (size_t)strtoull(argv[1], NULL, 10);
    if (argc > 2) iters = atol(argv[2]);

    unsigned long* buf = (unsigned long*)malloc(n * sizeof(*buf));
    if (!buf) { perror("malloc"); return 1; }
    memset(buf, 0, n * sizeof(*buf));

    fprintf(stdout, "random_heap: buf=%p len=%zu bytes pid=%d\n",
            (void*)buf, n * sizeof(*buf), (int)getpid());
    fflush(stdout);

    unsigned long s = 0x12345678cafef00d;
    unsigned long acc = 0;
    for (long i = 0; i < iters; ++i) {
        size_t idx = (size_t)(xorshift64(&s) % n);
        acc       += buf[idx];           /* read  */
        buf[idx]   = (unsigned long)i;   /* write */
    }

    fprintf(stdout, "random_heap: done, acc=%lu\n", acc);
    free(buf);
    return 0;
}
