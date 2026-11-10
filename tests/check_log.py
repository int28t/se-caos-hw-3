"""Независимая проверка очередей, разрешений, интервалов и итогового баланса."""
import re


def verify(text, config):
    settings = dict(min_green=4, max_green=10, clearance=2, tick_ms=1, target=0)
    streams, phases, conflicts = {}, {}, {}
    for row in config.splitlines():
        t = row.split('#')[0].split()
        if not t:
            continue
        if t[0] == 'stream':
            streams[t[1]] = dict(capacity=int(t[5]), crossing=int(t[6]), kind=t[2])
        elif t[0] == 'phase':
            phases[t[1]] = set(t[2:])
        elif t[0] == 'conflict':
            conflicts[frozenset(t[1:3])] = int(t[3])
        elif len(t) == 2 and t[0] in settings:
            settings[t[0]] = int(t[1])
    queues = {s: [] for s in streams}
    counts = {s: [0, 0, 0] for s in streams}
    active, arrivals, pedestrian = {}, {}, set()
    phase, state, since, cleared, stopped = set(), 'initial', 0, None, False
    sequence, last_time, wait_sum, summaries = 0, 0, 0, set()
    tick = settings['tick_ms'] * 1000000
    for line in text.splitlines():
        match = re.match(r'\[t=(\d+)\] (.*?): (.*)$', line)
        assert match, line
        _t, event, rest = match.groups()
        fields = dict(re.findall(r'(\w+)=([^ ]+)', rest))
        number, now = int(fields['событие']), int(fields['время_нс'])
        assert number == sequence + 1 and now >= last_time, line
        assert int(_t) == now // tick, line
        sequence, last_time = number, now
        if stopped:
            assert event in ('Итог потока', 'ИТОГО'), line
        name = fields.get('поток')
        ident = int(fields.get('id', '0'))
        if event == 'Начало фазы':
            assert not active and fields['имя'] in phases
            if cleared is not None:
                assert now - cleared >= settings['clearance'] * tick
            phase, state, since = phases[fields['имя']], 'green', now
        elif event == 'Въезд закрыт':
            assert state == 'green' and now - since >= settings['min_green'] * tick
            state = 'drain'
        elif event == 'Интервал':
            assert state == 'drain' and not active
            state, cleared = 'clearance', now
        elif event == 'Сигналы':
            mask = int(fields['маска'])
            expected = sum(1 << i for i, s in enumerate(streams) if s in phase) if state in ('green', 'drain') else 0
            assert mask == expected
        elif event == 'Прибытие':
            assert ident not in arrivals and name in streams
            arrivals[ident] = now
            counts[name][0] += 1
        elif event == 'Отказ':
            assert len(queues[name]) == streams[name]['capacity'] and ident in arrivals
            counts[name][2] += 1
        elif event == 'Очередь':
            assert ident in arrivals and ident not in queues[name]
            queues[name].append(ident)
            assert len(queues[name]) == int(fields['размер']) <= streams[name]['capacity']
            if streams[name]['kind'] == 'pedestrian':
                pedestrian.add(ident)
        elif event == 'Запрос пешехода':
            assert ident in pedestrian
            pedestrian.remove(ident)
        elif event == 'Разрешение':
            assert state == 'green' and name in phase and name not in active
            assert now - since < settings['max_green'] * tick
            assert queues[name] and queues[name].pop(0) == ident
            for other in active:
                assert conflicts.get(frozenset((name, other)), 1) == 0
            if settings['target']:
                assert sum(c[1] for c in counts.values()) + len(active) < settings['target']
            deadline = int(fields['срок'])
            assert deadline == now + streams[name]['crossing'] * tick
            active[name] = ident, deadline, now
            assert int(fields['очередь']) == len(queues[name])
        elif event == 'Выход':
            saved, deadline, entered = active.pop(name)
            assert saved == ident and now >= deadline
            waiting = (entered - arrivals[ident]) // tick
            assert int(fields['ожидание']) == waiting
            wait_sum += waiting
            counts[name][1] += 1
        elif event == 'Завершение':
            assert not stopped
            stopped = True
        elif event == 'Итог потока':
            expected = (*counts[name], len(queues[name]), int(name in active))
            assert tuple(int(fields[k]) for k in ('прибыло', 'прошло', 'отказов', 'ожидают', 'в_зоне')) == expected
            summaries.add(name)
        elif event == 'ИТОГО':
            expected = (sum(c[0] for c in counts.values()), sum(c[1] for c in counts.values()),
                        sum(c[2] for c in counts.values()), sum(map(len, queues.values())), len(active))
            assert tuple(int(fields[k]) for k in ('прибыло', 'прошло', 'отказов', 'ожидают', 'в_зоне')) == expected
            assert expected[0] == sum(expected[1:]) and int(fields['ожидание']) == wait_sum
        else:
            assert event == 'Состояние', event
    assert stopped and summaries == set(streams) and not pedestrian
    assert 'ИТОГО:' in text
    return sum(c[1] for c in counts.values())
