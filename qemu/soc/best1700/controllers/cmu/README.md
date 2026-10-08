# BEST1700 CMU clock command ports

SYS and AON CMU clock enable/disable words address a shared control latch.
Writing one to ENABLE sets that bit; writing one to DISABLE clears it.
Zero leaves the state unchanged. Reads of either word return the same latch.
The implementation updates both words in the owning CMU register bank, so
the existing MMIO readers and secure aliases observe one state. Byte and
halfword commands affect only the addressed lanes.

| CMU | Clock group | ENABLE | DISABLE | Writable bits |
| --- | --- | --- | --- | --- |
| SYS | HCLK | `0x000` | `0x004` | 31:0 |
| SYS | PCLK | `0x008` | `0x00c` | 31:0 |
| SYS | OCLK | `0x010` | `0x014` | 31:0 |
| SYS | SYS_CLK | `0x060` | `0x064` | 29:0 |
| SYS | PLL_CLK | `0x0e4` | `0x0e8` | 7:0 |
| SYS | DSI_CLK | `0x100` | `0x104` | 31:0 |
| SYS | QCLK | `0x160` | `0x164` | 31:0 |
| SYS | XCLK | `0x178` | `0x17c` | 31:0 |
| SYS | UCLK | `0x244` | `0x248` | 31:0 |
| AON | TOP_CLK | `0x004` | `0x008` | 31:0 |
| AON | ACLK | `0x040` | `0x044` | 31:0 |
| AON | TOP_CLK1 | `0x0a0` | `0x0a4` | 22:0 |
| AON | OCLK | `0x180` | `0x184` | 31:0 |

All latches start at zero, preserving the model's existing reset defaults.
The boot code must issue its own enable commands; in particular, opening
UART0 requires setting SYS PCLK bit 11, which its opened check reads from
the DISABLE word. SYS PLL_CLK bit 5 is a writable PLL enable control, not a
timer-ready status bit.

This models control state and readback, not oscillator startup delays or
the complete physical clock tree. Peripheral transactions are not yet
gated by these latches. Timer frequency selection remains owned by the
SYS CMU divider at `0x5c`; this helper does not change it. Reset command
ports and other CMU registers remain the responsibility of their owners.

`tests/test_best1700_cmu_clock_qtest.py` covers paired readback, accumulation,
clearing, masks, byte lanes, independent banks and reset. A synthetic M55
program additionally follows the UART clock/opened check and writes to the
real UART model only when that check succeeds.
