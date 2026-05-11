#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    size_t size_mb   = argc > 1 ? static_cast<size_t>(std::atol(argv[1])) : 64;
    int    iters     = argc > 2 ? std::atoi(argv[2]) : 4;
    size_t n_bytes   = size_mb * 1024 * 1024;
    size_t n_longs   = n_bytes / sizeof(long);

    // `volatile` is what actually keeps the compiler from eliminating
    // the read loop below; the `if (sink == 0)` escape at the end is
    // visible but insufficient on its own under -O2.
    auto* buf = static_cast<volatile long*>(
        std::malloc(n_bytes));
    if (!buf) {
        std::perror("malloc");
        return 1;
    }

    for (size_t i = 0; i < n_longs; ++i) buf[i] = static_cast<long>(i);

    long sink = 0;
    for (int it = 0; it < iters; ++it) {
        for (size_t i = 0; i < n_longs; ++i) {
            sink += buf[i];
        }
    }

    if (sink == 0) std::puts("zero");

    std::free(const_cast<long*>(buf));
    std::fprintf(stderr, "[linear] done: %zu MB x %d iterations\n",
                 size_mb, iters);
    return 0;
}
