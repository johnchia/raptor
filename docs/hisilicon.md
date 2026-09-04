# HiSilicon gen4 (Hi3516EV200 / EV300) backend

Status of the `PLATFORM=HI3516EV200` and `PLATFORM=HI3516EV300` HAL backend:
what works, what has not been written yet, and the handful of facts about this
silicon that are worth knowing before reading the code. Brought up on an
Hi3516EV300 + IMX335 board running OpenIPC (`hi3516ev300_lite`, 16 MB NOR).

**EV200 and EV300 are one backend, not two.** That is measured, not assumed: an
EV300 board reports the MPP version string `Hi3516EV200_MPP_V1.0.1.2 B030
Release` on every `/proc/umap` node. One MPP tree, one library set, one ABI.
Every guard in `src/hisi_v4/` is `HAL_HISI_GEN4` and never a part macro, so
adding Hi3516DV200 or Hi3518EV300 costs a word in `HISI_GEN4_PLATFORMS`, a
`caps_hisilicon.inc` block, and no code at all.

**Hi3516CV610 will not be a variant of this.** It is a V5 part with a different
MPI ABI behind identically-named symbols, and it gets `src/hisi_v5/` as a
sibling directory when a board exists. See `PLAN-hi3516ev200.md`, risk R12.

This is an index, not the reference. Each decision is argued where it applies,
in the file that makes it.

## Implemented

Written, building and **running on hardware**: an Hi3516EV300 with a 5 MP
IMX335 produces two H.264 streams, 1920x1080 and 640x360, that `ffprobe`
decodes off the shared-memory rings. A 90-second run held 24.9 fps against a
configured 25 on both, with no dropped or reset frames and no errors.

| Area | State |
|---|---|
| Library loading | `libsecurec` → VQE trio → `libmpi.so` or `libgk_api.so` + `libhi_mpi.so`, all `RTLD_LAZY \| RTLD_GLOBAL` |
| SYS / VB lifecycle | teardown-first, then `VB_SetConfig` → `VB_Init` → `SYS_Init`, unwound in exact reverse |
| Chip identification | SCSYSID0 at `0x12020EE0`, decoded to `Hi3516EV300` and friends |
| MPP version | `HI_MPI_SYS_GetVersion`, surfaced through `sys_get_version` and rvd's banner |
| Platform check | fatal on a non-gen4 part, warning on a different gen4 part |
| Media clock | `sys_get_timestamp` / `sys_rebase_timestamp`, so SEI timecodes work |
| Register access | `sys_read_reg32` / `sys_write_reg32` through `/dev/mem` |
| GPIO / IR-cut | `src/hal_gpio.c` verbatim — vendor-neutral sysfs |
| uClibc-ABI bridge | `__ctype_b`, `__fgetc_unlocked`, `_stdlib_mb_cur_max` exported from the executable |
| Goke sensor shims | the six `GK_API_*` forwarders eight of the 34 shipped sensor drivers need |
| Sensor discovery | `/etc/sensors/*.ini` — driver name, object symbol, lane map, RAW depth, Bayer order, VI device attribute. No sensor table in the code |
| MIPI receiver | `/dev/hi_mipi` ioctls in the vendor's own order, resets bracketing `SET_DEV_ATTR` |
| VI | device → pipe → channel, `VI_OFFLINE_VPSS_OFFLINE` — the mode the VI→VPSS bind actually describes; the samples' online modes carry nothing here |
| ISP | 3A registration, `MemInit`/`SetPubAttr`/`Init`, and the `HI_MPI_ISP_Run` thread |
| VPSS | one group with 3DNR on, channels as framesources |
| VENC | H.264, H.265, MJPEG (JPEG rides it — PT_JPEG refuses the attr set and divinus never uses it); CBR/VBR/FIXQP; bind, poll, collect, IDR, runtime bitrate/GOP/fps |
| Snapshots | duty-cycled MJPEG channel: receive and bind live in `enc_start`/`enc_stop`, because an idle-but-bound destination queues VPSS pictures until VB pool 0 is gone. Quality is the FIXQP qfactor via `enc_set_jpeg_qp` |
| Orientation | `hflip` / `vflip` as every VPSS channel's `bMirror` / `bFlip`: seeded from `[image]` before the channels are created and rewritten live by `isp_set_hflip` / `isp_set_vflip`. Not the sensor's `pfnMirrorFlip`, which the OpenIPC sensor libraries leave out, and not the VI channel's bits: VI mirror stalls the channel on the EV300 (flip alone streams) |
| Rotation | `[image] rotate` of 90, 180 or 270 through `HI_MPI_VPSS_SetChnRotation`, per VPSS channel. The channel keeps the configured size and emits it turned, so rvd creates the encoder (and the snapshot channel sharing the framesource) with width and height swapped for 90 and 270, and rod places the overlay on the turned picture |
| Exposure readback | `isp_get_exposure` from `HI_MPI_ISP_QueryExposureInfo`: exposure time in µs, the sensor's analogue and digital gains and the ISP's digital gain multiplied into one 1024-per-unit `total_gain`, and the AE's average luma. This is what the OSD's `%total_gain%` and `%ae_luma%`, the console's exposure and gain lines and ric's day/night decision read |
| Image knobs | `brightness`, `contrast` and `saturation` on the CSC (`ISP_CSC_ATTR_S` `u8Luma`/`u8Contr`/`u8Satu`, 0..100, 50 is unity). One attribute carries all three, so every write is a get-modify-set of all three, and the tuning file has no section for it -- 50 is what "as the tuning left it" means here. CSC saturation is a flat adjustment of the YUV the ISP emits and does not touch the Bayer-side `ISP_SATURATION_ATTR_S`, whose per-ISO table `[static_saturation]` sets or, where the file has none, the sensor library's own calibrated per-gain ladder does. The same attribute also carries `u8Hue` on the same scale, `bLimitedRangeEn` (full-range output by default), `enColorGamut` and a 3x3 `stCscMagtrx` with input and output DC offsets; raptor writes none of those. `ae_comp` on `stAuto.u8Compensation` (0..255) is the setpoint the AE converges its target luma on; nothing varies it with light or ISO, and no load writes it either -- `[static_ae]` has no `Compensation` key -- so its neutral is the AE library's own default, 56 here, learned at each load rather than assumed. Its caps therefore offer no `auto` -- there is no curve to hand the field back to, which is what `has_auto` asks everywhere in the HAL, and both SigmaStar ports and IMP answer the same way for this same knob. Reset is the affordance instead: `reset-isp` writes `caps.neutral` and drops the key from the file. The setter still accepts `RSS_ISP_AUTO`, so a config written when the button existed keeps loading. Contrast `drc_strength` below, which is the case `auto` is for; `drc_strength` pins `ISP_DRC_ATTR_S` in manual mode (0..1023) and holds the `[dynamic_linear_drc]` engine's column while pinned, `auto` handing it back -- and the caps offer `auto` only where that section gave the tuning a curve to hand back to, since a control that promises one varying with the light should not appear over a file with a single static strength. Every set is re-applied after each tuning load, because the load rewrites the attributes two of them live in. `isp_get_knob_caps` reports the units and neutrals; `raptorctl rvd get-isp` shows them |
| Sensor rate | `[sensor] fps` overrides the mode INI's `Isp_FrameRate`: `isp_set_sensor_fps` is a get-modify-set of the ISP public attribute's `f32FrameRate` on the running pipe (the sensor driver's fps callback reprograms VMAX from it), and `isp_get_sensor_fps` reads it back. The 5 MP IMX335 INI said 30; the EV300 encodes 5 MP at 20, and at 30 VI lost ~9% of frames to `VbFail` even with the stream at 25. OpenIPC's `imx335_i2c_5M.ini` now says 20 (and `raw_bitness=10`, which is what the sensor has always sent), so the override is a preference rather than a correction |
| Frame rate | VI needs seven VB blocks in pool 0 at 5 MP; with six it loses ~5% of frames to `VbFail` and the pipe drops to 25-26 fps, which VPSS's source/destination ratio then scales again — that is what made a request of 20 fps produce 17. With seven, VI holds 30 and 20 fps means 19.7. Watch `/proc/umap/vi`'s `LostFrame`/`VbFail`, not just `/proc/umap/vb` |
| ISP tuning | `/usr/share/raptor/iq/<sensor>.ini` (an override; raptor installs nothing there) or else the image's `/etc/sensors/iq/<sensor>.ini`, applied on the first encoded frame — the static sections (`static_saturation` included, through lib_hiawb's `HI_MPI_ISP_SetSaturationAttr`) via get-modify-set on `HI_MPI_ISP_Set*`, the dynamic ones (`dynamic_linear_drc`, `dynamic_dehaze`, `dynamic_gamma`) by engines that follow AE's ISO once a second (`hal_dyn.c`), and `static_3dnr` by handing the whole ladder to the VPSS driver to select from per frame, or the same once-a-second engine where it will not take one (`hal_nrx.c`). `$RSS_ISP_TUNING` overrides the path. See the three ISO sections below |
| OSD | RGN overlays, ARGB1555, attached to the *VENC* channel so each stream carries its own — the region record holds the channel, which is the fix for divinus's every-region-on-channel-0 defect. Registered and attached are separate states: rvd sets attributes before the channel exists, so attach is deferred to the bind, and destroy detaches first (HiMPP refuses to destroy a channel that still carries regions), so a region survives an encoder restart. Eight overlays per channel. No privacy cover: the vendor gives VENC no COVER budget, so `RSS_OSD_COVER` is `NOTSUP` and rvd degrades. `/proc/umap/rgn` shows what is attached where |
| Audio | one AI device against the inner codec: `HI_MPI_AI_*` for capture, `/dev/acodec` ioctls for volume/gain/mute. rad encodes in software; VQE/AENC/AO stay absent with reasons in `hal_audio.c`. Mono only: HiMPP returns stereo as two planes and the frame contract carries one, so `chn_count = 2` is refused with `RSS_ERR_NOTSUP` rather than delivering the left channel as if it were both. Mic gain clamps at 15 — the driver accepts 16 but that value falls out of the 4-bit field and all but mutes the preamp; `v4_aud.h` records the measurement. `/dev/acodec` is exclusive-open, so nothing else can inspect the codec while rad holds it. MPP is one system per SoC and rvd's init tears it down first, so restarting rvd destroys rad's AI device; rad notices after about a second of failed reads and reattaches on its own once rvd is back, about 17 s end to end |

### Encoder quality: QP bounds live in a second structure

`min_qp`/`max_qp` are not part of the channel attribute on gen4. They live in
`VENC_RC_PARAM_S`, written through `HI_MPI_VENC_SetRcParam` — a second call
against a second structure, which `hisi_enc_apply_rc_param` now makes after
`CreateChn` and after every attribute rewrite.

Until it did, the driver's own bounds applied: **24..51**, read back from the
board. Under those, raptor's I-frames ran away (1 MB each at `gop = 100`, against
a 625 KB whole-second budget at 5 Mbps/20 fps) and its P-frames collapsed to pay
for them, giving a picture that was *worst* at every keyframe and recovered over
the GOP. Setting the bounds removes that -- but see "a mitigation, not the root
cause" below before reading the wide bounds as the *reason*: majestic runs fine
on them.

Two things about the structure are worth knowing before touching it:

- **The union is not the same shape across codecs.** H.264 and H.265 agree on
  the CBR form, but their VBR forms differ: H.264 puts `bQpMapEn` at +16 where
  H.265 puts `u32MaxQp`, so the wrong form shifts all four QP bounds one word
  late *and* enables a QP map nobody asked for — and the call still returns
  success. `v4_venc.h` carries the probed offsets and one type per real shape.
- **It has to be read before it is written.** Three quarters of it is the
  macroblock texture thresholds, the row QP delta and the first-frame start QP,
  all driver-tuned. `hisi_enc_apply_rc_param` is a get-modify-set for that
  reason; building the struct fresh would silently retune macroblock-level rate
  control to zero.

Confirmed on the board through `/proc/umap/rc`, whose last three columns are the
current QP and the active bounds: `32 24 51` unset, `28 28 42` with
`min_qp = 28`/`max_qp = 42` configured.

Measured back to back on one scene, H.265 5 Mbps VBR at 2592x1944, `gop = 40`,
alternating so drift shows up as disagreement between the pairs:

| bounds | bitrate | I-frame acutance | GOP peak | keyframe sawtooth |
| --- | --- | --- | --- | --- |
| driver 24..51 | 5.14 Mbps | 12.04 | 18.19 | 1.79x |
| driver 24..51 | 4.64 Mbps | 11.74 | 18.77 | 1.97x |
| 28..42 | 2.69 Mbps | 17.95 | 18.09 | **1.00x** |
| 28..42 | 4.44 Mbps | 19.14 | 19.38 | **1.01x** |

1.01x is what majestic holds on the same board. Bitrate is not comparable across
the rows -- the two bounded captures are 45 minutes apart and the light moved --
but the sawtooth is a ratio taken inside each capture, so it is not affected.

The bounds also settle the second defect. Read straight off the ring with no
RTSP consumer attached, so nothing is requesting recovery keyframes, `gop = 40`
over 200 frames gives:

- **unbounded:** 8 keyframes, at 37,38 / 78,79 / 119,120 / 160,161 -- pairs one
  frame apart, both members real pictures.
- **bounded:** 5 keyframes, at 21 / 61 / 101 / 141 / 181 -- singles, exactly 40
  apart.

### The bounds are a mitigation, not the root cause

Running the same experiment on majestic settles what the numbers above do not.
Majestic was reconfigured to raptor's *unbounded* condition on the same board
and scene -- `minQp: 24`, `maxQp: 51`, and `maxIp` loosened from its shipped 2
to the driver's 20 -- and it did not care:

| majestic | minQp..maxQp | maxIp | bitrate | I-frame | peak | sawtooth | I-frames/20s |
| --- | --- | --- | --- | --- | --- | --- | --- |
| shipped | 28..42 | 2 | 5.37 Mbps | 16.88 | 17.34 | 1.02x | 11 |
| loose QP | 24..51 | 2 | 5.29 Mbps | 16.96 | 17.65 | 1.03x | 11 |
| loose both | 24..51 | 20 | 5.34 Mbps | 17.42 | 17.93 | 1.03x | 11 |
| shipped | 28..42 | 2 | 5.16 Mbps | 17.57 | 17.82 | 1.01x | 11 |

Flat at every setting, and never a duplicate keyframe -- 11 I-frames in 20 s at
`gop = 40`/20 fps is the expected 10, where unbounded raptor gave 19.

So the QP bounds are **not** why raptor sawtoothed. Raptor is sensitive to loose
bounds and majestic is not, which means something else in raptor's encoder setup
makes the controller unstable and the bounds merely constrain it enough that the
difference stops showing. Writing them is still correct -- a configured bound
that goes nowhere is its own bug, and the measured improvement is real -- but it
closed the symptom, not the cause.

This also rules out `u32MaxIprop` as the missing lever: majestic is flat at 20,
the same value raptor runs at.

### The cause was `stat_time`

`stat_time` is the rate controller's statistics window in seconds, in the
channel attribute rather than the RC parameters. `hisi_enc_fill_rc` wrote 1,
the vendor sample's value; majestic writes 4. Same board, same scene, bounds
left unwritten so the driver's 24..51 applies, `gop = 40` at 20 fps:

| `stat_time` | bounds | bitrate | I-frame | peak | sawtooth | I-frames/20 s | IDR pattern (ring, no consumer) |
|---|---|---|---|---|---|---|---|
| 1 | driver 24..51 | 5.12 Mbps | 12.14 | 17.43 | **1.64x** | 19 | pairs: 48,49 89,90 130,131 ... |
| 4 | driver 24..51 | 5.32 Mbps | 17.38 | 17.87 | **1.02x** | 11 | singles 40 apart |
| 4 | 28..42 | 3.94 Mbps | 17.28 | 17.42 | **1.01x** | 11 | singles 40 apart |
| 4 | driver 24..51 | 5.36 Mbps | 18.09 | 18.54 | **1.02x** | 11 | singles 40 apart |
| 1 | driver 24..51 | 5.05 Mbps | 13.11 | 18.73 | **1.71x** | 20 | pairs: 51,52 92,93 133,134 ... |

Both defects -- the sawtooth and the doubled IDRs -- go with the one change,
and come back when it is reverted. `hisi_enc_fill_rc` now writes 4. The
`/proc/umap/rc` `IPRatio` column reads 2-4 with `stat_time = 1` and 39-57 with
4, which fits a window (1 s) shorter than the GOP (2 s) never letting the
controller account a whole GOP; that mechanism is plausible, not proven -- a
`gop = 20` cell at `stat_time = 1` would test it.

`-1` on either side of `set-qp-bounds` puts the driver's value back on that
side, and `-1` on both restores the P and I pairs as the driver held them
before raptor's first write. The values are captured at the first apply per RC
mode; on this board a fresh rvd reads 24..51 after the teardown in `hal_init`.
Out-of-range values are refused with `RSS_ERR_INVAL` at runtime, where a config
value is clamped -- a config is a wish, a control call is an instruction.

The same structure carries `u32MinIprop`/`u32MaxIprop` (the driver defaults to
`1..20` — a direct cap on I-frame size relative to P) and `stSceneChangeDetect`
(default `bDetectSceneChange = 1`, `bAdaptiveInsertIDRFrame = 0`). Neither is
written; neither is needed for the defects above. The bounded channel runs at a
lower bitrate than the unbounded one (3.9 against 5.3 Mbps in the table) because
the QP floor of 28 leaves bits unspent, which is the trade the bound makes.

### 3DNR follows the ISO ladder

`[static_3dnr]` is not an ISP section. It is a ladder of VPSS 3DNR parameter
blocks in HiSilicon's X-param text, one per ISO step -- nine of them in the
imx307 file, twelve in the image's imx335 -- written to the VPSS group through `HI_MPI_VPSS_SetGrpNRXParam` -- the same
text PQTools prints and the SDK's `scene_auto` sample reads. `hal_nrx.c` parses
it with a tag-driven tokenizer (the sample's one big `sscanf` breaks on two tags
the shipped files add), and lays each rung over the driver's own structure.

The whole ladder then goes in at once, in the parameter's AUTO form, and the
driver does the selecting: `VPSS_DRV_PrepareNRxV3Param` reads the ISO from the
ISP itself and interpolates between the two rungs either side of it, per frame.
Its rules are param count 1..16, every threshold in 100..3276800, and strictly
ascending; a ladder that breaks one of them, or a driver that will not take the
form, falls back to the SDK sample's arrangement -- raptor reads the ISO from AE
once a second off the encoder's frame hook, blends the two neighbouring rungs in
the sample's stop space, and writes MANUAL, logging when it crosses a rung.

AUTO used to be refused on the EV300 with `0xa0078003`, and the conclusion drawn
was that the driver did not implement it. It does. The headers raptor was
transcribed from are accurate but the wrong version: the board runs MPP
**V1.0.1.2** and the SDK we hold is **V1.0.1.0**, and `NRc` grew between them
from four bytes to twelve (an `HI_BOOL` and a `PRESFC` appended, taking the
struct to a 4-byte alignment). `VPSS_NRX_V3_S` is therefore 932 bytes, not 922,
so `stNRXAuto` sits at +936 and raptor was writing it at +928. The driver read the
`pastNRXParam` pointer as `u32ParamNum`, found it outside 1..16, and said
`ILLEGAL_PARAM`. The eight bytes cost a second thing quietly: `NRc` moved from
+918 to +920, so the file's `TRC` was landing in the driver's `SFC` and its
`TPC` in the driver's `TFC`, while the file's own `SFC` and `TFC` went into
padding. `v4_vpss.h` now carries the driver's layout with the disassembly it was
read from, and `-mode` and `-presfc` -- tags the V1.0.1.0 header does not have
fields for -- have somewhere to land.

Nothing else on the surface moved, which was the obvious next question. Every
`libmpi` wrapper bulk-copies its caller's struct into an ioctl, so the ioctl's
`_IOC_SIZE` *is* that struct's size: across the 351 MPI functions present in
both V1.0.1.0 and V1.0.1.2, exactly one changed, this one (944 -> 952).
`ISP_MOD_PARAM_S` also went 8 -> 12, and `hal_common.c` forwards it as a
`void *`, so raptor never sees its shape. The ISP, AE and AWB attributes do not
cross an ioctl one for one -- the libraries walk the caller's struct field by
field into a shared context -- so there the check is the set of offsets each
function reaches through its argument pointer; all 27 this backend calls are
identical between the two versions, and only `SetStatisticsConfig` differs, by
no longer reading two fields it used to. The gap the sweeps leave is a field
moving *inside* a struct whose total size did not change; nothing suggests one
did.

Measured on the EV300 at ISO 105, same scene, three frames each, on the twelve
rungs the board's `imx335.ini` carries. The control is the identical binary
built with the old offsets; the middle column is the fixed binary with AUTO
held off by a deliberately out-of-range first threshold, so it and the control
differ only in where `NRc` sits:

| | old offsets, MANUAL | fixed, MANUAL | fixed, AUTO |
|---|---|---|---|
| `SetGrpNRXParam` in AUTO | `0xa0078003` | (not attempted) | accepted |
| Laplacian variance | 4079 | 4236 (+3.9%) | 4360 |
| mean gradient | 15.51 | 15.82 (+2.0%) | 16.17 |

The first two columns are the `NRc` realignment alone: the rung's `SFC` is 5,
and the old build was feeding the driver's `SFC` the rung's `TRC` instead,
over-smoothing a daylight frame. The third is not a clean like-for-like against
the second -- it ran on the unmodified ladder, whose first threshold is 100 and
not the 50 that forced the fallback -- so read it as "AUTO lands in the same
place", not as a measurement of AUTO against MANUAL.

Why it matters is the night. VPSS 3DNR has been on since bring-up, but on the
driver's default strength. At ISO 12.6k that left the encoder with this, same
scene, a minute apart:

| | ISO-100 rung (daylight tuning) | ISO-12800 rung |
|---|---|---|
| instantaneous bitrate (`/proc/umap/rc`) | 94 Mbps | 9 Mbps |
| VI frame rate | 20 fps | 30 fps |
| `VbFail` in ~40 s | 949 | 2 |

The driver's view of what landed is `/proc/umap/vpss`, "VPSS GPR0 3DNR PARAM".

The night control, like for like. Majestic's default lets AE slow the shutter
(`slowShutter: low`, 10 fps at 13.5k lines); with `slowShutter: disabled` it
holds the sensor at 30 fps and one frame of exposure, which is what raptor does,
and the two land on the same ISO. Same scene, 20 s each, 2592x1944 H.265 VBR
5 Mbps:

| | sensor fps | lines | ISO | bitrate | QP |
|---|---|---|---|---|---|
| majestic, slowShutter disabled | 30 | 4492 | 12058 | 5.5 Mbps | 29 |
| raptor | 30 | 4492 | 11733 | 12.3 Mbps | 42 (ceiling) |

So the 3DNR ladder closed most of the gap and not all of it: raptor still
needed twice the bits at its QP ceiling. A diff of `/proc/umap/isp` under the
two said where to look next: the 3DNR rung was the same ladder rung, but DRC
strength was 512 under raptor and 300 under majestic. That is the next section.

Majestic's AE ceiling, for the record: the shipped route runs to 83 ms, and
raptor applies it, yet raptor's AE stops at one frame; the one exposure field
raptor never writes is the exposure attribute's time range, left at the driver's
default. Slow shutter is out of scope here.

### The dynamic ISP sections follow the same tick

Three sections of the IQ file are tables over an axis rather than values:
`[dynamic_linear_drc]` (seventeen DRC fields, one column per `IsoLevel`, ten
of them from ISO 100 to 51200), `[dynamic_dehaze]` (a strength per
`IsoThresh`) and `[dynamic_gamma]` (three whole curves, one per exposure
band). Until 2026-09-01 the load applied each at its first column and left it,
so the night ran on the daylight DRC. `hal_dyn.c` now owns them: it keeps the
one once-a-second AE query (`HI_MPI_ISP_QueryExposureInfo`) off the encoder's
frame hook, hands the ISO to the 3DNR ladder as before, blends the DRC and
dehaze columns linearly in ISO between the two either side (the `scene_auto`
sample's `SCENE_Interpulate`), and picks the gamma curve by exposure (ISO x
integration time / 100, the sample's `SCENE_CalculateExp`, against
`gammaExpThreshHtoL` as the sample's video path does), fading a change over
`Interval` steps at 100 ms. Every write is a fresh get-modify-set, so the
static sections' curves stay underneath. Three failed writes stop that engine
alone. The load line reads:

```
isp tuning: [dynamic_linear_drc] 10 columns, ISO 100..51200; AE at ISO 105, strength 418; tracking ISO
isp tuning: [dynamic_dehaze] 9 columns, ISO 100..12800; AE at ISO 105, strength 58; tracking ISO
isp tuning: [dynamic_gamma] 3 tables; AE at exposure 11565, table 0 written; tracking exposure
drc: ISO 105 -> 12058, now below column 7 (ISO 12800); strength 202
gamma: exposure 16948 -> 4012178, table 0 -> 2, fading over 10 steps
```

The 512 was worse than the wrong column. The file's `[static_drc]` sets
`DRCOpType = 1`, manual strength, and the static load wrote the column's
`Strength` into the auto field, which the driver does not read in manual mode.
What was on the wire was the driver's default, at every ISO, and the whole row
was dead letter. The sample routes the strength by `enOpType`; so does
`hal_dyn.c`. The ISO 100 column (420) was never applied either.

Same night scene, a minute apart, the old build and this one, both at the
encoder's QP ceiling:

| | ISO | DRC strength (`/proc/umap/isp`) | bitrate | QP |
|---|---|---|---|---|
| dynamic sections at column 0 | 11336 | 512 | 12.4 Mbps | 42 |
| dynamic sections tracking ISO | 11819 | 202 | 7.9 Mbps | 42 |

Against majestic's 5.5 Mbps at QP 29 in the table above there is a gap left,
and the encoder is still at its ceiling. What the ISP dump still shows
different is not in the file: the Bayer-NR coarse strengths (`CoarseStr`, 146
flat under raptor, 147/80/80/147 under majestic) and parts of the sharpen table
are fields no section of `imx335.ini` names, so they are majestic's own
defaults against the driver's. Majestic's 300 DRC strength is not in the file
either -- the ISO 12800 column is 200 -- which reads as majestic's own IR-mode
term (the sample adds one) rather than a different column.

### The night, second pass: the ladder was the file's

2026-09-02, the same EV300 + IMX335 at ISO 15k to 22k under street light,
against majestic on the same board and an SSC377QE + IMX335 beside it. The
picture had "detail obliterated, noise low", and the first question was
whether the HAL was doing that. It was not: at ISO 15k `/proc/umap/vpss` held
the file's ISO 12800 rung field for field, and above 12800 the nine-rung ladder
has nowhere to go. The smearing was `imx335.ini`'s own top rung.

What majestic runs turned out to be a different file. Its log reads `Loading
IQ profile /etc/sensors/iq/default.ini`, and `default.ini` is a symlink to
`imx307.ini`. Its live 3DNR at ISO 3335 is imx307's ISO 3200 rung to the
number, and at ISO 16130 the interpolation between imx307's 12800 and 25600
rungs -- twelve rungs to ISO 204800, lighter spatial strengths, motion
thresholds that keep climbing. Its sharpen, LDCI, Bayer NR and DRC-off match
neither file nor the driver's defaults; they are its own, and so is the grey:
the driver's saturation does not fall with ISO, and majestic's picture is
monochrome at ISO 16k with `colorToGray` off. Majestic is closed, so that is
as far as the reading goes.

Measured on JPEG snapshots, 900x700 crops, high-pass luma standard deviation
("hp", detail and grain together) and chroma standard deviation on a flat wall:

| run | ISO | wall hp | wall Cb / Cr |
|---|---|---|---|
| stock imx335.ini | 15468 | 27.5 | 9.4 / 6.0 |
| stock, majestic's SFS/TFS/MATH in rung 8 | 16454 | 26.1 | 10.1 / 6.3 |
| imx307 ladder alone, every other module at driver defaults | 22617 | 19.4 | 7.4 / 5.2 |
| imx335 file with the imx307 ladder | 21341 | 24.9 | 9.8 / 6.3 |
| majestic, colour on, slow shutter off | 15637 | 14.4 | 2.1 / 1.8 |
| majestic default (slow shutter, ~8 fps) | 3335 | 15.0 | 2.3 / 1.8 |

Two conclusions. The imx307 ladder is the better night ladder for this sensor,
and the imx335 file's static sections add noise over the driver's defaults at
night (an ablation to say which was cut short: the fourth rvd restart in a row
OOM-killed the outgoing rvd mid-teardown, the ISP driver left CMA pages "still
in use", and the board needed a reboot; the section after this one is about
why, and what changed so that it cannot recur that way). The
chroma gap is saturation: at those gains raptor was still carrying colour
that majestic had already given up on.

So `/etc/sensors/iq/imx335.ini` now carries imx307's `[static_3dnr]`, in
OpenIPC's `hisilicon-osdrv-hi3516ev200` package where the rest of that
directory lives. raptor ships no IQ files of its own: a tuning belongs to
the package that knows the part, and a second copy on a different search
path is a way to run one file while reading another. Same scene, one restart
apart:

| run | ISO | wall hp | wall Cb / Cr | grass hp |
|---|---|---|---|---|
| stock imx335.ini | 22535 | 25.7 | 9.0 / 6.2 | 8.2 |
| with the imx307 ladder | 20318 | 24.0 | 4.9 / 3.3 | 10.6 |

The roof tiles resolve, the colour blotches are gone, and the grass shows
more grain -- the lighter ladder's trade, the same one majestic makes.

Both runs above also carried a `[static_saturation]` AutoSat fade -- 128 flat
to ISO 800, zero from 25600 -- so most of that chroma drop is the fade and not
the ladder. The section came out again on 2026-09-03, because saturation was
never a gap in the tuning. `libsns_imx335.so` registers `cmos_get_awb_default`
with lib_hiawb and hands it `AWB_SENSOR_DEFAULT_S.stAgcTbl`: a calibrated
sixteen-column per-gain saturation table, 128 at unity down to 56 from 1024x,
with a separate WDR variant picked off the mode file's `Mode=`. AutoSat
overrides that table rather than filling an empty one, and Sony's numbers for
this module beat a curve fitted by eye on one night. The loader still applies
the section when a file carries one -- imx307.ini does, and its AutoSat is
close to a copy of its own cmos table.

Still open, in order of what it would buy: the imx335 static sections that
cost noise at night (sharpen was the one measured: hp 26.3 to 22.7 without
it), sensor rate at night (`[ircut] night_fps` exists and is the slow-shutter
lever majestic's default uses for a 4.6x lower ISO), and whether the sensor
library's saturation table wants any override at high gain -- measured
against it, not fitted by eye.

The loader still tries `/usr/share/raptor/iq/<sensor>.ini` before
`/etc/sensors/iq`, and `$RSS_ISP_TUNING` still overrides both. Nothing
installs into the first any more -- it is there to hold one file on a board
without editing a vendor package, which is what a tuning session wants.

### Restarts, the OOM killer, and the 24 MiB the kernel actually has

The wedge above was read at the time as "restarts leak MMZ". Measured
afterwards, a clean restart leaks nothing: rvd's teardown takes 1.5 s,
`/proc/media-mem` goes from 76 MiB used to 32 KiB, and five
`S95raptor restart`s in a row with a snapshot between each came back to the
same 76524 KiB every time. What went wrong the first time was one SIGKILL --
the OOM killer's -- in the middle of that teardown, and everything after it
(the "1 pages are still in use!" warnings, the next rvd blocked in
`down_interruptible` inside `mmz_mmb_free`) was the ISP driver's state after
a process died mid-ioctl. The question is why the kernel was out of memory
on a 128 MiB board whose `free` showed 15 MiB free.

The bootargs say `mmz_allocator=cma mmz=anonymous,0,0x42000000,96M`: 96 of
the 128 MiB is a CMA reserve, and `/proc/zoneinfo` puts the managed total at
120.8 MiB, so what the kernel has outside the reserve is 24.8 MiB. CMA pages
can hold *movable* allocations (page cache, anon, tmpfs) while MMZ is not
using them, and the kernel counts them in MemFree, but it cannot put its own
allocations there -- slab, page tables, driver kmallocs, the squashfs
decompressor -- and it does not fill them first either: a movable page takes
a non-CMA free page while one exists and spills into CMA only after. So the
steady state with rvd running is a non-CMA region that is full. From
`/proc/pagetypeinfo`, 8 non-CMA pageblocks against 24 CMA ones, zero free
pages of type Movable at every order, and MemFree minus CmaFree -- the
number that decides whether the next kernel allocation succeeds -- between
1.5 and 2.3 MiB over 190 s of sampling, restarts included. `min_free_kbytes`
is 632. The system lives on reclaiming the daemons' own text pages out of
that region and faulting them back (`workingset_refault` 1791 two hours into
a boot), and one bad moment is an OOM.

At 2592x1944 RAW12, the mode the ablation ran in, MMZ held 12 MiB more than
at 2592x1520 (the VB pool alone is 63.8 MiB against 52.5), which leaves
about 8 MiB of CMA for movable pages to spill into instead of 14 to 21. That
is the difference between "restarts are fine" and "the fourth one dies", and
it is a difference of margin, not of mechanism.

When the kernel does pick a victim it picks by resident size, and rvd's
resident size includes every shared ring its consumers map: score 58 here,
306 on an SSC333 that hit the same thing on 2026-08-23, the largest process
in both tables. It is the one process whose death frees nothing, because the
pipeline's buffers are the kernel's, and the one whose death by SIGKILL
costs a reboot. Two changes, neither of which touches how much memory is
used:

- rvd writes -1000 to `/proc/self/oom_score_adj` at startup
  (`rvd_main.c`). The OOM killer now takes rsd, rhd, rod or whichever
  consumer is largest, which costs a stream until the next `S95raptor
  restart`, and leaves the kernel-side pipeline intact. With nothing
  killable left the kernel panics and `panic=20` in the bootargs reboots the
  board, which is the recovery the wedge needed anyway. rvd also logs one
  line at startup with the number above in it (`mem: 120 MiB, 96 MiB of it a
  CMA reserve; outside the reserve 24 MiB with 3 MiB free`), so the next
  report of this kind can be read against it.
- The init script gives rvd ten seconds before SIGKILL instead of three
  (`config/S31raptor`). The HAL's own worst case -- the 3A thread not
  returning from `HI_MPI_ISP_Run` and being abandoned after
  `HISI_ISP_JOIN_TIMEOUT_MS` (2000) -- takes a 1.5 s teardown past the old
  limit, and a SIGKILL there is the same driver state as the OOM one.

What widened the margin itself, since done: VB's second common pool is gone.
It existed to hold the VPSS channel outputs, and because `hal_init` is not
told the streams -- `rss_multi_sensor_config_t` carries sensors, not streams
-- its blocks could only be a guess, the sensor's geometry capped at
1920x1080. On this board that guess was wrong twice over. The main stream
runs at the sensor's own 2592x1944, does not fit a 3,110,400-byte block, and
fell back to pool 0 to compete with VI for the seven blocks measured to keep
`VbFail` at zero. The sub-stream, its only remaining user, was handed one of
those blocks per 640x480 frame: `/proc/umap/vb` showed a single VENC-held
block, 11.9 MiB of CMA reserved to carry 1.8 MiB of pictures, with pool 0 at
`MinFree 0` beside it.

With the pool dropped, every channel shares pool 0's seven blocks. Three
minutes with both streams pulled over RTSP: VI at 20 fps, `LostFrame` and
`VbFail` both 0 across 10,888 frames, pool 0 steady at 2 free of 7. MemFree
went from 1.4 MiB to 13.2 MiB and MemAvailable from 6.1 to 18.0, which is
the movable-page headroom the OOM story above is about. The 96 MiB reserve
is OpenIPC's bootarg, not raptor's.

The other half of the fix is written and does nothing here.
`hal_fs_create_channel` *is* handed the stream geometry, so
`hisi_fs_pool_acquire` cuts a VB pool to a small channel's own frame with
`HI_MPI_VB_CreatePool` and points the channel at it with
`HI_MPI_VPSS_AttachVbPool` -- the pool is created fine, and the attach
returns 0xa0078008, VPSS / `EN_ERR_NOT_SUPPORT`, whether it is called before
`EnableChn` or after. This driver has no such operation; the code probes
once, says so at INFO, and leaves the channels on the common pool.

### Restarting rvd alone

There is a second way to leave the driver holding pages nothing can free,
and it has nothing to do with memory pressure. rvd's teardown ends in
`HI_MPI_SYS_Exit`, and on HiMPP that is not process-local: the kernel side
is `SysIoctl -> CMPI_ExitModules`, which runs *every* registered module's
exit, the audio ones rad holds included. Tear one down with its buffers
still referenced and `VB_DestroyPool` trips `free_contig_range` --

```
WARNING: ... free_contig_range   31 pages are still in use!
  ... VB_DestroyPool <- AIVbFree <- AiDisableDev <- AI_Exit <- CMPI_ExitModules
alloc_contig_range: [45fe0, 45fff) PFNs busy
```

-- after which `HI_MPI_VB_SetConfig` returns `HI_ERR_VB_BUSY` (0xa0018012)
for every rvd that follows, including ones started long after every process
involved has exited. Only reloading the kernel modules clears it.

So restarting rvd is an ordered operation, and rcd is what orders it:
`restart_rvd` in `rcd/rcd_apply.c` releases rad's input and output, restarts
rvd, and puts them back, with the resume outside the success path because a
skipped one leaves audio dead while every audio setting still reads
correctly. `raptorctl config apply` and the console's restart-tier keys have
always gone that way.

What did not was `raptorctl rvd restart`, which built `{"cmd":"restart"}`
and sent it to rvd's own socket. Two changes close that:

- raptorctl routes that one verb through rcd
  (`{"cmd":"restart","daemons":["rvd"]}`) rather than sending it, and
  refuses -- rather than falling back to the raw send -- when rcd is not
  running.
- rvd refuses a restart while rad reports `ai_enabled` or `ao_enabled`
  (`rvd_ctrl.c`, ahead of the common handler that serves the verb). It asks
  the holder rather than the caller, so rcd's bracket passes by having
  released audio rather than by being recognised. A rad that is down or not
  answering is not holding, and does not block a restart -- measured: with
  rad stopped the raw path restarts cleanly and MMZ comes back two blocks
  lighter.

Verified on the EV300 at 2592x1944: bracketed restart, rvd pid moves, 51 MMZ
blocks before and after, encoders back, rad resumed, nothing in dmesg; raw
restart refused with the reason and rvd still serving.

What neither covers is rvd being SIGKILLed, or dying between the release and
the resume -- rcd's own comment says as much. Deliberately: rad would have to
watch for rvd's ring and re-attach to itself, and the recovery for a camera
that has got into that state is to restart it.

## Not written yet

Not decisions — unwritten phases. Each returns `RSS_ERR_NOTSUP` through
`RSS_HAL_CALL`'s NULL guard, so rvd starts, reports what it cannot do, and
carries on rather than failing partway into a stage that does not exist.

- **Thermal: unsupported.** The EV300 kernel exposes no thermal zone and
  no hwmon device (`/sys/class/thermal` and `/sys/class/hwmon` are empty),
  so there is nothing for the generic reader to read; the `%soc_temp%`
  OSD variable renders `--` here. Dropped rather than deferred: a chip
  temperature would need a vendor register the SDK does not document.
- The ISP knob ops beyond brightness, contrast, `ae_comp` and
  `drc_strength` — saturation, sharpness, the gain ceilings, defog,
  antiflicker — have no op yet. Saturation and sharpness would go the way
  of DRC (a pin over a per-gain curve the tuning owns) and need the same
  argument SigmaStar's backend makes before they are worth a knob.
- **The rest of the night gap against majestic.** The ladder and the
  saturation are now the file's ("The night, second pass"). What
  `/proc/umap/isp` still shows different is majestic's own: Bayer-NR
  `CoarseStr` 80 on the green channels against the driver's 152, a sharpen
  table twice as strong, LDCI at sigma 58 / BlcCtrl 200, DRC off. None of
  it is in a file, and nothing here invents values.

## Deliberately absent

These are decisions. Each is argued at the point it applies, in an
`OP COVERAGE` comment at the top of the relevant file — read those before
implementing anything listed here as missing.

- **Zero-copy stream references.** `enc_get_rmem_info` and
  `enc_inject_stream_shm` are unimplemented, which is what keeps rvd's
  *refmode* off: rvd falls back to copying publication when `ref_base` is
  zero. gen4's stream buffer is a per-channel ring sized by `u32BufSize` —
  exactly the shape that recycles — and nobody has measured whether a
  published reference survives `ReleaseStream`. The SigmaStar backend
  measured the equivalent on MI and found it does not. Anyone implementing
  these has to measure first. See `hal_encoder.c`.
- **Rotation at runtime.** `[image] rotate` is read at pipeline init only: a
  turn of 90 or 270 changes the encoder geometry, which means recreating the
  encoder channel, and there is no ctrl command for that yet.
- **The QP knobs and RC options.** gen4 puts them in `VENC_RC_PARAM_S`, a
  separate structure from the channel attribute with a per-codec union of its
  own. They are a coherent group and belong in one commit that transcribes
  it, not scattered across whichever ops map onto its leading fields.
- **Per-stream noise reduction.** 3DNR is a *group* attribute on gen4
  (`VPSS_GRP_ATTR_S.bNrEn`), so it cannot vary per stream. There is one
  setting and every stream shares it. Strength lives in the ISP tuning that
  Phase 3 reaches.
- **`fs_set_delay` / `fs_set_max_delay`.** `HI_MPI_VPSS_SetGrpDelay` is a
  group control, so a per-channel op would silently affect every stream.

## The pipeline, and where each piece lives

```
sensor ──MIPI──▶ VI dev 0 ──▶ VI pipe 0 (== the ISP) ──▶ VI chn 0
                                                            │
                                                      SYS_Bind
                                                            ▼
                                              VPSS group 0 (3DNR)
                                               │        │        │
                                             chn 0    chn 1    chn 2   ← framesources
                                               │        │        │
                                          SYS_Bind  SYS_Bind  SYS_Bind
                                               ▼        ▼        ▼
                                            VENC 0   VENC 1   VENC 2   ← encoders
```

Two facts about this shape are worth carrying:

**The ISP is the VI pipe.** There is no ISP device. Every `HI_MPI_ISP_*`,
`HI_MPI_AE_*` and `HI_MPI_AWB_*` call is keyed on `VI_PIPE`, which is why
`hisi_state.h` names `HISI_VI_PIPE` rather than leaving a bare 0 at forty
call sites.

**`HI_MPI_ISP_Run` never returns.** It is the 3A loop and runs for the life
of the pipeline, so it gets a thread. Teardown stops it with
`HI_MPI_ISP_Exit` and then joins with a two-second bound — cancelling it
instead leaves the vendor library's locks held and deadlocks the next
bring-up in the same process.

## Where the sensor configuration comes from

There is no sensor table in `src/hisi_v4/`, and there must not be one. 34
`libsns_*.so` ship on a stock image, each with its own lane map and bit
depth; a table would cover whichever subset someone tested and go stale
every time OpenIPC adds a sensor.

Instead `hisi_sensor.c` reads the vendor's own `/etc/sensors/<mode>.ini` —
the same file majestic reads — and takes from it:

| Key | Used for |
|---|---|
| `DllFile` | which `libsns_*.so` to `dlopen` |
| `Sensor_type` | the `stSns*Obj` symbol inside it |
| `raw_bitness` | MIPI data type, VI pixel format, and VI bit width, all derived from one number |
| `lane_id` | the MIPI lane map, `-1` for a disabled lane |
| `Isp_Bayer`, `Isp_FrameRate` | `ISP_PUB_ATTR_S` |
| `DevRect_*` | the sensor's output size **and origin** |
| the `vi_dev` block | `VI_DEV_ATTR_S`, verbatim |

`DevRect_x` and `DevRect_y` are the ones easy to drop: the IMX335's 5 MP
mode starts at (200, 20), and processing from the origin instead puts a
green band down two edges.

If the INI names no `Sensor_type`, or names one the library does not export,
the loader reads the driver's own `.dynsym` and looks for a `stSns*Obj`
symbol. That is what lets a driver scavenged from a firmware dump work with
no rebuild and no knowledge of its symbol name.

The file is found by scanning `/etc/sensors` for any `.ini` whose name
contains the configured sensor name, because the names are not derivable —
the IMX335 ships as both `imx335_i2c_4M.ini` and `5M_imx335.ini`. When
several match, the first in sorted order wins and every candidate is logged.

## Sensors are a userspace concern here

A gen4 sensor driver is a userspace `libsns_<model>.so` exporting an
`ISP_SNS_OBJ_S` vtable. The kernel side — `hi_mipi_rx.ko`, `hi_sensor_i2c.ko`,
`sys_config.ko` — is sensor-agnostic, with lane and WDR configuration pushed
down as ioctls on `/dev/hi_mipi`. A driver scavenged from a firmware dump drops
in with a config edit and no rebuild, provided it is T5-tier and pairs with a
same-generation `libisp.so`; better still,
`ref/openhisilicon/libraries/sensor/hi3516ev200/` builds 30 of them from source
under the same licence as raptor.

**There is no `/proc/jz/sensor` equivalent**: the sensor's identity *is*
whichever `libsns_*.so` gets dlopened, and the kernel never probes the bus. What
the kernel side does keep is the name its module loader was given:
`sys_config.ko` takes a `sensors=` parameter (the vendor's `load3516ev300` and
OpenIPC's `load_hisilicon` both pass it, OpenIPC from U-Boot's `sensor=`
variable, which its own autodetect wrote), and a module parameter reads back
from `/sys/module/<module>/parameters/sensors`. When `[sensor] name` is absent
from `/etc/raptor.conf` the backend reads that — any module ending in
`sys_config`, first name of the list, `unknown` counts as none — and logs where
the name came from. A configured name always wins. With neither, init fails and
says so; rvd's own warning that the name was not auto-detected still prints
first, because rvd's detection is the Ingenic procfs read and this one happens
in the backend. On a stock OpenIPC board the same name is in
`isp.sensorConfig` in `/etc/majestic.yaml`, the `DllFile=` line inside the INI
it points at, and U-Boot's `sensor=` variable.

## The trampolines, and why they live where they do

Ten symbols on a gen4 board are left undefined by every vendor library and have
to come from the *executable*. They are defined at the top of
`src/hisi_v4/hal_common.c`, and that placement is the load-bearing part —
`raptor-hal` ships as `libraptor_hal_video.a`, a static archive whose members
are extracted only when something already linked references them. Nothing in
raptor references `__ctype_b`. A standalone `quirks.c` would compile, archive,
and never link, and `--export-dynamic` cannot export what was never linked.
`hal_common.c` defines `rss_hal_create`, so it is always extracted.

Three of the ten are the uClibc ABI, needed by `libsecurec.so`: every vendor
`.so` declares `NEEDED libc.so.0` while the rootfs is musl. `__ctype_b` is a
*data* symbol, so its relocation binds eagerly when `libsecurec.so` is mapped —
and `libsecurec.so` is a `DT_NEEDED` of `libmpi.so`. No `dlopen` ordering can
fix it afterwards, which is why `hisi_check_trampolines()` runs before the
first vendor `dlopen` and not at ISP-open time.

The other six are `GK_API_*` sensor callbacks that eight of the 34 shipped
drivers call. `majestic` defines none of them, so those eight drivers cannot be
loading under it at all.

Verify a build with:

```sh
readelf -d rvd | grep NEEDED          # no libmpi, no libisp: dlopen only
nm -D rvd | grep -E 'ctype_b|GK_API_' # all ten present in .dynsym
```

## Soft-float, and why it is a compile error

Every binary on a gen4 board — `libmpi.so`, `libisp.so`, `libsecurec.so`, all
six `lib_hi*.so`, all 34 `libsns_*.so`, and `majestic` — has **no
`Tag_ABI_VFP_args`**, while carrying `Tag_FP_arch: VFPv4`. The FPU is used; the
calling convention is soft-float. The tuple is `arm-openipc-linux-musleabi`,
never `musleabihf`, and this is the one ARM platform in raptor where that is
true.

A hard-float build links, loads and runs, and hands garbage to every float
argument crossing into MPI. `src/hisi_v4/v4_common.h` carries `#error` on
`__ARM_PCS_VFP` so the mistake cannot survive a compile.

## Tuning is text, mostly

SigmaStar's CUS3A binary has no gen4 equivalent. Tuning here is two layers of
INI, both plain text:

- `/etc/sensors/<mode>.ini` — sensor object name, `DllFile`, MIPI lane map, RAW
  bitness, VI device attributes. Wiring, not image quality.
- `/etc/sensors/iq/<sensor>.ini` — the IQ proper: `[static_ae]`, the
  `[static_aerouteex]` gain ladder, the metering grid. 1398 lines for an IMX335.

### `[module_state]` says which of a file's own sections count

A tuning file may open with `[module_state]`, a list of `bStaticXxx` /
`bDynamicXxx` flags. It is not decoration: the vendor's own loader — the SDK's
`scene_auto` sample, `src/core/hi_scene_setparam.c` — opens every
`HI_SCENE_SetXxx` with the matching flag and returns without touching the ISP
when it is clear. A section sitting in the file under a `"0"` is documentation,
not tuning, and applying it anyway rewrites a module the author meant to leave
alone.

The shipped `imx307.ini` is exactly that file: it carries a full
`[static_sharpen]`, `[static_ldci]`, `[static_drc]`, `[static_nr]`,
`[static_dehaze]`, `[static_dpc]` and `[static_saturation]`, and turns every one
of them off. It is also the only OpenIPC IQ file that carries the section at
all; `imx335.ini` and the rest have none, and a file without one means all of
its sections, which is what this loader did before the mask existed.

The map from flag to section is the vendor's, surprises included:

- `[static_aerouteex]` has no flag of its own. `HI_SCENE_SetStaticAE` writes the
  route and the exposure attributes together, under `bStaticAE`.
- the AE weight table is nested inside that same function, which reaches
  `SetAeWeightTab` only if `bAeWeightTab`, so `[static_aeweight]` needs both
  flags.
- the 3DNR ladder answers to `bDyanamic3DNR` — the vendor's spelling, typo and
  all. `HI_SCENE_SetStatic3DNR` is `#if 0`'d out of the SDK (it is the old
  VI-pipe NRX V1 path), so `bStatic3DNR` gates nothing; the live consumer of
  `[static_3dnr]` is `HI_SCENE_SetDynamic3DNR`, which interpolates the ladder by
  ISO the way `hal_nrx.c` does. That is how `imx307.ini` carries
  `bStatic3DNR = "0"` and still has a working 3DNR ladder.

Inside a `[module_state]` that is present, a flag that is not listed is off: the
vendor's parser writes only the flags it finds into a struct that started
zeroed. The mask is read in a pass of its own before anything is applied, so it
holds wherever in the file it sits, and the load summary names what it turned
off:

```
isp tuning: <file>: 8 modules applied; [module_state] off: static_drc static_dehaze static_sharpen static_dpc
```

On `imx307.ini` the mask leaves four modules: the exposure attributes and the
AE route, which `bStaticAE` carries together, plus the gamma curves and the
3DNR ladder.

Binary tuning does also exist, produced by the Windows PQTools; the board's
`/usr/sbin/pqtools` fetches the PC application and an on-target agent from
`github.com/openipc/pqtools`. The lite images ship INI only. Phase 3 does the
INI half first: it needs no Windows host, and the result is diffable in a way
the SigmaStar side is not.

## Measured ceilings

From `/proc/umap` on a live 5 MP pipeline, and these are what
`caps_hisilicon.inc` publishes rather than guesses:

- `VencMaxChnNum = 3` — a driver-level cap, below `RVD_MAX_STREAMS`, and a
  module parameter: OpenIPC's `load_hisilicon` passes 3, the vendor's load
  script passes nothing, and the driver creates a fourth channel without
  complaint when loaded with 4 (measured: main, sub and a snapshot channel
  for each, all four created). The HAL reads the parameter back from
  `/sys/module/<venc module>/parameters/VencMaxChnNum` in `hal_init` and
  publishes that as `max_enc_channels`, so caps say what this image loaded.
  Snapshot channels are encoder channels too, and with 3 two video streams
  leave room for one: rvd defaults `stream1`'s `jpeg` off with a log line
  saying why, and `/snap.jpg` serves from the main stream's channel. A fourth
  channel asked for explicitly on a 3-channel load is refused by the driver
  with INVALID_CHNID at CreateChn and the pipeline does not start. The
  openipc-raptor image loads the module with 4.
- `VPSS_MAX_PHY_CHN_NUM = 3`, of which **two** can carry a stream. Channel 0 is
  spent on the group's input: `HI_MPI_SYS_Bind` overwrites the destination
  channel with 0 whenever the destination is VPSS, so the VI edge always lands
  there and channel 0 cannot also be an output. Measured — a stream on channel 0
  never produces at any resolution, alone or alongside another — and the vendor's
  samples agree without saying so, using `VPSS_CHN0` only in the configurations
  that bind nothing to the group. `max_fs_channels` is 2 for this reason.
- VI/VPSS coupling ships as `VI_OFFLINE_VPSS_ONLINE` and is set to
  `VI_OFFLINE_VPSS_OFFLINE`, before `HI_MPI_SYS_Init`, because that is where it
  can be set at all. `VI_ONLINE_VPSS_OFFLINE` would save the raw round trip
  through DDR and is the vendor's own single-sensor default, but the driver
  refuses it at 2592x1944.
- VB is one pool of sensor-sized blocks, shared by VI, the group and the
  channel outputs, and its block count is bounded by CMA, not by the zone.
  The second, stream-sized pool it used to have was a guess `hal_init` had no
  way to make well; see the memory section above for what removing it bought.
  This board runs `mmz_allocator=cma`, so a pool is one contiguous allocation:
  six sensor blocks (45 MiB) succeed and seven (53 MiB) fail with VB / NOMEM on
  a 96 MiB zone reporting 95 MiB free. The board's own majestic fails the same
  way with its shipped `blkCnt: 7`.
- The sensor ini's `DevRect_x`/`DevRect_y` are read, reported, and not applied.
  The IMX335 entry says (200, 20); used as a window they ask for 200 + 2592
  columns of a 2592-wide stream, and the pipe never completes a line.

## Building

```sh
./build.sh hi3516ev300 /path/to/openipc-firmware/output
```

which expands to

```sh
make PLATFORM=HI3516EV300 \
     CROSS_COMPILE=arm-openipc-linux-musleabi- \
     SYSROOT=<output>/host/arm-openipc-linux-musleabi/sysroot
```

The toolchain is OpenIPC's own Buildroot SDK — the `toolchain` release of
`OpenIPC/firmware`, `toolchain.hisilicon-hi3516ev200.tgz`, GCC 13.3.0. It
defaults to ARMv7 + NEON with a soft-float ABI, so no `-mcpu` or `-mfloat-abi`
override is needed.

The HAL half needs no sysroot at all, because the backend links nothing:

```sh
make -C raptor-hal PLATFORM=HI3516EV300 CROSS_COMPILE=arm-openipc-linux-musleabi-
```

The ABI transcriptions in `v4_*.h` pin their offsets with `_Static_assert`, but
those describe 32-bit ARM EABI and the host test suite defines them away, so
they only run on a cross-compile. To run them on purpose, with no SDK and no
sysroot:

```sh
make -C raptor-hal/tests abi-check-hisi CROSS_COMPILE=arm-openipc-linux-musleabi-
```

A pass says the headers agree with the offsets probed on the board; unlike the
SigmaStar checks there is no vendor header to agree with.

`build-standalone.sh` rejects HiSilicon, exactly as it rejects SigmaStar: there
is no self-contained dependency set for these platforms.
