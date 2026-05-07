#pragma once

#include <cstdint>
#include <linux/perf_event.h>
#include <string_view>

namespace memtracer {

// Decoded view of a PERF_RECORD_SAMPLE for our chosen sample_type bits
// (see kSampleType in perf_event.hpp).
//
// Field order in the kernel-emitted record body (when present, in this exact
// order — this is fixed by the kernel ABI, see perf_event.h documentation):
//
//   { u64 ip;          }   PERF_SAMPLE_IP
//   { u32 pid, tid;    }   PERF_SAMPLE_TID
//   { u64 time;        }   PERF_SAMPLE_TIME
//   { u64 addr;        }   PERF_SAMPLE_ADDR
//   { u64 period;      }   PERF_SAMPLE_PERIOD
//   { u64 data_src;    }   PERF_SAMPLE_DATA_SRC
//
// We do not currently request CALLCHAIN, RAW, BRANCH_STACK, etc. If those are
// added later, parse_sample() must be extended *in the same kernel order*.
struct ParsedSample {
    uint64_t ip       = 0;
    uint32_t pid      = 0;
    uint32_t tid      = 0;
    uint64_t time     = 0;        // CLOCK_MONOTONIC_RAW nanoseconds (perf default)
    uint64_t addr     = 0;        // memory address accessed (PEBS-provided)
    uint64_t period   = 0;        // dynamic period (when PERF_SAMPLE_PERIOD set)
    uint64_t data_src = 0;        // memory hierarchy info; bitfield, see perf_event.h
};

// Decoded view of a PERF_RECORD_MMAP2 record.
// filename points into the record body — caller must copy if it needs to outlive
// the visitor invocation.
struct ParsedMmap2 {
    uint32_t pid   = 0;
    uint32_t tid   = 0;
    uint64_t addr  = 0;
    uint64_t len   = 0;
    uint64_t pgoff = 0;
    uint32_t maj   = 0;
    uint32_t min   = 0;
    uint64_t ino   = 0;
    uint64_t ino_generation = 0;
    uint32_t prot  = 0;
    uint32_t flags = 0;
    std::string_view filename;
};

// hdr->type must equal PERF_RECORD_SAMPLE.
ParsedSample parse_sample(const perf_event_header* hdr);

// hdr->type must equal PERF_RECORD_MMAP2.
ParsedMmap2 parse_mmap2(const perf_event_header* hdr);

// Decoded view of PERF_RECORD_LOST so we can report dropped samples. The
// kernel emits this when the consumer (us) falls behind.
struct ParsedLost {
    uint64_t id   = 0;
    uint64_t lost = 0;
};
ParsedLost parse_lost(const perf_event_header* hdr);

}  // namespace memtracer
