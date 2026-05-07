/* mostly_libc — heavy memcpy / printf to put libc on the IP histogram top.
 *
 * Expected tracer output:
 *   * IP histogram dominated by "libc.so.6" (memcpy/memmove + stdio paths),
 *     with only a small fraction attributed to this binary.
 *   * ADDR histogram split between heap (the source/dest buffers) and the
 *     stdout buffer (libc bss/data, depending on the libc version).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    size_t n = 4ull * 1024 * 1024;        /* 4 MiB per buffer */
    int iters = 200;
    if (argc > 1) n     = (size_t)strtoull(argv[1], NULL, 10);
    if (argc > 2) iters = atoi(argv[2]);

    char* a = (char*)malloc(n);
    char* b = (char*)malloc(n);
    if (!a || !b) { perror("malloc"); return 1; }
    memset(a, 0xA5, n);
    memset(b, 0x5A, n);

    fprintf(stdout, "mostly_libc: a=%p b=%p len=%zu pid=%d\n",
            (void*)a, (void*)b, n, (int)getpid());

    for (int i = 0; i < iters; ++i) {
        memcpy(b, a, n);
        memcpy(a, b, n);
        if ((i & 31) == 0) fprintf(stdout, "mostly_libc: i=%d\n", i);
    }

    fprintf(stdout, "mostly_libc: done, a[0]=%d b[0]=%d\n", (int)a[0], (int)b[0]);
    free(a); free(b);
    return 0;
}
