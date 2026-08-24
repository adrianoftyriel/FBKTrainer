# Safety

This program exists to drive a PA towards feedback on purpose, and eventually to
do it for days with nobody in the room. Read this before running it.

---

## What can actually go wrong

**A compression driver cooking.** This is the realistic risk, and it is not
dramatic — nothing bangs, the driver just quietly loses output and then stops.
Acoustic feedback is a sustained sinusoid, usually somewhere between 2 and 8 kHz,
which is close to the worst-case thermal load a HF driver can be given. A howl
that rings for ten seconds is doing far more damage than a loud show.

**Hearing.** Feedback arrives fast and gets loud faster. The ceiling in this
program is measured at the measurement microphone, not at your ears, and where you
stand is not where the microphone is.

**The unattended failure.** Neither the WING nor the X32 reverts anything if the
computer driving it crashes, sleeps or loses the network. The fader just stays
where it was last commanded and the room howls until somebody walks in. This is
the failure this program is most carefully built against, and the one it can least
completely solve on its own.

---

## What the program does about it

| Protection | What it catches |
|---|---|
| **SPL ceiling**, hard, at the measurement mic | The level getting away, for any reason |
| **Warning level** with proportional pullback | Approaching the ceiling; gentle near the warning, aggressive near the top |
| **Absolute gain cap** above the start point | A bug in the search asking for something absurd |
| **Small verified steps** — one at a time, confirmed by console read-back | Dropped OSC packets leaving the rig somewhere it was never commanded |
| **Immediate unbounded reductions** | Anything, quickly |
| **Burst limit and cooldown** | Sustained HF energy into a driver |
| **Thermal budget**, first-order model | Duty cycle across a long run |
| **Watchdog thread** | The control thread stalling |
| **`tick()` gap detection** | Both threads starving together — a machine going to sleep |
| **Console silence and mismatch detection** | The mixer unreachable, or not where it was told to be |
| **Dead microphone detection** | The rig going blind while making noise |
| **Screen saver / sleep inhibited** | Windows suspending mid-run |
| **Routing self test** | The gain going to the wrong channel entirely |
| **Repeated panic sends** | A single lost mute packet |

Every abort **latches**. Recovery is a deliberate act by a person. An automatic
one would let the rig re-enter the condition that caused the abort, indefinitely,
unattended — which is exactly the scenario the abort exists to prevent.

---

## What it cannot do, and what you must do instead

The program can only ever act through the console. Everything below is outside
that, and no amount of software covers it.

**Use a training rig you can afford to lose.** A cheap powered speaker with a
replaceable HF driver. Not your good boxes. The whole point of the exercise is
generating the thing that damages them.

**Set a hardware limiter on the console**, conservatively, on the channel and on
the output. This is the layer that works when this program is not running at all.

**Keep the amp gain physically low.** Give the software a small range to work in.
If the maximum achievable SPL in the room is bounded by the amplifier, then a
total software failure is bounded too.

**Wired ethernet only.** OSC over WiFi during a gain ramp is how you lose a
driver. The program treats console silence as a reason to stop, so an unreliable
link mostly produces aborted runs — but "mostly" is doing real work in that
sentence.

**Disable Windows sleep, hibernation and automatic restarts.** The program asks
Windows not to sleep, and that request is not binding on updates.

**Do not run it in a shared building without telling people.** Days of
intermittent feedback bursts is not a neutral thing to inflict on neighbours.

**Wear hearing protection when you are in the room with it**, and do not stand in
front of the speaker to watch.

---

## Calibrating the measurement microphone

Every acoustic limit here is in dB SPL. The bridge from the digital domain to the
room is one measured constant, and without it those limits do not correspond to
anything — so the program will not raise gain until it is set.

Present an acoustic calibrator to the capsule (94 dB SPL at 1 kHz is the usual
one), enter its level, and press Measure. The constant is:

```
splAtFullScale = calibratorLevel - measuredDbFS
```

A typical measurement chain lands somewhere near 120. Values outside 60–180 are
rejected, because they mean something is wrong with the calibration rather than
with the microphone, and accepting one would scale every safety limit in the
program by the size of the mistake.

**Recalibrate whenever the preamp gain changes.** The constant is a property of
the whole chain, not of the capsule.

---

## The limits, and choosing them

Defaults, in `SafetyLimits`:

| Limit | Default | Notes |
|---|---|---|
| `ceilingSplDb` | 100 | Hard stop at the measurement mic |
| `warnSplDb` | 94 | Pullback begins here |
| `maxGainAboveStartDb` | 12 | Absolute cap above the operator's working point |
| `maxStepDb` | 0.5 | Largest single step |
| `minStepIntervalMs` | 120 | Between steps |
| `maxBurstMs` | 400 | Longest deliberate excursion past the margin |
| `minCooldownMs` | 4000 | Enforced rest after a burst |
| `normalMarginDb` | 3 | Normal running stays this far from instability |
| `thermalBudgetS` | 20 | Equivalent seconds at the ceiling |
| `thermalTimeConstantS` | 90 | Assumed driver recovery |
| `watchdogTimeoutMs` | 750 | Dead-man's switch |
| `consoleAckTimeoutMs` | 1000 | Before the mixer counts as unreachable |

These are conservative on purpose. 100 dB SPL is loud enough to excite a room
properly and quiet enough that a mistake is unpleasant rather than expensive.
400 ms is long enough to measure a growth rate; letting a howl ring buys no
information and costs driver life.

The limits belong to the **rig**, not to the run. A run is something you start,
and things you start get started carelessly at two in the morning. A rig is
something you set up once with the boxes in front of you, and the limits belong to
the boxes.

Validation rejects limits that contradict each other — a warning level above the
ceiling, a cooldown shorter than the burst it follows — because a set of limits
that cannot be satisfied is not a limit.

---

## If something goes wrong

**STOP** on the Run panel mutes the channel and floors the fader immediately,
sending several times over. It does not wait for confirmation, because there is
nothing to confirm to.

If the program is not responding, pull the network cable. The console keeps its
last commanded fader position — so also pull the amp, or mute at the console
surface.

The console surface always wins. Nothing here defeats a physical mute.
