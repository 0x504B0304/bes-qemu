"""Synthetic guest reset stays in-process by default or exits when opted in."""
import os
from pathlib import Path
import select
import subprocess
import tempfile

from tools.qmp_client import Qmp


def main():
    qemu = os.environ['MIBAND_QEMU']
    ap = os.environ['MIBAND_AP']
    with tempfile.TemporaryDirectory(prefix='bes-guest-reset-') as temp:
        for enabled in (False, True):
            path = Path(temp) / f'qmp-{enabled}.sock'
            with tempfile.TemporaryFile() as log:
                process = subprocess.Popen([qemu, '-M',
                    f'bes1503,bt-backend=synthetic,guest-reset-shutdown={"on" if enabled else "off"}',
                    '-kernel', ap, '-accel', 'qtest', '-qtest', 'stdio',
                    '-qmp', f'unix:{path},server=on,wait=off', '-display', 'none',
                    '-monitor', 'none', '-serial', 'none', '-serial', 'none'],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
                qmp = None
                try:
                    qmp = Qmp(path, process)
                    self_property = {'path': '/machine', 'property': 'guest-reset-shutdown'}
                    assert qmp.call('qom-get', self_property) is enabled
                    try:
                        qmp.call('qom-set', {**self_property, 'value': not enabled})
                    except RuntimeError as error:
                        assert 'startup-only' in str(error), error
                    else:
                        raise AssertionError('reset policy changed after startup')
                    process.stdin.write(b'writel 0xe000ed0c 0x05fa0004\n')
                    process.stdin.flush()
                    assert select.select([process.stdout], [], [], 5)[0], 'AIRCR write stalled'
                    reply = process.stdout.readline()
                    assert reply.startswith(b'OK'), reply
                    if enabled:
                        assert process.wait(timeout=5) == 0
                        log.seek(0)
                        assert b'guest reset requested' in log.read()
                    else:
                        qmp.call('query-status')
                        assert process.poll() is None, 'default reset unexpectedly exited'
                finally:
                    if qmp:
                        qmp.close()
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=5)
                    process.stdin.close()
                    process.stdout.close()
    print('PASS: AIRCR reset, opt-in clean shutdown, default restart, startup-only policy')


if __name__ == '__main__':
    main()
