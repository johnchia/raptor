# Sensor mode files, HiSilicon gen5 (HiMPP V5.0, hi3516cv6xx)

One file per sensor OpenIPC's `load_hisilicon` can load. `raptor-hal`'s
`src/hisi_v5/hisi_sensor.c` reads them; `make install` puts them in
`/etc/sensors`, which is where the gen4 backend already looks.

## Why raptor carries these at all

A gen4 OpenIPC image ships `/etc/sensors` from the vendor's `osdrv`
package. **A cv6xx image ships nothing**: `hisilicon-opensdk` builds the
sensor libraries from source and has no config directory. Without these
files there is no lane map, no Bayer order and no sensor geometry anywhere
on the board, and the pipeline cannot be configured at all.

## Where the numbers come from

The Hi3516CV610 PQTools configs, `configs/<sensor>/<sensor>_<mode>.ini` in
the 1.0.1.0 drop. Those are already in the V5 dialect — they name
`ot_isp_pub_attr`, `combo_dev_attr_t` and `ot_vi_dev_attr` fields directly —
so every value here is the vendor's own, restated in the gen4 INI dialect
that `hisi_sensor.c` parses. Read from the SDK, not copied out of it.

Per sensor that means: `DevRect_w`/`DevRect_h` from `sns_size`,
`Isp_Bayer` from `bayer_format`, `lane_id` from `mipi_attr.lane_id`,
`i2c_dev` from the `[isp.0]` block, and the whole `[vi_dev]` block from
`[vi_dev.0]`.

## Per-die overrides

`[<section>.<die>]` beats `[<section>]` for the same key, with `<die>`
derived from `/proc/umap/sys`'s part name — `hi3516cv608`, `hi3516cv610`.

This exists because **the die caps the geometry**. `clk_cfg.c` clocks the
CV608's ISP at 148.5 MHz against the CV610's 198, and the vendor ships a
separate `_608` config for every sensor running it at 2304x1296 where the
CV610 config runs 4M or 5M. The default block here is the CV610 mode and
`[vi_dev.hi3516cv608]` is the two lines that change.

## lane_id is board wiring

It is not a property of the sensor. The vendor's own configs disagree for
the same part family — `sc4336p` is `0|2` where `gc4023` beside it is `0|1`
— because they describe different reference boards. A wrong lane map gives
a MIPI receiver that never completes a line: `/proc/umap/mipi_rx` shows a
detected width that does not match the configured one, and
`/proc/umap/vi`'s frame rate stays 0. If a board is wired differently,
this is the field to change.

## Coverage

| file | vendor config | notes |
| --- | --- | --- |
| `gc4023.ini` | `gc4023_4M30.ini`, `gc4023_3M30_608.ini` | |
| `os04d10.ini` | `os04d10_4M30.ini`, `os04d10_3M30_608.ini` | the bench board's sensor |
| `sc431hai.ini` | `sc431hai_4M30.ini`, `sc431hai_3M30_608.ini` | four lanes |
| `sc4336p.ini` | `sc4336p_4M30.ini`, `sc4336p_3M30_608.ini` | lanes `0\|2` |
| `sc450ai.ini` | `sc450ai_4M30.ini`, `sc450ai_3M30_608.ini` | 2688x1520 |
| `sc500ai.ini` | `sc500ai_5M30.ini`, `sc500ai_3M30_608.ini` | 2880x1620 |
| `imx307.ini` | none | **unverified** — see below |
| `os02m10.ini` | none | **unverified** — see below |

`imx307` and `os02m10` are OpenIPC additions rather than vendor ones: they
have libraries on the image but no PQTools config, so their values come
from the gen4 vendor INIs for the same parts (`smtsec_imx307_i2c_*.ini`,
`sp2308_i2c_1080p.ini`) plus this family's lane convention. Treat the lane
map and the Bayer order in those two as starting points, not as measured.

`libsns_sp2308.so` on this image exports `g_sns_os02m10_obj` — it is an
os02m10 library under another name — so `os02m10.ini` serves both and
`hisi_sensor.c` finds the symbol by scanning when the derived name misses.
