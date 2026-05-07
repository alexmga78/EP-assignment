#pragma once

#include <atomic>

namespace memtracer {

// Signal handling for the tracer.
//
// install() registers handlers for SIGINT, SIGTERM, and SIGCHLD that:
//   * flip g_running to false (for the loop condition)
//   * write one byte to a self-pipe so a poll() in the loop returns
//     immediately even if no perf event has fired
//
// wakeup_fd() exposes the read end of the self-pipe; the Tracer adds it to
// its pollfd set.
//
// drain_wakeups() empties the self-pipe (no-op if nothing pending). Call
// after poll() returns to keep the pipe from filling up.
class SignalHandler {
public:
    static void install();
    static int  wakeup_fd();
    static void drain_wakeups();

    // True until SIGINT / SIGTERM / SIGCHLD are observed.
    static std::atomic<bool> g_running;
    static std::atomic<bool> g_child_died;
};

}  // namespace memtracer
