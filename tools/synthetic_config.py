# SPDX-License-Identifier: GPL-2.0-or-later
"""A deliberately synthetic, redistributable board for ABI and boot tests."""
import json
from pathlib import Path


def configuration():
    zero = '0' * 64
    return dict(abi=1,
        board=dict(id='synthetic', width=128, height=128, framebuffer_bpp=16,
            charger_bus=0, panel_te_gpio=0, battery_channel=0, thermistor_mask=2,
            thermistor_mv=900, battery_divider_uv_per_mv=3000,
            curve_uv=[4300000-i*10000 for i in range(101)], touch_model='inferred-report',
            touch_bus=5, touch_address=32, touch_irq_gpio=1, touch_chip_id=1, panel_id=0,
            config_sha256=zero, nand_bus=1,
            nand=dict(page_size=2048, spare_size=64, pages_per_block=64, page_count=262144,
                      jedec_id=[0, 0, 0]), charger_probe_revision=7, touch_sleep_gesture=0,
            touch_double_tap_status=1, touch_reset_gpio=-1),
        firmware=dict(id='synthetic', board='synthetic', config_sha256=zero, storage_sha256=zero,
            ble_service_uuid='1800', ble_tx_uuid='2a00', ble_rx_uuid='2a01',
            miwear_service_uuid='1801', gattc_service_uuid='1802', boot_sp=0x20010000,
            boot_pc=0x0c000001, nor_address=0, nor_image_offset=0, nor_size=0x1000000,
            resource_first_page=0, resource_page_count=1, ble_mtu=23, ble_chunk_size=20,
            mass_slice_length=1024, mass_types=[1], mass_type_count=1,
            flash_offset=0, resource_format='romfs', legacy_journal=False))


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    args = p.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    (args.directory/'hardware.json').write_text(json.dumps(configuration(), indent=2)+'\n')
    # Thumb B . executes forever with no proprietary firmware.
    (args.directory/'guest.bin').write_bytes(bytes.fromhex('fee7'))
