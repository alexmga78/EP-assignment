#pragma once

#include "mmap_tracker.hpp"
#include "perf_event.hpp"
#include "records.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace memtracer {

inline constexpr size_t kBucketsPerObject = 64;

struct Bucket {
    uint64_t reads  = 0;
    uint64_t writes = 0;
};

// Snapshot of one object's R/W histogram. The `buckets` array divides
// [start, end) into kBucketsPerObject equal-size slices. The plotter renders
// these as a bar chart; many small objects show up as 64 bars each.
struct ObjectStats {
    std::string display;
    uint64_t    start = 0;
    uint64_t    end   = 0;
    std::array<Bucket, kBucketsPerObject> buckets{};
    uint64_t    total_reads  = 0;
    uint64_t    total_writes = 0;
};

// StatsAggregator owns:
//   * `code_` — ObjectStats keyed by the IP-resolved region's start address.
//                Buckets index instructions within their containing object.
//   * `data_` — ObjectStats keyed by the ADDR-resolved region's start address.
//                Buckets index data addresses within their containing object.
//
// Both halves are independent; a single sample updates one entry in each (or
// neither, when the address is unresolvable).
//
// When window_ns > 0, every sample is also enqueued in `window_` along with
// the bucket it landed in. roll_window(now_ns) decrements those buckets as
// the samples age out — this is the bonus task: "show the memory access
// distribution for the past N seconds".
class StatsAggregator {
public:
    explicit StatsAggregator(uint64_t window_ns = 0);

    void on_sample(const ParsedSample&  s,
                   MemAccessOp          op,
                   const MappedObject*  ip_obj,
                   const MappedObject*  addr_obj);

    // Called when a new MMAP2 record arrives. Used to (re)seed the display
    // name / range so the snapshot has up-to-date metadata even if no sample
    // has yet been attributed to the new region.
    void on_mmap(const MappedObject& obj);

    // Window mode only: remove samples older than (now_ns - window_ns).
    void roll_window(uint64_t now_ns);

    uint64_t window_ns() const { return window_ns_; }
    uint64_t total_samples() const { return total_samples_; }
    uint64_t lost_samples()  const { return lost_samples_;  }
    void note_lost(uint64_t n) { lost_samples_ += n; }

    // Snapshot copies (cheap relative to the cost of a sample). The plotter
    // calls these on its tick.
    std::vector<ObjectStats> code_snapshot() const;
    std::vector<ObjectStats> data_snapshot() const;

private:
    struct TimedSample {
        uint64_t ts;
        uint8_t  op;          // 0=Load, 1=Store
        uint64_t code_key;    // 0 if code unresolved
        uint8_t  code_bucket;
        uint64_t data_key;    // 0 if data unresolved
        uint8_t  data_bucket;
    };

    static uint8_t bucket_of(uint64_t addr, uint64_t start, uint64_t end);
    void apply(ObjectStats& o, uint8_t bucket, MemAccessOp op, int64_t delta);
    ObjectStats& touch(std::unordered_map<uint64_t, ObjectStats>& tbl,
                       const MappedObject& obj);

    std::unordered_map<uint64_t, ObjectStats> code_;
    std::unordered_map<uint64_t, ObjectStats> data_;

    uint64_t window_ns_      = 0;
    std::deque<TimedSample> window_;

    uint64_t total_samples_  = 0;
    uint64_t lost_samples_   = 0;
};

}  // namespace memtracer
