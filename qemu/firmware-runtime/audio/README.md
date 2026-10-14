# Audio helper input

The existing optional audio runtime accepts `audio-shim` as a file path, or
`audio-shim-hex` as a bounded inline helper (exactly one source). Studio supplies
the latter through `hardware-config=-` on an anonymous startup pipe, so the
decrypted version-bound helper need not be written to a file or command line.
Both inputs use the same AP digest, ABI, configuration identity, hook-byte and
entry-range validation. Inline input is at most64KiB decoded and must be even
length valid hexadecimal; the enclosing configuration remains limited to256KiB.
Configuration is startup-only. The core has no licensing/decryption logic.

Audio installation preserves requested binding-auth and binding-UI patches
in their existing validation order. Runtime reset restores the helper image and
its zeroed state. No change to the PCM/MMIO/encoding hot paths is introduced.

`tests/test_audio_inline_config.py` runs with a synthetic AP and synthetic
helper, without product mappings or vendor images. It covers installation,
startup locking, malformed/oversized input, digest/ABI mismatch, ambiguous
file+inline sources and combined binding patches. Run with `MIBAND_QEMU` set
to a native QEMU executable. Original source and licensing obligations remain
unchanged; an input encoding does not alter those obligations.
