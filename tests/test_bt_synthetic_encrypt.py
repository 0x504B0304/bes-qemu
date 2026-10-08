#!/usr/bin/env python3
"""Validate synthetic LE Encrypt against the NIST AES vector through the mailbox."""
from tests.bt_qtest import request


def main():
    command = bytes.fromhex('01 17 20 20') + bytes(range(16))
    command += bytes.fromhex('00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff')
    response = request(command)
    expected = bytes.fromhex('04 0e 14 01 17 20 00 69 c4 e0 d8 6a 7b 04 30 d8 cd b7 80 70 b4 c5 5a')
    assert response == expected, response.hex()
    print('SYNTHETIC_LE_ENCRYPT_OK NIST_AES_VECTOR')


if __name__ == '__main__':
    main()
