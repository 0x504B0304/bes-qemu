# SPDX-License-Identifier: GPL-2.0-or-later
"""Avoid QEMU's optional symlink install tree on unprivileged Windows hosts.

The actual binaries, Meson install targets and pc-bios files remain intact.
Native launchers provide the data directory with -L; they do not use qemu-bundle.
"""
from pathlib import Path
import sys


def prepare(root):
    path=Path(root)/'meson.build'
    original="meson.add_postconf_script(find_program('scripts/symlink-install-tree.py'))"
    replacement="if host_machine.system() != 'windows'\n  "+original+"\nendif"
    text=path.read_text()
    if replacement not in text:
        if text.count(original)!=1:raise ValueError('unrecognized QEMU install tree setup')
        path.write_text(text.replace(original,replacement))
    # QEMU 8.2 acknowledges pipe bytes even when the QMP frontend is paused.
    # Keep the byte until chr_accept_input signals that the frontend can take it.
    path=Path(root)/'chardev/char-win-stdio.c'
    text=path.read_text()
    if 'win_stdio_accept_input' in text:return
    changes=[('    uint8_t win_stdio_buf;','    uint8_t win_stdio_buf;\n    bool input_pending;'),
        ('static void win_stdio_thread_wait_func(void *opaque)',
         'static void win_stdio_accept_input(Chardev *chr)'),
        ('    Chardev *chr = CHARDEV(opaque);\n    WinStdioChardev *stdio = WIN_STDIO_CHARDEV(opaque);\n\n'
         '    if (qemu_chr_be_can_write(chr)) {\n'
         '        qemu_chr_be_write(chr, &stdio->win_stdio_buf, 1);\n'
         '    }\n\n    SetEvent(stdio->hInputDoneEvent);\n}',
         '    WinStdioChardev *stdio = WIN_STDIO_CHARDEV(chr);\n\n'
         '    if (stdio->input_pending && qemu_chr_be_can_write(chr)) {\n'
         '        stdio->input_pending = false;\n'
         '        qemu_chr_be_write(chr, &stdio->win_stdio_buf, 1);\n'
         '        SetEvent(stdio->hInputDoneEvent);\n'
         '    }\n}\n\n'
         'static void win_stdio_thread_wait_func(void *opaque)\n{\n'
         '    WinStdioChardev *stdio = WIN_STDIO_CHARDEV(opaque);\n'
         '    stdio->input_pending = true;\n'
         '    win_stdio_accept_input(CHARDEV(opaque));\n}'),
        ('    cc->chr_set_echo = qemu_chr_set_echo_win_stdio;',
         '    cc->chr_set_echo = qemu_chr_set_echo_win_stdio;\n'
         '    cc->chr_accept_input = win_stdio_accept_input;')]
    for original,replacement in changes:
        if text.count(original)!=1:raise ValueError('unrecognized Windows stdio layout')
        text=text.replace(original,replacement)
    path.write_text(text)


if __name__=='__main__':prepare(sys.argv[1])
