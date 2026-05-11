#!/usr/bin/env python3
"""
Live memory-access plotter.

Reads newline-delimited JSON from stdin (piped from my_tracer) and displays:
  - Top subplot: stacked bar chart of reads/writes per CODE object (IP side).
  - Bottom subplot: bucket heatmap per DATA region (address side).

Usage: usually launched by my_tracer via popen().
       Can also be tested standalone:
           echo '{"ts_ns":1000000000,"ip":"0x1","addr":"0x2","rw":"r",
                  "ip_obj":"libc.so.6","region":"[heap]",
                  "region_type":"heap","bucket":3}' | python3 plot/plotter.py

Optional arguments (passed as argv):
    --window  N   Sliding window in seconds (default: 5).
    --interval N  Animation interval in milliseconds (default: 500).
"""

import os
import sys
import json
import argparse
import threading
import collections
from typing import Deque, Dict, List, Tuple

import matplotlib
import matplotlib.patches as mpatches
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from matplotlib.animation import FuncAnimation

# Try interactive backends in order; fall back to WebAgg (browser-based).
# switch_backend() does a real runtime check without creating any figure.
for _backend in ("TkAgg", "Qt5Agg", "GTK3Agg", "WebAgg"):
    try:
        plt.switch_backend(_backend)
        break
    except Exception:
        continue

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("--window",   type=float, default=5.0)
parser.add_argument("--interval", type=int,   default=500)
args, _ = parser.parse_known_args()

WINDOW_NS:   int = int(args.window * 1e9)   # sliding window in nanoseconds
INTERVAL_MS: int = args.interval

NUM_BUCKETS = 64   # must match stats.h

# ---------------------------------------------------------------------------
# Shared state (producer = stdin reader thread, consumer = matplotlib thread)
# ---------------------------------------------------------------------------
_lock = threading.Lock()

# Each entry: dict with keys ts_ns, rw, ip_obj, region, region_type, bucket
_samples: Deque[dict] = collections.deque()
_monitoring_ended = False
_exit_scheduled   = False   # only schedule os._exit once
_is_webagg        = False   # set after backend is chosen

def _reader_thread():
    """Background thread: read JSON lines from stdin and append to _samples."""
    global _monitoring_ended
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            rec = json.loads(raw)
        except json.JSONDecodeError:
            continue
        with _lock:
            _samples.append(rec)
    # stdin closed: tracer has exited
    with _lock:
        _monitoring_ended = True

def _enter_to_exit():
    """Block until Enter is pressed on the terminal, then terminate."""
    try:
        with open("/dev/tty") as tty:
            tty.readline()
    except Exception:
        pass
    os._exit(0)

reader = threading.Thread(target=_reader_thread, daemon=True)
reader.start()

# ---------------------------------------------------------------------------
# Figure setup
# ---------------------------------------------------------------------------
fig, (ax_code, ax_data) = plt.subplots(
    2, 1, figsize=(14, 9),
    gridspec_kw={"height_ratios": [1, 2]}
)
fig.suptitle("Live Memory Access Monitor", fontsize=13, fontweight="bold")
fig.tight_layout(pad=3.0)

# ---------------------------------------------------------------------------
# Animation update
# ---------------------------------------------------------------------------
def update(_frame):
    with _lock:
        ended = _monitoring_ended
        # --- prune stale samples outside the sliding window ---
        if _samples:
            latest_ts = _samples[-1]["ts_ns"]
            cutoff     = latest_ts - WINDOW_NS
            while _samples and _samples[0]["ts_ns"] < cutoff:
                _samples.popleft()

        snapshot = list(_samples)

    # Update figure title based on monitoring state.
    global _exit_scheduled
    if ended:
        if _is_webagg:
            fig.suptitle("Memory Access Monitor  [MONITORING ENDED — press Enter in terminal to exit]",
                         fontsize=11, fontweight="bold", color="red")
        else:
            fig.suptitle("Memory Access Monitor  [MONITORING ENDED]",
                         fontsize=12, fontweight="bold", color="red")
        if not _exit_scheduled:
            _exit_scheduled = True
            if _is_webagg:
                print("\n[tracer] monitoring ended — press Enter to close the plot server.",
                      flush=True, file=sys.stderr)
                threading.Thread(target=_enter_to_exit, daemon=True).start()
            else:
                threading.Timer(1.0, lambda: os._exit(0)).start()
    else:
        fig.suptitle("Live Memory Access Monitor  [monitoring…]",
                     fontsize=13, fontweight="bold", color="black")

    # ---------------------------------------------------------------- ax_code
    # Aggregate reads/writes per ip_obj.
    code_reads:  Dict[str, int] = collections.defaultdict(int)
    code_writes: Dict[str, int] = collections.defaultdict(int)
    for s in snapshot:
        obj = s.get("ip_obj", "UNKNOWN")
        if s["rw"] == "r":
            code_reads[obj]  += 1
        else:
            code_writes[obj] += 1

    ax_code.cla()
    if code_reads or code_writes:
        all_objs = sorted(set(code_reads) | set(code_writes))
        reads_v  = [code_reads.get(o,  0) for o in all_objs]
        writes_v = [code_writes.get(o, 0) for o in all_objs]
        x = range(len(all_objs))
        ax_code.bar(x, reads_v,  label="reads",  color="#4e8df5", alpha=0.85)
        ax_code.bar(x, writes_v, label="writes", color="#f5704e", alpha=0.85,
                    bottom=reads_v)
        ax_code.set_xticks(list(x))
        ax_code.set_xticklabels(all_objs, rotation=30, ha="right", fontsize=8)
        ax_code.legend(loc="upper right", fontsize=8)
    ax_code.set_title(
        f"Accesses by code object (last {args.window:.0f}s)",
        fontsize=10)
    ax_code.set_ylabel("Sample count", fontsize=8)
    ax_code.yaxis.set_major_locator(ticker.MaxNLocator(integer=True))

    # ---------------------------------------------------------------- ax_data
    # Aggregate per-region per-bucket.
    # bucket_data[region] = list of (reads, writes) length NUM_BUCKETS
    bucket_data: Dict[str, List[Tuple[int, int]]] = {}
    for s in snapshot:
        region = s.get("region", "UNKNOWN")
        bkt    = int(s.get("bucket", 0))
        bkt    = max(0, min(NUM_BUCKETS - 1, bkt))
        if region not in bucket_data:
            bucket_data[region] = [(0, 0)] * NUM_BUCKETS
        r, w = bucket_data[region][bkt]
        if s["rw"] == "r":
            bucket_data[region][bkt] = (r + 1, w)
        else:
            bucket_data[region][bkt] = (r, w + 1)

    ax_data.cla()
    if bucket_data:
        regions = sorted(bucket_data)
        n_regions = len(regions)
        bar_width = 0.8 / max(n_regions, 1)

        # Fixed: reads=blue, writes=orange. Regions distinguished by hatch.
        READ_COLOR  = "#4e8df5"
        WRITE_COLOR = "#f5704e"
        HATCHES = ["", "//", "xx", "..", "++", "\\\\", "oo"]

        for i, region in enumerate(regions):
            buckets = bucket_data[region]
            xs = [b + i * bar_width for b in range(NUM_BUCKETS)]
            rs = [bv[0] for bv in buckets]
            ws = [bv[1] for bv in buckets]
            hatch = HATCHES[i % len(HATCHES)]
            ax_data.bar(xs, rs, width=bar_width,
                        color=READ_COLOR,  alpha=0.85, hatch=hatch,
                        label=region)
            ax_data.bar(xs, ws, width=bar_width,
                        color=WRITE_COLOR, alpha=0.85, hatch=hatch,
                        bottom=rs)

        # Build legend: one entry per region (by hatch) + reads/writes color key.
        legend_handles = []
        for i, region in enumerate(regions):
            hatch = HATCHES[i % len(HATCHES)]
            legend_handles.append(
                mpatches.Patch(facecolor="lightgrey", hatch=hatch, label=region))
        legend_handles.append(mpatches.Patch(facecolor=READ_COLOR,  label="reads"))
        legend_handles.append(mpatches.Patch(facecolor=WRITE_COLOR, label="writes"))
        ax_data.legend(handles=legend_handles, loc="upper right",
                       fontsize=7, ncol=min(n_regions + 2, 5))

        ax_data.set_xticks(range(0, NUM_BUCKETS, 8))
        ax_data.set_xticklabels([str(b) for b in range(0, NUM_BUCKETS, 8)],
                                 fontsize=8)

    ax_data.set_title(
        f"Bucket fill per data region (last {args.window:.0f}s)",
        fontsize=10)
    ax_data.set_xlabel("Bucket index", fontsize=8)
    ax_data.set_ylabel("Sample count", fontsize=8)
    ax_data.yaxis.set_major_locator(ticker.MaxNLocator(integer=True))

    return []


anim = FuncAnimation(fig, update, interval=INTERVAL_MS, blit=False,
                     cache_frame_data=False)

# For WebAgg: print the URL so the user knows where to open it.
if matplotlib.get_backend() == "WebAgg":
    _is_webagg = True
    port = int(matplotlib.rcParams.get("webagg.port", 8988))
    print(f"\n>>> Open your browser at: http://localhost:{port}/\n",
          flush=True, file=sys.stderr)

plt.show()
