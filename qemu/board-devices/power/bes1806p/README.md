# BES1806P candidate PMU

This board endpoint exposes identification (`0x18f0`, secondary ID `0x0650`,
revision zero) and the physical PWRKEY input over the AP/CP shared ISPI bus.
The board selects chip-select zero and the host key. It is a candidate
simulation scenario, not identification of the physical Configurable part.
The controller follows CR0's frame width: the 25-bit external command format
and the padded normal format access the same endpoint. Normal register
addresses retain all ten bits instead of aliasing to eight bits. Channel-enable
register `0x27`, START, completion status, and interval resampling also use the
existing GPADC conversion model.

The original firmware accepts this identity as its external-GPIO-capable PMU
variant. PWRKEY uses register `0x44` bits 3/4 to enable press/release interrupts,
and register `0x5e` bit 9 for the physical pressed level, with bits 10/11 for
edge pending. Pending is latched independently of masking and cleared by guest
write-one-to-clear; acknowledging an edge never releases a held button.
The transport ORs slave IRQs into the SoC PMU interrupt input (AP IRQ23).
Duplicate host key events do not create new edges. System reset clears pending,
configuration, and the pressed level, while keeping the endpoint attached.
Host keyboard input uses the shared bridge dispatcher so GPIO and PMU buttons
can be held and released independently without competing for QEMU's sole
keyboard event handler.

Other PMU registers still use the controller's existing register/GPADC fallback.
Power rails, general PMU GPIOs, RTC, OTP, and charger behaviour are not implemented
by this endpoint. In particular this is not yet a modem-enable GPIO model.

## Soft poweroff

Page 0 register `0x4f` bit 0 latches the ordinary soft-poweroff command and
asserts the named `poweroff` output once. Its register backing remains in the
shared ISPI controller: the endpoint observes a completed write without taking
ownership of the register, preserving GPADC bit 5 and interval resampling.
Bit 6 alone does not remove power; its variant-dependent meaning is unqualified.
Page-select transactions, other chip selects and other pages do not trigger it.
Reset clears the output/latch without requesting shutdown.

The board configuration connects this output to `miband-guest-power`, which
requests standard QEMU `guest-shutdown` through the main loop. This is distinct
from PWRKEY IRQ delivery, host `system_powerdown`, and any AON global-reset path.
It does not model shipment mode, battery-backed RTC, charging wake or analog
rail timing. Synthetic coverage is in `tests/test_pmu_poweroff_qtest.py`;
original-firmware poweroff and subsequent cold boots require separate evidence.
