#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <linux/perf_event.h>
#include <vector>

namespace memtracer {

// PerfRingBuffer wraps the mmap'd kernel ring buffer associated with a perf
// fd. The shared layout is one metadata page followed by 2^N data pages.
//
// Producer: kernel, advances `meta->data_head`.
// Consumer: us,    advances `meta->data_tail`.
//
// drain() walks every record between data_tail and data_head, invoking the
// visitor with a pointer to a contiguous (possibly copy-stitched) record. The
// visitor MUST NOT retain the pointer past its call.
class PerfRingBuffer {
public:
    PerfRingBuffer(int perf_fd, uint32_t data_pages_log2);
    ~PerfRingBuffer();

    PerfRingBuffer(const PerfRingBuffer&) = delete;
    PerfRingBuffer& operator=(const PerfRingBuffer&) = delete;

    int fd() const { return fd_; }

    // Visitor: void(const perf_event_header*).
    // Records that straddle the buffer boundary are copied into a small
    // scratch buffer so the visitor always sees them contiguously.
    template <typename Visitor>
    void drain(Visitor&& visit);

private:
    int   fd_           = -1;
    void* base_         = nullptr;     // start of mmap region (metadata page)
    size_t mmap_bytes_  = 0;
    perf_event_mmap_page* meta_ = nullptr;
    uint8_t* data_      = nullptr;     // start of data pages
    size_t   data_size_ = 0;           // 2^N * pagesize

    std::vector<uint8_t> scratch_;     // for stitched records
};

template <typename Visitor>
void PerfRingBuffer::drain(Visitor&& visit) {
    // Acquire-load the kernel's write cursor; pairs with the kernel's release
    // store. data_tail is only ever advanced by us, so a relaxed load is fine.
    const uint64_t head = __atomic_load_n(&meta_->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail       = __atomic_load_n(&meta_->data_tail, __ATOMIC_RELAXED);

    while (tail < head) {
        const uint64_t off = tail % data_size_;
        // The header always fits in the buffer because the kernel pads sizes
        // to a multiple of 8 and never produces a header smaller than 8 bytes.
        const auto* hdr = reinterpret_cast<const perf_event_header*>(data_ + off);
        const uint16_t rec_size = hdr->size;

        const perf_event_header* contig_hdr;
        if (off + rec_size <= data_size_) {
            // Record is wholly contiguous — pass the kernel pointer directly.
            contig_hdr = hdr;
        } else {
            // Stitch the two halves into the scratch buffer and hand that out.
            if (scratch_.size() < rec_size) scratch_.resize(rec_size);
            const size_t first = data_size_ - off;
            std::memcpy(scratch_.data(),         data_ + off, first);
            std::memcpy(scratch_.data() + first, data_,       rec_size - first);
            contig_hdr = reinterpret_cast<const perf_event_header*>(scratch_.data());
        }

        visit(contig_hdr);
        tail += rec_size;
    }

    // Release-store our consumer cursor so the kernel sees we've consumed.
    __atomic_store_n(&meta_->data_tail, tail, __ATOMIC_RELEASE);
}

}  // namespace memtracer
