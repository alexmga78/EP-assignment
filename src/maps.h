#pragma once

#include <cstdint>
#include <map>
#include <string>

// Type of the memory region
enum class RegionType {
    FILE_TEXT,  // executable segment of a mapped file
    FILE_DATA,  // data/bss segment of a mapped file
    HEAP,
    STACK,
    VDSO,
    ANON,       // anonymous mapping (JIT, etc.)
    UNKNOWN,
};

struct Region {
    uint64_t    start;
    uint64_t    end;        // exclusive
    std::string name;       // object name (e.g. "libc.so.6") or "[heap]"
    RegionType  type;
};

// Address-to-region map.
// Thread-safety: not needed — all updates and lookups happen in the single
// perf record consumer thread.
class Maps {
public:
    // Seed from /proc/<pid>/maps.
    void seed(pid_t pid);

    // Insert or replace a region (called on PERF_RECORD_MMAP2).
    void insert(uint64_t start, uint64_t len, const std::string& filename,
                bool is_exec);

    // Look up the region that contains `addr`.  Returns nullptr if not found.
    const Region* lookup(uint64_t addr) const;

private:
    // Key: region start address.  Entries must not overlap.
    std::map<uint64_t, Region> regions_;
};
