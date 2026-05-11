#pragma once

#include "maps.h"
#include "stats.h"
#include "ipc.h"

#include <cstdint>
#include <functional>
#include <vector>

#include <linux/perf_event.h>
#include <sys/types.h>

// One perf event fd plus its mmap'd ring buffer. `prev_head` is our own
// tail cursor: the kernel only ever reads `data_tail` in the meta page,
// so we keep a private copy of how far we've drained without touching it
// (and only publish to `data_tail` once per drain pass).
struct PerfEvent {
    int      fd   = -1;
    void*    base = nullptr;
    size_t   mmap_size = 0;
    uint64_t prev_head = 0;
};

// Runs a PEBS-precise sampling group (loads + stores) against one tracee.
class PerfSession {
public:
    explicit PerfSession(pid_t child_pid, Maps& maps, Stats& stats, Ipc& ipc);
    ~PerfSession();

    // Opens both event fds and mmaps their ring buffers. Throws on failure
    // - typical causes are EACCES from perf_event_paranoid > 2, and ENODEV
    // when running on a CPU without PEBS-LL (Intel pre-Nehalem, Atom, AMD).
    void open(int ring_pages = 512);

    void enable();
    void disable();

    // Drains both ring buffers. Returns false once PERF_RECORD_EXIT for
    // our child has been seen. Must be called once more after disable()
    // at shutdown to flush records the kernel produced between the last
    // epoll wakeup and the child's exit.
    bool drain();

    int load_fd()  const { return loads_.fd; }
    int store_fd() const { return stores_.fd; }

private:
    pid_t       child_pid_;
    Maps&       maps_;
    Stats&      stats_;
    Ipc&        ipc_;

    PerfEvent   loads_;     // MEM_INST_RETIRED.ALL_LOADS
    PerfEvent   stores_;    // MEM_INST_RETIRED.ALL_STORES

    bool        got_exit_ = false;

    // is_leader=true on the first event in the group: only the leader
    // carries the mmap/comm/task tracking bits, so we hear about the
    // tracee's mappings exactly once per event.
    PerfEvent open_event(uint64_t event_config, bool is_leader,
                         int group_fd, int ring_pages);

    void drain_one(PerfEvent& ev, bool is_store);

    void handle_sample(const char* data, size_t size, bool is_store);
    void handle_mmap2(const char* data, size_t size);
    void handle_exit(const char* data, size_t size);
};
