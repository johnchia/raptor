# Four things wrong on a T31, found from the console

Field report from a Wyze Cam v3 (T31X, gc2053, 1920x1080) on the 2026-08-27
nightly, build `8307ecf`, OpenIPC base. Every finding below was reproduced on
that board; the numbers are measurements, not estimates.

Two of the four are the same bug wearing different clothes, so they are written
up together.

| # | Symptom | Where it lives | State |
|---|---------|----------------|-------|
| 1 | Exposure compensation offers -255..255; anything below 0 is refused | rcd schema vs. the T31 HAL's caps table | **fixed**, board-verified |
| 2 | Exposure compensation is the only knob with an `auto` button, and pressing it fails | same | **fixed**, board-verified |
| 3 | The IR-cut filter does not move, in either direction | `ric`, default `pulse_ms` | **default raised to 30 ms**; one check still owed |
| 4 | Any live rate-control change kills the encoder channel and wedges rvd's teardown | `hal_enc_set_rc_mode` | **fixed**, board-verified |

---

## 1 and 2: the one knob nobody can describe

### What you see

The Image tab draws exposure compensation as a slider from -255 to 255. Drag it
below zero and the write is refused. It is also the only control on the tab
with an `auto` button — and clicking that fails too:

    # raptorctl rvd set-ae-comp -10
    {"status":"error","reason":"failed (-1)"}
    # raptorctl rvd set-ae-comp auto
    {"status":"error","reason":"failed (-22)"}

Meanwhile every other knob on the tab — brightness, contrast, saturation,
sharpness, sinter, temper — has no `auto` button at all, which is correct and
is what makes exposure compensation look like the odd one out.

### The measured range

Bisected on the board with `raptorctl rvd set-ae-comp`, restoring 128 after:

    -255  error (-1)      128  ok
      -1  error (-1)      255  ok
       0  ok              256  error (-1)
       1  ok              512  error (-1)
                        65536  error (-1)

So `IMP_ISP_Tuning_SetAeComp` on T31 accepts **0..255**, and the value the
camera boots with is 128. It is a byte with a midpoint neutral, exactly like
every other Ingenic tuning knob. The published -255..255 is wrong at both ends.

### Why both symptoms have one cause

`rcd`'s key table is compile-time and global; the range belongs to the silicon.
The console knows this and prefers the camera's own answer, falling back to the
schema only when the camera has not given one — `bounds()` in
`rhd/console.html`:

    function bounds(k) {
      const c = k.section === "image" ? ISP.caps[k.key] : null;
      if (!c) return {min: k.min, max: k.max, neutral: null,
                      auto: k.auto === true, enabled: true};
      return {min: c.min, max: c.max, neutral: c.neutral,
              auto: k.auto === true && c.auto !== false, enabled: c.enabled !== false};
    }

That fallback is optimistic in both directions on purpose — the comment above it
argues that a control drawn from a range too wide refuses at the edges, while
one hidden for want of an answer cannot be used at all. Fine, except for which
knob lands in it.

`get-isp` on the board publishes twelve caps rows and ae_comp is not among them:

    "caps":{"brightness":{"min":0,"max":255,"neutral":128,"auto":false,...},
            ... "backlight_comp":{"min":0,"max":10,"neutral":0,"auto":false,...}}

because `imp_knob_caps[]` in `raptor-hal/src/hal_isp.c` deliberately omits it.
The comment there says why:

> ae_comp is absent: IMP_ISP_Tuning_SetAeComp takes a bare int with no
> documented bound, and inventing one here would be a guess wearing the
> clothes of a capability.

Defensible when written. The consequence is that the **one** knob with no
capability answer is the **only** one that gets the optimistic fallback — and
the fallback supplies both halves of what is wrong:

- the range becomes the schema's -255..255, which on this SoC is half wrong;
- `auto` becomes `k.auto === true` with no `c.auto !== false` to veto it, and
  the schema says `auto_ok` is true because `LIVE_ISP()` sets it for every ISP
  knob. Every other row is vetoed by its caps entry saying `"auto":false`; this
  one has nothing to veto it.

So the exact inversion: the knob the platform declined to describe is the knob
drawn with the most confidence.

The schema's -255..255 is not arbitrary either — the comment on
`rcd_schema.c:338` records that it was widened from 0..255 for SigmaStar, which
states exposure compensation in EV steps around zero. One compile-time table,
two platforms whose ranges do not overlap. That is the structural half of the
problem, and it is the reason the runtime caps path exists at all.

### The fix

The row is added, now that it has been measured:

    {"ae_comp", 0, 255, 128},

The original objection was against *inventing* a bound. This one is not
invented: 0..255 with neutral 128 is what the SDK on this part accepts,
bisected above, and it makes ae_comp the same shape as every other IMP tuning
byte. The row fixes the range and removes the auto button in one stroke,
because it carries `has_auto = false` like all the others.

`neutral` matters as much as the bounds: without a caps row the console's
`shown()` falls through to `b.min`, so a fresh camera draws the slider hard
left when the hardware is sitting at the midpoint.

`hal_isp_set_ae_comp` now clamps through `hal_clamp_u8` like every sibling
setter, and for a reason particular to this knob: rcd's key table is one table
for every platform and still says -255..255, correctly, because SigmaStar
states compensation in EV steps around zero. So an out-of-range value arrives
here as a matter of course rather than as a caller's mistake, and the nearest
in-range value is a better answer than a refusal the caller cannot act on. A
range this layer publishes has to be a range it enforces, or a client that drew
its control from that answer still gets a bare -1 back.

On the board afterwards:

    "caps":{... "ae_comp":{"min":0,"max":255,"neutral":128,"auto":false,"enabled":true} ...}

    # raptorctl rvd set-ae-comp -10     ->  {"status":"ok"}   (clamps to 0)
    # raptorctl rvd set-ae-comp 1000    ->  {"status":"ok"}   (clamps to 255)
    # raptorctl rvd set-ae-comp auto    ->  {"status":"error","reason":"failed (-22)"}

The last one is still a refusal and should be: nothing on this family has an
auto/manual op_type, `has_auto` is false, and the console no longer offers the
button that would send it.

### The same crack, two more knobs

`max_again` and `max_dgain` are also absent from `imp_knob_caps[]`, and their
schema rows say 0..160. Before anything in this session was touched, the board
reported a value above that ceiling, and writes above it are accepted:

    "max_again":205                      <- read before any write
    # raptorctl rvd set-max-again 217
    {"status":"ok"}

A value outside the slider's own maximum, and a maximum the hardware does not
agree with. (A freshly started rvd settles at 160, so the schema figure is at
least a plausible default -- it is the ceiling that is wrong, not the shape.)

These two escape the auto-button half of the bug only because they use `LIVE()`
rather than `LIVE_ISP()`, so `auto_ok` is false — luck, not design. Not fixed
here: unlike ae_comp their real ceiling was never bisected, and `rvd`'s
`get-isp` does not emit caps for them at all, so a HAL row alone would not
reach a client. Left as measured, not as guessed.

---

## 3: the IR-cut filter never moves, because the pulse is too short

### What you see

`ric` reports day mode and the picture is heavily red — the signature of an
IR-cut filter that is not in the path.

    # raptorctl ric status
    {"status":"ok","mode":"auto","state":"day", ...}

Driving the filter by hand changes nothing. Whole-frame channel means from
`rtsp://…/stream0`, six seconds after each command, ISP mode untouched
throughout (the `ircut` verb moves the rail alone):

    baseline      R/G=1.958  B/G=1.056
    ircut night   R/G=1.985  B/G=1.037
    ircut day     R/G=2.068  B/G=1.015
    ircut night   R/G=2.056  B/G=1.032
    ircut day     R/G=2.014  B/G=1.062

No separation. R/G near 2.0 in daylight is IR flooding all three channels with
red worst — the filter is out and staying out.

### The cause

This board has a motor-driven (H-bridge) filter: `gpio_ircut = 52`,
`gpio_ircut2 = 53`. `ric_ircut_drive` pulses one rail high, waits `pulse_ms`,
then parks both low. The default is 10 ms, and `ric_main.c:135` explains it:

> Dual-GPIO coil pulse. 10ms is what the thingino ircut script has … so the
> default follows the fleet.

10 ms is not enough to throw this camera's coil. Same protocol, with `pulse_ms`
raised through `raptorctl ric set-threshold`, each attempt starting from the
opposite position asserted with a known-good 250 ms pulse:

    pulse=10   R/G=1.997  B/G=0.981   filter did not move
    pulse=12   R/G=1.001  B/G=0.978   moved
    pulse=15   R/G=1.000  B/G=0.977   moved
    pulse=18   R/G=0.988  B/G=0.984   moved
    pulse=20   R/G=1.002  B/G=0.975   moved
    pulse=30   R/G=0.993  B/G=0.973   moved
    pulse=50   R/G=0.994  B/G=0.977   moved
    pulse=75   R/G=1.002  B/G=0.972   moved
    pulse=250  R/G=0.994  B/G=0.982   moved

R/G moves from 2.0 to 1.0 the moment the pulse is long enough. That is the
filter, and nothing else on this camera does that to the colour.

**The default is marginal rather than simply wrong**, which is worse. Twelve
cycles at 10 ms, each starting from a night position asserted with a known-good
250 ms pulse, interleaved in the same session with a control run at 30 ms:

    pulse=10ms   9 miss / 12 cycles
    pulse=30ms   0 miss /  8 cycles

The individual 10 ms landings were 1.859, 1.890, 2.138, 2.190, 1.940, 1.783,
**0.979**, **0.985**, 1.842, 1.923, **0.979**, 1.895 -- three successes
scattered through nine failures, in no pattern. When it fails the camera sits
in the wrong filter position until the next automatic transition, which under
`trigger = luma` may be hours. That is exactly the shape of a fault nobody can
reproduce on demand.

### The awkward part: this was measured before, and passed

`9679136` lowered the default from 100 ms, and did not do it carelessly:

> thingino's ircut script has driven every dual-GPIO board at 10ms since
> thingino-daynight existed, and exposes the same knob as `IRCUT_PULSE_US`. A
> report against the packaged raptor (thingino-firmware #1380, WLTB-Gino) asked
> for the 10ms value after unreliable filter movement on a Wyze Cam3.
>
> Measured on a dual-GPIO Wyze Cam3 before changing anything: 20 cycles at
> 100ms and 20 at 10ms, every landing verified by AE signature and snapshot,
> zero misses either way.

Same camera model, same pulse width, forty cycles, clean. This unit misses
three quarters of the time. Both measurements are sound and they disagree,
which means the conclusion is not "10 ms is wrong" but **10 ms has no margin,
and how much margin a unit needs is not constant across units of the same
model** -- coil tolerance, supply, temperature, stiction on an older mechanism.
The cliff on this one sits between 10 and 12 ms; on gtxaspec's it sits below
10. A default parked on top of a cliff passes wherever the cliff is lower.

Note also that the report the change was made *for* was itself about unreliable
filter movement, and asked for 10 ms because that is what the rest of the fleet
used -- alignment, not a measurement that 10 ms had headroom.

### Is it the platform, not the unit?

Reasonable hypothesis, since gtxaspec's clean run and this one were not on the
same image: thingino and OpenIPC differ in kernel and toolchain, and the pulse
is a `usleep` between two sysfs writes. If this board's `usleep(10000)` were
delivering less than 10 ms, the unit would be exonerated and the default would
be fine.

It is not. Timed on the board with the same open/write/close sequence
`ric_ircut_drive` uses, the energised window at `pulse_ms = 10`:

    energised=10.375 ms   (usleep took 10.068; writes 0.163 before, 0.145 after)
    energised=10.367 ms   (usleep took 10.071; writes 0.187 before, 0.109 after)
    energised=10.435 ms   (usleep took 10.100; writes 0.143 before, 0.192 after)
    energised=10.453 ms   (usleep took 10.087; writes 0.227 before, 0.139 after)
    energised=10.347 ms   (usleep took 10.073; writes 0.183 before, 0.091 after)

and no tick rounding at any width the knob can ask for:

    pulse_ms =  1  ->  1.07 ms
    pulse_ms =  5  ->  5.07 ms
    pulse_ms = 12  -> 12.09 ms
    pulse_ms = 30  -> 30.08 ms

`/proc/timer_list` reports `.resolution: 1 nsecs`, so high-resolution timers
are on and `usleep` is not being rounded up to a jiffy. Each sysfs write costs
0.09-0.23 ms, which lengthens the window rather than shortening it. This board
asks for 10 ms and the coil gets 10.4.

The toolchain is not a candidate either: `usleep` is a libc call onto
`nanosleep`, and no amount of codegen difference changes how long the kernel
sleeps. (The C library does differ -- this image is uClibc -- but the measured
result settles it regardless of which one is in the path.)

What that does *not* rule out is the mirror image: a kernel that rounds
`usleep(10000)` **up** would hand the coil ~20 ms while the config still said
10, which is above this unit's 12 ms cliff and would reconcile both
measurements at a stroke. Testing that needs a thingino board and
`/proc/timer_list` on it; if its resolution is a jiffy rather than a
nanosecond, the fleet's "10 ms" has never been 10 ms and the whole
disagreement is explained. Worth ten minutes on the bench camera before
assuming unit variance.

Either way the conclusion for the default is the same: on hardware that does
deliver exactly 10 ms, 10 ms is not enough here.

### Recommendation

**Raise the shipped default to 30 ms**, and treat the number as a margin rather
than a target. Done — `ric_main.c` and the config template both carry 30, and
the harness now asserts it by asking ric what it loaded rather than by timing
a pulse it cannot resolve.

The reasoning, given that 10 ms was a deliberate and measured decision:

- It is not a return to 100 ms. The two arguments in `9679136` were fleet
  alignment and not holding the coil ten times longer than necessary; 30 ms
  keeps most of the second and gives up only part of the first.
- 12 ms worked every time here and is the wrong answer: a default one
  millisecond above a measured cliff is the same bug with a smaller margin.
  30 ms is three times the cliff on this unit and still three times under the
  old default.
- The cost is a `usleep` on a transition that happens about twice a day. There
  is nothing to trade against it -- no stream interruption, no current draw
  worth naming at these durations.
- `pulse_ms` is already there, already clamped to 1..1000, for a mechanism that
  disagrees in either direction. Someone who wants the fleet's 10 ms still has
  it.

What would settle it properly is `/proc/timer_list` on a thingino board (see
above) and then a second and third unit. Two Wyze Cam3s disagreeing by more
than 3x on the same width is only the finding if both were really getting the
same width.

Worth considering alongside a bigger number: **the transition has no
confirmation**. ric drives the coil and assumes it landed. It already reads AE
every poll, and a day transition that did not move the filter shows up in the
exposure within a second or two -- the same R/G signature this investigation
used. A transition that re-pulsed once when the picture did not change would
make the width a performance question rather than a correctness one. That is a
larger change and belongs in its own discussion, not smuggled into a default.

Two smaller things found alongside:

- **The log cannot tell the two directions apart.** Both branches log the
  parked state, not the pulse, so a day transition and a night transition print
  the same line:

      ric_daynight.c:286: ircut: gpio 52=0, gpio 53=0 (night)
      ric_daynight.c:300: ircut: gpio 52=0, gpio 53=0 (day)

  Only the trailing word differs. Logging the rail that was actually driven,
  and for how long, is what this investigation wanted and did not have.

- **`ric set-threshold pulse_ms` does not persist.** It changes the running
  value and logs `threshold pulse_ms set to 100`, but `/etc/raptor.conf` still
  carries only the commented-out default, so a restart silently returns to
  10 ms. Worth checking against the other thresholds — if they do persist, this
  one is an omission rather than a policy.

---

## 4: a rejected rate-control change kills the channel and wedges the restart

The most serious of the four. It has two halves: the change never works, and
failing at it leaves the encoder unusable.

### Every mode change fails

On stream1 (H.264, 640x360) and stream0 (H.265, 1920x1080), both starting from
`cbr`:

    chn1 cbr             -> {"status":"ok","rc_mode":"cbr"}                stream ALIVE
    chn1 vbr             -> {"status":"error","rc_mode":"vbr"}             stream DEAD
    chn1 capped_vbr      -> {"status":"error","rc_mode":"capped_vbr"}      stream DEAD
    chn1 capped_quality  -> {"status":"error","rc_mode":"capped_quality"}  stream DEAD
    chn1 fixqp           -> {"status":"error","rc_mode":"fixqp"}           stream DEAD
    chn1 smart           -> {"status":"error","rc_mode":"smart"}           stream DEAD
    chn0 vbr             -> {"status":"error","rc_mode":"vbr"}             stream DEAD
    chn0 smart           -> {"status":"error","rc_mode":"smart"}           stream DEAD

Only setting the mode it already has succeeds. Every actual change is refused,
in both codecs, and every refusal kills the stream. `rvd` restarted between
each row.

### Why the SDK refuses: a union read as one mode and written as another

`IMPEncoderAttrRcMode` in `ingenic-headers/T31/1.1.6/en/imp/imp_encoder.h` is a
tag plus a **union**:

    typedef struct {
      IMPEncoderRcMode rcMode;
      union {
        IMPEncoderAttrFixQP         attrFixQp;
        IMPEncoderAttrCbr           attrCbr;
        IMPEncoderAttrVbr           attrVbr;
        IMPEncoderAttrCappedVbr     attrCappedVbr;
        IMPEncoderAttrCappedQuality attrCappedQuality;
      };
    } IMPEncoderAttrRcMode;

and the arms do not line up. CBR has no `uMaxBitRate`, so every field after the
target bitrate sits one `uint32_t` earlier than VBR's:

    IMPEncoderAttrCbr           IMPEncoderAttrVbr
      uTargetBitRate  u32         uTargetBitRate  u32
      iInitialQP      i16         uMaxBitRate     u32
      iMinQP          i16         iInitialQP      i16
      iMaxQP          i16         iMinQP          i16
      iIPDelta        i16         iMaxQP          i16
      iPBDelta        i16         iIPDelta        i16
      eRcOptions      u32         iPBDelta        i16
      uMaxPictureSize u32         eRcOptions      u32
                                  uMaxPictureSize u32

`hal_enc_set_rc_mode` reads the current attributes, overwrites `rcMode`, and
then patches the union **through the arm of the target mode** — while the bytes
in it were written by the SDK through the arm of the *current* one:

    int ret = IMP_Encoder_GetChnAttrRcMode(chn, &rcAttr);   /* filled as CBR */
    rcAttr.rcMode = vendor_mode;                            /* now says VBR */
    case IMP_ENC_RC_MODE_VBR:
        rcAttr.attrVbr.uTargetBitRate = bitrate_kbps;
        rcAttr.attrVbr.uMaxBitRate = bitrate_kbps * 4 / 3;
        rcAttr.attrVbr.uMaxPictureSize = bitrate_kbps;
        if (rcAttr.attrVbr.iMaxQP == 0) rcAttr.attrVbr.iMaxQP = 45;
        if (rcAttr.attrVbr.iMinQP == 0) rcAttr.attrVbr.iMinQP = 20;

Three fields are overwritten unconditionally, so those are fine. The rest are
CBR's bytes reinterpreted:

| VBR field | receives | patched? |
|---|---|---|
| `uTargetBitRate` | — | yes |
| `uMaxBitRate` | CBR `iInitialQP`+`iMinQP` as one u32 | yes |
| `iInitialQP` | CBR `iMaxQP` | no |
| `iMinQP` | CBR `iIPDelta` | only if zero |
| `iMaxQP` | CBR `iPBDelta` | only if zero |
| `iIPDelta` | CBR's alignment padding before `eRcOptions` | no |
| `iPBDelta` | low half of CBR `eRcOptions` | no |
| `eRcOptions` | CBR `uMaxPictureSize` | no |
| `uMaxPictureSize` | past the end of the CBR arm — union tail | yes |

(CBR is 24 bytes, VBR 28; the offsets above are the natural MIPS layout, `i16`
fields packed and `eRcOptions` realigned to 4.)

The `if (… == 0)` guards are precisely the wrong test. These fields are never
zero — they hold *another field's value*, and an I/P delta of -1 arriving as
`iMinQP` is a QP bound of -1, which the encoder is right to refuse. The kernel
says so:

    rvd[1865]: [ERROR] hal_encoder.c:1224: SetChnAttrRcMode(1, mode=2) failed: -1
    kernel: Encoder: Codec_Encode_SetRcParam faied

Channel *creation* is unaffected and that is the tell: `hal_enc_create_chn`
builds the whole struct from zeroed memory, which is why the mode named in
`raptor.conf` works and only the live change does not.

The correct pattern is already in this file, four hundred lines down.
`hal_enc_set_qp_ip_delta` switches on `rcAttr.rcMode` — the mode the bytes were
written as — and touches only that arm. `hal_enc_set_rc_mode` switches on the
target. That is the whole difference.

### The fix

The arm is built from nothing. `hal_enc_set_rc_mode` no longer reads the
channel's current rate-control attributes at all — a value that exists in both
arms means the same thing only by coincidence, and one that exists in neither
is not preserved by copying it.

On the new SDK the seed comes from `IMP_Encoder_SetDefaultParam`, the same
initialiser channel creation uses, told the channel's own geometry read back
with `IMP_Encoder_GetChnAttr`. That matters more than it looks: the real
`SetDefaultParam` fills fields this header gives no meaning to, so seeding from
it is the only way to be sure the arm is whole rather than whole as far as we
can see. The QP bounds and picture caps are then the same defaults creation
applies, unconditionally — there is no caller value left in the arm to
preserve, so the `if (… == 0)` guards are gone with the bytes that motivated
them. The hybrid (T32) and old (T10/T20/T21/T23/T30) branches get the same
treatment field by field, mirroring their own creation paths; they carry the
identical hazard and could not be tested on hardware here.

The QP bounds a caller configured live in rvd's stream config, not in the SDK,
and reach the encoder through `set-qp-bounds` and through the next channel
creation. Nothing that was reachable before is lost.

On the board afterwards, every mode on both codecs, `rvd` restarted between
none of them:

    chn1 vbr             -> {"status":"ok","rc_mode":"vbr"}                stream ALIVE
    chn1 capped_vbr      -> {"status":"ok","rc_mode":"capped_vbr"}         stream ALIVE
    chn1 capped_quality  -> {"status":"ok","rc_mode":"capped_quality"}     stream ALIVE
    chn1 fixqp           -> {"status":"ok","rc_mode":"fixqp"}              stream ALIVE
    chn1 cbr             -> {"status":"ok","rc_mode":"cbr"}                stream ALIVE
    chn0 vbr             -> {"status":"ok","rc_mode":"vbr"}                stream ALIVE
    chn0 smart           -> {"status":"ok","rc_mode":"smart"}              stream ALIVE
    chn0 cbr             -> {"status":"ok","rc_mode":"cbr"}                stream ALIVE

and `raptorctl rvd restart` completes in the usual fourteen seconds afterwards,
which it could not do before.

### Pinned by a test, and the test was checked against the bug

`raptor-hal/tests/t_enc_imp.c` is new and exists for this one claim. It
compiles the shipping `hal_encoder.c` against T31's headers, fakes
`GetChnAttr` to describe a CBR channel holding exactly the poison — `iIPDelta`
and `iPBDelta` of -1 — and asserts what reaches `SetChnAttrRcMode`.

`IMP_Encoder_GetChnAttrRcMode`, the call that read the current arm, is left in
the abort stubs deliberately: reaching for the channel's existing attributes
again kills the suite rather than failing an assertion.

A test that passes against both the old and new code would be worth nothing, so
it was run against the unfixed encoder. It aborts immediately on
`GetChnAttrRcMode`; given a fake for that call so the value assertions can be
reached too, it produces the bug in full:

    FAIL test_a_vbr_switch_does_not_inherit_cbrs_bytes: iMinQP inherited CBR's iIPDelta
    FAIL test_a_vbr_switch_does_not_inherit_cbrs_bytes: iMinQP is the mode's own default, got -1
    FAIL test_a_vbr_switch_does_not_inherit_cbrs_bytes: iMaxQP is the mode's own default, got -1
    FAIL test_fixqp_carries_a_qp_and_no_bitrate: iInitialQP 3000 is a QP

The last line is the clearest statement of the whole bug: CBR's target bitrate,
3000 kbps, arriving as FIXQP's initial quantiser.

### Why it wedges

A refused change is not a no-op. Within ten seconds the frame loop reports the
channel gone — and takes its sibling with it:

    rvd[1865]: [WARN ] rvd_frame_loop.c:201: stream3: enc_poll failing (chn 3, last=-1)
    rvd[1865]: [WARN ] rvd_frame_loop.c:201: stream1: enc_poll failing (chn 1, last=-1)

chn 3 is the JPEG channel at the same resolution; the failed call on chn 1 took
it down too. `rsd` then reconnects its ring reader every two seconds forever.

rvd itself keeps answering — `raptorctl rvd status` still returns
`"ready":true` — which is exactly why this reads as a wedge rather than a
crash. But it can no longer be restarted:

    # raptorctl rvd restart
    restarting
    Failed to send to rvd: connection failed

All four encoder threads exit cleanly, and then teardown never finishes:

    rvd[1865]: [INFO ] rvd_frame_loop.c:395: encoder thread[0] exiting
    rvd[1865]: [INFO ] rvd_osd.c:997: osd update thread exiting
    rvd[1865]: [INFO ] rvd_frame_loop.c:395: encoder thread[2] exiting
    rvd[1865]: [INFO ] rvd_frame_loop.c:395: encoder thread[3] exiting
    rvd[1865]: [INFO ] rvd_frame_loop.c:395: encoder thread[1] exiting
    ric[884]:  [WARN ] ric_daynight.c:148: rvd has not answered for 10s -- day/night is frozen in day mode

Eighteen threads remain, all of them the vendor's: `group_update` x6,
`ENC(0..3)-update_f`, `isp_tuning_deam`, `shm_thread`. The blocker is visible in
procfs — with no gdb on this board, `/proc/PID/task/TID/wchan` is the debugger:

    tid 1865 (rvd)              wchan: hrtimer_nanosleep
    tid 1873 (rvd)              wchan: avpu_codec_ioctl     <- stuck in the codec driver
    tid 1872 (shm_thread)       wchan: futex_wait_queue_me
    tid 1874 (isp_tuning_deam)  wchan: hrtimer_nanosleep

A thread parked in `avpu_codec_ioctl` that never returns, with the main thread
spinning in a sleep loop inside libimp waiting for it. `IMP_Encoder_DestroyChn`
cannot complete, so rvd cannot exit.

### Recovery

Better than feared — **no reboot needed**:

    kill -9 $(pidof rvd)      # the process does die; it is S, not D
    raptorctl rvd start       # comes back clean, both streams alive

Verified across eight forced failures in this session. The SDK state survives
the kill, which is worth knowing: the wedge is rvd's teardown path, not the
driver permanently.

### Still worth doing

One thing the header settles and this change does not: `SetChnAttrRcMode` is
documented as supporting `FIXQP, CBR, VBR, CAPPED_QUALITY`, and `CAPPED_VBR` is
not on that list — yet `hal_translate_rc_mode` maps both `RSS_RC_SMART` and
`RSS_RC_CAPPED_VBR` onto it, and `get-enc-caps` on this board already reports
`smart_rc: false`. It is accepted in practice on this part; the mapping is
worth revisiting on its own.

And the second half of the original failure is untouched, because the union fix
removes the cause rather than the consequence. A refusal from this SDK is still
not transactional — the kernel had already touched the codec when it printed
`Codec_Encode_SetRcParam faied` — so a future refusal from some other cause
would wedge the same way:

- **A failed `set-rc-mode` should restart the channel, not leave it dead.** rvd
  has `stream-restart` already; a refusal known to poison the channel should
  use it rather than returning `{"status":"error"}` over a stream that has
  silently stopped.
- **Teardown needs a deadline.** rvd trusts `IMP_Encoder_DestroyChn` to return.
  When it does not, the daemon is unkillable by its own restart path and only
  an out-of-band `kill -9` recovers it — which no supervisor and no console
  button is going to do. A bounded wait followed by `_exit()` turns an
  unrecoverable wedge into a restart.

---

## Reproducing

Board at 192.168.1.108, root. Colour measurements are whole-frame channel means
from a single RTSP frame downsampled to 160x90, six seconds after the command,
via `ffmpeg -rtsp_transport tcp -i rtsp://192.168.1.108:554/stream0 -frames:v 1`.
Relative channel ratios only — absolute levels move with AE and say nothing.

The board was left running the fixed `rvd` (md5 `9c7f225c`, T31 uClibc, built
2026-08-28) with `pulse_ms = 100` set live so the IR-cut filter actually moves.
That does not persist, so a restart returns it to the marginal 10 ms until the
default changes. Everything else is as found: ae_comp live at 128 with no key
in the file, `max_again` back at its fresh-start 160, both streams `cbr`,
`ric` on `auto`, all
daemons in their prior state. The stock binary is at `/tmp/rvd.orig`.

One thing noticed on the way out and not chased: resetting an `[image]` key
triggers an apply that tries to restart `rad`, which is deliberately stopped on
this camera (`[audio] enabled = false`), and the reset returns
`"apply_error":"rad did not come back within 26 s"` having otherwise
succeeded. Two smaller ones in the same bracket: `set-rc-mode` with an explicit
bitrate does not update `enc_cfg.bitrate`, so `get-bitrate` afterwards reports
the old figure; and `rc_mode` was not written to `/etc/raptor.conf` by a live
change, so it does not survive an rvd restart.
