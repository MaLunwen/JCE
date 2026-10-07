#!/usr/bin/env python3
"""Companion to load_suite: worker CPU phases, main-thread submit, no GPU-complete claim."""
import re


def validate_async(log, phase, frame_limit=200):
    executors = re.findall(r'benchmark-load-async-executor: mode=(\d+) workers=(\d+)', log)
    if len(executors) != 1 or int(executors[0][0]) != 1 or int(executors[0][1]) < 1:
        raise ValueError('actual threaded executor unavailable')
    fields = ('request_frame completion_frame worker_main main_thread worker_thread '
              'queue_ns worker_wall_ns poll_gap_ns ready_elapsed_ns submit_ns ok').split()
    pattern = r'benchmark-load-async: ' + ' '.join(key+r'=(\d+)' for key in fields)
    matches = re.findall(pattern, log)
    if len(matches) != 1:
        raise ValueError('async load completion unavailable or duplicated')
    row = dict(zip(fields, map(int, matches[0])))
    if row['request_frame'] != 20 or not 20 <= row['completion_frame'] < frame_limit \
            or row['worker_main'] or row['main_thread'] == row['worker_thread'] \
            or not row['main_thread'] or not row['worker_thread'] or not row['ok'] \
            or row['submit_ns'] != phase['submit_ns'] \
            or sum(row[p] for p in ('queue_ns','worker_wall_ns','poll_gap_ns','submit_ns')) != row['ready_elapsed_ns']:
        raise ValueError('async thread/completion/timeline evidence invalid')
    row['execution_mode'] = 'threaded'
    row['workers'] = int(executors[0][1])
    return row


if __name__ == '__main__':
    from load_suite import main
    main(async_mode=True)
