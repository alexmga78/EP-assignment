#include "ring_buffer.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace memtracer {

PerfRingBuffer::PerfRingBuffer(int perf_fd, uint32_t data_pages_log2) : fd_(perf_fd) {
    const long pg = ::sysconf(_SC_PAGESIZE);
    if (pg <= 0) throw std::runtime_error("sysconf(_SC_PAGESIZE) failed");

    const size_t data_pages = (size_t)1 << data_pages_log2;
    data_size_  = data_pages * (size_t)pg;
    mmap_bytes_ = data_size_ + (size_t)pg;     // 1 metadata page + N data pages

    void* p = ::mmap(nullptr, mmap_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, perf_fd, 0);
    if (p == MAP_FAILED) {
        throw std::runtime_error(std::string("mmap perf ring: ") + std::strerror(errno));
    }
    base_ = p;
    meta_ = reinterpret_cast<perf_event_mmap_page*>(base_);
    data_ = reinterpret_cast<uint8_t*>(base_) + pg;

    scratch_.reserve(4096);
}

PerfRingBuffer::~PerfRingBuffer() {
    if (base_) ::munmap(base_, mmap_bytes_);
}

}  // namespace memtracer
