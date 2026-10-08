#!/usr/bin/env python3
"""Exercise the DCS panel boundary without QEMU or BEST1503 state."""
import shlex
import subprocess
import tempfile
from pathlib import Path

from tools.source_layout import include_flags, source_file


PROGRAM = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "miband_panel_device.h"
static unsigned presents, blanks, edges;
static void present(void *opaque, const uint8_t *p, unsigned w, unsigned h) {
    (void)opaque; assert(w == 2 && h == 2); presents++;
    if (!p) blanks++;
}
static void edge(void *opaque) { (void)opaque; edges++; }
int main(void) {
    MiBandPanelDevice *panel = miband_panel_new(0xe624, 2, 2);
    assert(panel); MiBandPanelEndpoint ep = miband_panel_endpoint(panel);
    miband_panel_connect_display(panel, NULL, present);
    miband_panel_connect_te(panel, NULL, edge);
    assert(ep.status(ep.opaque).visible);
    assert(ep.command(ep.opaque, 0xda, true).value == 0);
    assert(ep.command(ep.opaque, 0xdb, true).value == 0xe6);
    assert(ep.command(ep.opaque, 0xdc, true).value == 0x24);
    uint8_t frame[16]; memset(frame, 0x5a, sizeof(frame));
    assert(ep.transfer(ep.opaque, frame, sizeof(frame)));
    assert(ep.status(ep.opaque).gram_valid && ep.status(ep.opaque).frame_id == 1);
    assert(!memcmp(ep.snapshot(ep.opaque), frame, sizeof(frame)));
    ep.command(ep.opaque, 0x35, false); ep.tick(ep.opaque); assert(edges == 1);
    ep.command(ep.opaque, 0x28, false); assert(blanks == 1);
    ep.command(ep.opaque, 0x29, false); assert(presents >= 3);
    ep.command(ep.opaque, 0x01, false);
    assert(!ep.status(ep.opaque).gram_valid && ep.command(ep.opaque, 0x10, false).value == 0);
    uint8_t reply[3], parameter = 0;
    assert(miband_panel_dcs_read(panel, 4, reply, 3) == 3);
    assert(reply[0] == 0 && reply[1] == 0xe6 && reply[2] == 0x24);
    assert(miband_panel_dcs_read(panel, 4, reply, 2) == -1);
    assert(miband_panel_dcs_read(panel, 0xff, reply, 3) == -1);
    assert(miband_panel_dcs_read(panel, 0x0a, reply, 1) == 1 && reply[0] == 8);
    assert(miband_panel_dcs_write(panel, 0x11, NULL, 0));
    assert(miband_panel_dcs_write(panel, 0x29, NULL, 0));
    assert(miband_panel_dcs_read(panel, 0x0a, reply, 1) == 1 && reply[0] == 0x1c);
    assert(miband_panel_dcs_write(panel, 0x12, NULL, 0));
    assert(miband_panel_dcs_write(panel, 0x39, NULL, 0));
    assert(miband_panel_dcs_read(panel, 0x0a, reply, 1) == 1 && reply[0] == 0x74);
    assert(miband_panel_dcs_write(panel, 0x13, NULL, 0));
    assert(miband_panel_dcs_write(panel, 0x38, NULL, 0));
    assert(miband_panel_dcs_write(panel, 0x35, &parameter, 1));
    assert(miband_panel_dcs_read(panel, 0x0e, reply, 1) == 1 && reply[0] == 0x80);
    parameter = 1;
    assert(miband_panel_dcs_write(panel, 0x35, &parameter, 1));
    assert(miband_panel_dcs_read(panel, 0x0e, reply, 1) == 1 && reply[0] == 0xc0);
    parameter = 2;
    assert(!miband_panel_dcs_write(panel, 0x35, &parameter, 1));
    parameter = 0x7f;
    assert(miband_panel_dcs_write(panel, 0x51, &parameter, 1));
    assert(miband_panel_dcs_read(panel, 0x52, reply, 1) == 1 && reply[0] == 0x7f);
    assert(!miband_panel_dcs_write(panel, 0x51, NULL, 1));
    assert(miband_panel_dcs_write(panel, 0x01, NULL, 0));
    assert(miband_panel_dcs_read(panel, 0x52, reply, 1) == 1 && reply[0] == 0);
    miband_panel_free(panel); puts("PANEL_DEVICE_OK command-id-frame-retention-te-boundary");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='bes-panel-device-') as directory:
        directory = Path(directory)
        source = directory / 'test.c'
        source.write_text(PROGRAM)
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True))
        subprocess.run(['clang-23', '-Wall', '-Wextra', '-Werror', *include_flags(),
                        str(source), str(source_file('miband_panel_device.c')),
                        *flags, '-o', str(directory / 'test')], check=True)
        subprocess.run([str(directory / 'test')], check=True, timeout=10)


if __name__ == '__main__':
    main()
