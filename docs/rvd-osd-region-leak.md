# The OSD region leak: rvd never gives a slot back

Open. Pre-existing, reproduced on the SSC377QE board, unfixed. Written up so
the next person to hit it does not spend the afternoon reading their config.

## What it looks like

The overlay quietly stops. Not all of it — the elements that were already on
screen keep updating, and the ones you just added never appear. Nothing in the
config is wrong, `raptorctl config get` reads back exactly what you set, and rod
reports the element as rendering. The only trace is in the log:

    hal_osd.c:598: osd: all 16 region slots in use
    rvd_osd.c:229: osd_create_region(s0/bottom_left) failed: -12

and it repeats every five seconds, forever, because the retry path never gives
up. `S95raptor restart` clears it and the overlay is whole again, which is what
makes this so easy to misfile as a transient.

## What is actually happening

Two independent facts collide.

**rvd's region table is per stream; the HAL's pool is not.** `RVD_OSD_MAX_REGIONS`
in `rvd/rvd.h` is 16, and `st->osd_regions` is `[RVD_MAX_STREAMS][16]` — rvd
believes it may hold 16 regions *per stream*. `I6C_OSD_REGION_MAX` in
`raptor-hal/src/infinity6c/infinity6c_state.h` is also 16, but `st->osd[]` there
is a single flat array indexed by the handle, shared by every stream and every
group. Two streams of five elements plus a privacy cover each is twelve of the
sixteen before anything has gone wrong. The matching numbers are a coincidence
that reads like a design.

**Nothing ever frees a region whose producer is gone.** When rod exits, its SHM
objects vanish, and the staleness sweep in `rvd_osd_check` notices within a
second:

    RSS_INFO("osd %d/%s: producer gone, clearing", s, reg->name);

It closes the SHM, blanks the local buffer, pushes the blank, and stops there.
`reg->active` stays true and `reg->hal_handle` stays valid, so `alloc_region_slot`
will not reuse the rvd slot and `hal_osd_destroy_region` is never called for the
HAL one. The only path that destroys regions is `rvd_osd_deinit_stream`, which
runs when rvd tears the stream down — that is, when rvd exits.

That much is harmless as long as rod comes back with the *same* element names:
`try_open_shm` sees the SHM reappear, logs `producer restarted, reopening`, and
reuses the handle it already holds. The leak needs rod to come back with
*different* names. Then `scan_new_shm` finds them, does not recognise them,
takes fresh rvd slots and asks the HAL for fresh handles, while the previous
generation's handles are still held by regions whose SHM will never come back.
Each generation of names costs a full set of HAL slots, and the fourth or fifth
one runs the pool dry.

## How to reproduce it

Rename the overlay elements and restart rod, twice, under a long-lived rvd:

    raptorctl config set osd.top_left template '%time%'
    /etc/init.d/S95raptor restart rod     # generation 1: top_left ...
    # edit the sections to different names
    /etc/init.d/S95raptor restart rod     # generation 2: new handles

The migration from position-carrying element names (`[osd.timestamp]`,
`[osd.uptime]`) to slot names (`[osd.top_left]`, `[osd.top_right]`) is exactly
this, which is how it surfaced: every element in the file changed name at once,
so one migration burned a whole generation.

## What the fix has to do

Reaping the region when the producer goes is the obvious move and it is wrong on
its own — rod restarting is normal, and destroying the HAL region every time
would tear the overlay down and rebuild it on every rod bounce, which is both
visible and a lot of MI churn for nothing. The distinction that matters is
"producer restarted with this element" versus "this element is gone", and rvd
cannot tell them apart from a missing SHM alone; it has to wait.

So: keep the current blank-and-hold behaviour, and add a deadline. A region
whose SHM has been absent for longer than a few rod restart intervals is
retired — `osd_show_region(..., 0)`, `osd_unregister_region`, `osd_destroy_region`,
free the local buffer, clear `active` and the name, and leave the rvd slot for
`alloc_region_slot` to hand out again. The existing `no_update_ticks` counter is
the wrong one to hang this on (it counts clean ticks on a *live* SHM); the
staleness sweep needs a counter of its own, reset by `try_open_shm` on a
successful reopen.

Two smaller things worth doing in the same change:

- **Make the two limits agree, or stop pretending they are separate.** rvd
  budgeting 16 per stream against a 16-region global pool is the part that
  turns a slow leak into a hard wall. Either the HAL exports its capacity and
  rvd respects it, or `RVD_OSD_MAX_REGIONS` is documented as a per-stream
  bound that the backend may refuse well short of.

- **Stop the retry storm.** `create_region` failing clears `reg->name`, so
  `scan_new_shm` does not recognise the element next tick and tries again, at
  0.2 Hz, forever. A pool-exhaustion failure is not going to fix itself while
  the pool is still full; back off, and say once that the overlay is short of
  slots rather than logging the same MI error until the box reboots.

## Where the code is

- `raptor/rvd/rvd_osd.c` — `alloc_region_slot`, `create_region`,
  `scan_new_shm`, the staleness sweep in `rvd_osd_check`, `rvd_osd_deinit_stream`
- `raptor/rvd/rvd.h` — `RVD_OSD_MAX_REGIONS`, `osd_regions[][]`
- `raptor-hal/src/infinity6c/hal_osd.c` — `hal_osd_create_region` (the slot
  scan and the error at line 598), `hal_osd_destroy_region`
- `raptor-hal/src/infinity6c/infinity6c_state.h` — `I6C_OSD_REGION_MAX`

The same shape exists in the other backends: the pool is per-backend and flat
everywhere, so the leak is not i6c-specific even though that is where it was
caught.
