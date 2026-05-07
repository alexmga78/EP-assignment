#include "perf_event.hpp"

#include <asm/unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <linux/perf_event.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace memtracer {

namespace {

// glibc does not provide a wrapper for perf_event_open(2); call the syscall directly.
long perf_event_open(struct perf_event_attr* attr, pid_t pid, int cpu,
                     int group_fd, unsigned long flags) {
    return ::syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

// MEM_INST_RETIRED encoding for Intel (Nehalem and newer).
//   event = 0xD0
//   umask = 0x81  (ALL_LOADS)   or   0x82 (ALL_STORES)
//
// perf raw config layout (lowest 32 bits): umask << 8 | event.
//
// VERIFY ON INTEL: confirm with `perf list` and `perf stat -e cpu/event=0xd0,umask=0x81/u ...`.
// On some newer Intel parts the encoding is exposed under a different name
// (e.g. MEM_INST_RETIRED.ALL_LOADS_PS) but the raw 0xD0/0x81 still works.
constexpr uint64_t kEventCode = 0xD0;
constexpr uint64_t kUmaskLoad  = 0x81;
constexpr uint64_t kUmaskStore = 0x82;

uint64_t raw_config_for(MemAccessOp op) {
    uint64_t umask = (op == MemAccessOp::Load) ? kUmaskLoad : kUmaskStore;
    return (umask << 8) | kEventCode;
}

}  // namespace

PerfEvent::PerfEvent(MemAccessOp op, pid_t target_pid, const Options& opts) : op_(op) {
    struct perf_event_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.type           = PERF_TYPE_RAW;
    attr.size           = sizeof(attr);
    attr.config         = raw_config_for(op);
    attr.sample_period  = opts.sample_period;
    attr.sample_type    = kSampleType;
    attr.read_format    = 0;

    // Run only in user mode; we don't have CAP_SYS_ADMIN in the general case.
    attr.disabled       = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv     = 1;

    // PEBS request. precise_ip=2 → "request constant skid" (true PEBS sample
    // with the data linear address populated in PERF_SAMPLE_ADDR).
    // VERIFY ON INTEL: if this fails with EOPNOTSUPP / EINVAL on the partner's
    // box, drop to precise_ip=1 to confirm the rest of the pipeline.
    attr.precise_ip     = 2;

    // We want both code-mapping events (for IP attribution) and data-mapping
    // events (for ADDR attribution to mmap'd data segments).
    attr.mmap           = 1;
    attr.mmap2          = 1;
    attr.mmap_data      = opts.mmap_data ? 1 : 0;
    attr.task           = 1;
    attr.comm           = 1;

    // Counters arm themselves the moment the child execve()s. This is the
    // synchronization story end-to-end:
    //   ChildProcess fork()  → child blocks on pipe
    //   PerfEvent ctor       → fd opened with disabled=1, enable_on_exec=1
    //   PerfRingBuffer mmap  → kernel-side ring ready
    //   ChildProcess.release → write to pipe → child execvp → counters fire
    attr.enable_on_exec = 1;

    // Follow fork()s and clone()s. NOTE: with inherit=1 + per-PID monitoring,
    // inherited events get their *own* ring buffers. For a single-threaded
    // target this doesn't matter; for a multi-threaded target the partner
    // should switch to per-CPU monitoring (cpu = 0..nproc, pid = -1).
    attr.inherit        = 1;

    // Wake us up after every record. Trades wakeup overhead for latency; OK
    // for a coarse-grained tool. Set higher (e.g. 64) if profiling overhead.
    attr.wakeup_events  = 1;

    // pid = target_pid, cpu = -1: count for this PID across any CPU.
    int fd = (int)perf_event_open(&attr, target_pid, /*cpu*/ -1,
                                  /*group_fd*/ -1, /*flags*/ 0);
    if (fd < 0) {
        int e = errno;
        throw std::runtime_error(
            std::string("perf_event_open(") + (op == MemAccessOp::Load ? "ALL_LOADS" : "ALL_STORES") +
            ") failed: " + std::strerror(e) +
            " — check kernel.perf_event_paranoid and that the CPU is Intel Nehalem+");
    }
    fd_ = fd;
}

PerfEvent::~PerfEvent() {
    if (fd_ >= 0) ::close(fd_);
}

void PerfEvent::enable()  { ::ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0); }
void PerfEvent::disable() { ::ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0); }
void PerfEvent::reset()   { ::ioctl(fd_, PERF_EVENT_IOC_RESET, 0); }

}  // namespace memtracer
