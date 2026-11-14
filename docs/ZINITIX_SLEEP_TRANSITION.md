# Zinitix double taps across display sleep

Band 9 real-firmware qualification found a narrow but reproducible transition:
the panel receives sleep/display-off before the AP sends the Zinitix SLEEP
command. A double tap in that interval delivered its first ordinary contact
and acknowledgement while the controller was awake; SLEEP then discarded tap
history, leaving the second tap as a single sleeping tap. The screen remained
black for five seconds, with QMP and managed MI running normally.

Evidence outside the repositories in `band9-qualification-20261113`:

- `native-a6801916` reproduced this in a mixed workload after 21 completed
  cycles. Controller state/timestamps show the first completed tap awake, then
  SLEEP, then the second tap asleep. The failed long run remains recorded.
- `native-a61b56cb` deliberately timed a double tap after panel sleep began and
  independently reproduced the same transition. A later explicit second double
  tap recovered the real UI; NSH and full resources remained usable.
- `touch-sleep-boundary.json` records a separate synthetic bus reproduction.

For boards selecting `touch_sleep_gesture=1`, the recognizer now follows the
physical input stream while awake as well as asleep. Ordinary report ACKs and
SLEEP retain an eligible completed/in-progress tap; only a completed double
tap while sleeping emits a wake indication. Awake contacts still use the
ordinary report FIFO. Explicit wake, reset, acknowledgement of a pending wake,
time-domain changes, time/distance expiry and invalid long presses retain their
existing cancellation semantics. AF single-tap/slide boards are unchanged.

This is an explicit host-input recognition approximation, like the existing
timing/distance thresholds; it is not a claim about proprietary silicon's
internal gesture history. It does not inject firmware event queues, auto-repeat
input after a failure, or change firmware bytes, clocks or sleep policy.

Seven synthetic QTests exercise actual QMP input, I2C reports and gesture ACKs:
normal awake report consumption across SLEEP, an in-progress press crossing
SLEEP, single versus double tap, repeated gesture reads, ACK/resume, expiry,
long presses, distance and reset. The old traced engine failed exactly the two
transition cases; the candidate passes all seven without skips. They use
redistributable synthetic board/image data and the portable QTest transport.

The follow-up suite adds four AF-protocol regression cases: enabled single-tap
latching without UI contacts, completed slide-up versus press/orphan release,
rejection of taps/downward/horizontal strokes, and the unarmed legacy contact
wake path. All eleven pass on the same candidate in 1.330 seconds without
skips (`zinitix-all-protocols.log`). This bus coverage does not by itself
establish real-firmware acceptance for every board selecting those protocols.

The candidate's prepared Windows SHA-256 is
`798323d8c661abf229a2c7aebf4a8c53e5d83724e8891ca9b763a7596bdee431`.
Real replay `native-b1cc7737` again places SLEEP between the two taps, now
retaining `first_tap=1`; it produces gesture status 1 and panel display-on,
with a visible frame at the 0.437-second sample. Fresh NSH and complete resource
queries pass afterwards. The separate mixed long run `native-1d829229` then
passes thirty minutes: 120 cycles, 84 actual blank-to-visible recoveries,
maximum observed wake 0.891 seconds, unchanged resource IDs, fresh NSH and a
fresh ready native clock after load. The MI owner is running and unpoisoned;
cleanup leaves no QEMU/GDB children. This is bounded Windows development-core
evidence, not indefinite or cross-platform endurance.

Real input smoke on the same candidate covers Band 10's two sleep/wake/input
cycles (`native-59149af0`, which later stops on another model's guide-page
harness assumption), and Band 10 Pro/Band 11's two cycles each
(`native-09b2f693`, PASS/CLOSED). Application-list and returned stock-face
frames are visually inspected; fresh NSH and resource queries pass. The first
Band 10 Pro attempt's unchanged guide page was normal UI behavior, not a wake
failure; the completed test uses its visible Skip control before the next
cycle. These smoke tests do not replace full regression of all six old models.
No release core lock has been updated.
