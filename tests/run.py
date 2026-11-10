#!/usr/bin/env python3
"""Один набор предметных сценариев и жизненного цикла для четырёх backend."""
import argparse
import concurrent.futures
import ctypes
import errno
import json
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import sys
import tempfile
import time
from check_log import verify

ROOT = Path(__file__).resolve().parents[1]
PARSER = argparse.ArgumentParser()
PARSER.add_argument('--build-dir', type=Path, required=True)
ARGS = PARSER.parse_args()
BUILD = ARGS.build_dir.resolve()
RESULTS = BUILD / 'results'
RESULTS.mkdir(exist_ok=True)
LIB = ctypes.CDLL(None, use_errno=True)
LIB.sem_open.restype = ctypes.c_void_p
LIB.sem_close.argtypes = [ctypes.c_void_p]
BASE = ('ticks 0\ntarget 4\ntick_ms 5\nmin_green 2\nmax_green 4\nclearance 2\n'
        'stream A car north straight 8 2 1000\nphase cars A\n')
RUNS = []


def absent(stderr, session):
    assert not session.exists(), session
    match = re.search(r'Сессия: sid=(\d+) .*ролей=(\d+)', stderr)
    if match:
        sid, roles = map(int, match.groups())
        for index in range(3 + 3 * roles):
            name = f'/cr3-{sid:016x}-{index}'.encode()
            ctypes.set_errno(0)
            if not index:
                fd = LIB.shm_open(name, os.O_RDWR, 0)
                if fd >= 0:
                    os.close(fd)
                assert fd == -1
            else:
                sem = LIB.sem_open(ctypes.c_char_p(name), 0)
                if sem not in (None, ctypes.c_void_p(-1).value):
                    LIB.sem_close(sem)
                    raise AssertionError(name)
            assert ctypes.get_errno() == errno.ENOENT, name
    for pid in re.findall(r'Роль: index=\d+ pid=(\d+)', stderr):
        try:
            os.kill(int(pid), 0)
        except ProcessLookupError:
            continue
        raise AssertionError('Остался PID ' + pid)


def command(backend, config, log, session):
    if backend in ('shm', 'fifo'):
        return [sys.executable, ROOT/'scripts/run_session.py', '--transport', backend,
                '--build-dir', BUILD, '--session', session, config, log]
    return [BUILD/('crossroad_'+backend), config, log, '--session', session]


def run(backend, name, text, expected=0, number=None, full=False, closed=False, blocked=False, worker_signal=False):
    folder = RESULTS / (backend+'-'+name)
    folder.mkdir(exist_ok=True)
    config, log, session = (folder/x for x in ('input.conf', 'model.log', 'session'))
    config.write_text(text)
    log.unlink(missing_ok=True)
    with open(folder/'stderr', 'w') as err, open(folder/'stdout', 'w') as out:
        child = subprocess.Popen([str(x) for x in command(backend, config, Path('/dev/full') if full else log, session)],
                                 stdout=subprocess.PIPE if closed or blocked else out, stderr=err, start_new_session=True)
        if closed:
            child.stdout.close()
        try:
            if number:
                limit = time.monotonic()+5
                while time.monotonic() < limit:
                    if log.exists() and 'Начало фазы:' in log.read_text():
                        break
                    assert child.poll() is None
                    time.sleep(.002)
                if worker_signal:
                    roles = re.findall(r'Роль: index=1 pid=(\d+)', (folder/'stderr').read_text())
                    assert roles
                    os.kill(int(roles[0]), number)
                else:
                    child.send_signal(number)
            code = child.wait(timeout=15)
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=7)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
            if blocked:
                child.stdout.close()
    errors = (folder/'stderr').read_text()
    assert code == expected, (backend, name, code, errors)
    assert not re.search(r'AddressSanitizer|runtime error:|LeakSanitizer', errors), errors
    absent(errors, session)
    if expected in (0, 3, 130, 143, 1):
        content = (folder/'stdout').read_text() if full else log.read_text()
        passed = verify(content, text)
        targets = re.findall(r'^target (\d+)', text, re.M)
        if targets and int(targets[-1]) and expected == 0:
            assert passed == int(targets[-1])
        if not closed and not blocked and not full:
            assert content == (folder/'stdout').read_text()
    RUNS.append(dict(backend=backend, scenario=name, code=code))


def manual(backend):
    folder = RESULTS / (backend+'-manual')
    folder.mkdir(exist_ok=True)
    config, log, session = (folder/x for x in ('input.conf', 'model.log', 'session'))
    config.write_text(BASE)
    roles = [('controller', None), ('direction', 'A'), ('participants', 'A')]
    with open(folder/'stdout', 'w') as out, open(folder/'stderr', 'w') as err:
        children = []
        try:
            for role, stream in roles:
                args = [BUILD/('crossroad_'+role), config, '--session', session, '--transport', backend]
                children.append(subprocess.Popen([str(x) for x in args + (['--stream', stream] if stream else [])], stdout=out, stderr=err))
            # Рабочие могут ждать публикацию до запуска координатора.
            coordinator = subprocess.Popen([str(x) for x in [BUILD/('crossroad_'+backend), config, log, '--session', session]], stdout=out, stderr=err)
            children.append(coordinator)
            for child in children:
                assert child.wait(timeout=12) == 0
        finally:
            for child in children:
                if child.poll() is None:
                    child.kill()
                child.wait()
    absent((folder/'stderr').read_text(), session)
    assert verify(log.read_text(), BASE) == 4


def registration(backend):
    folder = RESULTS/(backend+'-registration')
    folder.mkdir(exist_ok=True)
    config, log, session = (folder/x for x in ('input.conf', 'model.log', 'session'))
    config.write_text(BASE)
    log.unlink(missing_ok=True)
    with open(folder/'stdout', 'w') as out, open(folder/'stderr', 'w') as err:
        owner = subprocess.Popen([str(x) for x in [BUILD/('crossroad_'+backend), config, log, '--session', session]], stdout=out, stderr=err)
        worker = None
        try:
            args = [str(x) for x in [BUILD/'crossroad_controller', config, '--session', session, '--transport', backend]]
            worker = subprocess.Popen(args, stdout=out, stderr=err)
            limit = time.monotonic()+4
            while 'Роль: index=0' not in (folder/'stderr').read_text():
                assert owner.poll() is None and time.monotonic() < limit
                time.sleep(.002)
            duplicate = subprocess.run(args, stdout=out, stderr=err, timeout=6)
            assert duplicate.returncode == 1
            mismatch = folder/'other.conf'
            mismatch.write_text(BASE+'seed 1\n')
            wrong = subprocess.run([str(x) for x in [BUILD/'crossroad_direction', mismatch,
                '--session', session, '--transport', backend, '--stream', 'A']], stdout=out, stderr=err, timeout=6)
            assert wrong.returncode == 1
            # Недостающие роли не позволяют начать модель; startup завершается ошибкой.
            assert owner.wait(timeout=12) == 1 and worker.wait(timeout=6) == 1
        finally:
            for child in (owner, worker):
                if child and child.poll() is None:
                    child.kill()
                    child.wait()
    text = log.read_text()
    assert 'Начало фазы:' not in text
    verify(text, BASE)
    absent((folder/'stderr').read_text(), session)


def main():
    for backend in ('named', 'unnamed', 'shm', 'fifo'):
        for source in sorted((ROOT/'data').glob('*.conf')):
            run(backend, source.stem, source.read_text())
        for strategy in ('fixed', 'adaptive'):
            for mode in ('target', 'period'):
                for seed in (0, 4294967295):
                    text = BASE + f'strategy {strategy}\nseed {seed}\n'
                    if mode == 'period':
                        text += 'ticks 40\ntarget 0\n'
                    run(backend, f'{strategy}-{mode}-{seed}', text)
        for number in (signal.SIGINT, signal.SIGTERM):
            run(backend, 'signal-'+str(number), BASE+'target 0\n', 128+number, number)
        maximum = 'ticks 0\ntarget 8\ntick_ms 5\nmin_green 1\nmax_green 4\nclearance 1\n'
        maximum += ''.join(f'stream S{i} car north straight 64 1 1000\n' for i in range(16))
        maximum += ''.join(f'conflict S{i} S{j} 0\n' for i in range(16) for j in range(i+1, 16))
        maximum += 'phase all ' + ' '.join(f'S{i}' for i in range(16)) + '\n'
        run(backend, 'maximum', maximum)
        run(backend, 'worker-signal', BASE+'target 0\n', 143, signal.SIGTERM, worker_signal=True)
        run(backend, 'unreachable', BASE.replace('8 2 1000', '8 2 0'), 3)
        run(backend, 'full', BASE, 1, full=True)
        run(backend, 'closed', BASE, 1, closed=True)
        run(backend, 'blocked', BASE+'target 0\ntick_ms 1\n', 1, blocked=True)
        with concurrent.futures.ThreadPoolExecutor(2) as pool:
            list(pool.map(lambda n: run(backend, 'parallel-'+str(n), BASE), (0, 1)))
        if backend in ('shm', 'fifo'):
            manual(backend)
            registration(backend)
    folder = RESULTS/'invalid'
    folder.mkdir(exist_ok=True)
    for i, text in enumerate(('', BASE+'seed -1\n', BASE+'seed 4294967296\n', BASE+'ticks 1x\n',
                              BASE+'phase cars A\n', BASE+'\0', '#'*16385)):
        config = folder/f'{i}.conf'
        config.write_text(text)
        result = subprocess.run([str(x) for x in [BUILD/'crossroad_named', config, folder/'log', '--session', folder/'session']], capture_output=True)
        assert result.returncode == 2 and not (folder/'session').exists()
    config = folder/'safe.conf'
    config.write_text(BASE)
    result = subprocess.run([str(BUILD/'crossroad_named'), str(config), str(config)], capture_output=True)
    assert result.returncode == 1 and config.read_text() == BASE
    # Оракул обязан замечать нарушение, а не только принимать реальные журналы.
    good = RESULTS/'named-minimal'/'model.log'
    text = good.read_text()
    verify(text, (RESULTS/'named-minimal'/'input.conf').read_text())
    bad = re.sub(r'прошло=\d+', 'прошло=9999', text)
    try:
        verify(bad, (RESULTS/'named-minimal'/'input.conf').read_text())
    except AssertionError:
        pass
    else:
        raise AssertionError('Оракул принял неверные итоги')
    summary = dict(runs=len(RUNS), manual=2, registration=2, platform=platform.platform(), result='passed', cases=RUNS)
    (RESULTS/'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
