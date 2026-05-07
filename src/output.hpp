#pragma once

#include "mmap_tracker.hpp"
#include "options.hpp"
#include "perf_event.hpp"
#include "records.hpp"
#include "stats.hpp"

#include <memory>

namespace memtracer {

// Output abstraction: the Tracer drives this for every record dispatched
// from the perf ring buffer plus a periodic snapshot.
//
// Two implementations live in this project:
//   * TextSink  — human-readable, writes to stderr; intended for ad-hoc
//                 debugging on the partner's box.
//   * JsonlSink — newline-delimited JSON on stdout; intended to be piped
//                 into scripts/live_plot.py.
class OutputSink {
public:
    virtual ~OutputSink() = default;

    virtual void on_sample(const ParsedSample& s,
                           MemAccessOp         op,
                           const MappedObject* ip_obj,
                           const MappedObject* addr_obj) = 0;

    virtual void on_mmap(const MappedObject& obj) = 0;

    // Periodic histogram dump.
    virtual void on_snapshot(const StatsAggregator& stats) = 0;

    // Kernel reported it dropped `n_lost` samples (consumer too slow).
    virtual void on_lost(uint64_t n_lost) = 0;

    virtual void on_exit(int child_exit_code) = 0;
};

std::unique_ptr<OutputSink> make_sink(const Options& opts);

}  // namespace memtracer
