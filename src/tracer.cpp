#include "tracer.hpp"
#include "signal_handler.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <linux/perf_event.h>
#include <poll.h>
#include <unistd.h>

namespace memtracer {

Tracer::Tracer(Options opts)
  : opts_(std::move(opts)),
    stats_((uint64_t)opts_.window_ms * 1'000'000ull) {
    sink_ = make_sink(opts_);
}

Tracer::~Tracer() = default;

uint64_t Tracer::monotonic_ns() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

int Tracer::run() {
    SignalHandler::install();

    start_child();
    open_perf_events();
    mmap_ring_buffers();
    release_child_and_seed();

    last_snapshot_ns_ = monotonic_ns();
    event_loop();

    // Flush any stragglers in the ring buffers that arrived after the loop
    // exit condition tripped — they may include the very last samples.
    drain_event(*rb_loads_,  MemAccessOp::Load);
    drain_event(*rb_stores_, MemAccessOp::Store);

    if (child_exit_code_ < 0) {
        // Child may still be alive (we exited via SIGINT). Reap it now.
        child_exit_code_ = child_->wait_for_exit();
    }

    // Final snapshot + exit notice.
    if (opts_.window_ms > 0 && last_sample_ts_ > 0) {
        stats_.roll_window(last_sample_ts_);
    }
    sink_->on_snapshot(stats_);
    sink_->on_exit(child_exit_code_);
    return child_exit_code_;
}

void Tracer::start_child() {
    child_ = std::make_unique<ChildProcess>(opts_.child_argv);
}

void Tracer::open_perf_events() {
    // Order matters slightly: opening events on the still-blocked child is
    // safe (the child has a valid pid and will not exec until we say so).
    ev_loads_  = std::make_unique<PerfEvent>(MemAccessOp::Load,  child_->pid(), opts_);
    ev_stores_ = std::make_unique<PerfEvent>(MemAccessOp::Store, child_->pid(), opts_);
}

void Tracer::mmap_ring_buffers() {
    rb_loads_  = std::make_unique<PerfRingBuffer>(ev_loads_->fd(),  opts_.mmap_pages_log2);
    rb_stores_ = std::make_unique<PerfRingBuffer>(ev_stores_->fd(), opts_.mmap_pages_log2);
}

void Tracer::release_child_and_seed() {
    // Unblock the child → execvp fires → enable_on_exec arms both counters.
    child_->release();

    // Seed the address-space view from /proc/<pid>/maps. There is a brief
    // window after exec() during which the dynamic linker is mapping libs;
    // a single snapshot here gives us the steady-state initial layout, and
    // any *additional* mmaps after this point arrive as PERF_RECORD_MMAP2.
    //
    // VERIFY ON INTEL: if the partner sees "<unknown>" attribution for early
    // samples, this is the place to debug — try a small usleep here to let
    // the linker finish, or repeat the load_from_proc() once after a short
    // delay to backfill.
    mmap_.load_from_proc(child_->pid());

    // Inform the sink of the initial layout so it can pre-populate UI slots.
    mmap_.for_each([this](const MappedObject& o) {
        sink_->on_mmap(o);
        stats_.on_mmap(o);
    });
}

void Tracer::event_loop() {
    pollfd fds[3];
    fds[0].fd = rb_loads_->fd();        fds[0].events = POLLIN;
    fds[1].fd = rb_stores_->fd();       fds[1].events = POLLIN;
    fds[2].fd = SignalHandler::wakeup_fd(); fds[2].events = POLLIN;

    while (SignalHandler::g_running.load(std::memory_order_relaxed)) {
        // poll timeout = whatever's nearest: snapshot deadline.
        const uint64_t now = monotonic_ns();
        const uint64_t snap_due = last_snapshot_ns_ + (uint64_t)opts_.snapshot_ms * 1'000'000ull;
        int timeout_ms;
        if (snap_due <= now) {
            timeout_ms = 0;
        } else {
            uint64_t delta_ns = snap_due - now;
            timeout_ms = (int)std::min<uint64_t>(delta_ns / 1'000'000ull, 1000);
            if (timeout_ms == 0) timeout_ms = 1;
        }

        int r = ::poll(fds, 3, timeout_ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "memtracer: poll failed: %s\n", std::strerror(errno));
            break;
        }

        // Drain in priority order: data first (the actual content), wakeup last.
        if (fds[0].revents & POLLIN) drain_event(*rb_loads_,  MemAccessOp::Load);
        if (fds[1].revents & POLLIN) drain_event(*rb_stores_, MemAccessOp::Store);
        if (fds[2].revents & POLLIN) SignalHandler::drain_wakeups();

        // POLLHUP / POLLERR on the perf fds means the underlying task died.
        if ((fds[0].revents | fds[1].revents) & (POLLHUP | POLLERR)) {
            // Child has exited; do one last drain via the next loop iter then bail.
            int code = -1;
            if (child_->try_reap(code)) child_exit_code_ = code;
            break;
        }

        if (SignalHandler::g_child_died.load(std::memory_order_relaxed)) {
            int code = -1;
            if (child_->try_reap(code)) {
                child_exit_code_ = code;
                // Continue one more iteration to drain any final records, then exit.
                SignalHandler::g_running.store(false);
            }
        }

        emit_periodic_snapshot_if_due(monotonic_ns());
    }
}

void Tracer::drain_event(PerfRingBuffer& rb, MemAccessOp op) {
    rb.drain([&](const perf_event_header* hdr) { on_record(hdr, op); });
}

void Tracer::on_record(const perf_event_header* hdr, MemAccessOp op) {
    switch (hdr->type) {
    case PERF_RECORD_SAMPLE: {
        ParsedSample s = parse_sample(hdr);
        if (s.time > last_sample_ts_) last_sample_ts_ = s.time;
        const MappedObject* ip_obj   = mmap_.lookup(s.ip);
        const MappedObject* addr_obj = mmap_.lookup(s.addr);
        stats_.on_sample(s, op, ip_obj, addr_obj);
        sink_->on_sample(s, op, ip_obj, addr_obj);
        break;
    }
    case PERF_RECORD_MMAP2: {
        ParsedMmap2 m = parse_mmap2(hdr);
        mmap_.handle_mmap2(m);
        // Re-fetch the inserted region so we hand the sink the post-merge view.
        if (const MappedObject* o = mmap_.lookup(m.addr)) {
            sink_->on_mmap(*o);
            stats_.on_mmap(*o);
        }
        break;
    }
    case PERF_RECORD_MMAP: {
        // We requested mmap2; classic mmap should not normally appear, but
        // some kernels still emit it for early init. Ignore; the next /proc
        // re-scan would catch anything important.
        break;
    }
    case PERF_RECORD_LOST: {
        ParsedLost l = parse_lost(hdr);
        stats_.note_lost(l.lost);
        sink_->on_lost(l.lost);
        break;
    }
    case PERF_RECORD_EXIT: {
        // Child task exited. Don't break the loop here (let try_reap handle
        // the sequencing); just note it for diagnostics.
        break;
    }
    default:
        // Other record types (COMM, FORK, THROTTLE, UNTHROTTLE, ...) are not
        // currently consumed. The kernel sized them correctly so the ring
        // buffer cursor is already advanced by the drain() loop.
        break;
    }
}

void Tracer::emit_periodic_snapshot_if_due(uint64_t now_ns) {
    const uint64_t snap_due = last_snapshot_ns_ + (uint64_t)opts_.snapshot_ms * 1'000'000ull;
    if (now_ns < snap_due) return;
    last_snapshot_ns_ = now_ns;
    if (opts_.window_ms > 0 && last_sample_ts_ > 0) {
        stats_.roll_window(last_sample_ts_);
    }
    sink_->on_snapshot(stats_);
}

}  // namespace memtracer
