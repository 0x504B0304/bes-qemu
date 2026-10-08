"""Native PCM endpoint regression; clang23 only, artifacts outside checkout."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]

class Pcm(unittest.TestCase):
    def test_endpoint(self):
        compiler = 'clang-23'
        version = subprocess.check_output([compiler, '--version'], text=True)
        self.assertRegex(version, r'clang version 23\.')
        with tempfile.TemporaryDirectory(prefix='bes-pcm-') as tmp:
            tmp = Path(tmp)
            for name, rate in [('ok.wav', 16000), ('bad.wav', 48000)]:
                with wave.open(str(tmp / name), 'wb') as w:
                    w.setparams((1, 2, rate, 0, 'NONE', ''))
                    w.writeframes(bytes(i & 255 for i in range(1282)))
            exe = tmp / 'test-pcm'
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-g',
                            '-I', str(ROOT / 'qemu/ip'),
                            str(ROOT / 'qemu/bridges/audio/miband_pcm.c'),
                            str(ROOT / 'tests/test_pcm.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe), str(tmp/'ok.wav'), str(tmp/'bad.wav')], check=True)

if __name__ == '__main__':
    unittest.main()
