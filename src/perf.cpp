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

// ---------------------------------------------------------------------------
// perf_event_open syscall wrapper
// ---------------------------------------------------------------------------

static long perf_event_open(struct perf_event_attr* attr, pid_t pid, int cpu,
                             int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

// ---------------------------------------------------------------------------
// PerfSession constructor / destructor
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// PerfSession::open
// ---------------------------------------------------------------------------

PerfEvent PerfSession::open_event(uint64_t event_config, bool is_leader,
                                  int group_fd, int ring_pages)
{
    struct perf_event_attr attr{};
    attr.size             = sizeof(attr);
    attr.type             = PERF_TYPE_RAW;
    attr.config           = event_config;
    attr.sample_period    = 1000;       // sample every 1000 events
    attr.precise_ip       = 2;          // request PEBS
    attr.disabled         = 1;
    attr.exclude_kernel   = 1;
    attr.exclude_hv       = 1;
    attr.sample_type      = PERF_SAMPLE_IP
                          | PERF_SAMPLE_ADDR
                          | PERF_SAMPLE_TID
                          | PERF_SAMPLE_TIME;

    if (is_leader) {
        attr.mmap            = 1;     // receive PERF_RECORD_MMAP
        attr.mmap2           = 1;     // receive PERF_RECORD_MMAP2 (preferred)
        attr.mmap_data       = 1;     // also record non-exec (data/heap) mmaps
        attr.comm            = 1;     // receive PERF_RECORD_COMM
        attr.task            = 1;     // receive PERF_RECORD_EXIT / FORK
    }

    PerfEvent ev;
    ev.fd = static_cast<int>(
        perf_event_open(&attr, child_pid_, -1, group_fd, 0));
    if (ev.fd < 0) {
        throw std::runtime_error(
            std::string("perf_event_open failed: ") + strerror(errno));
    }

    // mmap: 1 meta page + ring_pages data pages.
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
    // Event config values for Skylake+ (and most post-Nehalem) Intel CPUs.
    // MEM_INST_RETIRED.ALL_LOADS  : event=0xD0, umask=0x81 → raw=0x81D0
    // MEM_INST_RETIRED.ALL_STORES : event=0xD0, umask=0x82 → raw=0x82D0
    constexpr uint64_t ALL_LOADS  = 0x81D0;
    constexpr uint64_t ALL_STORES = 0x82D0;

    loads_  = open_event(ALL_LOADS,  /*is_leader=*/true,  -1,        ring_pages);
    stores_ = open_event(ALL_STORES, /*is_leader=*/false, loads_.fd, ring_pages);
}

// ---------------------------------------------------------------------------
// Enable / disable
// ---------------------------------------------------------------------------

void PerfSession::enable()
{
    ioctl(loads_.fd,  PERF_EVENT_IOC_RESET,  0);
    ioctl(stores_.fd, PERF_EVENT_IOC_RESET,  0);
    ioctl(loads_.fd,  PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

void PerfSession::disable()
{
    ioctl(loads_.fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
}

// ---------------------------------------------------------------------------
// Ring buffer draining
// ---------------------------------------------------------------------------

// Safely copy `len` bytes from the ring buffer (which may wrap) into `out`.
static void ring_read(const char* data_base, uint64_t mask,
                      uint64_t offset, void* out, size_t len)
{
    size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const char* rb = data_base + page_size; // skip meta page
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

    // Acquire fence: ensure we see all writes to the ring buffer.
    __sync_synchronize();
    uint64_t head = meta->data_head;
    __sync_synchronize();

    uint64_t tail    = ev.prev_head;
    uint64_t size_rb = meta->data_size;       // always a power of two
    uint64_t mask    = size_rb - 1;

    while (tail != head) {
        // Read the 8-byte record header.
        struct perf_event_header hdr{};
        ring_read(static_cast<char*>(ev.base), mask, tail, &hdr, sizeof(hdr));

        if (hdr.size == 0) break; // should not happen, but guard against hang

        uint64_t record_end = tail + hdr.size;

        // Read the full record into a local buffer.
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
            // Older kernels may emit MMAP instead of MMAP2; we ignore it here
            // because seeding from /proc/<pid>/maps already covers it.
            break;
        case PERF_RECORD_EXIT:
            handle_exit(payload, pay_len);
            break;
        default:
            break;
        }

        tail = record_end;
    }

    // Update tail so the kernel knows we've consumed up to `head`.
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

// ---------------------------------------------------------------------------
// Record handlers
// ---------------------------------------------------------------------------

// Layout for PERF_SAMPLE_IP | PERF_SAMPLE_ADDR | PERF_SAMPLE_TID | PERF_SAMPLE_TIME
// (in the order specified by the sample_type bits, lowest bit first).
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

// perf_record_mmap2 payload (abbreviated — only the fields we need).
// Full layout: pid, tid, addr, len, pgoff, maj, min, ino, ino_gen, prot, flags, filename
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
    // char filename[] follows
};

void PerfSession::handle_mmap2(const char* data, size_t size)
{
    if (size < sizeof(Mmap2Payload) + 1) return;

    Mmap2Payload m{};
    std::memcpy(&m, data, sizeof(m));

    // filename is null-terminated and starts right after the fixed fields.
    const char* fname = data + sizeof(Mmap2Payload);
    size_t      max   = size - sizeof(Mmap2Payload);
    std::string filename(fname, strnlen(fname, max));

    bool is_exec = (m.prot & 0x4) != 0; // PROT_EXEC
    maps_.insert(m.addr, m.len, filename, is_exec);

    // Ensure a RegionStats entry exists for this region.
    const Region* r = maps_.lookup(m.addr);
    if (r) stats_.ensure_region(*r);
}

// perf_record_exit: we only care that our child exited.
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
