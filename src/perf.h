#pragma once

#include "maps.h"
#include "stats.h"
#include "ipc.h"

#include <cstdint>
#include <functional>
#include <vector>

#include <linux/perf_event.h>
#include <sys/types.h>

// Represents one perf event FD + its ring-buffer mapping.
struct PerfEvent {
    int      fd   = -1;
    void*    base = nullptr;    // mmap base (meta page)
    size_t   mmap_size = 0;     // total mmap size in bytes
    uint64_t prev_head = 0;     // last consumed data_head
};

// Top-level perf session: manages two events (loads + stores) for one child.
class PerfSession {
public:
    explicit PerfSession(pid_t child_pid, Maps& maps, Stats& stats, Ipc& ipc);
    ~PerfSession();

    // Open perf events and mmap ring buffers. Throws on error.
    void open(int ring_pages = 512);

    // Enable collection (call after child is running).
    void enable();

    // Disable collection.
    void disable();

    // Drain all pending records from both ring buffers.
    // Returns false when a PERF_RECORD_EXIT for our child was seen.
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

    // Open a single perf event.  `is_leader` enables mmap/comm tracking.
    PerfEvent open_event(uint64_t event_config, bool is_leader,
                         int group_fd, int ring_pages);

    // Process all new records in one ring buffer.
    void drain_one(PerfEvent& ev, bool is_store);

    // Handlers for individual record types.
    void handle_sample(const char* data, size_t size, bool is_store);
    void handle_mmap2(const char* data, size_t size);
    void handle_exit(const char* data, size_t size);
};
