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

## Not written yet

Not decisions — unwritten phases. Each returns `RSS_ERR_NOTSUP` through
`RSS_HAL_CALL`'s NULL guard, so rvd starts, reports what it cannot do, and
carries on rather than failing partway into a stage that does not exist.

- **ISP tuning** (Phase 3), **audio** (Phase 4), **OSD** (Phase 5),
  **thermal** (Phase 6).

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

**There is no `/proc/jz/sensor` equivalent, and there cannot be one**: the
sensor's identity *is* whichever `libsns_*.so` gets dlopened. So there is nothing
for the backend to auto-detect and **`[sensor] name` is required in
`/etc/raptor.conf`**; rvd warns and lets the backend fail if it is missing. On a stock OpenIPC board the name to use is the one three
other places already agree on — `isp.sensorConfig` in `/etc/majestic.yaml`, the
`DllFile=` line inside the INI it points at, and U-Boot's `sensor=` environment
variable.

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

- `VencMaxChnNum = 3` — a driver-level cap, below `RVD_MAX_STREAMS`.
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

`build-standalone.sh` rejects HiSilicon, exactly as it rejects SigmaStar: there
is no self-contained dependency set for these platforms.
