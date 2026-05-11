#pragma once

#include "maps.h"

#include <cstdint>
#include <cstdio>
#include <string>

// IPC layer: serializes sample events as newline-delimited JSON and writes them
// to a FILE* (typically a pipe opened to the Python plotter subprocess).
class Ipc {
public:
    // pipe_file  : writable FILE* (from popen or fopen).
    // is_pipe    : true if opened with popen() — destructor will use pclose();
    //              false if opened with fopen() — destructor will use fclose().
    // Takes ownership and closes on destruction.
    Ipc(FILE* pipe_file, bool is_pipe);
    ~Ipc();

    // Disable copy.
    Ipc(const Ipc&)            = delete;
    Ipc& operator=(const Ipc&) = delete;

    // Write one JSON record for a memory access sample.
    void send_sample(uint64_t ip, uint64_t addr, bool is_write, uint64_t ts_ns,
                     const Region* ip_region, const Region* addr_region);

    // Flush the underlying stream (call periodically from the main loop).
    void flush();

private:
    FILE* file_     = nullptr;
    bool  is_pipe_  = false;

    static std::string region_name(const Region* r);
};
