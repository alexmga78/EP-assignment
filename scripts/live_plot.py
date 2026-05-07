#!/usr/bin/env python3
"""live_plot.py — consume memtracer's JSONL on stdin and render live histograms.

Layout:
    Top row    — per-object IP histogram (which library / binary issued the access).
    Bottom row — per-object ADDR histogram (which memory region was touched).

Each object becomes one stacked bar (reads + writes), with kBucketsPerObject (=64)
sub-bars showing the within-object distribution. The bar heights update on every
tracer 'snapshot' record. Run with --window N to overlay the tracer's rolling
window length in the title (the actual windowing happens in C++).

Usage:
    ./bin/memtracer --sink=jsonl ... | python3 scripts/live_plot.py
    ./bin/memtracer --sink=jsonl ... | python3 scripts/live_plot.py --window 2
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
from collections import deque
from dataclasses import dataclass
from typing import Optional

import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation


@dataclass
class Snapshot:
    total_samples: int = 0
    lost: int = 0
    window_ns: int = 0
    code: list = None     # list of object dicts as emitted by the JSONL sink
    data: list = None

    def __post_init__(self):
        if self.code is None: self.code = []
        if self.data is None: self.data = []


class StreamReader(threading.Thread):
    """Reads JSONL from sys.stdin in a background thread, keeping only the most
    recent snapshot. Mmap and exit events are tracked but no history is kept."""

    def __init__(self):
        super().__init__(daemon=True)
        self.lock = threading.Lock()
        self.latest: Optional[Snapshot] = None
        self.exit_code: Optional[int] = None
        self.lost_total = 0
        self.mmap_count = 0
        self.recent_log = deque(maxlen=20)

    def run(self):
        for raw in sys.stdin:
            raw = raw.strip()
            if not raw:
                continue
            try:
                rec = json.loads(raw)
            except json.JSONDecodeError as e:
                self.recent_log.append(f"bad json: {e}")
                continue
            t = rec.get("type")
            if t == "snapshot":
                with self.lock:
                    self.latest = Snapshot(
                        total_samples=rec.get("total_samples", 0),
                        lost=rec.get("lost", 0),
                        window_ns=rec.get("window_ns", 0),
                        code=rec.get("code", []),
                        data=rec.get("data", []),
                    )
            elif t == "mmap":
                self.mmap_count += 1
                self.recent_log.append(f"mmap {rec.get('display')} @ 0x{rec.get('start',0):x}")
            elif t == "lost":
                self.lost_total += rec.get("n", 0)
                self.recent_log.append(f"LOST {rec.get('n')} samples")
            elif t == "exit":
                self.exit_code = rec.get("code")
                self.recent_log.append(f"child exit code = {self.exit_code}")
            # 'sample' records are not displayed individually; the tracer's
            # snapshot already aggregates them.


def render(ax, objects: list, title: str) -> None:
    ax.clear()
    ax.set_title(title, fontsize=10)
    if not objects:
        ax.text(0.5, 0.5, "(no samples yet)", ha="center", va="center",
                transform=ax.transAxes, color="gray")
        ax.set_xticks([])
        ax.set_yticks([])
        return

    # Lay out one cluster of 64 bars per object, with a labelled gap between
    # clusters. Reads on top of writes (stacked).
    BUCKETS = 64
    GAP = 4
    xs, reads, writes, tick_pos, tick_lbl = [], [], [], [], []
    cursor = 0
    for o in objects:
        buckets = o.get("buckets", [])
        if len(buckets) != BUCKETS:
            continue
        cluster_start = cursor
        for i, (r, w) in enumerate(buckets):
            xs.append(cursor + i)
            reads.append(r)
            writes.append(w)
        tick_pos.append(cluster_start + BUCKETS / 2)
        lbl = f'{o.get("display","?")}\nR={o.get("reads",0)} W={o.get("writes",0)}'
        tick_lbl.append(lbl)
        cursor += BUCKETS + GAP

    ax.bar(xs, writes, width=1.0, color="tab:red",  label="writes")
    ax.bar(xs, reads,  width=1.0, color="tab:blue", label="reads", bottom=writes)
    ax.set_xticks(tick_pos)
    ax.set_xticklabels(tick_lbl, rotation=0, fontsize=7)
    ax.set_ylabel("samples")
    ax.legend(loc="upper right", fontsize=7)
    ax.margins(x=0.01)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--window", type=float, default=0.0,
                    help="display-only: rolling window seconds (informational)")
    ap.add_argument("--interval", type=int, default=300,
                    help="frame interval in ms (default: 300)")
    args = ap.parse_args()

    reader = StreamReader()
    reader.start()

    fig, (ax_code, ax_data) = plt.subplots(2, 1, figsize=(13, 8))
    fig.suptitle("memtracer — live", fontsize=12)

    def update(_frame):
        with reader.lock:
            snap = reader.latest
        if snap is None:
            ax_code.clear(); ax_data.clear()
            ax_code.text(0.5, 0.5, "waiting for first snapshot…",
                         ha="center", va="center", transform=ax_code.transAxes)
            ax_data.set_axis_off(); ax_code.set_axis_off()
            return

        win_str = (f"window={snap.window_ns/1e9:.2f}s"
                   if snap.window_ns else "lifetime")
        if args.window > 0 and snap.window_ns == 0:
            win_str = f"(plotter --window {args.window}s requested but tracer in lifetime mode)"
        fig.suptitle(
            f"memtracer — total={snap.total_samples}  lost={snap.lost}  "
            f"mmaps={reader.mmap_count}  {win_str}", fontsize=11)
        render(ax_code, snap.code, "CODE — by IP (which object issued the access)")
        render(ax_data, snap.data, "DATA — by addr (which region was accessed)")
        fig.tight_layout(rect=(0, 0, 1, 0.95))

    _anim = FuncAnimation(fig, update, interval=args.interval, cache_frame_data=False)
    try:
        plt.show()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
