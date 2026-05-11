#pragma once

#include "maps.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

static constexpr int NUM_BUCKETS = 64;

// Per-bucket counters for a data region.
struct BucketStats {
    uint64_t reads  = 0;
    uint64_t writes = 0;
};

// Aggregated stats for a single memory region (data side).
struct RegionStats {
    uint64_t                      start;
    uint64_t                      end;
    std::string                   name;
    RegionType                    type;
    std::array<BucketStats, NUM_BUCKETS> buckets;
};

// Aggregated stats for a code object (instruction side).
struct CodeStats {
    uint64_t reads  = 0;
    uint64_t writes = 0;
};

// Thread-safety: same as Maps — single consumer thread.
class Stats {
public:
    // Record a memory access.
    //   ip       : instruction pointer at time of access
    //   addr     : memory address accessed
    //   is_write : true = store, false = load
    //   ts_ns    : PERF_SAMPLE_TIME timestamp (nanoseconds)
    //   ip_region, addr_region: looked-up regions (may be nullptr)
    void record(uint64_t ip, uint64_t addr, bool is_write, uint64_t ts_ns,
                const Region* ip_region, const Region* addr_region);

    // Ensure a region entry exists (called when PERF_RECORD_MMAP2 arrives).
    void ensure_region(const Region& r);

    const std::unordered_map<std::string, CodeStats>& code_stats() const {
        return code_stats_;
    }
    const std::unordered_map<std::string, RegionStats>& region_stats() const {
        return region_stats_;
    }

private:
    // Key: object display name (e.g. "libc.so.6", "[heap]", "UNKNOWN")
    std::unordered_map<std::string, CodeStats>   code_stats_;
    std::unordered_map<std::string, RegionStats> region_stats_;

    // Get or create a RegionStats entry for the given region.
    RegionStats& get_or_create_region(const Region& r);
};
