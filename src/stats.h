#pragma once

#include "maps.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// 64 buckets per region matches the plotter's horizontal resolution and
// keeps RegionStats small enough to hold thousands of regions cheaply.
static constexpr int NUM_BUCKETS = 64;

struct BucketStats {
    uint64_t reads  = 0;
    uint64_t writes = 0;
};

struct RegionStats {
    uint64_t                      start;
    uint64_t                      end;
    std::string                   name;
    RegionType                    type;
    std::array<BucketStats, NUM_BUCKETS> buckets;
};

struct CodeStats {
    uint64_t reads  = 0;
    uint64_t writes = 0;
};

// Single-consumer, same threading model as Maps.
class Stats {
public:
    // Records one memory access. `ip_region` / `addr_region` may be null
    // when an address falls outside any known region - e.g. kernel-side
    // accesses we don't track, or a sample arriving for a page that has
    // already been munmap'd by the tracee.
    void record(uint64_t ip, uint64_t addr, bool is_write, uint64_t ts_ns,
                const Region* ip_region, const Region* addr_region);

    // Creates an empty RegionStats entry for `r` if one doesn't exist.
    // Called eagerly on PERF_RECORD_MMAP2 so unused regions still appear
    // in the final summary as zero rows.
    void ensure_region(const Region& r);

    const std::unordered_map<std::string, CodeStats>& code_stats() const {
        return code_stats_;
    }
    const std::unordered_map<std::string, RegionStats>& region_stats() const {
        return region_stats_;
    }

private:
    // Region key format: "<name>@<start>" (or "ANON@<start>" / "UNKNOWN").
    // The start address is part of the key so two anonymous mappings at
    // different addresses don't collide into a single bucket histogram.
    std::unordered_map<std::string, CodeStats>   code_stats_;
    std::unordered_map<std::string, RegionStats> region_stats_;

    RegionStats& get_or_create_region(const Region& r);
};
