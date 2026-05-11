#include "perf.h"
#include "maps.h"
#include "stats.h"
#include "ipc.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// glibc has no perf_event_open wrapper; the syscall must be invoked directly.
static long perf_event_open(struct perf_event_attr* attr, pid_t pid, int cpu,
                             int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

PerfSession::PerfSession(pid_t child_pid, Maps& maps, Stats& stats, Ipc& ipc)
    : child_pid_(child_pid), maps_(maps), stats_(stats), ipc_(ipc)
{}

PerfSession::~PerfSession()
{
    auto close_ev = [](PerfEvent& ev) {
        if (ev.base && ev.mmap_size) {
            munmap(ev.base, ev.mmap_size);
            ev.base = nullptr;
        }
        if (ev.fd >= 0) {
            close(ev.fd);
            ev.fd = -1;
        }
    };
    close_ev(loads_);
    close_ev(stores_);
}

PerfEvent PerfSession::open_event(uint64_t event_config, bool is_leader,
                                  int group_fd, int ring_pages)
{
    struct perf_event_attr attr{};
    attr.size             = sizeof(attr);
    attr.type             = PERF_TYPE_RAW;
    attr.config           = event_config;

    // Event-count period rather than freq=N: a fixed period keeps the
    // sample rate proportional to the workload's memory-op density, so
    // the live plotter's bar heights stay comparable across regions
    // regardless of how busy each region's code is.
    attr.sample_period    = 1000;

    // precise_ip=2 requests PEBS with constant skid (precise_ip=3 isn't
    // universally available). PEBS is also what populates the ADDR field
    // for memory events - without it, PERF_SAMPLE_ADDR is always zero on
    // Intel for the load/store-retired events we use below.
    attr.precise_ip       = 2;

    attr.disabled         = 1;
    attr.exclude_kernel   = 1;
    attr.exclude_hv       = 1;

    // The bit order of PERF_SAMPLE_* (lowest bit first) determines the
    // field order in the resulting record payload; SampleRecord below
    // must mirror it.
    attr.sample_type      = PERF_SAMPLE_IP
                          | PERF_SAMPLE_ADDR
                          | PERF_SAMPLE_TID
                          | PERF_SAMPLE_TIME;

    if (is_leader) {
        // Both mmap and mmap2 set so older kernels (pre-3.12) that only
        // emit MMAP still wake us up. mmap_data covers non-executable
        // mappings (heap grow, shared-mem segments) that PEBS samples
        // will later land in.
        attr.mmap            = 1;
        attr.mmap2           = 1;
        attr.mmap_data       = 1;
        attr.comm            = 1;
        attr.task            = 1;
    }

    PerfEvent ev;
    ev.fd = static_cast<int>(
        perf_event_open(&attr, child_pid_, -1, group_fd, 0));
    if (ev.fd < 0) {
        throw std::runtime_error(
            std::string("perf_event_open failed: ") + strerror(errno));
    }

    // Perf ring layout: one meta page (struct perf_event_mmap_page) followed
    // by `ring_pages` data pages. The kernel requires ring_pages to be a
    // power of two and rejects anything else with EINVAL.
    size_t page_size  = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    ev.mmap_size      = page_size * (1 + ring_pages);
    ev.base = mmap(nullptr, ev.mmap_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, ev.fd, 0);
    if (ev.base == MAP_FAILED) {
        close(ev.fd);
        throw std::runtime_error(
            std::string("mmap ring buffer failed: ") + strerror(errno));
    }
    ev.prev_head = 0;
    return ev;
}

void PerfSession::open(int ring_pages)
{
    // Skylake+ raw event encodings (event | umask << 8). Documented in
    // Intel SDM Vol. 3B §19 and listed in libpfm4's intel_skl_events.c.
    // The same encodings work on Ice Lake, Tiger Lake, and Alder Lake
    // P-cores; on hybrid CPUs the tracee must be pinned to P-cores (see
    // main.cpp::pin_to_pcores) because the E-core PMU lacks these events.
    constexpr uint64_t ALL_LOADS  = 0x81D0;  // MEM_INST_RETIRED.ALL_LOADS
    constexpr uint64_t ALL_STORES = 0x82D0;  // MEM_INST_RETIRED.ALL_STORES

    loads_  = open_event(ALL_LOADS,  /*is_leader=*/true,  -1,        ring_pages);
    stores_ = open_event(ALL_STORES, /*is_leader=*/false, loads_.fd, ring_pages);
}

void PerfSession::enable()
{
    ioctl(loads_.fd,  PERF_EVENT_IOC_RESET,  0);
    ioctl(stores_.fd, PERF_EVENT_IOC_RESET,  0);
    // PERF_IOC_FLAG_GROUP enables every event in the group atomically;
    // this matters because PEBS-LL counts samples relative to the group
    // leader's enable point.
    ioctl(loads_.fd,  PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

void PerfSession::disable()
{
    ioctl(loads_.fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
}

// Copies `len` bytes from the ring starting at `offset`. The ring is
// power-of-two-sized, so `(offset & mask)` is its linear position; a
// single record can straddle the wrap point and require two memcpy halves.
static void ring_read(const char* data_base, uint64_t mask,
                      uint64_t offset, void* out, size_t len)
{
    size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const char* rb = data_base + page_size;  // skip the meta page
    auto* dst = static_cast<char*>(out);

    size_t first = static_cast<size_t>((offset & mask) );
    size_t avail = static_cast<size_t>(mask + 1) - first;

    if (avail >= len) {
        std::memcpy(dst, rb + first, len);
    } else {
        std::memcpy(dst, rb + first, avail);
        std::memcpy(dst + avail, rb, len - avail);
    }
}

void PerfSession::drain_one(PerfEvent& ev, bool is_store)
{
    auto* meta = static_cast<struct perf_event_mmap_page*>(ev.base);

    // perf userspace ring protocol (Documentation/userspace-api/perf_ring_buffer.rst
    // and include/uapi/linux/perf_event.h): the kernel publishes data_head
    // with release semantics; userspace must read it with acquire, consume
    // up to head, then publish data_tail with release. __sync_synchronize()
    // provides both barriers on the architectures we target.
    __sync_synchronize();
    uint64_t head = meta->data_head;
    __sync_synchronize();

    uint64_t tail    = ev.prev_head;
    uint64_t size_rb = meta->data_size;       // always a power of two
    uint64_t mask    = size_rb - 1;

    while (tail != head) {
        struct perf_event_header hdr{};
        ring_read(static_cast<char*>(ev.base), mask, tail, &hdr, sizeof(hdr));

        // A zero-length header would loop forever; the kernel is not
        // supposed to emit one, so this is a corruption-bailout, not a
        // normal exit path.
        if (hdr.size == 0) break;

        uint64_t record_end = tail + hdr.size;

        std::vector<char> buf(hdr.size);
        ring_read(static_cast<char*>(ev.base), mask, tail, buf.data(), hdr.size);

        const char* payload = buf.data() + sizeof(hdr);
        size_t      pay_len = hdr.size - sizeof(hdr);

        switch (hdr.type) {
        case PERF_RECORD_SAMPLE:
            handle_sample(payload, pay_len, is_store);
            break;
        case PERF_RECORD_MMAP2:
            handle_mmap2(payload, pay_len);
            break;
        case PERF_RECORD_MMAP:
            // Pre-3.12 kernels emit MMAP without the prot/flags fields we
            // need to classify text vs data. We already seeded from
            // /proc/<pid>/maps, so dropping legacy records is harmless
            // on any kernel that also emits MMAP2 (which all our targets do).
            break;
        case PERF_RECORD_EXIT:
            handle_exit(payload, pay_len);
            break;
        default:
            break;
        }

        tail = record_end;
    }

    meta->data_tail = head;
    __sync_synchronize();
    ev.prev_head = head;
}

bool PerfSession::drain()
{
    drain_one(loads_,  false);
    drain_one(stores_, true);
    return !got_exit_;
}

// PERF_SAMPLE_* fields appear in the payload in ascending bit order, not
// in the order they were OR'd into sample_type. Our sample_type sets
// IP (bit 0) | TID (bit 1) | TIME (bit 2) | ADDR (bit 3), giving the
// layout below. Any change to sample_type in open_event() requires a
// matching change here.
struct SampleRecord {
    uint64_t ip;
    uint32_t pid;
    uint32_t tid;
    uint64_t time;
    uint64_t addr;
};

void PerfSession::handle_sample(const char* data, size_t size, bool is_store)
{
    if (size < sizeof(SampleRecord)) return;

    SampleRecord s{};
    std::memcpy(&s, data, sizeof(s));

    const Region* ip_region   = maps_.lookup(s.ip);
    const Region* addr_region = maps_.lookup(s.addr);

    stats_.record(s.ip, s.addr, is_store, s.time, ip_region, addr_region);
    ipc_.send_sample(s.ip, s.addr, is_store, s.time, ip_region, addr_region);
}

// PERF_RECORD_MMAP2 fixed prefix, see include/uapi/linux/perf_event.h
// (perf_event_type comment block). A null-terminated filename follows the
// flags field and is the only variable-length part of the record.
struct Mmap2Payload {
    uint32_t pid;
    uint32_t tid;
    uint64_t addr;
    uint64_t len;
    uint64_t pgoff;
    uint32_t maj;
    uint32_t min;
    uint64_t ino;
    uint64_t ino_gen;
    uint32_t prot;
    uint32_t flags;
};

void PerfSession::handle_mmap2(const char* data, size_t size)
{
    if (size < sizeof(Mmap2Payload) + 1) return;

    Mmap2Payload m{};
    std::memcpy(&m, data, sizeof(m));

    const char* fname = data + sizeof(Mmap2Payload);
    size_t      max   = size - sizeof(Mmap2Payload);
    std::string filename(fname, strnlen(fname, max));

    // PROT_EXEC = 0x4 from <sys/mman.h>; hardcoded here so this header
    // only depends on <linux/perf_event.h> for the record layout.
    bool is_exec = (m.prot & 0x4) != 0;
    maps_.insert(m.addr, m.len, filename, is_exec);

    // Eagerly create a stats entry so a region the workload mapped but
    // never touched still shows up (as a zero row) in the final summary.
    const Region* r = maps_.lookup(m.addr);
    if (r) stats_.ensure_region(*r);
}

// PERF_RECORD_EXIT payload, see perf_event.h::perf_event_type. We only
// look at `pid` to detect when the traced child terminates.
struct ExitPayload {
    uint32_t pid;
    uint32_t ppid;
    uint32_t tid;
    uint32_t ptid;
    uint64_t time;
};

void PerfSession::handle_exit(const char* data, size_t size)
{
    if (size < sizeof(ExitPayload)) return;

    ExitPayload e{};
    std::memcpy(&e, data, sizeof(e));

    if (static_cast<pid_t>(e.pid) == child_pid_) {
        got_exit_ = true;
    }
}
