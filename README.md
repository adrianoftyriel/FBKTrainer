# FBKTrainer

A Windows application that brings up a feedback-training rig: it plays speech
into a PA through a vocal microphone, controls the console over OSC, and measures
what the room does — in order to learn how to suppress feedback **before** it
happens, rather than notching it out afterwards.

The model it produces is for [FBKSuppressor](https://github.com/adrianoftyriel/FBKSuppressor),
whose `HowlDetector` already accepts mode priors and calibrated thresholds. Those
interfaces are the target this program aims at.

---

## Status

**v0.1 — the rig, and nothing else.**

This build brings a rig up and proves it is wired correctly. It does not raise
gain on its own, does not induce feedback, and does not train anything. That is
deliberate: everything that comes later involves driving a PA towards instability
unattended for days, and the part that decides how far it may go has to be right
before any of the rest is safe to build.

What works now:

- Assign the four things the rig needs: **Speech Output**, **mixer IP**,
  **Vocal Channel**, **Listening Mic Input** (plus a vocal return, recommended).
- Discover the console's OSC addresses. Read-only — nothing is written, so no
  fader moves.
- Calibrate the measurement microphone against an acoustic calibrator.
- Run the **routing self test**, which proves the configured vocal channel really
  is the channel the microphone is on.
- Run the rig with the full safety envelope enforced, and watch it live.

What does not exist yet: the sweep-based loop measurement, the scoring harness,
the parameter search, and the offline simulator. See **Where this is going**.

---

## Why the safety layer came first

The eventual program raises the gain on a live PA on purpose and then leaves it
doing that for days with nobody in the room. Every other part of this can be
debugged by watching it misbehave. The safety envelope cannot, because its whole
job is the situations nobody is present for.

So `SafetySupervisor` is a pure state machine — no I/O, no threads, no clock.
Observations in, decisions out. That is what lets the envelope be tested at
timings impossible to arrange deliberately in a room: the packet that never
arrives, the microphone unplugged at three in the morning, the machine that
stalls for two seconds because Windows decided to install something.

The rules it enforces:

| | |
|---|---|
| **Permission, not limit** | `permittedGainDb` starts at off and is re-established from scratch every tick. A check that cannot be evaluated does not grant it. |
| **Aborts latch** | Recovery is manual. An automatic one would let the rig re-enter the condition that caused the abort, indefinitely, unattended. |
| **Asymmetric ramps** | Increases are stepped, rate-limited and individually confirmed by console read-back. Reductions are immediate and unbounded. |
| **No measurement, no gain** | Until a loop measurement exists, no increase above the start point is permitted — the instability point could be anywhere, including below where we already are. |
| **The cap outranks everything** | No measurement and no algorithm can raise gain past the configured ceiling. The search is the part of the program meant to change itself; a bug in it must not be expressible as a fader at +40. |
| **Dead-man's switch** | An independent watchdog thread asks only whether the supervisor has been ticked. `tick()` can only notice a stall once it resumes, which is already too late. |

1005 checks cover this, on four toolchains, with no audio hardware and no console.

---

## Why the routing self test exists

The configuration says "the vocal microphone is console channel 12". Nothing
checks that.

If it is really on channel 14, every gain command goes to a channel that is not
in the feedback loop — and the symptom is not an error. It is a rig that raises
gain steadily, sees no feedback, and concludes the room has enormous margin. It
would keep going until something else gave way.

So the wiring is proved by observation, not assertion: raise the assigned fader by
6 dB from a low reference position and check the level at the measurement
microphone follows. A channel that does not respond is not the channel we think
it is, and the rig will not run.

The same test settles a second thing discovery cannot: **what units the console's
fader is in**. Discovery can find which addresses answer, but a fader at −10 dB
reads as `-10.0` in one encoding and `0.5` in the other, and both are plausible.
Commanding a known move and checking the read-back agrees is what resolves it —
and it has to be resolved before any gain is raised, because wrong units make
every subsequent step the wrong size.

---

## Console support

**Behringer WING / WING Rack** — the address tree is *table-driven, not
hardcoded*. The WING's OSC tree is not the X32's and has moved between firmware
versions. A hardcoded address that turns out to be wrong does not fail cleanly; it
produces a program that sends commands into the void while believing it controls a
fader. Candidates are resolved against your actual console at setup and stored
with the rig.

**X32 / M32** — the configuration accepts it and validation rejects it, on
purpose, rather than quietly behaving like a WING. It comes after the WING path is
proven in a room.

---

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/tests/fbkt_tests
```

JUCE 8.0.15 is fetched and pinned, matching FBKSuppressor. The two must agree:
they share `fbk_dsp`, and a version skew between the program that trains a model
and the plugin that runs it is a difference nobody would think to look for.

`fbk_dsp` comes from a sibling FBKSuppressor checkout — `../FBKSuppressor` by
default, overridable with `-DFBKT_SUPPRESSOR_DIR=...`. It is optional at this
stage and the application builds without it. It is a sibling checkout rather than
a fetch so that the version under test is whatever is on the disk, which is what
you want while both repositories are moving together.

To build only the safety core and its tests — no JUCE, no audio stack, nothing
fetched:

```sh
cmake -S . -B build -DFBKT_BUILD_APP=OFF
```

### ASIO

Windows builds enable ASIO. From JUCE 8 the required headers are bundled, so no
external SDK is needed; enabling it does put the binary under the Steinberg ASIO
SDK licence terms. WASAPI remains the fallback, and is usable for setting a rig up
but not for running one.

---

## Setting a rig up

Read [`docs/SAFETY.md`](docs/SAFETY.md) first. Then, in order:

1. **Rig** — open the audio device and assign the ports. Everything must go
   through **one** device, in practice the console's own card. Two devices are two
   sample clocks, and two sample clocks drift; since the whole proactive model is
   trained on when things happen relative to each other, a drifting clock does not
   add noise to the data, it adds a slow lie.
2. **Console** — enter the mixer IP and the vocal channel, connect, and run
   discovery.
3. **Check** — calibrate the measurement microphone, then run the routing self
   test.
4. **Run** — the button stays greyed out until all of the above have passed.

---

## Where this is going

Three layers, in this order. Each produces the data the next one needs.

**Layer 1 — search the parameters that already exist.** `HowlDetector`'s
thresholds, `TonalCanceller`'s step sizes, the mask's attenuation limits: around
30 scalars, all currently hand-chosen against a synthetic voice. Also the
proactive arming policy — "given a mode 4 dB from instability, pre-arm a canceller
there at low step size before anything is audible" — which is a rule with about
six parameters in it, not a learned model. Black-box optimisation, interpretable
results.

**Layer 2 — learn the margin predictor.** The actual proactive brain: current
spectrum plus measured loop response plus gain, in; per-mode dB-to-instability
and onset probability, out.

**Layer 3 — the `SpectralSeparator` network.** The slot already exists in
FBKSuppressor and is currently a null stub. This is the voice-quality half:
learned per-band voice presence replacing the current heuristic, so the suppressor
stops dulling a voice when it acts.

Two things shape all of it:

**Sweeps, not howls.** Pushing gain until it howls yields a rare binary event, and
only ever tells you about the one mode that won the race. `SweepMeasurement` in
FBKSuppressor already measures the whole loop response, which turns the label into
a dense per-mode margin available at every frame — without howling. Confirmed
howls become validation that the margins were right, not the primary signal.

**A simulator, not just a room.** Days of unattended data in one room produces a
beautifully overfitted model of that room. Measured impulse responses let the loop
be closed in software, generating many virtual rooms faster than real time — and,
for the separator, training pairs where the clean voice and the interference are
known separately. The rig collects and validates; the simulator generates.

---

## Licence

Not yet chosen.
