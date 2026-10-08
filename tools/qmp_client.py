"""Minimal public QMP client for standalone core tests."""
import json
import socket
import time


class Qmp:
    def __init__(self, endpoint, process):
        deadline = time.monotonic() + 10
        while True:
            self.socket = socket.socket(socket.AF_INET if isinstance(endpoint, tuple) else socket.AF_UNIX)
            self.socket.settimeout(10)
            try:
                self.socket.connect(endpoint if isinstance(endpoint, tuple) else str(endpoint))
                break
            except OSError:
                self.socket.close()
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('QEMU failed to start; see qemu-stderr.log')
                time.sleep(.05)
        self.stream = self.socket.makefile('rwb')
        json.loads(self.stream.readline())
        self.call('qmp_capabilities')

    def call(self, name, arguments=None):
        message = {'execute': name}
        if arguments is not None:
            message['arguments'] = arguments
        self.stream.write((json.dumps(message) + '\n').encode())
        self.stream.flush()
        while True:
            reply = json.loads(self.stream.readline())
            if 'error' in reply:
                raise RuntimeError(reply['error'])
            if 'return' in reply:
                return reply['return']

    def close(self):
        self.stream.close()
        self.socket.close()

