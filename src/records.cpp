#include "records.hpp"

#include <cstring>

namespace memtracer {

namespace {
// Read a value from the record body, advancing the cursor.
template <typename T>
T read_at(const uint8_t*& p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}
}  // namespace

ParsedSample parse_sample(const perf_event_header* hdr) {
    // The body starts immediately after the header; field order matches the
    // PERF_SAMPLE_* bits we set in PerfEvent (see kSampleType).
    //
    // VERIFY ON INTEL: if you ever change kSampleType in perf_event.hpp, this
    // function MUST be updated; field order is fixed by the kernel ABI in the
    // order PERF_SAMPLE_* enum values are defined in <linux/perf_event.h>.
    const auto* p = reinterpret_cast<const uint8_t*>(hdr) + sizeof(perf_event_header);

    ParsedSample s;
    s.ip       = read_at<uint64_t>(p);          // PERF_SAMPLE_IP
    s.pid      = read_at<uint32_t>(p);          // PERF_SAMPLE_TID (pid first)
    s.tid      = read_at<uint32_t>(p);
    s.time     = read_at<uint64_t>(p);          // PERF_SAMPLE_TIME
    s.addr     = read_at<uint64_t>(p);          // PERF_SAMPLE_ADDR
    s.period   = read_at<uint64_t>(p);          // PERF_SAMPLE_PERIOD
    s.data_src = read_at<uint64_t>(p);          // PERF_SAMPLE_DATA_SRC
    return s;
}

ParsedMmap2 parse_mmap2(const perf_event_header* hdr) {
    // PERF_RECORD_MMAP2 body layout (from include/uapi/linux/perf_event.h):
    //   u32 pid, tid;
    //   u64 addr, len, pgoff;
    //   u32 maj, min;
    //   u64 ino, ino_generation;
    //   u32 prot, flags;
    //   char filename[];     // null-terminated
    const auto* p = reinterpret_cast<const uint8_t*>(hdr) + sizeof(perf_event_header);
    const auto* end = reinterpret_cast<const uint8_t*>(hdr) + hdr->size;

    ParsedMmap2 m;
    m.pid             = read_at<uint32_t>(p);
    m.tid             = read_at<uint32_t>(p);
    m.addr            = read_at<uint64_t>(p);
    m.len             = read_at<uint64_t>(p);
    m.pgoff           = read_at<uint64_t>(p);
    m.maj             = read_at<uint32_t>(p);
    m.min             = read_at<uint32_t>(p);
    m.ino             = read_at<uint64_t>(p);
    m.ino_generation  = read_at<uint64_t>(p);
    m.prot            = read_at<uint32_t>(p);
    m.flags           = read_at<uint32_t>(p);

    // Filename runs from p to the first NUL or end of record.
    const auto* fn = reinterpret_cast<const char*>(p);
    size_t maxlen = (size_t)(end - p);
    size_t fnlen = 0;
    while (fnlen < maxlen && fn[fnlen] != '\0') ++fnlen;
    m.filename = std::string_view(fn, fnlen);
    return m;
}

ParsedLost parse_lost(const perf_event_header* hdr) {
    const auto* p = reinterpret_cast<const uint8_t*>(hdr) + sizeof(perf_event_header);
    ParsedLost l;
    l.id   = read_at<uint64_t>(p);
    l.lost = read_at<uint64_t>(p);
    return l;
}

}  // namespace memtracer
