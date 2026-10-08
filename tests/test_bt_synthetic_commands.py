#!/usr/bin/env python3
"""Validate generic HCI and BES identity updates through the real mailbox."""
from tests.bt_qtest import request, request_many


def hci(opcode, params=b''):
    return b'\x01' + opcode.to_bytes(2, 'little') + bytes([len(params)]) + params


def complete(opcode, params=b'', status=0):
    return (bytes([4, 14, 4 + len(params), 1]) + opcode.to_bytes(2, 'little')
            + bytes([status]) + params)


def check_state():
    name = b'reference generic HCI bridge'.ljust(248, b'\x00')
    address = bytes.fromhex('11 22 33 44 55 c6')
    random_address = bytes.fromhex('a1 a2 a3 a4 a5 c6')
    features = bytes.fromhex('01 02 03 04 05 06 07 08')
    extended = bytes.fromhex('11 12 13 14 15 16 17 18')
    commands = [
        hci(0x0c13, name), hci(0x0c14),
        hci(0x0c13, b'bad'), hci(0x0c14),
        hci(0xfc32, address), hci(0x1009),
        hci(0xfc81, features), hci(0x1003), hci(0x2003),
        hci(0xfc82, b'\x00' + extended), hci(0x1004, b'\x00'),
        hci(0xfc72, b'\x01' + random_address), hci(0x1009),
        hci(0x2005, address), hci(0x1009),
        hci(0x2005, b'bad'), hci(0x1009),
        hci(0x0c03), hci(0x0c14), hci(0x1009),
        hci(0x0c00), hci(0x1009) + b'\x00', hci(0x1009),
    ]
    responses = request_many(commands)
    expected = [
        complete(0x0c13), complete(0x0c14, name),
        complete(0x0c13, status=0x12), complete(0x0c14, name),
        complete(0xfc32, address), complete(0x1009, address),
        complete(0xfc81, features), complete(0x1003, features), complete(0x2003, bytes(8)),
        # FC82 retains the legacy eight-byte stored window, including its page byte.
        complete(0xfc82, b'\x00' + extended),
        complete(0x1004, b'\x00\x00\x00' + extended[:7]),
        complete(0xfc72, b'\x01' + random_address), complete(0x1009, random_address),
        complete(0x2005), complete(0x1009, address),
        complete(0x2005, status=0x12), complete(0x1009, address),
        complete(0x0c03), complete(0x0c14, name), complete(0x1009, address),
        complete(0x0c00, status=1), complete(0x1009, status=0x12), complete(0x1009, address),
    ]
    assert len(responses) == len(expected)
    for command, actual, wanted in zip(commands, responses, expected):
        assert actual == wanted, (command[:4].hex(), actual.hex(), wanted.hex())
    print('SYNTHETIC_HCI_STATE_OK name=248 BES-identity=1 reset=1 invalid-no-mutation=1')


def main():
    response = request(bytes.fromhex('01 18 20 00'))
    assert len(response) == 15, response.hex()
    assert response[:7] == bytes.fromhex('04 0e 0c 01 18 20 00'), response.hex()
    print('SYNTHETIC_LE_RAND_OK completion=15 random_bytes=8')
    check_state()


if __name__ == '__main__':
    main()
