#pragma once

#include <cstdint>
#include <map>
#include <string>

enum class RegionType {
    FILE_TEXT,  // executable segment of a mapped file
    FILE_DATA,  // any non-executable segment of a mapped file (data, bss, rodata)
    HEAP,
    STACK,
    VDSO,
    ANON,       // anonymous mapping (e.g. JIT, manual mmap)
    UNKNOWN,
};

struct Region {
    uint64_t    start;
    uint64_t    end;        // exclusive
    std::string name;       // e.g. "libc.so.6", "[heap]"; empty for unnamed anon
    RegionType  type;
};

// Single-consumer: all access happens on the thread that drains perf records.
class Maps {
public:
    // Seeds the map from /proc/<pid>/maps. Throws on read failure.
    void seed(pid_t pid);

    // Inserts a region observed via PERF_RECORD_MMAP2, evicting any prior
    // entry that overlaps [start, start+len). Required because the kernel
    // reuses address ranges after munmap.
    void insert(uint64_t start, uint64_t len, const std::string& filename,
                bool is_exec);

    // Returns the region containing `addr`, or nullptr.
    const Region* lookup(uint64_t addr) const;

private:
    // Keyed by start address; entries are non-overlapping (invariant
    // enforced by insert()).
    std::map<uint64_t, Region> regions_;
};
