// linear.cpp — sequential heap traversal benchmark.
// Expected output: bucket histogram fills left-to-right.
//
// Usage:  ./benchmarks/linear [size_mb] [iterations]

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    size_t size_mb   = argc > 1 ? static_cast<size_t>(std::atol(argv[1])) : 64;
    int    iters     = argc > 2 ? std::atoi(argv[2]) : 4;
    size_t n_bytes   = size_mb * 1024 * 1024;
    size_t n_longs   = n_bytes / sizeof(long);

    auto* buf = static_cast<volatile long*>(
        std::malloc(n_bytes));
    if (!buf) {
        std::perror("malloc");
        return 1;
    }

    // Initialise (write pass).
    for (size_t i = 0; i < n_longs; ++i) buf[i] = static_cast<long>(i);

    // Read passes: sequential left-to-right.
    long sink = 0;
    for (int it = 0; it < iters; ++it) {
        for (size_t i = 0; i < n_longs; ++i) {
            sink += buf[i];
        }
    }

    // Prevent optimisation.
    if (sink == 0) std::puts("zero");

    std::free(const_cast<long*>(buf));
    std::fprintf(stderr, "[linear] done: %zu MB x %d iterations\n",
                 size_mb, iters);
    return 0;
}
