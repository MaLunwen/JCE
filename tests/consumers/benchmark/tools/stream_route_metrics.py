"""Measured route frames and whole-process resource samples; no inferred disk/upload counters."""
import math
from tools.benchmark_suite import read_frames, frame_summary

ROUTE = (0, 1, 2, 1, 0)


def process_metrics(process):
    raw = process.get('process_samples', [])
    if not raw:
        raise ValueError('missing process resource samples')
    previous = -1.0
    for row in raw:
        elapsed = row.get('elapsed_s')
        rss = row.get('rss_bytes')
        if not isinstance(elapsed, (int, float)) or not math.isfinite(elapsed) or elapsed < previous:
            raise ValueError('invalid process sample clock')
        if not isinstance(rss, int) or isinstance(rss, bool) or rss < 0:
            raise ValueError('invalid process RSS sample')
        previous = elapsed
    memory = {'peak_rss_bytes': max(row['rss_bytes'] for row in raw),
              'first_rss_bytes': raw[0]['rss_bytes'], 'last_rss_bytes': raw[-1]['rss_bytes'],
              'sample_count': len(raw),
              'scope': 'whole prefix process sampled RSS; not per-route residency or physical VRAM'}
    commit = [row['commit_bytes'] for row in raw if row.get('commit_bytes') is not None]
    if any(not isinstance(value, int) or value < 0 for value in commit):
        raise ValueError('invalid process commit sample')
    memory['peak_commit_bytes'] = max(commit) if commit else None
    usable = [row for row in raw if row.get('read_bytes') is not None]
    io = None
    if len(usable) >= 2:
        first, last = usable[0], usable[-1]
        counters = [row['read_bytes'] for row in usable]
        if any(not isinstance(value, int) or value < 0 for value in counters) or any(b < a for a,b in zip(counters,counters[1:])):
            raise ValueError('invalid process IO counter interval')
        elapsed = last['elapsed_s']-first['elapsed_s']
        if elapsed <= 0:
            raise ValueError('invalid process IO counter interval')
        delta = last['read_bytes']-first['read_bytes']
        io = {'read_bytes_delta': delta, 'sampled_seconds': elapsed,
              'read_bytes_per_second': delta/elapsed, 'read_decimal_MB_per_second': delta/elapsed/1000000,
              'scope': 'whole prefix OS process read counter; includes startup/cached IO, not physical cold disk throughput'}
    return {'process_memory': memory, 'process_io': io,
            'cold_disk_io': {'status': 'UNAVAILABLE', 'reason': 'no cache eviction or physical-disk attribution instrumentation'},
            'upload_bytes_per_frame': {'status': 'UNAVAILABLE', 'reason': 'no per-frame upload sample; route interval CPU-submitted payload is reported separately'}}


def route_metrics(path, process, steps):
    samples=read_frames(path,warmup=0,sample_count=None)
    routes=[]
    for step in range(steps):
        selected=[i for i,frame in enumerate(samples['frame_indices']) if step*200+1 <= frame <= step*200+190]
        if len(selected)!=190:
            raise ValueError('missing timed route frame samples')
        subset={key:[value[i] for i in selected] for key,value in samples.items() if key!='phase_ms'}
        subset['phase_ms']={key:[value[i] for i in selected] for key,value in samples['phase_ms'].items()}
        routes.append(dict(step=step,set=ROUTE[step],**frame_summary(subset)))
    return dict(routes=routes, **process_metrics(process))
