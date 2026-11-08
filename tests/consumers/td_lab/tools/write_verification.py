"""Summarize explicit current evidence; never promote historical/contended runs."""
from __future__ import annotations
import argparse
import hashlib
import json
from statistics import median
from datetime import datetime, timezone
from pathlib import Path
import verify_game as v
from web_performance_validation import validate_web_performance
from native_performance_evidence import validate_native_performance
from web_acceptance_validation import validate_input, validate_cleanup, validate_web_lifecycle
from web_render_quality_evidence import validate_web_render_quality


def identity(path):
    path = Path(path).resolve()
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'bytes': path.stat().st_size}


def load_evidence(path):
    record = identity(path)
    record['data'] = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    return record


def verify_bound_data(name, data, artifacts):
    native, editor, web = (artifacts.get(k, {}).get('sha256') for k in ('native', 'editor', 'web'))
    if name == 'performance-suite':
        validate_native_performance(data, {'native': native, 'editor': editor})
    elif name == 'web-input':
        validate_input(data, web)
    elif name == 'web-render-quality':
        validate_web_render_quality(data, web)
    elif name == 'web-gpu-cleanup':
        validate_cleanup(data, web)
    elif name == 'web-performance':
        if not web or data.get('jce_artifact_sha256') != web:
            raise ValueError('Web performance artifact changed')
        validate_web_performance(data, web)
    elif name == 'delivery-suite':
        if data['bundle_sha256'] != identity(data['bundle'])['sha256']:
            raise ValueError('Bundle changed after observation')
        if data['graphical_acceptance'] != 'PASS_WITH_NATIVE_AUTHORING_CONTEXT':
            raise ValueError('Contextual Bundle graphical acceptance missing')
    elif name in ('native-residency', 'editor-residency'):
        expected = native if name == 'native-residency' else editor
        if data.get('status') != 'PROCESS_RESIDENCY_OBSERVED' or data.get('artifact_sha256') != expected:
            raise ValueError('Residency result unavailable or artifact changed')
    elif name == 'web-lifecycle':
        validate_web_lifecycle(data, web)
    elif name in ('native-lifecycle', 'editor-lifecycle'):
        if data.get('status') != 'REQUIRED_CPU_LIFECYCLE_SCOPES_OBSERVED':
            raise ValueError('Lifecycle capture incomplete or invalid')
        if data['sha256'] != identity(data['trace'])['sha256']:
            raise ValueError('Lifecycle raw trace changed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', action='append', default=[], metavar='NAME=JSON',
                        help='Explicit additional evidence; original raw files are retained')
    parser.add_argument('--web', type=Path, help='Actual Web artifact associated with browser evidence')
    parser.add_argument('--out', type=Path, default=v.OUT / 'VERIFICATION.md')
    parser.add_argument('--final', action='store_true', help='Reject missing final acceptance evidence')
    args = parser.parse_args()
    artifacts = {'native': identity(v.GAME), 'editor': identity(v.EDITOR)}
    if args.web:
        artifacts['web'] = identity(args.web)
    evidence = {}
    for name in ('visual-suite', 'delivery-suite', 'reference-inputs', 'interaction-render',
                 'preview-sequence', 'parameter-audit', 'performance-suite'):
        path = v.OUT / (name + '.json')
        if path.exists():
            evidence[name] = load_evidence(path)
    for item in args.evidence:
        name, separator, path = item.partition('=')
        if not separator or not name or name in evidence:
            raise ValueError('Explicit evidence names must be unique')
        evidence[name] = load_evidence(path)
    required = {'visual-suite', 'delivery-suite', 'reference-inputs', 'interaction-render',
                'preview-sequence', 'parameter-audit', 'performance-suite', 'web-input',
                'web-performance', 'web-render-quality', 'web-gpu-cleanup', 'web-lifecycle', 'native-residency',
                'editor-residency', 'native-lifecycle', 'editor-lifecycle'}
    missing = sorted(required - evidence.keys())
    if args.final and missing:
        raise ValueError('Missing final evidence: ' + ', '.join(missing))
    for name, record in evidence.items():
        verify_bound_data(name, record['data'], artifacts)
    report = {'generated_utc': datetime.now(timezone.utc).isoformat(),
              'status': 'MEASURED_ACCEPTANCE_SUBSET' if args.final else 'DIAGNOSTIC_EVIDENCE_SUMMARY',
              'artifacts': artifacts, 'evidence': evidence, 'unavailable': missing,
              'reference_commit': '784129a685627a18f3d3f8fa594208000d9313e8',
              'limits': ['CPU lifecycle scopes do not certify GPU completion or every application path.',
                         'Refresh-capped FPS is not uncapped throughput or parity with every commercial engine.',
                         'Process RSS/private commit is not engine heap or whole-device memory.',
                         'Default Aileron typography intentionally differs from the original display font.',
                         'Raw original touch-rotate visual comparison remains failed; all six camera controls are separately recorded.',
                         'Targeted linked shaders and submitted uniforms do not certify every GPU fragment or full industrial-engine equivalence.',
                         'Contextual native Bundle Play is not portable machine-code delivery.',
                         'Finite Windows/Web observations do not certify every platform or long-term soak.']}
    lines = ['# Tower Defense 当前产物验收', '',
             '正常游戏采用有界 60 Hz 固定模拟；原作的逐渲染帧行为仅用于显式参考取证。',
             '以下结果绑定当前文件身份；历史失败和较弱重复仍保留在原始证据目录。', '',
             '| 产物 | 字节 | SHA-256 |', '|---|---:|---|']
    for name, row in artifacts.items():
        lines.append(f'| {name} | {row["bytes"]} | `{row["sha256"]}` |')
    if 'performance-suite' in evidence:
        groups = {}
        for row in evidence['performance-suite']['data']['runs']:
            groups.setdefault((row['host'], row['mode']), []).append(row)
        aggregates = []
        for (host, mode), rows in groups.items():
            centers = [row['kpi']['frame_wall']['p50_ms'] for row in rows]
            center = median(centers)
            counts = {}
            for phase in ('frame_cpu', 'frame_gpu'):
                samples = [row['kpi'][phase]['samples'] if row['kpi'][phase] else 0 for row in rows]
                counts[phase] = {'min': min(samples), 'max': max(samples)}
            aggregates.append({'host': host, 'mode': mode, 'repeats': len(rows),
                'median_run_p50_ms': center,
                'median_run_p95_ms': median(row['kpi']['frame_wall']['p95_ms'] for row in rows),
                'median_run_p99_ms': median(row['kpi']['frame_wall']['p99_ms'] for row in rows),
                'mad_run_p50_ms': median(abs(value-center) for value in centers),
                'worst_raw_frame_ms': max(row['kpi']['frame_wall']['max_ms'] for row in rows),
                'median_observed_paced_fps': median(row['fps'] for row in rows), 'valid_samples_per_run': counts})
        report['native_timing_aggregates'] = aggregates
        lines += ['', '下表按独立重复汇总；完整原始分布、尖峰与逐轮结果保留在 JSON。FPS 为实际呈现节奏，不是无限帧吞吐。', '',
                  '| 宿主 / 场景 | 重复 | P50 ms | P95 ms | P99 ms | P50 MAD ms | 观测 FPS | CPU/GPU 有效样本每轮 |',
                  '|---|---:|---:|---:|---:|---:|---:|---|']
        for row in aggregates:
            counts = row['valid_samples_per_run']
            formatted = '/'.join(f"{counts[phase]['min']}–{counts[phase]['max']}" if counts[phase]['max'] else 'N/A'
                                 for phase in ('frame_cpu', 'frame_gpu'))
            lines.append(f'| {row["host"]}/{row["mode"]} | {row["repeats"]} | {row["median_run_p50_ms"]:.3f} | {row["median_run_p95_ms"]:.3f} | {row["median_run_p99_ms"]:.3f} | {row["mad_run_p50_ms"]:.3f} | {row["median_observed_paced_fps"]:.3f} | {formatted} |')
    if 'web-performance' in evidence:
        lines += ['', '| Web 宿主 / 顺序 | FPS | RAF P95 ms | 回调 CPU 均值 ms |', '|---|---:|---:|---:|']
        for i, row in enumerate(evidence['web-performance']['data']['runs']):
            lines.append(f'| {row["host"]}/{i} | {row["fps"]:.3f} | {row["interval_ms"]["p95"]:.3f} | {row["cpu_ms"]["mean"]:.3f} |')
    if 'native-residency' in evidence and 'editor-residency' in evidence:
        lines += ['', '| 宿主 | RSS 中位 MiB | Private commit 中位 MiB | OS 线程中位 |', '|---|---:|---:|---:|']
        for name in ('native-residency', 'editor-residency'):
            summary = evidence[name]['data']['summary']
            lines.append(f'| {name} | {summary["rss_bytes"]["median"]/1048576:.3f} | {summary["commit_bytes"]["median"]/1048576:.3f} | {summary["thread_count"]["median"]:.0f} |')
    lines += ['', '| 当前证据 | 原始 JSON | SHA-256 |', '|---|---|---|']
    for name, row in evidence.items():
        lines.append(f'| {name} | `{row["path"]}` | `{row["sha256"]}` |')
    lines += ['', '缺失检查：' + (', '.join(missing) if missing else '上述明确要求的证据均存在。'), '',
              '边界：'] + ['- ' + item for item in report['limits']]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
    args.out.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
    print(report['status'], args.out)


if __name__ == '__main__':
    main()
