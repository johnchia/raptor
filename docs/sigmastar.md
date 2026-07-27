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

## Derived ABI, and the evidence for it

Two structures are not in any header and were derived by disassembling the
board's own `libmi_isp.so`. Both are recorded in `i6_isp.h` with the
disassembly quoted.

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

What performs the tear-down is **not yet identified**. The shape is known:
CUS3A re-auto-starts when a VPE channel's last output port goes down, and rvd
tunes on the *first* framesource enable — before the sub-stream, the OSD attach
and the encoder start. divinus loads at the end of `sdk_init` once its encoder
thread is running (`media.c:827`) and is fine on the same board and bin. Tuning
before the pipeline has finished being built is the leading explanation, and it
also explains why AWB appeared to break "randomly": it depends on how the port
enables interleave.

`hal_isp.c` therefore **detects and repairs** rather than assuming: the tuning's
own limits are snapshotted before anything can overwrite them, so a later
mismatch is positive evidence the ISP reverted, and the bin is reloaded. Driven
from `star_isp_tune_when_ready` — i.e. from the pipeline being built, which is
when the tear-down happens — with the exposure-poll path as a backstop. Bounded
at five attempts, because a reload that does not stick must not loop, and the
attempt count is diagnostic: one means a single startup event, repeated means
something is doing it continuously.

```
isp: AE is on sensor gain ..8192 but the tuning has ..131072 -- the ISP was
     reset after the load; reloading /etc/sensors/gc4653.bin (attempt 1)
```

On the bring-up board that fires exactly once. **This is a repair, not a fix.**
The fix is to tune where nothing later tears it down.

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
- **CUS3A also narrows the window while it runs**, so a ceiling the config
  states is re-asserted every couple of seconds. With nothing configured its
  window is left alone.

### Calibrating `night_gain` after all that

`night_gain` must sit under what the AE can actually reach, which is what the
startup limits line reports. The value currently in
`config/raptor-ssc30kq.conf` was chosen against the *untuned* 8x ceiling and
is **too low** now that tuning stays in effect; it needs redoing from a real
dusk reading.

Daylight reference from the bring-up board: `total_gain` 1026 (1.00x, full
headroom), `exposure_us` 9689, `ae_luma` 45.

Set thresholds from a dusk reading, and note that **`ae_luma` is a post-AE
measurement**: holding it near target is the AE's whole job, so it stays around
45 in a lit room and a dim one alike and only falls once the AE has spent its
shutter and gain. `night_luma` is a darkness backstop; `night_gain` is the
threshold that tracks failing light. Both are readable live via
`raptorctl ric status`, or on the picture — see the `[osd.uptime]` element in
`config/raptor-ssc30kq.conf`.

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
