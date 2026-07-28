# SigmaStar (Infinity6E / SSC30KQ) backend

Status of the `PLATFORM=INFINITY6E` HAL backend: what works, what is
deliberately absent, and what has not been tested. Brought up on an SSC30KQ +
GC4653 board running OpenIPC.

This is an index, not the reference. Every "deliberately absent" decision is
argued at the point it applies, in an `OP COVERAGE` comment block at the top of
the relevant file — `hal_isp.c`, `hal_osd.c`, `hal_audio.c`, `hal_encoder.c`.
Read those before implementing anything listed here as missing; several are
absent because the obvious implementation would be wrong, not because nobody
got to them.

## Working, board-verified

| Area | State |
|---|---|
| Video | H.264 and H.265, main + sub stream, over RTSP |
| Colour | Natural colour including working AWB |
| Orientation | hflip / vflip |
| Audio | Capture via `rad` |
| OSD | Text and image overlays through `rod` → SHM → `MI_RGN` |
| Day/night | IR-cut filter switching from the ISP's own AE |
| Exposure readback | `isp_get_exposure` — shutter, gain, AE scene luma |

## Deliberately absent

These are decisions, not gaps. Each returns `RSS_ERR_NOTSUP` through
`RSS_HAL_CALL`'s NULL guard, and rvd treats all of them as advisory.

- **Ring reference mode (zero-copy).** `enc_get_rmem_info` and
  `enc_inject_stream_shm` are unimplemented, and must stay that way until
  somebody solves buffer lifetime. MI recycles one stream buffer as soon as
  `MI_VENC_ReleaseStream` returns — three consecutive frames of 61634, 2582
  and 4073 bytes were seen landing at the same address (`0xb607c000`, phys
  `0x302c3000`). A published reference would be read back as whatever the next
  frame overwrote. rvd falls back to copying when `ref_base` is zero, which is
  what a NOTSUP `enc_get_rmem_info` produces. See `hal_encoder.c`.
- **IVS / motion detection.** No ops implemented, so rvd never sets
  `ivs_active` and the stage cannot appear in the pipeline.
- **Scalar ISP strength knobs that MI models as toggles or curves.**
  `isp_set_dpc_strength` and `isp_set_defog_strength` map onto single-bit MI
  fields; `isp_set_drc_strength`, `isp_set_highlight_depress` and
  `isp_set_backlight_comp` live in WDR/HDR modules whose manual blocks are
  multi-field curve descriptors; `isp_set_hue` is a 64-entry HSV LUT. Mapping
  one scalar onto a curve is a tuning decision, not a HAL one. See
  `hal_isp.c`.
- **`isp_set_wb` / `isp_get_wb`.** `AWB_SetAttr`'s 1464-byte payload does not
  follow the auto/manual offset convention the rest of the backend derives
  from, so it needs its own derivation. Deferred rather than guessed. (AWB
  itself works — this is only the manual override.)
- **`isp_set_bypass`.** No MI equivalent; the ISP cannot be bypassed while VPE
  is the only path to the encoder.
- **`hal_ircut_set`.** A stub on every platform, by design. Pin numbers,
  polarity, H-bridge-versus-single and pulse width are board configuration
  that the HAL has no way to acquire. `ric` owns the filter, driven from
  `[ircut]` in the config.

## Untested

Nothing below is known broken. It is unverified, which is not the same thing.

- **MJPEG.** Never exercised.
- **JPEG snapshots on their own VPE port.** The bind is new (2026-07-28,
  see below) and has not run on hardware yet. `logread | grep 'bind: VPE
  port'` must show four binds, not two.
- **`trigger = adc`** (photoresistor via SAR). The bring-up board has no
  photoresistor, and `CONFIG_MS_SAR` is off in its kernel, so there is no
  device to open either. The code path is inherited, not new.
- **`trigger = gpio`** (digital photosensor). Written for this backend but the
  board has no sensor fitted. The sysfs plumbing is the same as the IR-cut
  pins, which are verified; what is untested is the polarity and the sensor.
- **`trigger = photo`** (photometric state machine). Needs `ev`, which this
  platform does not report; `ric` warns once and falls back to the luma
  trigger.
- **The AE statistics lane order.** See below.

## JPEG snapshots need a VPE port of their own

rvd models a JPEG stream the way IMP does: it skips the bind chain
(`if (!s->is_jpeg)`, `rvd_pipeline.c`) and instead registers the JPEG
encoder channel into its paired video stream's encoder *group*, which is
already bound to the framesource. On Ingenic that is a real attachment —
`IMP_Encoder_RegisterChn` — and frames reach both registered channels.

MI has no encoder groups, so `hal_enc_register_channel` used to be a
validity check and nothing more, and the JPEG channel was left connected
to nothing at all. `MI_VENC_CreateChn` ran, `MI_VENC_StartRecvPic` ran,
and no VPE port was ever bound to it, so `enc_poll` timed out forever —
silently, because rvd treats a JPEG poll timeout as the expected "sensor
idle" case. `/snap` returned `No snapshot available yet` with a healthy
ring and a working H.264 stream. Fixed 2026-07-28.

The confirmation is one line, and it is worth keeping as the check:

```
# logread | grep 'bind: VPE port'
```

A pipeline with two video streams and JPEG on both must show **four**
binds. Two means the JPEG channels are floating.

Each JPEG channel now gets its own VPE output port, cloned from the
geometry of the port feeding its paired video stream. Sharing the video
stream's port would have been fewer lines, and the vendor rules it out
twice:

- `MI_SYS_BindChnPort2`'s Note: *"The source and destination ports must
  not have been previously bound"* — the source, not just the pair.
- MI_SYS §1.4.1 on the module structure: VPE *"has one InputPort and
  multiple OutputPorts ... Vpe shares the same data source, but must have
  different output formats of different specifications."* One output port
  per consumer is the model, not an optimisation of it.

The dedicated port is also what paces the channel. `MI_SYS_BindChnPort2`
takes separate source and destination frame rates, so the snapshot port
is bound at the JPEG stream's fps against a source running at the
sensor's, and MI drops the other 29 of every 30 frames in hardware. That
matters because `INFINITY6E`'s caps block does not set `.jpeg_pulse`, so
rvd's duty-cycling — the thing that stops an Ingenic JPEG channel
encoding at full rate and discarding almost all of it — is off here. On a
shared port nothing would pace the channel at all.

Two consequences worth knowing:

- **Snapshots carry no OSD.** `MI_RGN_AttachToChn` attaches a region to a
  *VPE port*, and the snapshot port is not the one rvd's overlays were
  attached to. On Ingenic the JPEG channel rides the group after the OSD
  stage, so it does get the burnt-in overlay. Closing this means
  attaching the paired group's regions to the snapshot port as well;
  it is a known difference, not an oversight.
- **Port budget.** `STAR_VPE_PORT_NUM` is 4 and a snapshot costs one, so
  two video streams plus two snapshots is exactly the board's capacity.
  Beyond that the register warns and the stream loses snapshots rather
  than failing — a board that cannot spare a port should lose its
  snapshots, not its video. Every such exit says so in the log.

## Derived ABI, and the evidence for it

Several structures are not in any header and were derived by disassembling the
board's own vendor libraries. Each is recorded next to its declaration with the
disassembly quoted. Two came out of `libmi_isp.so` and are in `i6_isp.h`:

- **`MI_ISP_CUS3A_GetAeStatus`** declares a **65-byte** payload (`movs r3,#65`,
  command `0x2e05`). The widely-copied waybeam definition is a 48-byte struct
  *on the stack*, so the vendor library overruns it by 17 bytes on every call.
  Field offsets are waybeam's and verified; the size is ours.
- **`MI_ISP_AE_GetAeHwAvgStats`** declares **46088 = 128 × 90 × 4 + 8**
  (command `0x2e01`), and the adjacent `MI_ISP_AWB_GetAwbHwAvgStats` declares
  **34568 = 128 × 90 × 3 + 8**. Two calls agreeing on a 128×90 grid at one byte
  per channel fixes the cell width at four bytes, which rules out waybeam's
  `short r, g, b, y` (that needs 92160) — so their `avgY` log line averages two
  cells per sample.

The same technique settled a live bug in `libmi_vif.so`, fixed 2026-07-28 and
recorded in `i6_vif.h`:

- **`MI_VIF_SetDevAttr`** copies **52 bytes** out of the pointer it is given
  (three 16-byte block copies plus a trailing word) and sends an ioctl whose
  payload length is hardcoded to 56 — four bytes of device id plus those 52.
  `i6_vif_dev` was **48**, missing the vendor's trailing `u32MultiDevMap`, so
  every call passed a word of `star_vif_bringup`'s own stack frame to the driver
  as a device bitmap. Now declared, asserted at 52, and set to 1 — board-tested.
- **`MI_VIF_SetChnPortAttr`** copies 32 with a 40-byte payload, and
  `i6_vif_port` was already 32. Asserted so it stays that way.

That bug is a good argument for these asserts existing at all. The bad read is
deterministic for a given binary, so it hid for months: OpenIPC's stack layout
happened to leave `1` in that word, which is exactly what the driver wants.
Rebuilding the same source under a Buildroot that defaults to
`-fstack-protector-strong` reordered the locals and left a fragment of the
sensor-name string there instead — `multidevmap 909402983`, which is
`0x36346367`, the bytes `gc46`. VIF then never synced: dmesg looped on
`_MI_VIF_EnqueueOutputTaskDev: layout type 2, bindmode 4 not sync err` and no
stream was ever produced, while the identical source built without hardening
streamed fine. Matching the vendor's copy size is the fix; matching one
compiler's stack layout is not.

Two red herrings from chasing that one, worth not repeating: the
`MI_VIF_IMPL_SetDevAttr ... not support` suffix is benign — a streaming camera
prints it too, and only the `multidevmap` value on that line differs — and
`Unhandled fault: external abort` lines on a thingino rootfs come from its
`/usr/sbin/soc` helper doing Ingenic `devmem` reads, nothing to do with raptor.

Where the sizes could not settle a question, the code refuses to guess. The
eight spare bytes could lead or trail, so `star_ae_luma` matches the grid
dimensions from AE status against **both** ends and reports **no luma at all**
if neither fits — averaging a guessed offset produces a number that looks like
a reading and moves the IR-cut filter. A board run settled it: the probe logged
`AE grid 32x32, cells at offset 8`, so the eight bytes lead and are the grid
dimensions themselves. 128×90 is the payload maximum, not the live grid.

**Still open:** the lane order within a cell is waybeam's word only. That run
returned `r=46 g=46 b=44 y=46` — consistent with `r,g,b,y`, but far too neutral
a scene to tell the lanes apart. A single log line from a strongly coloured
scene would settle it. Nothing depends on it: the lane used behaves like luma
and matches what `raptorctl ric status` reports.

## Zero means "not available"

A convention worth knowing before reading exposure code. `rss_exposure_t` is
zeroed when a reading is unavailable — no HAL support, or the ISP not yet
tuned. The Ingenic backend documented this first (a failed luma read leaves
`ae_luma` 0 so "day/night falls back to gain-only behavior"), but `ric`
honoured it on no platform: with `isp_get_exposure` absent, `ae_luma <
night_luma` evaluated `0 < 20` every poll and pinned the camera in night mode
forever. `ric` now gates each term on its field being reported and holds its
current mode when nothing is available. A live AE never reports a mean luma of
exactly 0, so treating 0 as silence costs nothing real.

`rod`'s `%ae_luma%`, `%total_gain%` and `%soc_temp%` follow the same rule and
render `--` rather than `0`.

## Calibration

`[ircut] night_luma` and `night_gain` ship at their Ingenic defaults, which are
**not** this platform's scale:

- `total_gain` is x1024 fixed point — sensor × ISP gain, 1024 = 1.0x. The
  inherited `night_gain = 80000` is about 78x.
- `ae_luma` is the mean of the AE grid's Y lane, 0–255 per cell — a different
  measurement from Ingenic's `GetAeLuma`.

### Something tears the tuning down after it loads

**Read this before believing any AE number from this platform.**
`sensor gain 1024..8192` with `shutter ..14000` are the limits the ISP reports
with **no tuning file loaded at all** — established on the board by watching a
deliberately failed load report exactly those figures. They are also where the
AE sat permanently, for a whole night, on a board whose tuning had loaded
successfully.

So the api bin lands and is then discarded. That it lands is not in doubt: two
different bins produce two different limit sets (`24576/100000` and
`131072/50000`), read back from the AE moments after the load. It simply does
not stay.

Every symptom that looked like a separate bug was this one:

- a night picture that was black apart from bright spots
- `total_gain` frozen at 8192 with the AE reporting `boundary=1`
- AE limits widened underneath the AE being ignored, then reverted
- an OEM `_night.bin` that forces monochrome under divinus doing nothing here
- deleting `gc4653.bin` changing nothing
- **auto white balance wrong under artificial light** — the symptom `19170e8`
  was written to fix by removing the 3A handoff, which was treating the wrong
  cause
- **`ae_comp`, and other `[image]` knobs, appearing not to work** — they are
  flushed *after* the tuning load, so the tear-down reverted them with it, and
  an AE already pinned against both its ceilings cannot act on a change of
  target luma anyway. They were written off as a vendor quirk for two days

The first and last of those are both **board-verified fixed, 2026-07-27**, on a
build whose repair fired once during bring-up: colour is correct under
artificial light, and ISP adjustments take effect. Two consequences. The
`RSS_ISP_3A_HANDOFF` hatch that existed to A/B the two 3A sequences once the
tear-down was dealt with has served its purpose and is **gone**, along with the
`MI_ISP_CUS3A_Enable` and `MI_ISP_DisableUserspace3A` bindings — which were
loaded as *hard requirements*, so a board whose `libmi_isp.so` lacked either
failed ISP init over symbols nothing called. And a knob that does not work on a
pinned control loop is evidence about the loop, not the knob.

What performs the tear-down is **not identified, and deliberately not chased
further**. The shape is known: CUS3A re-auto-starts when a VPE channel's last
output port goes down, and rvd tunes on the *first* framesource enable — before
the sub-stream, the OSD attach and the encoder start. divinus loads at the end
of `sdk_init` once its encoder thread is running (`media.c:827`) and is fine on
the same board and bin. Tuning before the pipeline has finished being built is
the leading explanation, and it also explains why AWB appeared to break
"randomly": it depends on how the port enables interleave.

Naming the exact step would mean bisecting vendor library behaviour that no
documentation describes, over board runs, and the answer would be worth only
this SDK build: any *other* step MI resets the ISP from — on another board, or
another vendor release — puts the tuning straight back where it started.
Detecting the state covers all of them, including the ones nobody has hit yet.
So the repair below is the answer here, not a placeholder for one.

`hal_isp.c` therefore **detects and repairs**: the tuning's own limits are
snapshotted before anything can overwrite them, so a later mismatch is positive
evidence the ISP reverted, and the bin is reloaded. Driven from
`star_isp_tune_when_ready` — i.e. from the pipeline being built, which is when
the tear-down happens — with the exposure-poll path as a backstop. Bounded at
five attempts, because a reload that does not stick must not loop, and the
attempt count is diagnostic: one means a single startup event, repeated means
something is doing it continuously.

```
isp: AE is on sensor gain ..8192 but the tuning has ..131072 -- the ISP was
     reset after the load; reloading /etc/sensors/gc4653.bin (attempt 1)
```

On the bring-up board that fires exactly once, right after the AE grid probe.

Two details that took a wrong turn to get right. A ceiling this config states is
*also* a legitimate reading, so the check accepts either it or the tuning's own
value as proof the tuning is in effect — the first version bailed out whenever
`max_again` was set, which quietly made that key a switch for turning the repair
off. And a reload replaces the ISP's state with the binary's, so the config
knobs go back on afterwards (`star_isp_flush_pending`), exactly as after the
first load.

The one blind spot: a `max_again` equal to the untuned default, 8192 here. A
reset then reads as the config being honoured. Left alone rather than
special-cased, because on this board the tuning's own ceiling is the one worth
having and the config says to leave `max_again` unset.

### The gain-limit units, and the ceiling that is not one

- **`max_again` / `max_dgain` are x1024 here, not Ingenic gain codes** (the
  vendor's own constant for a 32x cap is 32768), and rvd applies its Ingenic
  defaults — 160 and 80 — whether or not the config mentions the keys. Passed
  through unscaled those are ceilings of 0.16x and 0.08x: below unity, so not
  ceilings at all. The backend refuses any ceiling below unity, logs it once
  naming the unit mismatch, and leaves the tuning's value in charge.
- **The tuning's published ceiling is generous on this board** — `1024..131072`
  (128x) sensor gain, and `1024..1024` ISP gain, meaning no digital gain
  whatsoever. `max_dgain` is therefore inert here.
- **MI keeps its own ceiling when a higher one is written**, so a requested
  ceiling above the tuning's is clamped to it — an unexplained limit that did
  not take is much harder to spot than one that says it was clamped. waybeam hit
  this too: "isp.gainMax above the bin ceiling never stuck (found with
  gainMax=32000 vs bin 8192)" (`maruko_cus3a.c`). Note their 8192 is *their*
  board's bin, not this one's — the coincidence with the untuned default here
  cost several hours of misdiagnosis.
- **A ceiling the config states is re-asserted** every couple of seconds, since
  a tuning reload restores the binary's own values and would otherwise drop it.
  With nothing configured the AE's window is left alone — fighting an algorithm
  over numbers nobody asked for is how you get an AE that oscillates.
- **Neither key is worth setting on this board.** 128x is the widest it has, and
  anything set here only narrows it.

### `night_gain` is pinned, not calibrated

`night_gain` ships at **122880 (120x)**, near the top of what the AE can reach,
which the startup limits line reports as `total_gain tops out at 131072`.

Pinned rather than calibrated on purpose: "the AE has run out of gain" needs no
calibration and does not drift with the scene, the lens or the season, whereas a
dusk-reading threshold is only right for the dusk it was read at. The filter then
switches when the AE has spent everything and still cannot reach target — the
latest sensible moment, keeping colour as long as colour is possible. The cost is
that the last of the evening is a noisy colour picture at 120x; lower it (65536,
one stop) to switch earlier and get clean monochrome sooner.

Not `131072` itself, because the test is `total_gain > night_gain` and the AE
parks *exactly* on its ceiling in the dark: the ceiling as a threshold is a
trigger that never fires. The 120x value leaves slack for an AE that stops just
short.

Daylight reference from the bring-up board: `total_gain` 1026 (1.00x, full
headroom), `exposure_us` 9689, `ae_luma` 45.

`night_luma` stays a backstop for the case gain alone cannot cover — a bright
light in frame, where the AE holds gain low while the rest of the picture is
black. Note that **`ae_luma` is a post-AE measurement**: holding it near target
is the AE's whole job, so it sits around 45 in a lit room and a dim one alike and
only falls once the AE has spent its shutter and gain. Both values are readable
live via `raptorctl ric status`, or on the picture — see the `[osd.uptime]`
element in `config/raptor-ssc30kq.conf`.

## Building

`build-standalone.sh` does not support this platform and is not meant to: it
bootstraps a thingino mipsel toolchain release and an Ingenic SDK version map,
neither of which has a SigmaStar counterpart, and the MI libraries come off a
vendor SDK or a built image rather than a URL.

```sh
# Against an existing Buildroot output
./build.sh infinity6e /path/to/openipc-firmware/output

# Or build the whole image, which packages raptor itself
make BOARD=ssc30kq_raptor RAPTOR_SRCDIR=/path/to/raptor-repos
```

Host-side logic tests for the backend live in `raptor-hal/tests` (`make -C
tests test`): they `#include` the real translation units and stub MI through
the function pointers in `star_state_t`, which is possible because nothing in
`src/star` links `libmi_*.so` directly — every call goes through a `dlopen`'d
pointer.
