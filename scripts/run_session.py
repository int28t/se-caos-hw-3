#!/usr/bin/env python3
"""Запуск отдельных программ ролей; предметная модель остаётся в C."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--transport', choices=('shm', 'fifo'), required=True)
    parser.add_argument('--build-dir', type=Path, default=Path('build'))
    parser.add_argument('--session', type=Path)
    parser.add_argument('config', type=Path)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    session = args.session or Path(f'.crossroad-launch-{os.getpid()}')
    children, caught = [], []

    def interrupt(number, _frame):
        caught.append(number)
        for child in children:
            if child.poll() is None:
                child.send_signal(number)

    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, interrupt)
    try:
        streams = [row.split()[1] for row in args.config.read_text().splitlines()
                   if row.split() and row.split()[0] == 'stream']
        commands = [[build / ('crossroad_' + args.transport), args.config, args.log,
                     '--session', session]]
        for role in ('controller', 'direction', 'participants'):
            for stream in ([None] if role == 'controller' else streams):
                command = [build / ('crossroad_' + role), args.config,
                           '--session', session, '--transport', args.transport]
                commands.append(command + (['--stream', stream] if stream else []))
        for command in commands:
            if caught:
                break
            children.append(subprocess.Popen([str(x) for x in command], start_new_session=True))
        while children and children[0].poll() is None:
            time.sleep(.01)
        status = children[0].returncode if children else 1
        deadline = time.monotonic() + 6
        for child in children[1:]:
            code = child.wait(timeout=max(.01, deadline - time.monotonic()))
            if code != status:
                status = 1
        return 128 + caught[0] if caught and status != 1 else status
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(str(error), file=sys.stderr)
        return 1
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()
        deadline = time.monotonic() + 6
        for child in children:
            try:
                child.wait(timeout=max(.01, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()


if __name__ == '__main__':
    sys.exit(main())
