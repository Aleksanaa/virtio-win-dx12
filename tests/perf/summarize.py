#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Summarizes `vgd-test.ps1 Perf` runs: cuts each run's ETW trace (perf.csv) into uibench's scenarios and reports, per
# scenario, how regular the display's VSync was, how often and how evenly DWM composed and the screen flipped, how
# long DWM spent per frame, and how long GPU work took to complete (submit to done: the host round trip on Venus).
# Several runs are reported side by side, so two builds can be compared.
#   summarize.py RUN_DIR [RUN_DIR...]
import csv
import json
import sys
from collections import defaultdict
from pathlib import Path

DXGK = 'Microsoft-Windows-DxgKrnl'
DWM = 'Microsoft-Windows-Dwm-Core'

# The events used, by provider and id (their fields are named from the run's events.json).
VSYNC_DPC = (DXGK, 17)
MMIO_FLIP = (DXGK, 116)
PRESENT = (DXGK, 184)
QUEUE_PACKET_START = (DXGK, 178)
DMA_PACKET_START = (DXGK, 175)
DMA_PACKET_INFO = (DXGK, 177)
FRAME_START = (DWM, 10)
FRAME_STOP = (DWM, 12)
WANTED = {VSYNC_DPC, MMIO_FLIP, PRESENT, QUEUE_PACKET_START, DMA_PACKET_START, DMA_PACKET_INFO, FRAME_START,
          FRAME_STOP}

# tracerpt's fixed columns, before the events' own fields. Rows of providers whose opcodes it cannot name (Dwm-Core)
# lack the second one, Type, and every column after it moves one to the left.
COL_PROVIDER, COL_ID, COL_VERSION, COL_PID, COL_TIME, COL_DATA = 0, 2, 3, 9, 16, 19


def load_fields(run):
    fields = {}
    for e in json.loads((run / 'events.json').read_text(encoding='utf-8-sig')):
        fields[(e['provider'], e['id'], e['version'])] = [f for f in (e['fields'] or []) if f]
    return fields


def load_scenarios(run):
    scenarios = []
    for line in (run / 'uibench.txt').read_text(encoding='utf-8', errors='replace').splitlines():
        if line.startswith('scenario '):
            parts = line.split()
            kv = dict(p.split('=', 1) for p in parts[2:])
            scenarios.append((parts[1], int(kv['start']), int(kv['end'])))
    return scenarios


def num(s):
    s = s.strip().strip('"').strip()
    return int(s, 16) if s.lower().startswith('0x') else int(s)


def load_events(run, fields):
    """The wanted events, as (key, time in 100 ns, pid, {field: value}) in trace order."""
    events = []
    with open(run / 'perf.csv', encoding='utf-8-sig', errors='replace', newline='') as f:
        for row in csv.reader(f, skipinitialspace=True):
            if len(row) <= COL_TIME:
                continue
            provider = row[COL_PROVIDER].strip()
            if provider not in (DXGK, DWM):
                continue
            shift = -1 if row[1].strip().isdigit() else 0
            try:
                key = (provider, int(row[COL_ID + shift]))
            except ValueError:
                continue
            if key not in WANTED:
                continue
            names = fields.get((provider, key[1], int(row[COL_VERSION + shift])), [])
            first = COL_DATA + shift
            data = {n: row[first + i].strip().strip('"').strip() for i, n in enumerate(names) if first + i < len(row)}
            events.append((key, int(row[COL_TIME + shift]), num(row[COL_PID + shift]), data))
    events.sort(key=lambda e: e[1])
    return events


def pct(values, p):
    if not values:
        return float('nan')
    v = sorted(values)
    return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))]


def intervals_ms(times):
    return [(b - a) / 1e4 for a, b in zip(times, times[1:])]


def summarize(run):
    fields = load_fields(run)
    events = load_events(run, fields)
    dwm_pids = {pid for key, _, pid, _ in events if key[0] == DWM}

    # GPU packets: queued (QueuePacket start) and started on the GPU (DmaPacket start), to completed (the DMA
    # completed interrupt), matched by context and submit sequence.
    queued, started, latency = {}, {}, []
    for key, t, pid, d in events:
        if key == QUEUE_PACKET_START and 'SubmitSequence' in d:
            queued[(d['hContext'], d['SubmitSequence'])] = (t, pid)
        elif key == DMA_PACKET_START and 'ulQueueSubmitSequence' in d:
            started[(d['hContext'], d['ulQueueSubmitSequence'])] = t
        elif key == DMA_PACKET_INFO and 'COMPLETED' in d.get('InterruptType', ''):
            k = (d['hContext'], d['ulQueueSubmitSequence'])
            if k in queued:
                tq, qpid = queued.pop(k)
                ts = started.pop(k, tq)
                latency.append((t, qpid in dwm_pids, (t - tq) / 1e4, (t - ts) / 1e4))

    results = {}
    for name, start, end in load_scenarios(run):
        inside = [e for e in events if start <= e[1] <= end]
        secs = (end - start) / 1e7
        vsync = [t for key, t, _, _ in inside if key == VSYNC_DPC]
        flips = [t for key, t, _, _ in inside if key == MMIO_FLIP]
        frame_starts = [t for key, t, _, _ in inside if key == FRAME_START]
        frame_cpu, open_frame = [], None
        for key, t, _, _ in inside:
            if key == FRAME_START:
                open_frame = t
            elif key == FRAME_STOP and open_frame is not None:
                frame_cpu.append((t - open_frame) / 1e4)
                open_frame = None
        presents = [(t, pid) for key, t, pid, _ in inside if key == PRESENT]
        # DWM's present to the screen flip that shows it: the next flip after each of DWM's presents.
        to_flip, fi = [], 0
        for t, pid in presents:
            if pid not in dwm_pids:
                continue
            while fi < len(flips) and flips[fi] < t:
                fi += 1
            if fi < len(flips):
                to_flip.append((flips[fi] - t) / 1e4)
        gpu = [l for l in latency if start <= l[0] <= end]
        vi, fl, fr = intervals_ms(vsync), intervals_ms(flips), intervals_ms(frame_starts)
        period = pct(vi, 50)
        results[name] = {
            'secs': secs,
            'vsync_hz': len(vsync) / secs,
            'vsync_p50': period,
            'vsync_p99': pct(vi, 99),
            'vsync_max': max(vi, default=float('nan')),
            'vsync_late': sum(1 for v in vi if v > 1.5 * period),
            'dwm_fps': len(frame_starts) / secs,
            'dwm_gap_p50': pct(fr, 50),
            'dwm_gap_p99': pct(fr, 99),
            'dwm_gap_max': max(fr, default=float('nan')),
            'dwm_cpu_p50': pct(frame_cpu, 50),
            'dwm_cpu_p99': pct(frame_cpu, 99),
            'flip_fps': len(flips) / secs,
            'flip_gap_p95': pct(fl, 95),
            'flip_gap_p99': pct(fl, 99),
            'flip_gap_max': max(fl, default=float('nan')),
            'flip_hitches': sum(1 for v in fl if v > 2.5 * period) if period == period else 0,
            'present_to_flip_p50': pct(to_flip, 50),
            'present_to_flip_p99': pct(to_flip, 99),
            'gpu_packets': len(gpu),
            'gpu_dwm_p50': pct([l[2] for l in gpu if l[1]], 50),
            'gpu_dwm_p99': pct([l[2] for l in gpu if l[1]], 99),
            'gpu_all_p99': pct([l[2] for l in gpu], 99),
            'gpu_run_p99': pct([l[3] for l in gpu], 99),
        }
    return results


ROWS = [
    ('vsync_hz', 'VSync DPCs /s', '{:.1f}'),
    ('vsync_p50', 'VSync interval p50 ms', '{:.2f}'),
    ('vsync_p99', 'VSync interval p99 ms', '{:.2f}'),
    ('vsync_max', 'VSync interval max ms', '{:.1f}'),
    ('vsync_late', 'VSyncs >1.5 periods late', '{}'),
    ('dwm_fps', 'DWM frames /s', '{:.1f}'),
    ('dwm_gap_p50', 'DWM frame gap p50 ms', '{:.2f}'),
    ('dwm_gap_p99', 'DWM frame gap p99 ms', '{:.1f}'),
    ('dwm_gap_max', 'DWM frame gap max ms', '{:.1f}'),
    ('dwm_cpu_p50', 'DWM frame CPU p50 ms', '{:.2f}'),
    ('dwm_cpu_p99', 'DWM frame CPU p99 ms', '{:.2f}'),
    ('flip_fps', 'Flips /s', '{:.1f}'),
    ('flip_gap_p95', 'Flip gap p95 ms', '{:.1f}'),
    ('flip_gap_p99', 'Flip gap p99 ms', '{:.1f}'),
    ('flip_gap_max', 'Flip gap max ms', '{:.1f}'),
    ('flip_hitches', 'Flip gaps >2.5 periods', '{}'),
    ('present_to_flip_p50', 'DWM present->flip p50 ms', '{:.2f}'),
    ('present_to_flip_p99', 'DWM present->flip p99 ms', '{:.2f}'),
    ('gpu_packets', 'GPU packets', '{}'),
    ('gpu_dwm_p50', 'DWM GPU queue->done p50 ms', '{:.2f}'),
    ('gpu_dwm_p99', 'DWM GPU queue->done p99 ms', '{:.2f}'),
    ('gpu_all_p99', 'All GPU queue->done p99 ms', '{:.2f}'),
    ('gpu_run_p99', 'All GPU start->done p99 ms', '{:.2f}'),
]


def main():
    runs = [Path(a) for a in sys.argv[1:]]
    if not runs:
        sys.exit(__doc__ or 'usage: summarize.py RUN_DIR [RUN_DIR...]')
    summaries = [summarize(r) for r in runs]
    scenarios = list(dict.fromkeys(s for summary in summaries for s in summary))
    names = [r.name for r in runs]
    for r in runs:
        info = (r / 'info.txt').read_text(encoding='utf-8-sig', errors='replace').split('\n')
        print(f'{r.name}: ' + '; '.join(l for l in info if l.startswith(('driver', 'file viogpu3d', 'file vgd3d11',
                                                                         'file vgd_vulkan', 'logical'))))
    for scenario in scenarios:
        print(f'\n== {scenario}')
        print(f'{"":30}' + ''.join(f'{n:>16}' for n in names))
        for key, label, fmt in ROWS:
            cells = []
            for summary in summaries:
                v = summary.get(scenario, {}).get(key)
                cells.append('-' if v is None or v != v else fmt.format(v))
            print(f'{label:30}' + ''.join(f'{c:>16}' for c in cells))


if __name__ == '__main__':
    main()
