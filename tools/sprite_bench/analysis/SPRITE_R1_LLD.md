# R1 low-level design: de-glue (R1a) and specialise (R1b) the LINEBUF sprite renderer

Status: 2026-09-05, branch `sprite-rework`. Design deliverable only; no renderer code changed.
Implements section 3 of `SPRITE_REPLAN_FABLE.md` against the findings of
`PERFORMANCE_MEASUREMENTS.md` campaign 6, in the vocabulary of `SPRITE_IMPLEMENTATION_SPEC.md`.
Line numbers refer to `src/main/hwvideo/hwsprites.cpp` (1711 lines) and `hwsprites.hpp` (373 lines)
at HEAD; re-anchor on the quoted code if the file has drifted. Every code block below is the
text the implementer writes, modulo comments.

---

## 0. Frame

### 0.1 What R0 measured and what R1 does about it

R0 (`PERFORMANCE_MEASUREMENTS.md` campaign 6) split `draw_sprite_fast`'s time as DECODE 55-60%, OTHER
(glue) 24-29%, COPY 12-16%, and showed the S4-vs-shipped deficit is constant per *source row*
(CoV 0.09) not per pixel (CoV 0.25). Its objdump named five mechanisms:

| R0 item | mechanism at HEAD | fixed in |
|---|---|---|
| 1 | 9x `mul x,x,x13` per source word: `fill = v * 0x0001000100010001` per nibble (`decode_row` 413, 426) | R1b sec. 4.2 |
| 2 | `decode_row` and `replay` are real `bl`s from `draw_sprite_fast` (780-781), 3 pairs after unrolling | R1a sec. 3.2 |
| 3 | `r.color` (`SpriteRec` +52) reloaded from memory per nibble after each `memcpy` store | R1a sec. 3.1 |
| 4 | `sched()` called twice per source row (386-387: `sched` and `wsum` fold to the same body) | R1a sec. 3.3 |
| 5 | `bl memcpy` per destination row in `replay_colour_span`'s scalar D path (638), the `n < 8` case on NEON | R1b sec. 4.3 |

Plus the per-row glue the replan listed (section 1.3) and R0's OTHER bucket confirms: eight
`run[]` byte loads per word (415/429), `int64` bounds select-and-compare per word (393-394), the
`(d*zoom) >> 9` multiply per destination row (772-775), `config.s16_width`/`s16_height`
reloads and `std::min` in `replay` (487, 501), the S3 shadow-prefetch scan run for every sprite
(500-518), and the `glow_flush` dependent-load chain (735-744).

DECODE is per-nibble work (~1,500 cycles per source row at light load = ~15 cycles per source
nibble, from R0's 28 cntvct ticks/srcrow at 19.2 MHz and 1 GHz), so the highest-value change in
R1 is the **nibble body**: at HEAD it issues three memory operations (`ldr` color reload, `ldrb`
run byte, `str` fill) plus a 3-cycle `mul` per nibble on a core whose single load/store pipe is
the binding constraint (`MICROARCH_A7_A53.md` section 1). After R1b it issues one store and 5-6
single-cycle ALU ops. R1a delivers the memory-op part (hoisted `color`, one 8-byte `run` load
per word); R1b the `mul` and the store width.

### 0.2 Hard requirements (design to these; every change is a pure performance refactor)

1. **Bit-identical output** to `render_px` at both glow settings. The S0 harness must pass
   *unchanged* (source untouched): `selftest`, `unit` (s1a s1b s1c s2a s2b s2c s3 s4a s4b),
   `fuzz` (both replay modes x both glow values x four modes, plus `--negctrl`), `replay`
   (glow-on exact, glow-off F1/F2 classifier, zero unclassified), `golden`
   (`golden/hires_nw_attract20.txt`), and S0.8 whole-frame `CB_FRAME_CKSUM` equality with the
   `SPRITE_REF_PX` build at both glow settings in all four modes.
2. **Preserved exactly:** the fallback envelope (`render` 264: `ydelta == -1 || zoom <
   SPR_FAST_ZOOM_MIN || pend > SPR_PEND_MAX`) and `draw_sprite_px`; the glow rule (one deferred
   bit-12 clear at `lo-1` per maximal visible colour run iff `lo > cx0`, the `rtl`
   shadow-neighbour suppression, the `shadow_rows` filter, flush at sprite end and at ring-full);
   `shadow_rows` per-row semantics and `dest_clean`; the record/emit rules (drop zero-length,
   merge equal type, GAP sentinel); `SPR_*` constants; `sched()`'s table contents.
3. **A53-first; the NEON span code is untouched.** `replay_colour_span`'s `n >= 8`
   `vld1q_u16`/`vst1q_u16` loops (607-632) and `replay_shadow_span`'s `vorrq_u16` loop
   (660-666) move verbatim into static helpers and are not otherwise modified. No ARMv7 NEON
   work (that is R2). Everything R1 adds is ISA-agnostic scalar C++.

### 0.3 The harness-visible surface that must keep compiling and behaving

`tools/sprite_bench/test/sprite_test.cpp` reaches these private members through
`SpriteTestAccess` (lines 124-245 of the driver). Each must keep its exact signature and its
observable effect on the members named:

| hook | private member (HEAD) | observable contract the tests rely on |
|---|---|---|
| `decode_row(hw, r, j, pend)` | `void decode_row(const SpriteRec&, int32_t j, int32_t pend)` (380) | fills `line[]`, `rec[]`, and **the member `nrec`** (read back via `nrec(hw)`; unit s1c, s2c). Called with a bare `SpriteRec{}` where only bank/addr/pitch/flip/shadow/zoom/color are set (s1c) |
| `replay(hw, r, pixels, da, db, cx0, cx1)` | `void replay(const SpriteRec&, uint16_t*, int32_t, int32_t, int32_t, int32_t)` (464) | replays **the member `rec[0..nrec)`** with the **caller's** `cx0`/`cx1` (s2c passes `cx0 = -xpos`, `cx1 = 1`), pushes onto the member glow ring without flushing |
| `draw_sprite_fast(hw, r, pixels, pend)` | (754) | whole sprite, flushes the ring at its end (s2a, s2b, s2c) |
| `glow_flush(hw)` | (735) | flushes the member ring (s2c negative control) |
| `sched(hw, zoom)`, `wsum(hw, zoom)` | (290, hpp 175) | pointers into the cached slot; `run[i]` and `wsum[m]` values (s1a) |
| `replay_colour_span(hw, base, stride, da, db, lo, hi)`, `replay_shadow_span(...)` | (603, 656) | write exactly `[lo, hi)` on rows `[da, db)`, reading the member `line[]` and the member `replay_mode` (s4b compares against the `_s1` twins for every `0 <= lo < hi <= 96`, rows 1-4, both modes) |
| `replay_colour_span_s1`, `replay_shadow_span_s1` | (682, 703, `CB_RENDER_TEST`) | verbatim S1 scalar twins; **do not touch** |
| `nrec`, `rec[i]`, `line[i]`, `set_line_at` | members (hpp 193-195) | stay members with these names/types |
| `classify_mixed`, `nibrev`, `sprites`, `x1`/`x2`, `debug_disable_clip`, `stat_*`, `SPR_*` constants, `SPR_GAP/COLOUR/SHADOW`, `SpanRec`, `SpriteRec` | | unchanged |

`emit()` (323) and `prefetch_rom_row()` (445) are *not* referenced by the driver and may be
replaced.

---

## 1. Anatomy of HEAD (what moves where)

```
render (231-273)  --dispatch-->  draw_sprite_fast (754-788)
                                   |  d_lo/d_hi/cx0/cx1 per sprite (756-761)
                                   |  prefetch_rom_row x2 (764-768)
                                   |  while (d < d_hi):                      <- int64 mul per destination row (772, 775)
                                   |     prefetch_rom_row (778)              <- int64 x2 per row
                                   |     bl decode_row (780)                 <- R0 item 2
                                   |     |  sched(), wsum() (386-387)        <- R0 item 4
                                   |     |  int64 rowbase (389); per word int64 widx + 2 compares (393-394)
                                   |     |  per nibble: r.color reload (412/425), mul fill (413/426), 8-byte memcpy store, run[] ldrb (415/429)
                                   |     |  emit() through member nrec (323-333)
                                   |     bl replay (781)                     <- R0 item 2
                                   |        config.s16_width load, base (487-488)
                                   |        S3 prefetch scan for EVERY sprite (500-518): config.s16_height, std::min, record walk
                                   |        record loop (520-586): replay_colour_span (D: bl memcpy per row when n < 8, 638) / glow push / shadow mark + span
                                   glow_flush (787)                          <- dependent load chain (737-743)
```

After R1 (both steps):

```
render  --dispatch (unchanged)-->  draw_sprite_fast
                                     d_lo/d_hi early-out; make_ctx (ONE sched(), all constants)     [sec. 2]
                                     switch select_kernel -> draw_sprite_rows_t<FLIP,MAXRUN,SHRINK>  [sec. 4.1; R1a: one non-template body]
                                        first group by HEAD's scan, then da=db; db=min(da+run[j], d_hi)   [sec. 3.5]
                                        decode_row_k<FLIP,MAXRUN>  (always_inline; locals; per-row window; one run load; class fill)
                                        replay_k                   (always_inline; prefetch scan gated on ctx.shadow; colour_span_k ladder)
                                     glow_flush (x4 unrolled)
harness wrappers: decode_row(r,j,pend) / replay(r,pixels,da,db,cx0,cx1) build a ctx and call the same kernels
```

---

## 2. Common infrastructure (introduced by R1a, reused by R1b)

### 2.1 `hwarch.hpp` additions

```cpp
// Force-inline and loop-unroll hints (SPRITE_R1_LLD.md sec. 2.1). Present on every target.
#if defined(__GNUC__) || defined(__clang__)
  #define CB_ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
  #define CB_ALWAYS_INLINE __forceinline
#else
  #define CB_ALWAYS_INLINE inline
#endif
#define CB_STR_(x) #x
#if defined(__clang__)
  #define CB_UNROLL(n) _Pragma("clang loop unroll(full)")
#elif defined(__GNUC__)
  #define CB_UNROLL(n) _Pragma(CB_STR_(GCC unroll n))
#else
  #define CB_UNROLL(n)
#endif
```

`always_inline` is honoured at `-O2` (driver) and `-O3 -Ofast` (game); GCC 14 does not unroll
fixed-count loops at `-O2` without the pragma, so the driver build would otherwise take variable
shifts in the nibble loop (correct, slower; harmless but the pragma keeps the two builds alike).

### 2.2 `SpriteCtx` -- built once per sprite

Private nested struct in `hwsprites.hpp` (after `SpanRec`, hpp 155). Plain data, no methods.

```cpp
struct SpriteCtx
{
    // scratch (pointers to this object's members, so the kernels never touch `this`)
    uint16_t*       line;       // = this->line
    SpanRec*        rec;        // = this->rec
    // schedule: ONE sched() call per sprite (R0 item 4)
    const uint8_t*  run;        // sched(zoom)
    const uint8_t*  wsum;       // run + SPR_SCHED_LEN
    // ROM
    const uint32_t* rom;        // = this->sprites
    int32_t         rowoff0;    // bank*0x10000 + addr: word index of row 0 (may be negative, S0.4)
    int32_t         pitch;      // 0..255
    // decode constants
    uint32_t        color;      // 0x800..0xFF0
    uint32_t        smask;      // shadow ? 0xFFFFFFFF : 0
    int32_t         pend;       // x2 - xpos (decode's horizontal stop)
    int32_t         zoom;       // post-halving, 0x80..0xFFF
    uint8_t         shadow, flip, rtl, glow;   // 0/1
    // destination
    uint16_t*       base;       // pixels + top*stride + xpos (nullptr when built for decode only)
    int32_t         stride;     // config.s16_width
    int32_t         top;        // r.top (shadow_rows / glow filter use y = top + d)
    int32_t         d_lo, d_hi; // destination row range (HEAD 756-757)
    int32_t         cx0, cx1;   // row-local clip window (HEAD 760-761; cx1 widened under debug_disable_clip)
    int32_t         replay_mode;
};
```

Every value the per-row/per-nibble/per-word paths read at HEAD is here: `line`, `rec`, `run`,
`wsum`, `color`, `smask`, `pitch`, `pend`, `cx0`, `cx1`, `base`, `stride`, `d_hi`, `shadow`,
`rtl`, `glow`, ROM pointer and ROM window origin (`rom`, `rowoff0`).

### 2.3 `make_ctx`

```cpp
// hwsprites.cpp, before draw_sprite_fast. Non-const: sched() may populate a cache slot.
void hwsprites::make_ctx(const SpriteRec& r, uint16_t* pixels, int32_t pend, SpriteCtx& c)
{
    c.line    = line;
    c.rec     = rec;
    c.run     = sched((uint32_t)r.zoom);            // the only sched() call for this sprite
    c.wsum    = c.run + SPR_SCHED_LEN;
    c.rom     = sprites;
    c.rowoff0 = r.bank * 0x10000 + r.addr;          // int32: |bank*0x10000 + addr| <= 3*65536 + 65790
    c.pitch   = r.pitch;
    c.color   = (uint32_t)r.color;
    c.smask   = r.shadow ? 0xFFFFFFFFu : 0u;        // HEAD 388
    c.pend    = pend;
    c.zoom    = r.zoom;
    c.shadow  = r.shadow ? 1 : 0;
    c.flip    = r.flip   ? 1 : 0;
    c.rtl     = r.rtl    ? 1 : 0;
    c.glow    = glow     ? 1 : 0;
    c.stride  = (int32_t)config.s16_width;          // HEAD 487
    c.base    = pixels ? pixels + (ptrdiff_t)r.top * c.stride + r.xpos : nullptr;   // HEAD 488 (same address; ptrdiff_t instead of size_t for a negative top)
    c.top     = r.top;
    c.d_lo    = std::max<int32_t>(0, -r.top);                                        // HEAD 756
    c.d_hi    = std::min<int32_t>(r.H, (int32_t)config.s16_height - r.top);         // HEAD 757 == 501
    c.cx0     = (int32_t)x1 - r.xpos;                                                // HEAD 760
    c.cx1     = pend;                                                                // HEAD 761
#ifdef CB_RENDER_TEST
    if (debug_disable_clip) c.cx1 += 32;                                             // HEAD 484, moved from replay (per sprite == per call: cx1 is constant per sprite)
#endif
    c.replay_mode = replay_mode;
}
```

### 2.4 Why locals, not "pass by reference": the alias-reload mechanism (R0 item 3)

R0 saw `ldr w7, [x19, #52]` (`r.color`) re-executed for every nibble. The cause: each
`std::memcpy(line + pos, &fill, 8)` is a character-typed store that may alias *any* object the
compiler cannot prove disjoint. `r` is a `const SpriteRec&` to an unknown address; `this->line`
is at an unknown address relative to it; so after every store the compiler must assume
`r.color` may have changed and reload it. The same applies to `r.shadow`, `r.flip`, the `run`
pointer's target bytes, and the member `nrec` that `emit()` round-trips through memory.

**Passing a `SpriteCtx&` does not by itself fix this** -- `c.color` read after a `memcpy` store
has exactly the same problem. What fixes it is that every kernel **copies the fields it uses into
locals at entry**, and those locals never have their address taken. An automatic variable whose
address never escapes is not in the compiler's memory model at all (it is an SSA value), so no
store can alias it and it stays in a register for the whole row. The hot scalars `pos`, `words`,
`prev`, and the record count `n` are likewise locals; `emit_l` (sec. 2.5) takes and returns `n`
by value so its address is never formed. `std::memcpy(&runs, run + 8*words, 8)` and
`std::memcpy(p, &v4, 8)` *do* name the address of a local, but GCC/Clang/MSVC lower fixed-size
`memcpy` to a plain load/store before alias analysis, so the local remains an SSA value (R0
confirmed the HEAD store is already a plain `str`).

The context struct therefore exists to (a) compute each constant once per sprite instead of per
row, (b) give the kernels a single argument, and (c) keep `this` out of the kernels entirely
(`decode_row_k` is `static`). It is a convenience, not the alias fix; the locals are.

### 2.5 `emit_l` -- `emit()` on a local count

Replaces `hwsprites::emit` (323-333; delete it and its declaration hpp 201). Same three rules,
same order, value in / value out.

```cpp
static CB_ALWAYS_INLINE int32_t emit_l(hwsprites::SpanRec* rec, int32_t n, uint8_t type, int32_t pos)
{
    if (n > 0 && rec[n - 1].pos == (uint16_t)pos) n--;      // drop a zero-length record
    if (n > 0 && rec[n - 1].type == type) return n;          // merge equal type
    rec[n].pos = (uint16_t)pos; rec[n].type = type; rec[n].flags = 0;
    return n + 1;
}
```

(A private static member, as declared in sec. 2.6: `SpanRec` is private, so a free function
would need it made reachable.)

### 2.6 Kernel declarations (`hwsprites.hpp`, private, next to `draw_sprite_fast` hpp 285)

```cpp
    struct SpriteCtx { /* sec. 2.2, defined in full here (it needs SpanRec, hpp 155) */ };

    // R1: per-sprite kernel selection. K_GEN is the generic path (runtime flip, 8-byte fill,
    // closed-form vertical loop) used for the run-0 regime zoom >= 0x200.
    enum Kernel : uint8_t { K_F0R2, K_F1R2, K_F0R4, K_F1R4, K_GEN };
    static Kernel select_kernel(int32_t zoom, int32_t flip);

    void make_ctx(const SpriteRec& r, uint16_t* pixels, int32_t pend, SpriteCtx& c);

    static CB_ALWAYS_INLINE int32_t emit_l(SpanRec* rec, int32_t n, uint8_t type, int32_t pos);
    template <int MAXRUN> static CB_ALWAYS_INLINE void store_fill(uint16_t* p, uint32_t v);
    static CB_ALWAYS_INLINE void prefetch_rom_row_k(const SpriteCtx& c, int32_t jrow);

    // decode: returns the record count (the caller keeps it in a local; only the harness
    // wrapper decode_row() stores it into the member nrec).
    template <int FLIP, int MAXRUN>
    static CB_ALWAYS_INLINE int32_t decode_row_k(const SpriteCtx& c, int32_t j);

    CB_ALWAYS_INLINE void replay_k(const SpriteCtx& c, int32_t nrec, int32_t da, int32_t db);

    static CB_ALWAYS_INLINE void colour_span_k(const uint16_t* line, uint16_t* base, int32_t stride,
                                               int32_t da, int32_t db, int32_t lo, int32_t hi, int mode);
    static CB_ALWAYS_INLINE void shadow_span_k(uint16_t* base, int32_t stride,
                                               int32_t da, int32_t db, int32_t lo, int32_t hi);

    // the vertical loop; one instantiation per Kernel; called once per sprite (a real call is fine)
    template <int FLIP, int MAXRUN, bool SHRINK>
    void draw_sprite_rows_t(const SpriteCtx& c);
```

`FLIP` is `0`, `1`, or `2` (= read `c.flip` at run time; used only by `K_GEN`). In **R1a** the
templates do not exist yet: the same functions are written non-template (`decode_row_k(c, j)`,
`draw_sprite_rows(c)`) with the runtime substitutions listed in sec. 3.0; R1b adds the
parameters.

Member function templates declared in the class and defined in `hwsprites.cpp` are fine because
they are instantiated only there. `always_inline` on the in-class declaration is what GCC needs;
the definitions carry no attribute.

### 2.7 The harness wrappers (keep the HEAD signatures; bodies become thin)

```cpp
// HEAD 380-437 -> wrapper. Only the driver calls this (SpriteTestAccess::decode_row).
void hwsprites::decode_row(const SpriteRec& r, int32_t j, int32_t pend)
{
    SpriteCtx c;
    make_ctx(r, nullptr, pend, c);
    switch (select_kernel(r.zoom, r.flip)) {          // R1a: nrec = decode_row_k(c, j);
        case K_F0R2: nrec = decode_row_k<0, 2>(c, j); break;
        case K_F1R2: nrec = decode_row_k<1, 2>(c, j); break;
        case K_F0R4: nrec = decode_row_k<0, 4>(c, j); break;
        case K_F1R4: nrec = decode_row_k<1, 4>(c, j); break;
        default:     nrec = decode_row_k<2, 4>(c, j); break;
    }
}

// HEAD 464-587 -> wrapper. Only the driver calls this (SpriteTestAccess::replay).
void hwsprites::replay(const SpriteRec& r, uint16_t* pixels, int32_t da, int32_t db, int32_t cx0, int32_t cx1)
{
    SpriteCtx c;
    make_ctx(r, pixels, cx1, c);
    c.cx0 = cx0;                    // the caller's window, exactly as HEAD used its arguments
    c.cx1 = cx1;
#ifdef CB_RENDER_TEST
    if (debug_disable_clip) c.cx1 += 32;    // HEAD 484
#endif
    replay_k(c, nrec, da, db);      // member nrec: set by the decode_row wrapper above
}
```

These are the only places the member `nrec` is written/read; the game path never touches it.
Both wrappers stay compiled unconditionally (30 bytes each) so the header needs no new `#ifdef`.

---

## 3. Step R1a -- de-glue (gate: harness green; in-situ <= 1.00x shipped in every band)

### 3.0 Shape of R1a

R1a writes the kernels of sec. 5 **once, non-templated**, with these substitutions:

| sec. 5 text | R1a form |
|---|---|
| `template <int FLIP, int MAXRUN> decode_row_k` | `static int32_t decode_row_k(const SpriteCtx& c, int32_t j)`; `const bool flip = c.flip != 0;` |
| `store_fill<MAXRUN>(p, v)` | HEAD's fill, verbatim: `const uint64_t fill = (uint64_t)(uint16_t)v * 0x0001000100010001ULL; std::memcpy(p, &fill, 8);` |
| `template <..., bool SHRINK> draw_sprite_rows_t` | `void draw_sprite_rows(const SpriteCtx& c)`; the group step reads `if (c.zoom < 0x200)` at run time (sec. 3.5) |
| `select_kernel` switch in `draw_sprite_fast`/`decode_row` | direct calls |
| `colour_span_k` ladder (sec. 4.3) | HEAD 635-653 verbatim for `n < 8` (the `memcpy`/D' chunk loop) |

Everything else in sec. 5 is R1a. Items 3.1-3.8 below say what each part changes and why it
cannot change output.

### 3.1 Per-sprite context, kernels on locals (R0 item 3)

**Before:** `decode_row(const SpriteRec& r, int32_t j, int32_t pend)` (380) reads `r.zoom`,
`r.shadow`, `r.bank`, `r.addr`, `r.pitch`, `r.flip`, `r.color` from `r` and `nrec`, `line`,
`rec`, `sprites` from `this` inside its loops; `replay(...)` (464) reads `config.s16_width`,
`config.s16_height`, `r.H`, `r.top`, `r.rtl`, `glow`, `replay_mode`, `nrec`, `rec`.

**After:** `make_ctx` (sec. 2.3) computes all of them once per sprite; `decode_row_k` /
`replay_k` (sec. 5.2, 5.4) copy what they use into locals at entry (sec. 2.4).

**Invariant:** every field is a pure function of `(r, pixels, pend, this->{x1, glow,
replay_mode}, config)` which HEAD also treats as constant across the sprite (nothing in
`draw_sprite_fast`'s loop writes any of them), so a value computed once equals the value HEAD
recomputed each time.

**Codegen check:** in the disassembly of the row-loop function, the unrolled COLOUR body (eight
consecutive line stores) contains **no load instruction at all** (`ldr`, `ldrb`, `ldrh` with an
immediate offset = a struct-field reload). The only loads per word are the ROM word, the 8-byte
`runs` word, and (GAP/SHADOW classes) one `wsum` byte.

### 3.2 Force-inline `decode_row_k` and `replay_k` (R0 item 2)

**How:** `CB_ALWAYS_INLINE` on the declarations (sec. 2.6), not a restructure. The row loop
(`draw_sprite_rows` in R1a; the five `draw_sprite_rows_t` instantiations in R1b) is an ordinary
function called once per sprite from `draw_sprite_fast`'s `switch` -- one `bl` per *sprite* is
fine; the R0 problem was `bl` per *source row*. `colour_span_k`/`shadow_span_k`/`emit_l`/
`store_fill`/`prefetch_rom_row_k` are likewise `always_inline`.

Why the attribute rather than trusting `-O3`: R0 shows GCC declined to inline both functions
at `-O3 -Ofast` (they are large, called from an unrolled loop, and members), and the driver
builds at `-O2` where the heuristics are stricter still.

**Risk:** code size. Estimate per instantiation **[E]**: decode ~180 instructions (COLOUR 8x5,
MIXED 8x12, head/classify ~40) = ~0.7 KB; replay ~215 instructions (prefetch scan, record loop,
NEON copy + ladder, glow, shadow) = ~0.9 KB; loop ~0.2 KB; total ~1.8 KB. R1a: one copy, ~1.8 KB.
R1b: five copies, ~9 KB, of which a hi-res frame touches four (~7 KB). L1I is 32 KB on the A53
(`MICROARCH_A7_A53.md`, `test mule` notes); the tile/road renderers and NTSC filter run in other
phases of the frame, so the sprite hot set has the cache to itself but the A53's simple front-end
is sensitive to footprint. R1a is measured on its own precisely so R1b's growth is attributable;
the fallback if R1b's light band does not beat R1a's is to drop `always_inline` from `replay_k`
only (one `bl` per source row, -3.5 KB), keeping `decode_row_k` inlined.

**Codegen check:** `grep -E '\bbl\b'` over each row-loop function body lists only
`hwsprites::glow_flush()` (from `glow_push`'s ring-full path). No `bl` to `decode_row`,
`replay`, `sched`, `wsum`, `memcpy`, `memset`. `decode_row_k`/`replay_k` have no out-of-line
symbol in the binary.

### 3.3 One `sched()` per sprite (R0 item 4)

**Before:** `decode_row` 386-387, two calls per source row (`sched(r.zoom)`, `wsum(r.zoom)`;
each a `unique_ptr` load, null test and return).

**After:** `make_ctx` calls `sched()` once; `c.wsum = c.run + SPR_SCHED_LEN` (the layout
`wsum()` already encodes, hpp 175). `sched()` itself is unchanged except for one additional
assertion (sec. 4.1). **Invariant:** the slot pointer for a given zoom is stable for the object's
lifetime (populated once, never freed), so one lookup equals N.

### 3.4 Eight run bytes as one load

**Before:** `pos += run[words * 8 + k]` per nibble (415, 429): eight `ldrb`.

**After:** per word, before the class branch that needs it,
`uint64_t runs; std::memcpy(&runs, run + (size_t)words * 8, 8);` then
`pos += (int32_t)((runs >> (8 * k)) & 0xFFu)` per nibble -- with the loop unrolled these are
eight `ubfx` immediates. Little-endian byte `k` of the load is `run[8*words + k]`; on a
big-endian host the shift would have to be `56 - 8*k` -- guard with
`static_assert(std::endian::native == std::endian::little)` (C++20/23 is already required by
the driver) or a `#if` that keeps the byte loads there. Bounds: `words <= SPR_WMAX - 1 = 255`, so
the highest byte read is index `2047 = SPR_SCHED_LEN - 1`, inside the table. Alignment: the slot
is `new uint8_t[]` (16-aligned) and `8*words` is a multiple of 8; irrelevant on A64 but it is what
will let A32 use `ldrd` in R2.

**Invariant:** same eight byte values in the same order.

**Codegen check:** one `ldr xN, [xRun, xW]` (or `[xRun, xW, lsl #3]`) per word; zero `ldrb` in
the COLOUR body.

### 3.5 Vertical loop via `run[j]` -- no multiply per destination row, no division

**Before (HEAD 770-782):**

```cpp
int32_t d = d_lo;
while (d < d_hi) {
    const int32_t j = (int32_t)(((int64_t)d * r.zoom) >> 9);
    const int32_t da = d; d++;
    while (d < d_hi && (int32_t)(((int64_t)d * r.zoom) >> 9) == j) d++;
    const int32_t db = d;
    prefetch_rom_row(r, j + SPR_ROM_PF_ROWS);
    decode_row(r, j, pend);
    replay(r, pixels, da, db, cx0, cx1);
}
```

One multiply per destination row (plus one per group), `int64`.

**After (the body of `draw_sprite_rows[_t]`, sec. 5.6):**

```cpp
int32_t d  = c.d_lo;
int32_t j  = (d * zoom) >> 9;                       // int32: d <= 511, zoom <= 0xFFF -> < 2^21
int32_t da = d; d++;
while (d < d_hi && ((d * zoom) >> 9) == j) d++;     // first group: HEAD's own scan, <= run[j] <= 4 steps
int32_t db = d;
for (;;) {
    prefetch_rom_row_k(c, j + SPR_ROM_PF_ROWS);
    const int32_t nrec = decode_row_k<FLIP, MAXRUN>(c, j);
    replay_k(c, nrec, da, db);
    if (db >= d_hi) break;
    da = db;
    if (!SHRINK) {                                  // zoom < 0x200: recurrence
        j++;
        db = std::min<int32_t>(da + (int32_t)run[j], d_hi);
    } else {                                        // zoom >= 0x200 (run-0 regime): HEAD's scan, unchanged
        j = (da * zoom) >> 9;
        d = da + 1;
        while (d < d_hi && ((d * zoom) >> 9) == j) d++;
        db = d;
    }
}
```

`SHRINK` is `c.zoom >= 0x200` (a per-sprite runtime test in R1a; a template constant in R1b).

**Proof that `(j, da, db)` sequences are identical for `zoom < 0x200`.** Notation from
`SCHEDULE_VERIFICATION.md`: `N(i) = ceil(512*i/zoom)`; **C1** `run[i] = N(i+1) - N(i)` (exhaustive
over every zoom, and unit s1a); **C2** `src(d) = (d*zoom) >> 9 = max{i : N(i) <= d}`, so `src` is
monotone non-decreasing and `src(d) == j  <=>  N(j) <= d < N(j+1)`; **C4** the horizontal and
vertical schedules are the same function, so the horizontal table `run[]` gives the vertical
group sizes; **C5** `run[i] >= 1` for every `i` when `zoom < 0x200` (`512/zoom > 1`).

HEAD's groups are the maximal runs of consecutive `d` in `[d_lo, d_hi)` with equal `src(d)`,
i.e. `[N(j), N(j+1)) ∩ [d_lo, d_hi)` for each `j` that has a non-empty intersection, visited in
increasing `j`.

*First group.* Identical code to HEAD, so `(j_0, da_0, db_0)` are identical; in particular
`db_0 = min(N(j_0 + 1), d_hi)` because the scan stops at the first `d` with `src(d) != j_0`,
which by C2 is `d = N(j_0 + 1)`.

*Induction.* Suppose group `g` has `db_g = min(N(j_g + 1), d_hi) < d_hi`. HEAD's next group
starts at `d = db_g = N(j_g + 1)` with `j_{g+1} = src(N(j_g + 1))`. By C5, `run[j_g + 1] >= 1`,
so `N(j_g + 1) < N(j_g + 2)`, hence by C2 `src(N(j_g + 1)) = j_g + 1`: HEAD's `j` advances by
exactly one, which is the recurrence's `j++`. HEAD's group ends at the first `d >= N(j_g + 1)`
with `src(d) != j_g + 1`, i.e. `d = N(j_g + 2) = N(j_g + 1) + run[j_g + 1]` (C1), clipped to
`d_hi`: `db_{g+1} = min(da_{g+1} + run[j_{g+1}], d_hi)`, which is the recurrence. Both loops stop
when `db >= d_hi`. QED.

*Table bound.* `d <= d_hi - 1 <= 511` (`H = height << hires <= 512`, `height <= 256`), so for
`zoom <= 0x1FF`, `j <= (511 * 0x1FF) >> 9 = 510 < SPR_SCHED_LEN = 2048`; `run[j]` is always in the
table. (For `zoom >= 0x200` the recurrence is *not* used: it would visit empty run-0 groups and,
under fuzz's `zoom` up to `0xFFF` with `H = 512`, index past the table; HEAD's scan stays.)

*Why no division.* The replan's `N(j0+1) = ceil(512*(j0+1)/zoom)` would put a `udiv` in
`draw_sprite_fast`; the first-group scan costs at most `run[j_0] <= 4` multiplies and is HEAD's
own proven code, and the codegen checklist stays "no `sdiv`/`udiv` anywhere in `hwsprites.o`".

*Also unchanged:* `prefetch_rom_row_k(c, j0)`, `(c, j0 + 1)` before the loop and `(c, j +
SPR_ROM_PF_ROWS)` per group, exactly HEAD 764-768/778 (prefetch has no output effect anyway).

**Codegen check:** inside the `for(;;)` of the non-SHRINK instantiations, no `mul`/`smull`
except the `pitch * j` inside the inlined `decode_row_k`/`prefetch_rom_row_k` heads and
`d * stride` row addressing inside `replay_k`; `grep -cE '\b(sdiv|udiv)\b'` over the whole
`hwsprites` object = 0.

### 3.6 Per-row ROM window instead of per-word `int64` bounds

**Before (393-394):** per word `int64 widx = flip ? rowbase + pitch - 1 - words : rowbase +
words; if (widx < 0 || widx >= SPRITES_LENGTH) break;`.

**After:** per row (sec. 5.2 part b) compute `first` and `wlimit`, then loop `while (pos < pend
&& words < wlimit)` and index `rom[widx]` with `widx += flip ? -1 : +1`.

```cpp
const int32_t rowbase = c.rowoff0 + c.pitch * j;                       // |.| < 1.4M, int32 exact
const int32_t first   = flip ? rowbase + c.pitch - 1 : rowbase;
int32_t wlimit = 0;
if ((uint32_t)first < (uint32_t)SPRITES_LENGTH)                        // first >= 0 && first < LEN, one compare
    wlimit = flip ? std::min<int32_t>(SPR_WMAX, first + 1)
                  : std::min<int32_t>(SPR_WMAX, (int32_t)SPRITES_LENGTH - first);
```

**Equivalence, case by case.** HEAD breaks at the first `words` for which `words == SPR_WMAX`
or `widx(words)` is out of range, whichever comes first, having also tested `pos >= pend` at
the same loop head. Non-flip: `widx = first + words` increases; if `first` is out of range the
first iteration breaks (`wlimit = 0`); otherwise `widx < LEN <=> words < LEN - first`. Flip:
`widx = first - words` decreases; if `first` is out of range the first iteration breaks;
otherwise `widx >= 0 <=> words <= first`, i.e. `words < first + 1`, and `widx < LEN` holds
throughout. In every case HEAD's break index equals `min(SPR_WMAX, in-range count) = wlimit`,
and the `pos >= pend` test is unchanged. The terminator `break` (434) is unchanged. `int32`
suffices: `rowoff0 in [-508, 262,397]`, `pitch*j <= 255 * 4086`, sum `< 1.4e6`.

This deliberately does **not** take the replan's alternative (extend the fallback envelope with a
"ROM footprint in range" test): hard requirement 2 says the envelope is preserved exactly, and the
per-row window buys the same per-word saving at ~6 ALU ops per row.

**Codegen check:** no compare against `#0x40000` (SPRITES_LENGTH) inside the word loop; exactly
one per row.

### 3.7 Shadow bookkeeping only when `ctx.shadow`

**Before (500-518):** the S3 prefetch scan walks every record of every sprite, loading
`config.s16_height` and computing two `std::min` first.

**After:** `if (c.shadow) { scan }` with `c.d_hi` (sec. 5.4). The `shadow_rows` marking (580-583)
is already inside the `SPR_SHADOW` branch of the record loop and needs no further gate.

**Why output cannot change.** With `r.shadow == 0`: `decode_row` classifies a nibble `SPR_SHADOW`
only under `r.shadow && n == 10` (422), the all-SHADOW word class requires `r.shadow` (403), and
`classify_mixed`'s `a` term is masked by `smask == 0` (351). So no `SPR_SHADOW` record is ever
emitted for a non-shadow sprite; the prefetch scan (which `continue`s on every non-SHADOW record,
505) issued no prefetch, and the SHADOW branch (571-585) was never entered, so `shadow_rows` was
never marked. Skipping code that provably did nothing changes nothing. (Prefetch has no
architectural effect regardless.) A debug build may assert `c.shadow || rec[k].type != SPR_SHADOW`
in the record loop.

### 3.8 `glow_flush` unrolled x4

**Before (735-744):** one load, one `and`, one store per entry, serially dependent through the
store buffer only in the sense that the in-order core cannot issue the next load past a stalled
one.

**After:**

```cpp
void hwsprites::glow_flush()
{
    const int32_t n = glow_n;
    int32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint16_t* const p0 = glow_ring[i], *const p1 = glow_ring[i + 1],
                 *const p2 = glow_ring[i + 2], *const p3 = glow_ring[i + 3];
        const uint16_t v0 = *p0, v1 = *p1, v2 = *p2, v3 = *p3;     // four loads in flight before any store
#ifdef CB_SPRITE_STATS
        stat_glow_changed += (v0 & S16_PALETTE_ENTRIES ? 1 : 0) + (v1 & S16_PALETTE_ENTRIES ? 1 : 0)
                           + (v2 & S16_PALETTE_ENTRIES ? 1 : 0) + (v3 & S16_PALETTE_ENTRIES ? 1 : 0);
#endif
        *p0 = (uint16_t)(v0 & 0xfff); *p1 = (uint16_t)(v1 & 0xfff);
        *p2 = (uint16_t)(v2 & 0xfff); *p3 = (uint16_t)(v3 & 0xfff);
    }
    for (; i < n; i++) {
#ifdef CB_SPRITE_STATS
        if (*glow_ring[i] & S16_PALETTE_ENTRIES) stat_glow_changed++;
#endif
        *glow_ring[i] &= 0xfff;
    }
    glow_n = 0;
}
```

**Invariant:** ring entries within one flush are distinct addresses (each is `lo-1` of a
distinct `(row, maximal run)`; runs on one row are disjoint with a non-colour pixel between them,
and the ring is flushed at least at every sprite end, DESIGN_GLOW section 3), so reordering
loads ahead of stores reads the same values; and `&= 0xfff` is idempotent, so even a duplicate
would give the same result. `glow_push` (721-733) is unchanged.

### 3.9 `draw_sprite_fast` after R1a

```cpp
void hwsprites::draw_sprite_fast(const SpriteRec& r, uint16_t* pixels, int32_t pend)
{
    // HEAD 756-758 first: an empty row range returns before sched() is consulted, as HEAD did
    if (std::max<int32_t>(0, -r.top) >= std::min<int32_t>(r.H, (int32_t)config.s16_height - r.top)) return;
    SpriteCtx c;
    make_ctx(r, pixels, pend, c);
    draw_sprite_rows(c);                 // R1b: switch (select_kernel(r.zoom, r.flip)) { ... }
    glow_flush();                        // HEAD 787: once per sprite
}
```

### 3.10 R1a gate

Harness (sec. 7.1) green on both driver targets, WSL x86-64 and an ARM box; S0.8 (sec. 7.2);
codegen checks of 3.1-3.6 (sec. 7.3); in-situ (sec. 7.4) **<= 1.00x shipped in every band** on
`pi2-local`, `test mule` as cross-check. If R1a lands above parity, do not re-plan: R1b's fill
change is the next lever and its gate is the binding one.

---

## 4. Step R1b -- templated fill and copy ladder (gate: in-situ <= 0.90x shipped in every band)

### 4.1 Templates and dispatch

```cpp
hwsprites::Kernel hwsprites::select_kernel(int32_t zoom, int32_t flip)
{
    if (zoom >= 0x200) return K_GEN;                     // run-0 regime: generic path (runtime flip, 8-byte fill, scan)
    if (zoom >= 0x100) return flip ? K_F1R2 : K_F0R2;    // runs in {1,2}
    return flip ? K_F1R4 : K_F0R4;                       // 0x80 <= zoom < 0x100: runs in {2,3,4}
}
```

`draw_sprite_fast`'s body becomes

```cpp
    switch (select_kernel(r.zoom, r.flip)) {
        case K_F0R2: draw_sprite_rows_t<0, 2, false>(c); break;
        case K_F1R2: draw_sprite_rows_t<1, 2, false>(c); break;
        case K_F0R4: draw_sprite_rows_t<0, 4, false>(c); break;
        case K_F1R4: draw_sprite_rows_t<1, 4, false>(c); break;
        default:     draw_sprite_rows_t<2, 4, true >(c); break;   // K_GEN
    }
```

| instantiation | zoom | flip | fill | vertical loop | who hits it |
|---|---|---|---|---|---|
| `<0,2,false>` | `0x100..0x1FF` | no | 4-byte | recurrence | hi-res default (most sprites), lo-res near half |
| `<1,2,false>` | `0x100..0x1FF` | yes | 4-byte | recurrence | as above, flipped |
| `<0,4,false>` | `0x80..0xFF` | no | 8-byte shift-OR | recurrence | hi-res default close-ups |
| `<1,4,false>` | `0x80..0xFF` | yes | 8-byte shift-OR | recurrence | as above, flipped |
| `<2,4,true>` | `>= 0x200` | runtime | 8-byte shift-OR | HEAD scan | lo-res far sprites, `hiresprites`, fuzz |

Five row-loop instantiations, ~1.8 KB each **[E]** (sec. 3.2), ~9 KB total, ~7 KB hot per hi-res
frame. `decode_row_k` has four distinct bodies (`<0,2> <1,2> <0,4> <1,4>`) plus `<2,4>`;
`replay_k` is one body inlined five times. The generic instantiation keeps one source: the
kernel is written once with `if constexpr (FLIP == 2)` reading `c.flip`.

`MAXRUN` from `zoom` is exact by C5: `run[i] in {floor(512/zoom), ceil(512/zoom)}`, so `zoom >=
0x100 => run <= 2` and `zoom >= 0x80 => run <= 4`. Pin it in `sched()` (300): extend the existing
assertion to `assert(zoom < (uint32_t)SPR_FAST_ZOOM_MIN || (r <= 4 && (zoom < 0x100 || r <= 2)))`.

The harness wrappers (sec. 2.7) dispatch through the same `select_kernel`, so unit s1c/s2b/s2c
exercise the real instantiations for every zoom they generate.

### 4.2 Fill width by class (R0 item 1) -- the per-nibble body

```cpp
template <> CB_ALWAYS_INLINE void hwsprites::store_fill<2>(uint16_t* p, uint32_t v)
{
    const uint32_t v2 = v | (v << 16);                       // A64: one `orr wA, wV, wV, lsl #16`
    std::memcpy(p, &v2, 4);                                  //      one `str wA, [xLine, xPos, lsl #1]`
}
template <> CB_ALWAYS_INLINE void hwsprites::store_fill<4>(uint16_t* p, uint32_t v)
{
    const uint32_t v2 = v | (v << 16);
    const uint64_t v4 = (uint64_t)v2 | ((uint64_t)v2 << 32); // A64: `orr xA, xA, xA, lsl #32` (w-write zero-extends)
    std::memcpy(p, &v4, 8);                                  //      `str xA, ...`
}
```

`v = nibble | color <= 0xFFF`, so no masking is needed (HEAD's `and x7, x7, #0xffff` was the
`uint16_t` -> `uint64_t` widening for the multiply). Endianness is irrelevant: both halves hold
`v`.

**Multiply vs shift-OR, quantified (A53, in-order, `MUL` 3-cycle latency, 1/cycle throughput;
ALU ops 1-cycle, dual-issuable; `MICROARCH_A7_A53.md` sec. 1).** Value chain from the extracted
nibble to the store's data operand:

| variant | instructions | latency to store | issue slots |
|---|---|---|---|
| HEAD 8-byte: `orr` (color), `and` (widen), `mul` | 3 | 1 + 1 + 3 = **5** | 3 (mul on the single multiply pipe) |
| R1b `MAXRUN=4`: `orr`, `orr lsl#16`, `orr lsl#32` | 3 | 1 + 1 + 1 = **3** | 3 (any ALU) |
| R1b `MAXRUN=2`: `orr`, `orr lsl#16` | 2 | **2** | 2; and a 4-byte store (half the store-buffer bytes) |

Same instruction count for `MAXRUN=4`, two cycles less latency in front of the store, and the
multiply pipe freed; `MAXRUN=2` is one instruction and three cycles better than HEAD. The
multiply is never cheaper: it cannot beat one op, and at equal op count it has 3x the latency.
The in-order core cannot hide that latency behind the following nibble because the store is
issued in program order and the next nibble's `pos` add is behind it.

Per-nibble COLOUR step after R1b, `MAXRUN=2` (expected A64, nibble `k`):
`ubfx w5, w1, #(28-4k), #4` ; `orr w5, w5, wColor` ; `orr w5, w5, w5, lsl #16` ;
`str w5, [xLine, xPos, lsl #1]` ; `ubfx x6, xRuns, #8k, #8` ; `add xPos, xPos, x6` -- six
instructions, one memory op, no multiply, no load (HEAD: eight instructions, three memory ops,
one multiply). `MAXRUN=4` adds one `orr` and stores `x5`.

**Overrun safety (exact).** A `MAXRUN`-halfword store at `pos` writes positions `pos .. pos +
MAXRUN - 1`; the run of this nibble is `r = run[8*words + k] <= MAXRUN` (sec. 4.1), so positions
`pos + r .. pos + MAXRUN - 1` (at most `MAXRUN - 1` of them) are written spuriously. Either
(i) there is a following nibble in this row: it stores at `pos' = pos + r` and writes `pos' ..
pos' + MAXRUN - 1 ⊇ pos + r .. pos + MAXRUN - 1` (since `pos' = pos + r` and both stores are
`MAXRUN` wide), overwriting every spurious position with its own value before anything reads
`line[]`; a following GAP or all-SHADOW *word* does not store, but the spurious positions then
lie in `[pos_after_word, ...)` which is a GAP/SHADOW span, and `replay_k` never reads `line[]`
for GAP or SHADOW records (only `colour_span_k` reads it, over `[lo, hi) ⊆ [rec[k].pos,
rec[k+1].pos)` of a COLOUR record); or (ii) it is the last nibble of the row: the spurious
positions are `>= final pos` = the sentinel position (436), and every replayed `hi <=
rec[k+1].pos <= final pos`. Buffer bound: the word loop tests `pos < pend` at the word head, a
word advances `pos` by at most `8*MAXRUN <= 32`, so `final pos <= pend + 31` and the last store
ends at `<= pend + 31 + MAXRUN - 1 <= pend + 34 <= SPR_PEND_MAX + 34 = SPR_LB_LEN - 30`. Inside
`line[SPR_LB_LEN]`. This is HEAD's argument (355-359) with `MAXRUN` in place of 4; nothing about
records or positions changes, only the number of spurious halfwords written (fewer).

**Invariant:** for every position `p` that `replay_k` reads (`p in [lo, hi)` of a COLOUR record),
`line[p]` after R1b equals `line[p]` at HEAD: both are `nib | color` of the source nibble whose
run covers `p` (the last store to `p` is by that nibble in both).

**Codegen check:** in each row-loop instantiation, `grep -cE '\b(mul|madd|msub|smull|umull|smaddl|umaddl|umulh|smulh)\b'` counts only the per-row `pitch*j` (decode head, prefetch) and the per-destination-row `d*stride` (replay); the constant `0x0001000100010001` is gone: `grep -c 'movk.*#0x1, lsl #48'` = 0; between the first and eighth line store of the COLOUR body there is no `mul`-family instruction and no load.

### 4.3 Copy ladder in `colour_span_k` (R0 item 5)

```cpp
void hwsprites::colour_span_k(const uint16_t* line, uint16_t* base, int32_t stride,
                              int32_t da, int32_t db, int32_t lo, int32_t hi, int mode)
{
    const int32_t n = hi - lo;
    const uint16_t* const src = line + lo;
    if (n >= 8) {
#if defined(CB_HAVE_NEON)
        // HEAD 607-631 VERBATIM (D: vld1q/vst1q per row, final chunk re-based to n-8;
        // D': load each chunk once, store to every row), with `replay_mode` -> `mode`.
#else
        // HEAD 635-653 VERBATIM (D: memcpy per row; D': 8-byte chunks + halfword tail), `replay_mode` -> `mode`.
#endif
        return;
    }
    // R1b ladder, 1 <= n <= 7. Source loaded once, stored to every row: D and D' coincide here.
    (void)mode;
    if (n >= 4) {                                   // 4..7: [lo, lo+4) U [hi-4, hi) == [lo, hi)
        uint64_t a, b;
        std::memcpy(&a, src, 8);
        std::memcpy(&b, line + hi - 4, 8);
        for (int32_t d = da; d < db; d++) {
            uint16_t* const row = base + (ptrdiff_t)d * stride;
            std::memcpy(row + lo,     &a, 8);
            std::memcpy(row + hi - 4, &b, 8);
        }
    } else if (n >= 2) {                            // 2..3: [lo, lo+2) U [hi-2, hi) == [lo, hi)
        uint32_t a, b;
        std::memcpy(&a, src, 4);
        std::memcpy(&b, line + hi - 2, 4);
        for (int32_t d = da; d < db; d++) {
            uint16_t* const row = base + (ptrdiff_t)d * stride;
            std::memcpy(row + lo,     &a, 4);
            std::memcpy(row + hi - 2, &b, 4);
        }
    } else {                                        // n == 1
        const uint16_t a = *src;
        for (int32_t d = da; d < db; d++)
            (base + (ptrdiff_t)d * stride)[lo] = a;
    }
}
```

**Index math.** `4 <= n <= 7`: the stores cover `[lo, lo+4)` and `[hi-4, hi)`; `hi-4 >= lo`
(from `n >= 4`) and `hi-4 <= lo+3 < lo+4` (from `n <= 7`), so the union is exactly `[lo, hi)`
with an overlap of `8-n` halfwords that receives `line[p]` from both stores (both chunks are
copies of `line` at the same indices). `2 <= n <= 3`: `[lo, lo+2) U [hi-2, hi)`, `hi-2 >= lo`,
`hi-2 <= lo+1 < lo+2`. `n == 1`: `[lo, lo+1)`. No halfword outside `[lo, hi)` is written; the
destination is never read (stores only; the `a`/`b` loads are from `line`). The harness's
`unit s4b` compares this against `replay_colour_span_s1` for every `(lo, hi)`, `0 <= lo < hi <=
96`, rows 1-4, both modes, with guard rows/columns, so the ladder is directly gated.

**Invariant:** for each `(d, p)`, `d in [da, db)`, `p in [lo, hi)`: `row(d)[p] = line[p]`, and no
other address changes -- HEAD's contract (S4.3).

The public `replay_colour_span(base, stride, da, db, lo, hi)` (603) becomes a one-line forwarder
`colour_span_k(line, base, stride, da, db, lo, hi, replay_mode)`; `replay_shadow_span` (656)
forwards to `shadow_span_k`, whose body is HEAD 658-671 verbatim. The `_s1` twins (682-710) are
untouched.

**Codegen check:** no `bl memcpy` anywhere in the row-loop instantiations (every `memcpy` has a
constant size 2/4/8 and lowers to a store); the `n >= 8` NEON block still contains exactly the
HEAD `ld1`/`st1` pair pattern.

### 4.4 D vs D'

**Compiled default: D (`replay_mode = 0`).** Measured: D won on `test mule`, D' edged D by 2.1% on
`pi2-local` (`PERFORMANCE_MEASUREMENTS.md` campaign 5, 1.231 vs 1.205 overall) -- inside SKU variance, and both
measurements were taken with the `n < 8` path calling `memcpy` per row, which is where D' had
its edge (its scalar path had no libc call). The R1b ladder removes that asymmetry for `n < 8`
(the two modes are the same code there), leaving the `n >= 8` NEON loops as the only
difference: D streams one row at a time (the A53 store buffer merges consecutive 128-bit-aligned
stores into bursts, `MICROARCH` sec. 2); D' interleaves up to four row streams per chunk. Absent
a measured win for D' on the deciding box after R1b, the simpler streaming pattern ships. Both
stay correct and compiled; `-spritereplay 1` (main.cpp 933) and the driver's `--replay-mode`
keep D' reachable, and unit s4b/fuzz continue to test both. The R1b in-situ run measures both
(`-spritereplay 0` and `1`) and the heavy band decides; if D' wins by more than 3% there, flip the
member default (`hwsprites.hpp` 65: `int replay_mode = 1;`) -- one line.

### 4.5 R1b gate

Sec. 7.1-7.3 green; in-situ (sec. 7.4) **<= 0.90x shipped in every band** (expected <= 0.85x),
D/D' decided by the heavy band. Kill rule from the replan: if the light band is `> 1.00x` while
R0-style counters show rows/frame is no longer the dominant term, stop and go to R4
(direct-emit rows), not to more R1 tuning.

---

## 5. Reference implementation (final R1b form; R1a is sec. 3.0's substitution of it)

### 5.1 `prefetch_rom_row_k` (replaces `prefetch_rom_row`, HEAD 445-453)

```cpp
CB_ALWAYS_INLINE void hwsprites::prefetch_rom_row_k(const SpriteCtx& c, int32_t jrow)
{
    // jrow >= 0 always here (j0 = src(d_lo) >= 0 and we only add); HEAD's `if (jrow < 0) return` was dead.
    const int32_t rowbase = c.rowoff0 + c.pitch * jrow;
    const int32_t a = rowbase, b = rowbase + c.pitch - 1;      // first and last word of the row, either direction
    if ((uint32_t)a < (uint32_t)SPRITES_LENGTH) cb_prefetch_r(c.rom + a);
    if ((uint32_t)b < (uint32_t)SPRITES_LENGTH) cb_prefetch_r(c.rom + b);
}
```

(HEAD hinted `w_first`/`w_last` mirrored for flip; the *set* of two addresses is the same either
way, so the flip test is dropped.)

### 5.2 `decode_row_k` (replaces the body of HEAD 380-437)

```cpp
template <int FLIP, int MAXRUN>
int32_t hwsprites::decode_row_k(const SpriteCtx& c, int32_t j)
{
    // (a) hoist -- nothing below reads c.* or this->* inside a loop (sec. 2.4)
    uint16_t*       const line   = c.line;
    SpanRec*        const rec    = c.rec;
    const uint8_t*  const run    = c.run;
    const uint8_t*  const wsum   = c.wsum;
    const uint32_t* const rom    = c.rom;
    const uint32_t        color  = c.color;
    const uint32_t        smask  = c.smask;
    const bool            shadow = c.shadow != 0;
    const int32_t         pend   = c.pend;
    const bool            flip   = (FLIP == 2) ? (c.flip != 0) : (FLIP == 1);

    // (b) per-row ROM window (sec. 3.6)
    const int32_t rowbase = c.rowoff0 + c.pitch * j;
    const int32_t first   = flip ? rowbase + c.pitch - 1 : rowbase;
    int32_t wlimit = 0;
    if ((uint32_t)first < (uint32_t)SPRITES_LENGTH)
        wlimit = flip ? std::min<int32_t>(SPR_WMAX, first + 1)
                      : std::min<int32_t>(SPR_WMAX, (int32_t)SPRITES_LENGTH - first);
    const int32_t wstep = flip ? -1 : 1;

    int32_t pos = 0, words = 0, n = 0, widx = first;
    uint8_t prev = SPR_GAP;

    while (pos < pend && words < wlimit) {                        // HEAD 392-394 (sec. 3.6)
        uint32_t w = rom[widx];
        if (flip) w = nibrev(w);                                  // HEAD 397 (compiled in/out by FLIP)

        if (w == 0) {                                             // GAP word, HEAD 399-402
            if (prev != SPR_GAP) { n = emit_l(rec, n, SPR_GAP, pos); prev = SPR_GAP; }
            pos += wsum[words];
        } else if (shadow && w == 0xAAAAAAAAu) {                  // all-SHADOW word, HEAD 403-406
            if (prev != SPR_SHADOW) { n = emit_l(rec, n, SPR_SHADOW, pos); prev = SPR_SHADOW; }
            pos += wsum[words];
        } else {
            uint64_t runs;                                        // (c) one load for eight run bytes (sec. 3.4)
            std::memcpy(&runs, run + (size_t)words * 8, 8);
            if (!classify_mixed(w, smask)) {                      // COLOUR word, HEAD 407-416
                if (prev != SPR_COLOUR) { n = emit_l(rec, n, SPR_COLOUR, pos); prev = SPR_COLOUR; }
                CB_UNROLL(8)
                for (int k = 0; k < 8; k++) {
                    store_fill<MAXRUN>(line + pos, ((w >> (28 - 4 * k)) & 0xFu) | color);   // (d) sec. 4.2
                    pos += (int32_t)((runs >> (8 * k)) & 0xFFu);
                }
            } else {                                              // MIXED word, HEAD 417-431
                CB_UNROLL(8)
                for (int k = 0; k < 8; k++) {
                    const uint32_t nib = (w >> (28 - 4 * k)) & 0xFu;
                    const uint8_t t = (nib == 0 || nib == 15) ? SPR_GAP
                                    : ((shadow && nib == 10) ? SPR_SHADOW : SPR_COLOUR);
                    if (t != prev) { n = emit_l(rec, n, t, pos); prev = t; }
                    store_fill<MAXRUN>(line + pos, nib | color);
                    pos += (int32_t)((runs >> (8 * k)) & 0xFFu);
                }
            }
        }
        words++;
        widx += wstep;
        if ((w & 0xF0) == 0xF0) break;                            // HEAD 434: terminator after the whole (mirrored) word
    }
    return emit_l(rec, n, SPR_GAP, pos);                          // HEAD 436: sentinel
}
```

Class tests, emit order, position arithmetic and the terminator are HEAD's, statement for
statement; only *where the constants come from* (locals), *how many bytes each load/store moves*
(sec. 3.4, 4.2) and *how the ROM bound is tested* (sec. 3.6) differ.

### 5.3 `store_fill` -- sec. 4.2 (R1a: HEAD's multiply form, sec. 3.0)

### 5.4 `replay_k` (replaces the body of HEAD 464-587)

```cpp
void hwsprites::replay_k(const SpriteCtx& c, int32_t nrec, int32_t da, int32_t db)
{
    const SpanRec* const rec    = c.rec;
    uint16_t*      const base   = c.base;
    const int32_t        stride = c.stride, cx0 = c.cx0, cx1 = c.cx1, top = c.top;
    const bool           glow_on = c.glow != 0, rtl = c.rtl != 0;
    const int            mode   = c.replay_mode;

    if (c.shadow) {                                               // sec. 3.7: S3 scan only for shadow sprites
        const int32_t pf_hi = std::min<int32_t>(db + SPR_SHADOW_PF_ROWS * (db - da), c.d_hi);   // HEAD 501-502
        if (pf_hi > db) {
            for (int32_t k = 0; k + 1 < nrec; k++) {              // HEAD 504-516 verbatim, scrn_width -> stride
                if (rec[k].type != SPR_SHADOW) continue;
                const int32_t lo = std::max<int32_t>(rec[k].pos, cx0);
                const int32_t hi = std::min<int32_t>(rec[k + 1].pos, cx1);
                if (hi <= lo) continue;
                for (int32_t d = db; d < pf_hi; d++) {
                    uint16_t* const rowp = base + (ptrdiff_t)d * stride;
                    uintptr_t p = (uintptr_t)(rowp + lo) & ~(uintptr_t)63;
                    const uintptr_t p_last = (uintptr_t)(rowp + hi - 1);
                    for (; p <= p_last; p += 64) cb_prefetch_w((const void*)p);
                }
            }
        }
    }

    for (int32_t k = 0; k + 1 < nrec; k++) {                      // HEAD 520-586
        if (rec[k].type == SPR_GAP) continue;
        const int32_t lo = std::max<int32_t>(rec[k].pos, cx0);
        const int32_t hi = std::min<int32_t>(rec[k + 1].pos, cx1);
        if (hi <= lo) continue;

        if (rec[k].type == SPR_COLOUR) {
            colour_span_k(c.line, base, stride, da, db, lo, hi, mode);
            if (glow_on && lo > cx0) {                            // HEAD 543-570 verbatim (r.rtl -> rtl, r.top -> top)
                const bool suppress = rtl && k > 0 && rec[k - 1].type == SPR_SHADOW;
                uint16_t* const col = base + (lo - 1);
                for (int32_t d = da; d < db; d++) {
                    if (suppress) {
#ifdef CB_SPRITE_STATS
                        stat_glow_suppressed_rtl++;
#endif
                    } else {
                        const int32_t y = top + d;
                        if ((shadow_rows[y >> 6] >> (y & 63)) & 1ull) glow_push(col + (ptrdiff_t)d * stride);
#ifdef CB_SPRITE_STATS
                        else stat_shadow_bitmap_filtered++;
#endif
                    }
                }
            }
        } else {                                                  // SPR_SHADOW (unreachable when !c.shadow, sec. 3.7)
            for (int32_t d = da; d < db; d++) {                   // HEAD 580-583
                const int32_t y = top + d;
                shadow_rows[y >> 6] |= 1ull << (y & 63);
            }
            shadow_span_k(base, stride, da, db, lo, hi);
        }
    }
}
```

`glow_push` (721-733) is unchanged and still a member (the ring is per-object state the s2c
negative control drives directly).

### 5.5 `colour_span_k` / `shadow_span_k` -- sec. 4.3 (R1a: HEAD 603-654 / 656-672 bodies, static, `replay_mode` -> `mode`)

### 5.6 `draw_sprite_rows_t` -- sec. 3.5

```cpp
template <int FLIP, int MAXRUN, bool SHRINK>
void hwsprites::draw_sprite_rows_t(const SpriteCtx& c)
{
    const int32_t d_hi = c.d_hi, zoom = c.zoom;
    const uint8_t* const run = c.run;

    int32_t d  = c.d_lo;
    int32_t j  = (d * zoom) >> 9;                                 // HEAD 765/772, int32 (d <= 511, zoom <= 0xFFF)
    prefetch_rom_row_k(c, j);                                     // HEAD 766-767
    prefetch_rom_row_k(c, j + 1);
    int32_t da = d; d++;
    while (d < d_hi && ((d * zoom) >> 9) == j) d++;               // HEAD 775: first group by scan
    int32_t db = d;

    for (;;) {
        prefetch_rom_row_k(c, j + SPR_ROM_PF_ROWS);               // HEAD 778
        const int32_t nrec = decode_row_k<FLIP, MAXRUN>(c, j);    // HEAD 780, inlined, count in a local
        replay_k(c, nrec, da, db);                                // HEAD 781, inlined
        if (db >= d_hi) break;
        da = db;
        if constexpr (!SHRINK) {                                  // sec. 3.5 recurrence (zoom < 0x200)
            j++;
            db = std::min<int32_t>(da + (int32_t)run[j], d_hi);
        } else {                                                  // HEAD 772-776 (zoom >= 0x200)
            j = (da * zoom) >> 9;
            d = da + 1;
            while (d < d_hi && ((d * zoom) >> 9) == j) d++;
            db = d;
        }
    }
}
```

### 5.7 Deletions

`hwsprites::emit` (323-333, hpp 201), `hwsprites::prefetch_rom_row` (445-453, hpp 229),
the bodies of `decode_row`/`replay` (replaced by sec. 2.7 wrappers), the bodies of
`replay_colour_span`/`replay_shadow_span` (replaced by forwarders to the `_k` statics). The
`SpriteRec` struct, `parse_record`, `draw_sprite_px`, `render_px`, `render`, `sched`,
`classify_mixed`, `nibrev`, `glow_push`, the `_s1` twins and everything under `CB_RENDER_TEST`
are untouched.

---

## 6. Signature table

| HEAD | after R1 | notes |
|---|---|---|
| `void decode_row(const SpriteRec&, int32_t j, int32_t pend)` | same signature; body = wrapper (sec. 2.7) | writes member `nrec` |
| `void replay(const SpriteRec&, uint16_t*, int32_t da, int32_t db, int32_t cx0, int32_t cx1)` | same signature; body = wrapper | reads member `nrec`, caller's `cx0/cx1` |
| `void draw_sprite_fast(const SpriteRec&, uint16_t*, int32_t pend)` | same signature; body sec. 3.9 / 4.1 | |
| `void replay_colour_span(uint16_t*, int32_t, int32_t, int32_t, int32_t, int32_t)` | same; forwards to `colour_span_k(line, ..., replay_mode)` | s4b target |
| `void replay_shadow_span(...)` | same; forwards to `shadow_span_k` | s4b target |
| `void emit(uint8_t, int32_t)` | **removed** -> `static int32_t emit_l(SpanRec*, int32_t n, uint8_t, int32_t)` | |
| `void prefetch_rom_row(const SpriteRec&, int32_t) const` | **removed** -> `static void prefetch_rom_row_k(const SpriteCtx&, int32_t)` | |
| `void glow_flush()` | same; body sec. 3.8 | |
| -- | `struct SpriteCtx` (sec. 2.2) | private |
| -- | `enum Kernel`; `static Kernel select_kernel(int32_t zoom, int32_t flip)` | R1b |
| -- | `void make_ctx(const SpriteRec&, uint16_t* pixels, int32_t pend, SpriteCtx&)` | |
| -- | `template<int FLIP,int MAXRUN> static int32_t decode_row_k(const SpriteCtx&, int32_t j)` | R1a non-template |
| -- | `void replay_k(const SpriteCtx&, int32_t nrec, int32_t da, int32_t db)` | |
| -- | `template<int MAXRUN> static void store_fill(uint16_t*, uint32_t)` | R1b |
| -- | `static void colour_span_k(const uint16_t* line, uint16_t* base, int32_t stride, int32_t da, int32_t db, int32_t lo, int32_t hi, int mode)` | |
| -- | `static void shadow_span_k(uint16_t* base, int32_t stride, int32_t da, int32_t db, int32_t lo, int32_t hi)` | |
| -- | `template<int FLIP,int MAXRUN,bool SHRINK> void draw_sprite_rows_t(const SpriteCtx&)` | R1a: `void draw_sprite_rows(const SpriteCtx&)` |
| `hwarch.hpp` | `CB_ALWAYS_INLINE`, `CB_UNROLL(n)` | |
| `sched()` (290) | one extra `assert` term (sec. 4.1) | table contents unchanged |

---

## 7. Gates

### 7.1 Harness (both steps; both driver targets; WSL x86-64 and an ARM box)

```
cmake -S tools/sprite_bench/test -B tools/sprite_bench/test/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/sprite_bench/test/build -j
for T in sprite_test sprite_test_scalar; do B=tools/sprite_bench/test/build/$T
  $B selftest --roms <roms>                                      # twin + oracle fidelity, zero mismatches
  $B unit                                                        # s1a s1b s1c s2a s2b s2c s3 s4a s4b (s1b/s4a take ~1 min each)
  $B fuzz 200000 --roms <roms>                                   # 2 replay modes x 2 glow x 4 modes, whole-allocation memcmp
  $B fuzz 2000   --roms <roms> --negctrl                         # must print "harness OK" (mismatches observed)
  $B replay --roms <roms> --frames <captures> --replay-mode 0    # glow-on exact; glow-off F1/F2 classified, zero unclassified
  $B replay --roms <roms> --frames <captures> --replay-mode 1
  $B golden --roms <roms> --frames <captures>                    # equals golden/hires_nw_attract20.txt
done
```

Gates that bite specific R1 changes: **s1a** (the `run[]` values the vertical recurrence
depends on, and the `j` sequence vs the accumulator); **s1c** (records from the templated
`decode_row_k` via the wrapper, zoom bands `0x80..0x1F8`, `0x100..0x3F0`, `0x200`,
`0x401..0x7FF`, `0x80..0xFFF`, `pend` up to 3020 -- covers all five kernels and the overrun
bound); **s2c** (the wrappers' `nrec` hand-off and the caller's `cx0/cx1`); **s4b** (the copy
ladder for every `n` 1..96, both modes, vs the `_s1` twins); **s3** (shadow gating with
`dest_clean` both ways); **fuzz** (everything, including `x1 > 0` windows and out-of-range
`addr` for the per-row ROM window).

### 7.2 S0.8 whole-game

Three game builds with `-DCB_FRAME_CKSUM=1` (Release): the stage, the stage with
`-DSPRITE_REF_PX=ON`, and shipped `bdf7e9b`. For each of hi-res, hi-res wide, lo-res,
`hiresprites`, and for `-noglow` and default:
`CB_MAX_FRAMES=3000 ./cannonball-se -t 1 -perftest [-noglow]` from a cold start; the
`CB_FRAME_CKSUM frames=3000 cksum=...` line of the stage must equal the `SPRITE_REF_PX` build's
at the same glow setting. (Shipped vs `SPRITE_REF_PX -noglow` equality is reported, not
required: a difference must be covered by `replay`'s non-zero F1+F2 count.)

### 7.3 Codegen (game build, A53: `cmake -DCMAKE_BUILD_TYPE=Release` -> `-O3 -Ofast -mcpu=cortex-a53+crc`)

```
objdump -d --no-show-raw-insn -C build/cannonball-se > dis.txt
awk '/^[0-9a-f]+ <hwsprites::draw_sprite_rows/,/^$/' dis.txt > rows.txt      # R1a: draw_sprite_rows; R1b: 5 instantiations
grep -E '\bbl\b' rows.txt | grep -vc 'glow_flush'                             # == 0   (R1a; no decode_row/replay/sched/memcpy)
grep -cE '\b(sdiv|udiv)\b' dis.txt | (awk section hwsprites::)                # == 0   (both steps)
grep -c 'movk.*#0x1, lsl #48' rows.txt                                        # == 0   (R1b; the 0x0001000100010001 constant is gone)
grep -cE 'decode_row_k|replay_k' dis.txt                                      # == 0 symbols (fully inlined)
```

Manual: in each COLOUR body (eight consecutive `str w/x` to the line register, or `stp` pairs
GCC may fuse), confirm **no** `ldr*` (R1a: kills the `[x19, #52]` reload), **no** `mul`-family
(R1b), one `ubfx` per run byte. Also confirm no `str ... [sp, #..]` spills inside the body
(A64 has the registers; a spill means the hoist failed). The driver build (`-O2`) is not
subject to these checks.

### 7.4 In-situ (the campaign 5 / campaign 6 procedure of `PERFORMANCE_MEASUREMENTS.md`)

Deciding box `pi2-local` (Pi 2 v1.2, A53, 900 MHz fixed, heatsinked); cross-check `test mule`
(Zero 2 W, 1 GHz, `-j1` builds, stop `autostream autostream_monitor owntone vibra-mini` first).

1. `echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`; log
   `scaling_governor` and `scaling_cur_freq` immediately before the run set **and at the start of
   every run** (R0 lost a run set to a silent revert to `ondemand`); `vcgencmd get_throttled`
   must read `0x0` before and after every run; sample `measure_temp` every 20 s.
2. Three clean `git archive` exports, identical CMake defaults (Release, `PIXEL_ACCURACY=1`,
   `-march=native`): shipped `bdf7e9b`; the previous stage (for R1a: the S4 commit R0 measured,
   `73dc0c1`, or the branch HEAD just before R1a; for R1b: the R1a commit); the stage under test.
   Record `md5sum` of each binary. Apply the *same* windowed harness to all three
   (`patch_main.py`: cumulative `video.prof_ns_sprites` per 300 frames keyed by `prof_frames`,
   `CB_FRAME_CAP` hard stop; or its permanent `CB_SPRITE_STATS` equivalent if that has landed --
   either is fine so long as it is identical across the three trees).
3. Runs, strictly sequential, box otherwise idle:
   `SDL_VIDEODRIVER=offscreen CB_FRAME_CAP=9000 ./cannonball-se -60 -t 4` -- shipped (no flags);
   stage `-spritereplay 0`; stage `-spritereplay 1` (R1b, the D/D' decision); stage
   `-spritereplay 0 -noglow` once (glow-cost delta). All must reach `frames=9000`, `RC=0`.
4. Reduce: 30 windows of 300 frames (discard window 0); attract mode is deterministic so windows
   are scene-matched by frame index; tercile bands by *shipped* ns/frame; per band
   `ratio = sum(stage ns) / sum(shipped ns)`.
5. Gate: **R1a `<= 1.00` in every band; R1b `<= 0.90` in every band.** Report the three-band
   table and the overall ratio, as `PERFORMANCE_MEASUREMENTS.md` campaign 6 does. Optionally re-apply R0's
   `CB_R0_PROF` cntvct timers to the stage tree in a separate build to confirm the
   DECODE/OTHER/COPY split moved as predicted (not part of the gate).

---

## 8. Risks

1. **Code growth on an in-order front-end (R1b).** Five always-inlined instantiations, ~9 KB
   **[E]**, ~7 KB hot per hi-res frame vs ~2 KB today. L1I is 32 KB, but the A53's
   prefetching is described as "very basic" (`MICROARCH` sec. 8) and the sprite pass alternates
   with the NTSC filter threads on other cores, not this one, so the risk is I-cache
   *misses on the switch between kernels* within a frame rather than capacity. Mitigation: R1a
   is measured alone (one instantiation) so R1b's delta is attributable; fallback is to un-inline
   `replay_k` (one `bl` per source row, -3.5 KB) or to collapse `<0,4>`/`<1,4>` into the `K_GEN`
   instantiation (hi-res default rarely goes below `0x100`: `ZOOM_LOOKUP >> 1` spends 13% of runs
   at length 4, `SCHEDULE_VERIFICATION.md`). This is the risk I am most worried about, because it
   is the one the harness cannot see and only the in-situ number can.
2. **The hoist not materialising.** If GCC keeps any local in memory (address escaped, or
   register pressure in the MIXED path), R0 item 3 survives in a new guise. The sec. 7.3 manual
   check (no `ldr` in the COLOUR body, no `[sp]` spills) is the detector; the fix is to shrink
   the live set (e.g. pass `rec`/`line` as one base with offsets) -- A64 has 31 GPRs, so this is
   unlikely.
3. **R1a alone may not reach parity.** DECODE is 55-60% and R1a leaves the `mul` (R1b's item).
   The R1a gate is a checkpoint, not a kill: if R1a is above 1.00x but the OTHER bucket has
   collapsed, proceed to R1b.
4. **Harness coupling.** The wrappers must reproduce HEAD's member effects exactly (`nrec`
   written by `decode_row`, read by `replay`; the caller's `cx0/cx1`; ring pushes without flush).
   Unit s2c is the guard; sec. 2.7 is written against it.
5. **`-O2` driver vs `-O3` game.** `always_inline` and `CB_UNROLL` keep both builds structurally
   alike; behaviour is identical regardless (same C++), only the codegen checks are `-O3`-only.
6. **D/D' remains a few-percent, SKU-dependent call.** Both stay compiled; the default is a
   one-line change.

---

## 9. Deviations from `SPRITE_REPLAN_FABLE.md` section 3, and why

| replan | this LLD | reason |
|---|---|---|
| 3.2: extend the fallback envelope with a per-sprite ROM-footprint test; drop per-word bounds | per-row `wlimit` (sec. 3.6); envelope untouched | hard requirement 2 (envelope preserved exactly); same per-word saving |
| 3.3: `N(j0+1) = ceil(512*(j0+1)/zoom)`, one division per sprite | first group by HEAD's scan (`<= 4` multiplies), then the recurrence (sec. 3.5) | keeps "no `sdiv`/`udiv`" literally true; reuses proven code; no ceil arithmetic to prove |
| 3.1: "pass it by reference ... so the alias set of the memcpy stores no longer forces reloads" | by-reference alone does not remove the reloads; the kernels copy fields to locals at entry (sec. 2.4) | correctness of the mechanism; the R0 objdump would otherwise reappear with a different base register |
| 3.1: four instantiations `<FLIP, MAXRUN>` | five: `FLIP in {0,1,2}` with `2` = runtime, `SHRINK` template flag; `K_GEN = <2,4,true>` (sec. 4.1) | one kernel source for both regimes (no duplicated body to keep in sync); the run-0 regime keeps HEAD's scan which the recurrence cannot replace there |
| 3.2: `MAXRUN == 4` keeps the existing 8-byte store (multiply implied) | shift-OR chain (sec. 4.2), quantified: same op count, 3-cycle vs 5-cycle chain, multiply pipe freed | measurable and makes the "no `mul` in the nibble loop" check unconditional |
| 3.4: ladder described per mode | ladder is mode-independent for `n < 8` (source loaded once, sec. 4.3); D/D' differ only at `n >= 8` | simpler; removes the D' scalar advantage that made the S4 D/D' result ambiguous |
| 3.4: "shadow bookkeeping (prefetch scan and `shadow_rows` marking) skipped when `!shadow`" | only the prefetch scan needs a gate; marking is inside the SHADOW branch, unreachable for non-shadow sprites (sec. 3.7) | the same effect with one branch, and a stated proof |
| 3.4: D/D' "decided at the R1 gate" | D is the compiled default now; the R1b heavy band can flip it (sec. 4.4) | the ladder removes the asymmetry D' was winning on; a default is needed to ship |

---

## 10. Summary

* **R1a (de-glue, target parity):** `SpriteCtx` built once per sprite; kernels on locals;
  `always_inline` `decode_row_k`/`replay_k`; one `sched()`; one 8-byte `run` load per word;
  per-row ROM window; `run[j]` vertical recurrence after a HEAD-scan first group (no division);
  S3 scan gated on `shadow`; `glow_flush` x4. Harness wrappers keep every driver hook byte-for-byte.
* **R1b (specialise, target <= 0.90x):** `<FLIP, MAXRUN, SHRINK>` templates, five
  instantiations selected per sprite from `zoom`/`flip`; fill by shift-OR at the class width
  (4-byte store for `zoom >= 0x100`); the 1..7-halfword copy ladder replacing `memcpy`; the
  `n >= 8` NEON loops untouched; D as default, D' kept.
* **Highest-value change:** the nibble body -- from three memory ops plus a 3-cycle multiply per
  source nibble to one store and five or six single-cycle ALU ops (R1a's hoist + run word, R1b's
  fill). The single load/store pipe is the A53's binding constraint and DECODE is 55-60% of the
  time.
* **Biggest risk:** I-cache footprint of five always-inlined instantiations on the in-order A53;
  mitigated by measuring R1a alone first and by the documented fallback (un-inline `replay_k`).
