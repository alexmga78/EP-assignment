Made with ♡ by Alex and Andrei

# memtracer — Linux Perf-based memory access tracer

EP course assignment. Samples a child process's main-memory loads and stores via Intel PEBS
(Linux Perf Events), attributes each sample to the mapped object that performed it (`libc`,
`libz`, the binary itself, JIT-ed `[anon]`, …) and the object that was accessed (`[heap]`,
`[stack]`, a `.so` data segment, …), and emits a live JSONL stream that a Python plotter
turns into per-object histograms.

## Requirements

- **Intel** CPU (Nehalem or newer) — uses `MEM_INST_RETIRED.ALL_LOADS` / `ALL_STORES` (raw
  PMU event 0xD0, umasks 0x81 / 0x82). AMD CPUs do **not** expose this event.
- Linux 4.x+ (`perf_event_open` with PEBS / `precise_ip=2`).
- `g++` C++17, `gcc` for benchmarks, Python 3 with `matplotlib` for the plotter.
- May require:
  ```
  sudo sysctl -w kernel.perf_event_paranoid=1   # or 0/-1 depending on distro
  ```

## Build

```
make            # release-ish (-O2 -g)  → bin/memtracer
make debug      # -O0 -g3 -DDEBUG_LOG=1
make bench      # builds benchmarks/*.bin
make clean
```

## Use

Form: `memtracer [TRACER FLAGS] -- CHILD_CMD [CHILD ARGS...]`

```
./bin/memtracer --sink=text --period=10000 -- ./benchmarks/linear_heap.bin

./bin/memtracer --sink=jsonl --period=5000 --snapshot-ms=200 \
                -- ./benchmarks/linear_heap.bin \
  | python3 scripts/live_plot.py

./bin/memtracer --sink=jsonl --window=2000 --snapshot-ms=200 \
                -- ./benchmarks/random_heap.bin \
  | python3 scripts/live_plot.py --window 2
```

## Tracer flags

| flag                    | default | meaning                                                    |
|-------------------------|---------|------------------------------------------------------------|
| `--sink={text,jsonl}`   | `jsonl` | output sink                                                |
| `--period=N`            | `10000` | take a sample every N retired memory ops (per event type)  |
| `--snapshot-ms=N`       | `250`   | how often to emit a histogram snapshot                     |
| `--window=N`            | `0`     | rolling window in ms (0 = lifetime). Bonus task.           |
| `--mmap-pages=N`        | `128`   | perf ring buffer data pages (must be a power of two)       |
| `--no-mmap-data`        | off     | suppress MMAP records for non-executable mappings          |
| `--help`                | —       | usage                                                      |

## Source layout

See [the plan file](../../../.claude/plans/i-have-the-following-breezy-panda.md) for the full
design. Quick map:

```
src/main.cpp              entry; arg split at "--"; constructs Tracer
src/tracer.{hpp,cpp}      orchestrator; poll() loop; record dispatch
src/options.hpp           parsed CLI Options POD
src/child_process.{hpp,cpp}  fork+pipe+execvp helper
src/perf_event.{hpp,cpp}     perf_event_open RAII wrapper (LOAD / STORE)
src/ring_buffer.{hpp,cpp}    mmap'd perf ring; drain() callback
src/records.{hpp,cpp}        SAMPLE / MMAP2 record decoders
src/mmap_tracker.{hpp,cpp}   /proc/PID/maps + MMAP2 → MappedObject lookup
src/stats.{hpp,cpp}          per-object bucketed histogram, optional window
src/output.hpp               OutputSink interface
src/output_text.cpp          stderr human-readable sink
src/output_jsonl.cpp         stdout JSONL sink (default)
src/signal_handler.{hpp,cpp} SIGINT/SIGTERM/SIGCHLD self-pipe
scripts/live_plot.py         matplotlib FuncAnimation consumer
benchmarks/*.c               linear, random, libc-heavy micro-benchmarks
```

## Hand-off notes (for the partner with the Intel box)

The user developing the scaffold has an **AMD** machine and **could not run anything**. Every
non-trivial code path has been written, but the perf-touching paths have only been validated
against the kernel headers, not against silicon. A `// VERIFY ON INTEL:` marker is left at each
spot worth eyeballing first. Useful one-liners:

```
# Confirm the events exist on this CPU (no perf install needed):
perf list | grep -i mem_inst_retired

# Confirm the raw encoding works without our tracer in the way:
perf stat -e cpu/event=0xd0,umask=0x81/u ./benchmarks/linear_heap.bin

# Run our tracer in text mode, smallest possible period, see raw samples:
./bin/memtracer --sink=text --period=1000 -- ./benchmarks/linear_heap.bin 2>&1 | head -50
```

If `perf_event_open` returns `EACCES`, lower `kernel.perf_event_paranoid`. If it returns
`EOPNOTSUPP`, double-check the event encoding and that `precise_ip=2` is supported (try lowering
to `precise_ip=1` for a quick smoke test).
