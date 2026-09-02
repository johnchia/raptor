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
| Orientation | `hflip` / `vflip` at the sensor, through `pfnMirrorFlip` |
| Frame rate | VI needs seven VB blocks in pool 0 at 5 MP; with six it loses ~5% of frames to `VbFail` and the pipe drops to 25-26 fps, which VPSS's source/destination ratio then scales again — that is what made a request of 20 fps produce 17. With seven, VI holds 30 and 20 fps means 19.7. Watch `/proc/umap/vi`'s `LostFrame`/`VbFail`, not just `/proc/umap/vb` |
| ISP tuning | `/etc/sensors/iq/<sensor>.ini` applied on the first encoded frame — the static sections via get-modify-set on `HI_MPI_ISP_Set*`, the dynamic ones (`dynamic_linear_drc`, `dynamic_dehaze`, `dynamic_gamma`) and `static_3dnr` by engines that follow AE's ISO once a second (`hal_dyn.c`, `hal_nrx.c`). `$RSS_ISP_TUNING` overrides the path. See the two ISO sections below |
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

`[static_3dnr]` is not an ISP section. It is nine blocks of VPSS 3DNR
parameters in HiSilicon's X-param text, one per ISO step from 100 to 12800,
written to the VPSS group through `HI_MPI_VPSS_SetGrpNRXParam` -- the same
text PQTools prints and the SDK's `scene_auto` sample reads. `hal_nrx.c` parses
it with a tag-driven tokenizer (the sample's one big `sscanf` breaks on two tags
majestic's files add), lays each rung over the driver's own structure, and picks
the rung by the ISO AE reports, interpolating between neighbours in the sample's
stop space. The driver's own AUTO form, which would do the picking in the
kernel, is refused on the EV300 with `0xa0078003`; the sample never uses it
either. The pick runs once a second off the encoder's frame hook and logs when
it crosses a rung.

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

## Not written yet

Not decisions — unwritten phases. Each returns `RSS_ERR_NOTSUP` through
`RSS_HAL_CALL`'s NULL guard, so rvd starts, reports what it cannot do, and
carries on rather than failing partway into a stage that does not exist.

- **Thermal: unsupported.** The EV300 kernel exposes no thermal zone and
  no hwmon device (`/sys/class/thermal` and `/sys/class/hwmon` are empty),
  so there is nothing for the generic reader to read; the `%soc_temp%`
  OSD variable renders `--` here. Dropped rather than deferred: a chip
  temperature would need a vendor register the SDK does not document.
- The ISP *knob* ops — brightness, contrast, the gain ceilings — remain
  deferred until the tuning baseline is proven on the board.
- **The night bitrate gap against majestic is open.** With every section of
  `imx335.ini` applied and the dynamic ones tracking ISO, raptor still
  spends 7.9 Mbps at QP 42 where majestic spends 5.5 Mbps at QP 29 on the
  same scene at the same ISO (the tables under "3DNR follows the ISO
  ladder"). What `/proc/umap/isp` still shows different is not in the file:
  Bayer-NR `CoarseStr`, parts of the sharpen table, and a DRC strength of
  300 against the file's 200. Closing it means choosing values majestic
  carries in its own defaults, not reading them from the INI, and nothing
  here does that yet.

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
- **Rotation.** VPSS has no rotate on gen4. Mirror and flip are channel
  attributes; 90 degrees is not available at any stage.
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

Binary tuning does also exist, produced by the Windows PQTools; the board's
`/usr/sbin/pqtools` fetches the PC application and an on-target agent from
`github.com/openipc/pqtools`. The lite images ship INI only. Phase 3 does the
INI half first: it needs no Windows host, and the result is diffable in a way
the SigmaStar side is not.

## Measured ceilings

From `/proc/umap` on a live 5 MP pipeline, and these are what
`caps_hisilicon.inc` publishes rather than guesses:

- `VencMaxChnNum = 3` — a driver-level cap (a module parameter; OpenIPC's
  `load_hisilicon` passes 3), below `RVD_MAX_STREAMS`. Snapshot channels are
  encoder channels too, so two video streams leave room for one: rvd defaults
  `stream1`'s `jpeg` off with a log line saying why, and `/snap.jpg` serves
  from the main stream's channel. A fourth channel asked for explicitly is
  refused by the driver with INVALID_CHNID at CreateChn and the pipeline does
  not start.
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
- VB is two pools — sensor-sized for VI and the group, stream-sized for the
  channel outputs — and the block counts are bounded by CMA, not by the zone.
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
