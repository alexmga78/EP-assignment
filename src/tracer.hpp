#pragma once

#include "child_process.hpp"
#include "mmap_tracker.hpp"
#include "options.hpp"
#include "output.hpp"
#include "perf_event.hpp"
#include "records.hpp"
#include "ring_buffer.hpp"
#include "stats.hpp"

#include <memory>
#include <linux/perf_event.h>

namespace memtracer {

// Top-level orchestrator. Owns every other component. Lifetime mirrors the
// program lifetime; constructed once in main, run() drives until the child
// exits (or the user signals SIGINT).
class Tracer {
public:
    explicit Tracer(Options opts);
    ~Tracer();

    // Returns the child process exit code (or 128+signal). Throws on
    // unrecoverable setup failures.
    int run();

private:
    // Setup steps (called from run()):
    void start_child();             // ChildProcess ctor (forks; child blocks)
    void open_perf_events();        // open both PerfEvents on the child PID
    void mmap_ring_buffers();       // PerfRingBuffer for each event
    void release_child_and_seed();  // child.release(); read /proc/pid/maps

    // Event loop helpers:
    void event_loop();
    void drain_event(PerfRingBuffer& rb, MemAccessOp op);
    void on_record(const perf_event_header* hdr, MemAccessOp op);
    void emit_periodic_snapshot_if_due(uint64_t now_ns);

    // Time helper.
    static uint64_t monotonic_ns();

    Options                       opts_;
    std::unique_ptr<ChildProcess> child_;
    std::unique_ptr<PerfEvent>    ev_loads_;
    std::unique_ptr<PerfEvent>    ev_stores_;
    std::unique_ptr<PerfRingBuffer> rb_loads_;
    std::unique_ptr<PerfRingBuffer> rb_stores_;
    MemoryMap                     mmap_;
    StatsAggregator               stats_;
    std::unique_ptr<OutputSink>   sink_;

    uint64_t last_snapshot_ns_ = 0;
    uint64_t last_sample_ts_   = 0;   // newest perf timestamp seen, for window roll
    int      child_exit_code_  = -1;
};

}  // namespace memtracer
