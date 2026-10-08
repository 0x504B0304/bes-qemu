# SPDX-License-Identifier: GPL-2.0-or-later
"""Public hardware-config ABI 1: startup data, never executable/model-package code.

This schema generates checked C decoding, not product defaults. Keep this file
with the corresponding QEMU sources and build inputs when distributing binaries.
"""
TYPES = {
    'MiBandNandGeometry': {
        'page_size': (2048, 2048), 'spare_size': (64, 128),
        'pages_per_block': (64, 64), 'page_count': (1, 262144),
        'jedec_id': ('array', (0, 255), 3)},
    'MiBandBoardConfig': {
        'id': 'str', 'width': (1, 1024), 'height': (1, 1024),
        'framebuffer_bpp': (16, 32), 'charger_bus': (0, 5),
        'panel_te_gpio': (0, 95), 'battery_channel': (0, 6),
        'thermistor_mask': (1, 127), 'thermistor_mv': (0, 1800),
        'battery_divider_uv_per_mv': (1000, 10000),
        'curve_uv': ('pointer', (2500000, 5000000), 101, 'uint32_t'),
        'touch_model': 'str', 'touch_bus': (0, 7), 'touch_address': (1, 127),
        'touch_irq_gpio': (0, 95), 'touch_chip_id': (1, 65535),
        'panel_id': (0, 16777215), 'config_sha256': 'hash', 'nand_bus': (1, 1),
        'nand': 'MiBandNandGeometry', 'charger_probe_revision': (0, 255),
        'touch_sleep_gesture': (0, 1), 'touch_double_tap_status': (1, 2),
        'touch_reset_gpio': (-1, 95)},
    'MiBandFirmwareConfig': {
        **dict.fromkeys(('id', 'board'), 'str'),
        **dict.fromkeys(('config_sha256', 'storage_sha256'), 'hash'),
        **dict.fromkeys(('ble_service_uuid', 'ble_tx_uuid', 'ble_rx_uuid',
                        'miwear_service_uuid', 'gattc_service_uuid'), 'str'),
        **dict.fromkeys(('boot_sp', 'boot_pc'), (0, 4294967295)),
        'nor_address': (0, 16777215), 'nor_image_offset': (-16777216, 16777216),
        'nor_size': (1, 16777216), 'resource_first_page': (0, 262143),
        'resource_page_count': (1, 262144), 'ble_mtu': (23, 65535),
        'ble_chunk_size': (1, 65532), 'mass_slice_length': (1, 4294967295),
        'mass_types': ('pointer', (1, 255), (1, 32), 'uint8_t'),
        'mass_type_count': (1, 32), 'flash_offset': (0, 16777215),
        'resource_format': 'str', 'legacy_journal': 'bool'},
    'MiBandAudioHook': {'address': (0, 4294967295), 'entry': (8, 36),
                        'expected': ('array', (0, 255), 8)},
    'MiBandAudioProfile': {
        'id': 'str', 'ap_sha256': 'hash', 'config_sha256': 'hash',
        **dict.fromkeys(('flash_base', 'ram_base', 'mmio_base'), (0, 4294967295)),
        'legacy_abi2': 'bool', 'hooks': ('array', 'MiBandAudioHook', 7)},
    'MiBandBindingUiPatch': {
        'address': (0, 4294967295), 'guard_address': (0, 4294967295),
        'patch_size': (1, 8), 'guard_size': (1, 32),
        'expected': ('array', (0, 255), 8), 'replacement': ('array', (0, 255), 8),
        'guard': ('array', (0, 255), 32)},
}
TYPES['MiBandBindingAuthPatch'] = TYPES['MiBandBindingUiPatch']
for kind, size in [('Ui', 2), ('Auth', 8)]:
    TYPES[f'MiBandBinding{kind}Profile'] = {
        'id': 'str', 'ap_sha256': 'hash', 'config_sha256': 'hash',
        'flash_base': (0, 4294967295), 'patch_count': (1, size),
        'patches': ('array', f'MiBandBinding{kind}Patch', (1, size))}

SECTIONS = {
    'board': ('MiBandBoardConfig', 'miband_boards'),
    'firmware': ('MiBandFirmwareConfig', 'miband_firmwares'),
    'audio': ('MiBandAudioProfile', 'miband_audio_profiles'),
    'binding_ui': ('MiBandBindingUiProfile', 'miband_binding_ui_profiles'),
    'binding_auth': ('MiBandBindingAuthProfile', 'miband_binding_auth_profiles'),
}


def decoder_source():
    """Every field required, bounded and type checked; unknown fields rejected."""
    out = ['/* Generated from public tools/runtime_schema.py. */\n']
    def emit(target, obj, typ, depth=0):
        if typ in ('str', 'hash', 'bool') if isinstance(typ, str) else False:
            out.append(f'    {target} = read_{typ}({obj});\n')
        elif isinstance(typ, str):
            out.append(f'    decode_{typ}({obj}, &{target});\n')
        elif isinstance(typ[0], int):
            out.append(f'    {target} = read_int({obj}, {typ[0]}LL, {typ[1]}LL);\n')
        else:
            kind, item, length, *ctype = typ
            low, high = (length, length) if isinstance(length, int) else length
            v, i = f'a{depth}', f'i{depth}'
            out.append(f'    {{ QList *{v} = read_array({obj}, {low}, {high});\n')
            if kind == 'pointer':
                out.append(f'    {ctype[0]} *items = g_new0({ctype[0]}, qlist_size({v}));\n'
                           f'    {target} = items;\n')
            out.append(f'    size_t {i} = 0; const QListEntry *e{depth};\n'
                       f'    QLIST_FOREACH_ENTRY({v}, e{depth}) {{\n')
            emit(f'items[{i}]' if kind == 'pointer' else f'{target}[{i}]', f'e{depth}->value', item, depth+1)
            out.append(f'    {i}++;\n    }} }}\n')
    for name, fields in TYPES.items():
        out.append(f'static void decode_{name}(QObject *obj, {name} *p)\n{{\n'
                   f'    QDict *d = read_object(obj, {len(fields)});\n')
        for field, typ in fields.items():
            emit('p->'+field, f'required(d, "{field}")', typ)
        if name == 'MiBandFirmwareConfig':
            out.append('    require(qlist_size(qdict_get_qlist(d, "mass_types")) == p->mass_type_count, "mass type count");\n')
        if name.endswith('Profile') and 'patch_count' in fields:
            out.append('    require(qlist_size(qdict_get_qlist(d, "patches")) == p->patch_count, "patch count");\n')
        out.append('}\n')
    return ''.join(out).encode()
