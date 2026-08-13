# SigmaStar Infinity6C (SSC377) backend

Status of the `PLATFORM=INFINITY6C` HAL backend: what works, what is
deliberately absent, and what is written but not yet watched working. Brought up
on an SSC377QE board running an OpenIPC base image.

A separate backend from `INFINITY6E`, not a variant of it — see
[sigmastar.md](sigmastar.md) for that one. The two share silicon lineage and
almost no ABI: MI 3.0 gives `MI_SYS` and `MI_RGN` a leading SoC id, gives
`MI_VENC` a leading device, promotes the ISP from a set of tuning calls inside
VPE to a pipeline stage with its own device, channel and output ports, and moves
scaling to a module of its own (SCL). `src/infinity6c/` in raptor-hal is the
backend; `src/star/` is Infinity6E's. Neither is reachable from the other by
`#ifdef`, which is why they are separate directories selected by `BACKEND_DIR`.

This is an index, not the reference. Every "deliberately absent" decision is
argued where it applies, in an `OP COVERAGE` block or a file header comment in
raptor-hal — `hal_isp.c`, `hal_osd.c`, `hal_audio.c`, `hal_encoder.c`. Read
those before implementing anything listed here as missing: several are absent
because the obvious implementation would be wrong, not because nobody got to
them.

## The pipeline

    SNR ──▶ VIF ──▶ ISP ──▶ SCL ──┬──▶ VENC (H.26x main) ──▶ VENC (sub)
                                  └──▶ VENC (MJPEG)

Four things about that shape are worth knowing before reading any of the code:

- **A raptor framesource channel is an SCL output port**, and an encoder channel
  is fed by one. So a stream costs one port, and the scaler's four ports cap
  both channel counts at 4 — the same four, not four of each.
- **A second H.26x stream is a VENC cascade, not a second scaler port.** The
  sub-stream binds VENC(main) → VENC(sub). This was settled against the vendor
  SDK after the second-port approach failed, and the cascade is board-verified
  at dual 25 fps with no measurable cost to the main stream.
- **The ring leg needs IFC compression on its source port.** A `HW_RING` bind to
  VENC only moves data from an IFC-compressed port; a frame-fed leg takes none,
  and the MJPEG leg rejects IFC outright. The port is created uncompressed and
  switched once the bind type — and with it the consumer — is known.
- **`mi_vif` must be inserted before the sensor driver**, and the HAL enumerates
  the sensor's modes before enabling it. Do not reorder either.

## Working, board-verified

| Area | State |
|---|---|
| Video | H.264 and H.265, main + sub, over RTSP, 25 fps each |
| Audio | Capture via `rad`, `MI_AI` at 50 periods/s |
| OSD | Text on the VENC channel, covers on SCL, both streams at full resolution |
| Exposure readback | `isp_get_exposure` — shutter, gain, AE luma, AWB gains |
| Colour | Natural, once the per-sensor IQ binary loads — see below |
| ISP tuning | Measured moving the picture — see below for which knobs and how far |
| Media clock | `utc_offset_us` published in the ring, refreshed every 5 s |
| Restarting `rvd` | Live, with `rad` quiesced around it — see below |

## Known broken

**JPEG and MJPEG snapshots do not work on this backend.** Both JPEG channels are
created and then fail to get a source:

    venc chn 2: cloning SCL port 0 to 2 failed: -5
    MI_SYS_BindChnPort2(SCL port 0 -> VENC dev 8 chn 2) failed: -1610014702
    venc chn 3: paired video chn 1 is not bound to an SCL port yet, so there is
                no geometry to clone -- no snapshots on this stream

The second message is the more interesting one, and it is a consequence of the
cascade: the sub-stream has no SCL port of its own, so a JPEG channel paired with
it has nothing to clone. The first says the scaler refused a second port for the
full-resolution channel. `/snap` and `/mjpeg` are therefore unavailable, the
`jpeg0` and `jpeg1` rings exist but never advance past write sequence 0, and a
reader on them blocks forever rather than erroring.

This predates the ISP work — the same four lines appear under earlier builds —
and it has not been diagnosed. Whoever picks it up should start with whether the
scaler can serve a snapshot port at all alongside a `HW_RING` leg, since that
would decide between "clone a port" and "share the video port with a
transient-depth grab".

## The ISP tuning surface

`hal_isp.c` publishes brightness, contrast, saturation, defog, `ae_comp`,
antiflicker, day/night, hflip, vflip, temper and the sensor rate. Every ABI
number behind them is derived twice and the two agree — the maruko vendor
headers give the struct layouts, and the shipped `libmi_isp.so` states each
payload length itself in the descriptor it hands to
`MI_ISP_GENERAL_SetIspApiData`. `raptor-hal/tests/abi_iq_i6c.c` asserts the
table against the vendor headers at compile time.

Measured on an SSC377QE + IMX335, reading the picture out of the sub-stream ring
and decoding it, rather than by trusting a return code:

| Knob | Evidence |
|---|---|
| brightness | mean luma 110.2 at 30, 144.1 at 128, 198.7 at 230; MI 11 / 50 / 90 of 0..100 |
| saturation | mean chroma saturation 0.0099 at 20, 0.1522 at 128, 0.2675 at 250; readback exact, and returning to 128 restores 0.1522 to four decimals |
| day/night | saturation 0.1528 → **0.0000** → 0.1525, i.e. exactly monochrome and fully reversible |
| antiflicker | raptor 0/1/2 reach MI 0/2/1 — the swap is real, see below |
| `ae_comp` | 60 → MI −11 → reads back 57, with the neutral learned from the tuning as MI 0 in −20..20 |
| hflip | NCC 0.985 against the mirrored reference, 0.082 against the original — but only from a restart, see below |
| temper | reads back 146, which is 3DNR level 4 of 7 — the eight-step quantisation, as designed |
| sensor rate | `isp_set_sensor_fps` returns OK and rvd logs `sensor0 fps: 25` instead of its "neither settable nor readable" fallback |

**The antiflicker enum is not raptor's.** MI orders 60 Hz before 50 Hz and raptor
the other way, so the mapping is 0→0, 1→2, 2→1. A straight cast would silently
set the mains frequency the operator excluded — the one bug in this area that
would have been invisible in review and in testing, since both values "work".

Three mechanisms sit behind the one op surface, and telling them apart is most
of understanding the file:

| Ops | Mechanism |
|---|---|
| brightness, contrast, saturation, defog, `ae_comp`, antiflicker, day/night | the tuning API — one wrapper shape per module, driven from a table |
| hflip, vflip, temper | fields of the ISP channel's parameter block, `MI_ISP_SetChnParam` |
| sensor rate | `MI_SNR_SetFps` / `MI_SNR_GetFps` |

**hflip, vflip and temper are config settings, not runtime ones.** The ISP
channel accepts its parameter block only while being created; a running channel
returns `-1610121208` for the identical values that bring-up accepts. So:

```sh
raptorctl config set image hflip 1
raptorctl rad ai-disable && raptorctl rvd restart && raptorctl rad ai-enable
```

A live `raptorctl rvd set-hflip 1` answers `failed (-5)` and changes nothing —
deliberately: the request is rolled back rather than held for the next pipeline
build, because a command that reports failure and then takes effect minutes later
is worse than one that plainly fails. The log says so once, naming the config
route.

**Nothing is applied when rvd asks for it.** rvd drives its whole `[image]`
block during pipeline construction, which here is before the ISP channel exists
and well before the first frame — and a tuning value written before that frame
does not survive, because the IQ binary load and CUS3A's own AE initialisation
both write over the API store. So the knobs queue and are flushed once the load
and the 3A arm are done, and orientation and the 3DNR level are held for ISP
bring-up to put into the parameter block it already fills. A pipeline teardown
re-arms both, because the reload puts every module back to what the binary says.

That ordering is the single most expensive thing to rediscover here. It is also
why a knob that "does not work" is worth checking against the log before it is
worth debugging: the flush logs each value as it lands.

**Day/night is colour or monochrome, and nothing else.** There is no second IQ
profile to switch to, and the IR-cut filter and the illuminator are GPIOs that
`ric` drives itself through sysfs. `isp_set_running_mode` reaches
`MI_ISP_IQ_SetColorToGray`. Before this existed `ric` moved the filter while the
ISP was never told, which looked like a working night mode that did nothing to
the picture.

**Sharpness and spatial denoise are absent on purpose.** Both modules are
per-frequency-band arrays on maruko — sharpness is 6264 bytes against
Infinity6E's 1268 — with no place a single 0..255 scalar honestly lands. Setting
one band and calling it sharpness would be worse than `RSS_ERR_NOTSUP`.

## Deliberately absent

| Op | Why |
|---|---|
| `isp_set_sharpness`, `isp_set_sinter_strength` | per-band arrays, no scalar field — see above |
| `isp_set_wb`, `isp_set_hue`, DRC, DPC, highlight depress, backlight comp | no counterpart bound yet |
| `enc_set_color2grey` | grey is one ISP setting every stream draws from, so a per-channel encoder op would grey both and lie about it |
| `fs_get_frame`, `fs_release_frame` | raw readback needs a user-facing `MI_SYS` output port on the scaler. rvd already answers "raw snap not supported on this SoC" — and note the scaler refuses a second port for JPEG too, so the two are probably the same question |
| `audio_enable_ns` / `agc` / `hpf` / `aec`, `audio_register_encoder` | absent from the silicon's library, not merely unavailable: `libmi_ai.so` exports 22 symbols and not one is `vqe`, `aenc`, `aed`, `iaa` or `src`. The vendor's note: "In 2.19 and later versions of the API, MI_AI no longer includes the associated algorithm functions" |
| the `ao_*` family | playback; `libmi_ao` is never loaded and this backend is capture-only by design |
| IVS / motion detection | absent from both SigmaStar backends |

## Restarting `rvd` on a running camera

`raptorctl rvd restart` works, with one thing to do either side of it:

```sh
raptorctl rad ai-disable
raptorctl rvd restart
raptorctl rad ai-enable
```

`rvd` re-execs cleanly, the ring is never absent, throughput returns, and `rsd`,
`rod` and `ric` keep their PIDs and reattach without intervention. `rad` is the
exception: restarted underneath, it dies every time, silently — no shutdown or
error line, so a signal rather than a clean exit.

`rvd` and `rad` are the only two daemons that link the HAL, and `MI_SYS_Init` /
`MI_SYS_Exit` is process-global on this platform. `ai-disable` releases any held
period, disables the AI channel group, closes the device, calls `MI_SYS_Exit` and
`dlclose`s `libmi_ai` and `libmi_sys`, which leaves `rad`'s loop in a plain
`usleep` holding no MI reference. Its control socket is serviced in the same
thread as the read loop, so no period can be held part-way through the quiesce.
No daemon code was changed to make this work; the sequence is board-verified.

The exact signal is not proven. `rad` sits inside the blocking `MI_AI_Read`
ioctl for nearly the whole 20 ms period, so that — not the microsecond-wide read
of `frame.data` in the encoder — is where the fault window is. Which matters
before anyone "fixes" it: copying the frame and releasing early would look like
a fix without being one. A `SIGBUS`/`SIGSEGV` handler logging the faulting
address, plus `dmesg`, would settle it.

## Traps that cost real time

- **An unsupported OSD pixel format hangs the whole MI graph.** `ARGB8888` is
  the one to avoid; the symptom is not an error but a stall that reads as a
  resolution limit. Cover colour is VYU444 and cover size is in 8192ths.
- **`osdrv-i6c` ships 2022 MI libraries against a 2024 `mi.ko`.** The symptom is
  a dark picture: the mismatch breaks CUS3A's AE (a 112-byte structure against
  108). The fix is a coherent 2024-06 library set, keeping the 0907
  `cam_os_wrapper`. This is a property of the base image, not of raptor.
- **The C library is not a preference.** The vendor drop ships the whole MI set
  twice, one build needing `libc.so.0` and one `libc.so.6`, so the toolchain has
  to match whichever set the image carries. A mismatch is not a link error — the
  loader simply never finds a libc the MI libraries can use. The board this was
  brought up on runs uClibc.
- **`make rvd` after a raptor-hal change says "Nothing to be done"** and ships
  the old binary. Remove the target first.
- **A bad picture on this backend carries no information until the IQ binary has
  loaded.** With no tuning, colour and exposure are whatever CUS3A's defaults
  give. No picture at all is a real result; a green or dark one, before the
  first-frame load, is expected. The bring-up log says which case you are in.

## Building

```sh
./build.sh infinity6c /path/to/openipc/output <targets>
```

`build.sh` searches this family for a **uClibc or a glibc** sysroot tuple, and
stops naming the tuples it tried if it finds neither, rather than producing a
clean build of an unrunnable binary. Both are searched because on this family
the C library belongs to the image rather than to the chip — the vendor drop
ships the whole MI set twice, one build needing `libc.so.0` and one `libc.so.6`.

A **musl** image is a third real case, and the search does not cover it yet: an
OpenIPC output carries `arm-buildroot-linux-musleabihf`, so `build.sh` reports
no sysroot and exits even though the cross-compiler beside it is the right one.
Build standalone against such a tree until a musl tuple joins that list.

The HAL on its own, which needs no raptor tree:

```sh
make -C raptor-hal PLATFORM=INFINITY6C \
    CROSS_COMPILE=arm-openipc-linux-musleabihf- \
    SYSROOT=/path/to/openipc/output/staging
```

The ABI conformance check, which needs a vendor SDK and an ARM compiler but no
sysroot — `maruko` is this family's ISP header set:

```sh
make -C raptor-hal/tests abi-check-i6c \
    SIGMASTAR_SDK=/path/to/ssc377/release/include \
    CROSS_COMPILE=arm-openipc-linux-musleabihf-
```

## Where the ABI came from

`raptor-hal/sigmastar-headers/infinity6c/DERIVED.md` records how each structure
layout was established, module by module, and which ones have been through it.
The short version: `mi_isp.ko` names its own fields in its validation
predicates, the ioctl thunks state their payload sizes and id-word counts
outright, and the tuning API declares each module's payload length in the
wrapper's literal pool. Where a vendor header and a shipped blob disagree on a
size, **the blob wins** — it is what does the copying, and trimming a structure
to match a header has already cost this project working audio capture once.
