#pragma once

#include "maps.h"

#include <cstdint>
#include <cstdio>
#include <string>

// Serializes samples as NDJSON to a FILE* (typically a popen pipe to the
// Python plotter). The wire schema is consumed by plot/plotter.py.
class Ipc {
public:
    // Takes ownership of `pipe_file` and closes it on destruction.
    // `is_pipe` selects pclose() (true, popen) vs fclose() (false, fopen);
    // pclose() additionally reaps the child plotter process.
    Ipc(FILE* pipe_file, bool is_pipe);
    ~Ipc();

    Ipc(const Ipc&)            = delete;
    Ipc& operator=(const Ipc&) = delete;

    void send_sample(uint64_t ip, uint64_t addr, bool is_write, uint64_t ts_ns,
                     const Region* ip_region, const Region* addr_region);

    void flush();

private:
    FILE* file_     = nullptr;
    bool  is_pipe_  = false;

    static std::string region_name(const Region* r);
};
