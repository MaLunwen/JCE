"""Validate optional fixed-size cursor-reader observations, never completeness.

Observation clocks describe the loss read, not the overwrite instant. This
module never parses events, reconstructs omitted history or promotes a capture.
"""
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
TOP = frozenset(('version','clock_domain','read_calls','loss_reads','loss_events',
                 'stop_counts','first_loss','last_loss','last_read'))
SNAPSHOT = frozenset(('write','earliest','next_before','next_after','count','stop',
                      'blocked','published','published_valid'))
WITNESS = frozenset(('read','phase','clock_valid','observed_ns','loss_delta',
                     'lost_total','snapshot','previous'))


def _invalid(message):
    raise ValueError('reader diagnostic: ' + message)


def _uint(value, maximum=UINT64_MAX):
    if type(value) is not int or not 0 <= value <= maximum:
        _invalid('invalid exact unsigned integer')
    return value


def _keys(value, expected):
    if not isinstance(value, dict) or value.keys() != expected:
        _invalid('unknown or missing fields')


def _snapshot(value, capacity):
    _keys(value, SNAPSHOT)
    for field in SNAPSHOT - {'published_valid'}:
        _uint(value[field], UINT32_MAX)
    if type(value['published_valid']) is not bool:
        _invalid('snapshot availability must be bool')
    write, stop = value['write'], value['stop']
    if write == UINT32_MAX or value['count'] > 512 or stop > 4:
        _invalid('snapshot write/count/stop bound')
    after, before = value['next_after'], value['next_before']
    blocked, published, valid = value['blocked'], value['published'], value['published_valid']
    if stop == 4:
        if any((write,value['earliest'],value['count'],blocked,published,valid)) or after != before:
            _invalid('invalid-args snapshot predicate')
        return 0
    if write == 0 and (value['count'] or after != before or stop != 0):
        _invalid('empty reader cannot copy or advance')
    earliest = 0 if write == 0 else max(1, write - capacity + 1)
    if value['earliest'] != earliest:
        _invalid('earliest disagrees with capacity/write')
    effective_start = earliest if before == 0 else max(before, earliest)
    if write and (after < effective_start or value['count'] > after - effective_start or
                  after > max(effective_start, write + 1)):
        _invalid('snapshot cursor/count progress')
    if stop == 0:
        empty = write == value['count'] == before == after == 0
        if (not empty and after <= write) or blocked or published or valid:
            _invalid('caught-up snapshot predicate')
    elif stop == 1:
        if value['count'] != 512 or after > write or blocked or published or valid:
            _invalid('batch-limit snapshot predicate')
    elif stop == 2:
        if value['count'] >= 512 or blocked != after or after > write or published or valid:
            _invalid('slot-busy snapshot predicate')
    elif stop == 3:
        if value['count'] >= 512 or blocked != after or after > write or not valid or published >= blocked:
            _invalid('reserved-unpublished snapshot predicate')
    skipped = earliest - before if 0 < before < earliest else 0
    return skipped + after - effective_start - value['count'] if write else 0


def validate_reader_diagnostics(metadata):
    if 'reader_diagnostics' not in metadata:
        return
    value = metadata['reader_diagnostics']
    _keys(value, TOP)
    if metadata.get('mode') != 'continuous':
        _invalid('requires continuous mode')
    if type(value['version']) is not int or value['version'] != 1 or value['clock_domain'] != 'process-perf-ns':
        _invalid('unknown version or observation clock domain')
    capacity = _uint(metadata.get('capacity'), UINT32_MAX)
    if not capacity:
        _invalid('requires positive capture capacity')
    calls, losses, events = (_uint(value[key]) for key in ('read_calls','loss_reads','loss_events'))
    stops = value['stop_counts']
    if not isinstance(stops, list) or len(stops) != 5:
        _invalid('requires five stop counts')
    if sum(_uint(count) for count in stops) != calls or losses > calls or events < losses:
        _invalid('read/stop/loss accounting')
    if events > _uint(metadata.get('lost')):
        _invalid('observed raw cursor losses exceed capture losses')
    known_reads, known_deltas = {}, {}

    def observe(index, sample):
        delta = _snapshot(sample, capacity)
        if index in known_reads and sample != known_reads[index]:
            _invalid('conflicting snapshot for one read')
        known_reads[index] = sample
        known_deltas[index] = delta
        return delta

    if calls == 0:
        if value['last_read'] is not None:
            _invalid('zero reads must have no last snapshot')
    else:
        observe(calls, value['last_read'])

    def witness(item):
        _keys(item, WITNESS)
        index = _uint(item['read'])
        phase = _uint(item['phase'], UINT32_MAX)
        observed = _uint(item['observed_ns'])
        delta, total = _uint(item['loss_delta']), _uint(item['lost_total'])
        if not 1 <= index <= calls or phase > 2 or not delta or total < delta:
            _invalid('loss witness read/phase/delta bound')
        if ((phase == 0 and metadata.get('consumer_mode') != 'threaded') or
                (phase == 1 and metadata.get('consumer_mode') != 'cooperative')):
            _invalid('loss phase disagrees with consumer')
        if type(item['clock_valid']) is not bool or (not item['clock_valid'] and observed != 0):
            _invalid('loss clock availability/value disagreement')
        if observe(index, item['snapshot']) != delta:
            _invalid('loss delta disagrees with exact cursor progress')
        if index == 1:
            if item['previous'] is not None:
                _invalid('first read cannot have previous snapshot')
        else:
            observe(index - 1, item['previous'])
        return index, delta, total

    first, last = value['first_loss'], value['last_loss']
    if not losses:
        if events or first is not None or last is not None:
            _invalid('zero loss reads must have no loss evidence')
    else:
        first_index, first_delta, first_total = witness(first)
        last_index, last_delta, last_total = witness(last)
        if first_total != first_delta or last_total != events:
            _invalid('loss totals disagree with first/last witnesses')
        if first['phase'] == 2 and last['phase'] != 2:
            _invalid('terminal drain loss phase cannot regress')
        if losses == 1:
            if first != last:
                _invalid('one loss read needs identical witnesses')
        else:
            if last_index <= first_index or last_index - first_index + 1 < losses:
                _invalid('loss read indices not ordered')
            if (losses == 2 and first_delta + last_delta != events) or events < first_delta + last_delta + losses - 2:
                _invalid('first/last positive deltas disagree with loss count')
            if first['clock_valid'] and last['clock_valid'] and last['observed_ns'] < first['observed_ns']:
                _invalid('loss observation clock moved backwards')
    ordered = sorted(known_reads.items())
    for (left_index, left), (right_index, right) in zip(ordered, ordered[1:]):
        if right_index == left_index + 1:
            if right['next_before'] != left['next_after']:
                _invalid('consecutive snapshots break cursor ownership')
    known_writes = [sample['write'] for _, sample in ordered if sample['stop'] != 4]
    if any(right < left for left, right in zip(known_writes, known_writes[1:])):
        _invalid('known snapshot write sequence moved backwards')
    positive = {index: delta for index, delta in known_deltas.items() if delta}
    if len(positive) > losses or sum(positive.values()) > events:
        _invalid('retained cursor losses exceed declared aggregates')
    if losses and any(index < first_index or index > last_index for index in positive):
        _invalid('retained loss lies outside first/last witness reads')
    if len(known_reads) == calls and (len(positive) != losses or sum(positive.values()) != events):
        _invalid('fully retained read loss accounting disagrees')
    observed_stops = [0] * 5
    for sample in known_reads.values():
        observed_stops[sample['stop']] += 1
    if any(known > total for known, total in zip(observed_stops, stops)):
        _invalid('retained snapshots exceed declared stop counts')
