# Per-frame memory-work inventory (bus-contention reduction)

Consolidated from three read-only Sonnet sweeps (hwvideo +
video.cpp; sdl2/NTSC/GL; engine + main.cpp). Goal: cut the ~3 MB/frame the CPU
render pipeline pushes through the shared L2 on the Pi 2 (A7), to free 60fps
headroom and reduce the memory-bus contention that starves the audio thread.
Two categories: (A) eliminable per-frame memory work (clears/copies/allocs
that are redundant or avoidable), (B) random access reworkable into streaming.
All "verifiable" items are bit-identical and gated by the existing frame /
oracle checksum harnesses (tools/road_tiles, tools/sprite_bench, tools/ntsc_bench).

Context note: the audio underruns are primarily latency/scheduling, not
bandwidth (see tools/AUDIO_PERF_PLAN.md) - the direct audio fix is RT priority
(R2) + a robust consumer (R3). This inventory is COMPLEMENTARY: cutting
per-frame bus traffic reduces the contention the audio thread misses on, and
buys general 60fps headroom. The two efforts stack.

## Status

**Tier 1 has shipped** - commit `c94f086` ("Tier 1: cut per-frame memory work / bus
contention (output-identical)") implements all five items below. They are retained here as
the record of what was changed and why, not as outstanding work.

Tiers 2 and 3 remain open. Treat this file as a backlog for that work; it is indexed
from `tools/BACKLOG.md` alongside the other open items.

## Tier 1 - SHIPPED in c94f086

| # | Item | File | Bytes/frame | Risk |
|---|---|---|---|---|
| 1 | **Road background sky-fill writes rows the road foreground fully overwrites the same frame.** `road_fg_rows` mask is already computed (video.cpp:278) and already used to skip dead tile rows, but NOT applied to the road's own background fill. Skip the fill_n when road_fg_rows[yy] set. | hwroad.cpp:376 (lores), :653 (hires); wire like video.cpp:284,287 | up to ~143 KB (320x224) / **~724 KB (hires widescreen)** | Low. Overlap already proven+measured by check_foreground_rows/rows_both (hwroad.cpp:919-985). |
| 2 | **ohud per-frame heap alloc:** `new uint8_t[8]`/`delete[]` every gameplay tick for 8 local bytes. Use a stack array. Removes malloc/free bookkeeping that contends with the audio allocator. | ohud.cpp:249, :282 | tiny, but removes per-frame allocator traffic/locking | None. Behaviour-identical. |
| 3 | **Dead YM2151 clear_buffer** then unconditional overwrite (the original exemplar). Remove the clear. | ym2151.cpp:2020 | ~1-3 KB | None. |
| 4 | **Road/sprite-RAM per-frame swaps are real element copies** (`ram`<->`ramBuff`), should be a pointer/index flip -> eliminable to zero. | hwroad.cpp:304-318; hwsprites.cpp:172-185 | ~32 KB combined | Low-med (re-point write16/32 accessors + ram_buffer() callers). |
| 5 | **otraffic jump-table gather:** walks up to ~122 sprite entries and dereferences a 64B `oentry` per entry just to test one traffic bit, when <=8 are traffic. Carry an is-traffic flag into the small record (populated in a pass that already walks jump_table sequentially); early-out when max_traffic==0. | otraffic.cpp:~649-741 (+ osprites populate) | ~122x64B scattered -> <=8 | Low (data-source change, no compute change); spans 2 files. |

## Tier 2 - conditional (confirm first)

| # | Item | File | Bytes/frame | Condition |
|---|---|---|---|---|
| 6 | **Scanlines double-pass:** `apply_scanlines()` is a full second pass over the output after the NTSC blit already wrote it. Fold the odd-row dim into the blit's clamp-convert-store (write once). | rendersurface.cpp:1172-1218, called :1497 | **~674 KB** | ONLY if config.video.scanlines != 0 (repo default off). NEED TO CONFIRM the user's config. Medium risk (several vector store sites). Checksum-verifiable. |

## Tier 3 - understood, do NOT touch without a measure-first redesign

- **Sprite depth bucket-sort scatter** (osprites.cpp:366,432, `sprite_order[0x2000]`): ~8-16 KB/frame, ~100+ scattered writes. It IS the arcade's depth sort - behaviour/timing risk on a cycle-accurate port. Awareness only.
- **Tile-graphics fetch gather** (hwtiles.cpp:395,559, 32B out of 256 KB `tiles[]` per cell): content-dependent locality; reordering the draw loop doesn't reliably help. Profile cache-miss rate on real levels before any rework.
- **hires NTSC table gather** (snes_ntsc.cpp): already mitigated by the S3/S4 run-length ladder (flat/CK/PK). Do not re-chase.
- **lores NTSC gather** (snes_ntsc.cpp:380-431) and **oroad do_road** (~10-15 KB/frame, already sequential): dormant (hires default) / little to gain.

## Latent traps - guard/comment, no per-frame cost today

- ABGR byte-swap scratch-copy path in gl_backend.hpp (dead on this config; ~658 KB/frame if `set_game_pixel_format(ABGR)` ever wired on a driver lacking swizzle). Add a guard/comment.
- `apply_crt_bloom()` full-frame malloc+memcpy (call sites commented out; fix to a 2-3-row scratch before re-enabling).
- `Video::prepare_frame` disabled-video fallback scalar clear loop (video.cpp:259-265) -> memset; dead in normal play.
- omusic per-frame idempotent text re-blits (music-select screen only); copy_bg_color 1 KB/frame (rarely fires while driving).

## Recommended order

Tier 1 as a batch (each output-identical, each checksum-gated): #1 (biggest single win, mask already exists) + #2/#3 (free, and #2 helps the audio allocator directly) + #4 + #5. Then Tier 2 #6 IF scanlines are in the shipping config. Measure the aggregate frame-time / bus effect on the A7 after Tier 1. Tier 3 only with a dedicated measure-first pass.

Estimated Tier 1+2 ceiling: road sky-fill (~724 KB) + scanlines (~674 KB, if on) ~= up to ~1.4 MB/frame of the ~3 MB, plus the RAM-swap/alloc/traffic wins - a material dent in the bus contention.
