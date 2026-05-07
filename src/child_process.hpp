#pragma once

#include <string>
#include <sys/types.h>
#include <vector>

namespace memtracer {

// ChildProcess forks immediately in its constructor, returning to the parent
// as soon as the fork is complete. The child blocks reading on a sync pipe
// until the parent calls release() — this is the moment perf events with
// `enable_on_exec=1` are armed and ready to flip on, so counting starts at
// the very first instruction of the target binary.
class ChildProcess {
public:
    explicit ChildProcess(std::vector<std::string> argv);
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    pid_t pid() const { return pid_; }

    // Unblock the child so it execvp()s the target. Must be called once,
    // after perf_event_open and ring buffer mmap have succeeded.
    void release();

    // Block until the child exits; returns the WEXITSTATUS-equivalent code,
    // or 128+signo if it was killed. Returns -1 if waitpid failed.
    int wait_for_exit();

    // Non-blocking variant: returns true and sets exit_code if the child has
    // exited. Otherwise returns false.
    bool try_reap(int& exit_code);

private:
    void run_child_and_never_return();   // never returns; called inside child after fork

    std::vector<std::string> argv_;
    pid_t pid_ = -1;
    int sync_read_fd_  = -1;   // child's end (closed in parent)
    int sync_write_fd_ = -1;   // parent's end (closed in child)
    bool released_ = false;
    bool reaped_   = false;
    int  exit_code_ = -1;
};

}  // namespace memtracer
