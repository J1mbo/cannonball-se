# Cortex-A7 vs Cortex-A53: microarchitecture facts for the 4bpp→16bpp pixel loop

Scope: facts needed to choose a strategy for a loop that reads a 32-bit word of eight
4-bit pixels, extracts nibbles, compares each against two transparent codes, ORs in a
16-bit constant, and stores each result 1-4 times to consecutive destination halfwords
(sometimes across 2-3 rows, sometimes as read-modify-write for a transparency bit),
where the destination usually misses in cache.

Targets: Cortex-A7 (Raspberry Pi 2 v1.1, ARMv7-A, 32-bit `armhf`) and Cortex-A53
(Raspberry Pi 3 / Pi Zero 2 W). Per this repo's test mules, the A7 box builds 32-bit
ARMv7 code and the A53 box (`test mule`) builds **AArch64** (`-march=native` →
`-mcpu=cortex-a53+crc`). So this is a two-ISA comparison, not one ISA at two speeds: A7
only ever runs AArch32/ARMv7-A NEON, A53 here runs AArch64/ARMv8-A NEON.

Primary sources: Cortex-A7 MPCore TRM r0p5 (ARM DDI 0464F), Cortex-A53 MPCore Processor
TRM r0p4 (ARM DDI 0500J), the ARM NEON Programmer's Guide (DEN0018A), and the
AAPCS32/AAPCS64 procedure-call standards (ARM IHI 0042 / IHI 0055). ARM never published a
"Cortex-A53 Software Optimization Guide" with an instruction-timing table the way it did
for A57/A72/A77 — several sources confirm this explicitly, and none was located. Figures
from secondary sources are marked **[secondary, name]**; ARM-primary figures **[TRM]**.
Gaps are stated as gaps, not estimated.

## 1. Issue and dual-issue rules

Both cores are in-order, 8-stage pipelines ARM's own materials call **"partial
dual-issue"** [TRM/secondary, BDTI InsideDSP 2011] — two issue slots per cycle, not every
combination fills both.

| | Cortex-A7 | Cortex-A53 |
|---|---|---|
| Two ALU ops | Yes — "full ALU" + "partial ALU" pipe pair [secondary, BDTI] | Yes, most ALU ops slot-agnostic [secondary, GCC `cortex-a53.md`] |
| Load + ALU | Yes, separate load/store pipe [secondary, BDTI] | Yes [secondary, chipsandcheese; GCC model] |
| Store + ALU | Yes | Yes |
| Load + Store together | Not supported, single L/S pipe | **Not supported** — "two load/stores cannot simultaneously issue" [secondary, GCC model] |
| Multiply / NEON | Single-issue only [secondary, BDTI] | 128-bit NEON can't dual-issue with each other or scalar FP; one vector/FP port [secondary, chipsandcheese, Sonos blog] |
| Branch + ALU | Dual-issue-capable per ARM's table; restrictions unquantified | Slot-agnostic except multi-cycle-to-issue branches [secondary, GCC model] |

The hard, repeated constraint: **neither core co-issues a load and a store in the same
cycle**, nor two memory ops of any kind. A read-modify-write costs ≥2 issue cycles for
the memory ops alone. Independent measurement of the A7 (hardwarebug.org, 2014, which
notes "the public TRM does not include instruction timing information") found real
dual-issue opportunities narrower than ARM's label implies — e.g. `MOV`+`ADD`-immediate
paired, but "most instruction combinations cannot dual-issue." Treat ARM's pipeline
diagrams as an upper bound, not a guarantee.

## 2. Load and store timing

| | Cortex-A7 | Cortex-A53 |
|---|---|---|
| L1 load-to-use | 3 cycles, simple addressing [secondary, hardwarebug.org] | 3-4 cycles by addressing mode [secondary, chipsandcheese]; GCC model: 4 cycles flat; 7-cpu.com measured: 3 cycles |
| Store latency | `STR` issues in 1 cycle, 2-cycle result latency; loads forward to dependent stores "without delay" [secondary, hardwarebug.org] | ≈2-cycle store latency [secondary, GCC model] |
| Store buffer | 4×64-bit slots, write-merging [TRM, summarized] | STB "can merge multiple store transactions to the same 128-bit aligned address and combine writes into AXI or CHI bursts" [TRM, quoted, §2.1.6]. No entry count found. |
| Write-allocate | Write-back, write-allocate on read/write miss; BIU detects a full line being written before linefill completes (e.g. `memset`) and skips the fetch [TRM, summarized] | Not stated explicitly in accessible text. No no-write-allocate/streaming mode documented for either core. |
| L2 hit latency | Not found published | Disagreement across sources: ARM's own 2014 whitepaper (idealized 2 MB L2, cycle-accurate model) measured **8 cycles**; 7-cpu.com measured **15 cycles** on real silicon; chipsandcheese measured **~17 cycles (~9 ns)**. The ARM figure used a non-representative L2 config vs the Pi 3/Zero 2 W's ~512 KB L2 — treat 15-17 as realistic. |
| Memory latency | Not found published | ARM whitepaper: 183 cycles (simulated). 7-cpu.com: "15 + 128 ns" ≈169 cycles at 1.2 GHz — roughly corroborates. |

Both cores sustain **at most one store per cycle**, sharing that slot with loads — total
memory-op throughput, not store throughput alone, is the binding constraint.

## 3. Unaligned and wide accesses

Both cores execute unaligned word/doubleword loads and stores without faulting under
Linux (standard for ARMv7-A and ARMv8-A).

| | Cortex-A7 | Cortex-A53 |
|---|---|---|
| Crossing an 8-byte boundary | Not found published | **+1 cycle** [secondary, chipsandcheese] |
| Crossing a 64B line / 4KB page | Not found published for either core | Not found published |
| Wide store vs two narrow | `STRD` issues in **1 cycle**, same as one `STR` — roughly double the throughput of two `STR`s [secondary, hardwarebug.org] | No figure for `STP` specifically; GCC's model treats `LDP` (128-bit) as one 4-cycle, single-issue-slot op, consistent with the pattern — not independently confirmed for `STP`. |
| NEON `vst1` (64-bit) vs scalar 64-bit store | No head-to-head figure found. NEON is single-issue on A7 (§1), so `VST1.64` monopolizes the one issue slot a scalar `STRD` would share with a sister-pipe ALU op — scalar looks favorable when index/compare work needs hiding. | 128-bit NEON "cannot dual-issue with scalar FP or each other," runs "at half scalar throughput" [secondary, chipsandcheese]; GCC model gives a 64-bit NEON load 3 cycles (same class as scalar) but doesn't clarify dual-issue eligibility. No clean comparison found — flagged as a gap. |

## 4. Overlapping stores ("store widest run, then advance")

No document states an explicit penalty for a store followed by an overlapping store a
few bytes on. What is documented is the mechanism that makes it cheap: both store
buffers **merge writes landing in the same aligned window** before anything reaches the
bus — A53's STB merges "multiple store transactions to the same 128-bit aligned address"
into one burst [TRM, quoted]; A7's STB (4×64-bit slots) does the same over a narrower
64-bit window [TRM, summarized]. Combined with the one-memory-op/cycle limit (§1), the
reasoned conclusion (not an ARM-stated figure) is: two overlapping stores cost exactly
what two ordinary consecutive stores cost — one issue cycle each, no extra
read-modify-write from the overlap itself, provided both fall inside the merge window.
"Store the widest run, then advance by the actual run length" should be cheap on both
cores, bounded by instruction count rather than any overlap-specific hazard.

## 5. Branches

| | Cortex-A7 | Cortex-A53 |
|---|---|---|
| Predictor | Global history, **256-entry** pattern history table [TRM, quoted, §2.1.1] | Global history, **3072-entry** pattern history table [TRM via secondary, chipsandcheese/7-cpu.com] |
| Indirect / BTAC | **8-entry** BTAC [TRM, quoted] | **256-entry** indirect target array [secondary, chipsandcheese]; testing shows reliable tracking closer to ~2 targets/branch over ~64 sites than the raw count implies |
| Return stack | 8 entries [TRM, quoted] | 8 entries [TRM, quoted] |
| BTIC / mini-loop cache | 4-entry BTIC, each holding up to two I-cache fetches, eliding the shadow of small predicted-taken backward branches [TRM, quoted] | Single-entry BTIC, ~16 bytes [secondary, chipsandcheese] |
| Dedicated loop buffer | None (BTIC is loop-cache-*like*, far smaller than e.g. A15's 32-entry buffer) | None, same caveat |
| Mispredict penalty | Not found published | **7 cycles** [secondary, 7-cpu.com measured] |

Practical implication for both cores: a `switch`-as-jump-table exceeds A7's 8-entry BTAC
easily and sits within A53's 256-entry array in principle, but A53's own testing shows
real tracking is closer to "2 targets per branch across 64 sites" — a branchy per-nibble
dispatch is a worse bet than a branch-free mask/OR sequence on both.

## 6. NEON differences for 4bpp→16bpp expansion

A7 only ever executes AArch32/ARMv7-A NEON; A53 (built AArch64 here) executes
AArch64/ARMv8-A NEON — different instruction sets, not the same one at different speeds.

| Feature | ARMv7-A (A7) | AArch64 (A53) |
|---|---|---|
| Table lookup | `vtbl1/2/3/4_u8` — table built from 1-4 64-bit **D registers** (max 32 bytes), **result always 8 bytes** regardless of table size [DEN0018A + GCC/Rust intrinsics docs] | `vqtbl1q_u8`..`vqtbl4q_u8` — table is 1-4 full 128-bit **V registers** (16-64 bytes), index and **result 128-bit** |
| `vld2/vld3/vld4`, `vst2/vst4` | Present, on D/Q registers | Present, on the wider V-register file, same semantics |
| `vzip`/`vuzp` | Present | Present (intrinsic names retained over `zip1/zip2`/`uzp1/uzp2`) |
| `vmovl_u8` widening move | Present | Present |
| Shifts, mask compares, `vbsl` | Present | Present |
| Register file | **32 D-regs (64-bit)**, aliased in pairs as **16 Q-regs (128-bit)** — a 128-bit op ties up two D-register slots | **32 V-regs, each a full 128-bit register**, no aliasing |
| 128-bit op cost | 64-bit datapath: "a NEON core can process 128-bit operations... in 2 cycles" [secondary, ARM Cortex-A programmer's guide, search-synthesized] | 128-bit ops run at half scalar throughput, "1 vector op every 2 cycles for 128-bit loads" [secondary, chipsandcheese] — same 2x pattern, different cause (issue/port limits, not datapath width) |

Net effect: A7's `vtbl` caps at a 32-byte table, 8 bytes of output per instruction — a
poor fit for expanding 8 nibbles from one 32-bit word at once. A53's `vqtbl1q_u8` holds a
16-byte table and emits 16 bytes per instruction — more capable, not just faster.

## 7. Register pressure

| | ARMv7-A / AAPCS32 (A7) | AArch64 / AAPCS64 (A53 as built here) |
|---|---|---|
| GPRs | **13** (r0-r12) excl. sp/lr/pc: r0-r3 & r12 caller-saved scratch, r4-r8/r10/r11 (8) callee-saved via push/pop, r9 a platform register in some ABI variants [ARM IHI 0042] | **31** (x0-x30): x19-x28 (10) callee-saved, x9-x15 (7) caller scratch, x0-x8 (9) argument/scratch, x29 conventionally the frame pointer, x30 link register [ARM IHI 0055] |
| Spill cost here | 13 GPRs is tight for a source pointer, up to 3 destination row pointers, the fetched word, extracted nibbles, and counters — a realistic spill candidate; each spill contends for the single memory-op/cycle budget already saturated by real stores | ~2.4x the register file; the same loop should fit without spilling, leaving the memory-issue slot free for the algorithm's stores |

The starkest cross-core asymmetry found: A7 is genuinely register-starved for this loop
shape; A53 as built (AArch64) is not. A strategy tolerating spills on A53 may need fewer
live pointers or register reshuffling to avoid spill-induced memory contention on A7.

## 8. Prefetch

ARM's Cortex-A7 TRM §6.5 "Data prefetching" and the equivalent A53 material could not be
retrieved in full through the sources available. What was found: chipsandcheese
describes A53 as having "very basic prefetching capability, not driven by the branch
predictor" that struggles with large code footprints — the author's own analysis, not a
quoted ARM figure, and unclear whether it means instruction- or data-side prefetch. **No
confirmed hardware data-stream prefetcher, stream count, or pattern detection was found
documented for either core.** `PLD` (ARMv7-A) and `PRFM` (AArch64) are architecturally
defined hints with no defined completion time; no core-specific prefetch-distance
guidance was found for either chip. Given the destination "usually misses," this should
be measured on the project's existing test mules rather than assumed.

## 9. Other loop-relevant findings

- **Single memory pipe on both cores** (§1) is the most binding constraint — evaluate
  strategies by counted memory operations, not total instructions.
- Even at the realistic 15-17 cycle L2 figures, a destination-miss-heavy loop is
  dominated by the memory-op issue-slot limit and miss latency, not ALU/NEON throughput —
  reducing *stores to distinct cache lines* likely matters more than instruction tuning.
- No no-write-allocate or streaming-store mode is documented for either core: no software
  lever avoids the write-allocate linefill cost of a first miss, beyond A7's
  full-line-write detection (§2).

## Sources

**Primary (ARM):** Cortex-A7 MPCore TRM r0p5 (DDI 0464F, via Internet Archive OCR text
and direct PDF mirror) · Cortex-A53 MPCore Processor TRM r0p4 (DDI 0500J, via Internet
Archive OCR text) · ARM NEON Programmer's Guide (DEN0018A) · AAPCS32 (IHI 0042) / AAPCS64
(IHI 0055), github.com/ARM-software/abi-aa · "Performance Optimization for an ARM
Cortex-A53 System," Jason Andrews (Carbon Design Systems), ARM TechCon 2014, hosted at
developer.arm.com.

**Secondary:** Chester Lam, "ARM's Cortex A53: Tiny But Important," chipsandcheese.com
(2023) · hardwarebug.org, "Cortex-A7 instruction cycle timings" (2014) · Berkeley Design
Technology Inc., "ARM's Cortex-A7 and A15," InsideDSP (2011) · Sonos Tech Blog, "Assembly
still matters: Cortex-A53 vs M1" · GCC `gcc/config/arm/cortex-a53.md` scheduling model ·
7-cpu.com Cortex-A53 measured-latency page.

**Confirmed gap:** ARM has not published a "Cortex-A53 Software Optimization Guide" with
an instruction-timing table equivalent to those for A57/A72/A77/A78 — multiple
independent sources say so explicitly, and none was located. A leaked internal document
under that title circulates online but was deliberately not used, being marked
confidential and of doubtful provenance.
