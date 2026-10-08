# BEST1700 power controllers

The BTC and BTH controllers are separate sparse overlays on the shared AON PSC
window. All connected CPU address spaces see the same state for each controller.
Unowned PSC registers remain outside these models.

## BTH power and wake

The BTH model owns configuration/status/command/manual-control ports at offsets
`0x034..0x044`, interrupt wake mask/status at `0x130/0x13c`, and the three GPIO
wake mask/status banks. Configuration and command writes require the full
`0xcafe0000` key and supported fields. Status is hardware-owned; reading it
never acknowledges a peripheral. Manual REG fields take effect only when their
corresponding DR field selects software control.

A software automatic-down command schedules a virtual-time transition, gates
the BTH CPU, and retains pending wake inputs. An enabled wake or explicit up
command schedules power-up. Even a wake already pending at the down command
passes through the guest-programmed warm reset vector: resuming the old PC would
leave software power-down code in its terminal loop. The asynchronous target-CPU
reset path arbitrates CMU reset, PSC reset/power, and clock-stop ownership. A pure
clock stop preserves CPU context; a power/reset cycle fetches vectors again.

The machine retains peripheral IRQ levels across CPU/NVIC reset and re-drives
them afterward. CMU wake masks select the peripheral aggregate connected to PSC
wake bit 16; timer2 channels also connect to wake bits 5 and 6. Wake does not
clear an IPC IND, set DONE, consume a request, or synthesize an RPMsg response.
CMU mask bank 3 bit 31 is reserved for the separately routed NMI; external IRQ127
is not substituted for that input.

The model uses a one-nanosecond sequencing boundary to order asynchronous work;
this is not an analog settling-time or cycle-accuracy claim. CPU saved context,
vector tables, shared memory, and peripheral state remain available for guest
restoration. Full non-retained RAM/peripheral power-loss behavior is not yet
modeled. BISR controls are rejected rather than reporting fabricated completion.
GPIO wake inputs have a model interface, but board GPIO wiring and NMI wake are
not implemented here. The hardware-sleep request interface is not yet connected
to a CPU WFI/deep-sleep signal; the current guest path uses the software command.
The automatic sequencer clears the CMU LPU completion input on sleep entry and
asserts oscillator completion before releasing the CPU on warm wake. Cold and
manual power-up do not assert that completion. Guest writes cannot force it or
PLL readiness. PLL lock and analog oscillator wait timing remain outside this
feature.

## Validation

Set `MIBAND_BEST1700_QEMU` to an integrated build and run:

```sh
python3 -m unittest -v tests.test_best1700_bth_power_qtest.BthPowerTests
```

The tests cover keyed access, manual overrides, timer wake, live wake masks,
reset cancellation, shared CPU views, and synthetic Thumb programs that replace
their warm vector, stop executing while off, restore through that vector, and
acknowledge their own retained IPC interrupt. MTTCG coverage includes an IPC
request pending before power-down. Each test closes its QEMU before the next
instance starts. These tests do not by themselves qualify a complete firmware
shutdown sequence, long-running GUI, or every power domain.

A native Configurable run has exercised repeated warm restoration and completed the
original sensor syslog flush after a direct AON timer wake. This does not qualify
immediate IPC wake with empty CMU masks, the full power-off sequence, or GUI
endurance. The repeated MTTCG timer test uses both channels for three cycles
without a machine reset, with the guest itself acknowledging each interrupt.
