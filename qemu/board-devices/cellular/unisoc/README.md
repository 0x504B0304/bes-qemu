# External modem UART boot stage

`boot.py` models the UART negotiation used before the cellular CIT/CMUX
transport. The caller supplies physical supply/PBINT levels and virtual time;
the model contains no SoC MMIO addresses or guest hooks.

A powered modem starts emitting five `7e` bytes on a PBINT rising edge. The
host lowers PBINT and sends a ten-byte normal-boot selection. After validating
framing, checksum, command and transport parameters, the endpoint emits a
checksummed ACK with status 5. Unsupported boot modes receive status 6;
malformed traffic receives no ACK. Removing power cancels partial packets and
request timing. Machine reset also starts a new virtual clock epoch.

The repeating request interval is a configurable simulation policy. ACK means
only that this boot-stage endpoint accepted the selection. It does not signal
CMUX readiness, register a cellular service, provision an eSIM or establish a
PDP context. The board supply/PBINT wiring and runtime UART attachment are not
yet integrated into the standard S5 launcher.
