#pragma once

#include "options.hpp"

#include <cstdint>
#include <linux/perf_event.h>
#include <sys/types.h>

namespace memtracer {

enum class MemAccessOp : uint8_t { Load = 0, Store = 1 };

inline const char* op_to_str(MemAccessOp o) {
    return o == MemAccessOp::Load ? "R" : "W";
}

// The exact bits we ask perf to splice into each PERF_RECORD_SAMPLE record.
// The order in the record body is fixed by the kernel (see perf_event.h docs);
// records.cpp::parse_sample relies on this constant being unchanged.
inline constexpr uint64_t kSampleType =
      PERF_SAMPLE_IP
    | PERF_SAMPLE_TID
    | PERF_SAMPLE_TIME
    | PERF_SAMPLE_ADDR
    | PERF_SAMPLE_PERIOD
    | PERF_SAMPLE_DATA_SRC;

// Thin RAII wrapper around perf_event_open(2) for one MEM_INST_RETIRED counter.
// Holds the kernel fd; constructed disabled, flips on automatically when the
// child execve()s thanks to perf_event_attr.enable_on_exec=1.
class PerfEvent {
public:
    PerfEvent(MemAccessOp op, pid_t target_pid, const Options& opts);
    ~PerfEvent();

    PerfEvent(const PerfEvent&) = delete;
    PerfEvent& operator=(const PerfEvent&) = delete;

    int fd() const { return fd_; }
    MemAccessOp op() const { return op_; }

    // ioctl helpers — generally not needed when enable_on_exec is set, but
    // useful for unit tests and for stop/resume scenarios.
    void enable();
    void disable();
    void reset();

private:
    int fd_ = -1;
    MemAccessOp op_;
};

}  // namespace memtracer
