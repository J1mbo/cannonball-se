# Chris White / "reassembler" (djyt) and OutRun on the Amiga — research notes

**Bottom line up front:** the premise that Chris White "ported CannonBall to the
Amiga 1200 (68040) achieving 60fps sprite generation in low resolution" does
**not** match anything he has published. What he actually shipped in
2025–2026 is a *separate*, ground-up 68k-assembly project called **"OutRun:
Amiga Edition"**, which he and his own userbase repeatedly describe as
explicitly **not** based on CannonBall. Frame-rate targets are **25 or 30
fps** (matching the arcade's 30 fps and Amiga PAL 50 Hz timing), not 60 fps,
and real-world reports from 68030/68040/68060 owners cluster around
12–25 fps. No claim of "60 fps sprite generation" or a 68040-at-60MHz target
appears anywhere in the material found. This is documented in detail below,
with sources.

## 1. What the project actually is

Chris White (GitHub `djyt`, blog `reassembler.blogspot.com`, itch.io/YouTube
handle `reassembler` / `reassembler68k`, Twitter/X `@djyt`) is indeed the
original author of the CannonBall C++ OutRun engine
(https://github.com/djyt/cannonball). Separately, starting around March 2024
(EAB tech-demo post) and released properly from July 2025, he has been
building **"OutRun: Amiga Edition"** — sold "pay what you want" on itch.io:
https://reassembler68k.itch.io/outrun-amiga-edition

The store page states explicitly:

> "This is not an emulator. Nor is it based on Cannonball, MAME, or any
> existing project. It is a ground up rewrite in assembler, with a focus on
> performance."
(https://reassembler68k.itch.io/outrun-amiga-edition)

He confirms the same distinction in his own itch.io comment replies. When a
user asked about a "no traffic" mode he replied:

> "Could easily do a no-traffic mode though. In fact, it already exists in
> Cannonball... which I worked on previously, if you have tried that in the
> past."
(itch.io comments thread, https://reassembler68k.itch.io/outrun-amiga-edition/comments,
reply dated "192 days ago" relative to fetch date)

Other users in the same comment thread independently confirm CannonBall's
Amiga port (a different, C-based project by other authors, e.g.
`lantus/Cannonball-C` / `mheyer32/Cannonball-C`, ported by "Modern Vintage
Gamer" et al.) is a *third*, separate thing:

> "I previously played the Amiga conversion of the Cannonball engine on my
> A600GS, and while that was really good, that had some weird
> 'interpretations' of the game music and ran in a small, non resizable
> window. This is SOOOO much better." — user "boswell5469"

> "On Apollo V4 card ... the game shows only Copper lines on screen without
> the graphics. On V4 CannonBall works instead." — user "DanyPPC"

So there are, in total, three distinct things that could be confused with
each other: (a) `djyt/cannonball`, the original PC/C++ engine; (b) the
C-ported `Cannonball-C` Amiga build by other developers, unrelated to this
research target; and (c) Chris White's own hand-written 68k-assembly
"OutRun: Amiga Edition", which is the actual subject of his 2025–2026 blog
and video output.

**No `djyt/cannonball-amiga` repository, and no Amiga-specific branch of
`djyt/cannonball`, exists.** Checked `djyt`'s GitHub profile directly
(https://github.com/djyt?tab=repositories) — 7 repos total: `sonic2mod`,
`youtube` (Sonic-related supporting material, unrelated), `cannonball`
(the original C++ engine, 863 stars), `layout` (OutRun track editor),
`system16_sprite_viewer`, `xboard_memtest`, `mmlr`. Nothing Amiga-related.

## 2. Hardware, resolution, colour mode, and frame rate

From the itch.io store page requirements table:

- **Minimum spec:** Amiga, 68030 @ 50 MHz, PAL machine, AGA chipset, 2 MB
  Chip RAM, 8 MB Fast RAM, hard drive.
- **Recommended:** 68040 @ 25 MHz or better, Fast RAM ≥ 25 MB/s.
- A separate **68040+ build** exists with CPU-specific optimisations (see
  §3).
- CD32's **Akiko** custom chip is used for hardware-assisted chunky-to-planar
  conversion when present (added in v0.9.1).

No explicit pixel resolution or bitplane/colour-depth number is stated
in any fetched page (I could not confirm e.g. "320×256" or a specific colour
count for the port itself — only that AGA and "even 256 colours weren't
enough" per the Chapter 2 video title/description, implying more of the
256-colour AGA palette budget was needed than expected, likely due to
per-scanline copper palette tricks). This is a gap — see §6.

**Frame rate is 25 fps or 30 fps, selectable, not 60 fps.** Reassembler's own
comment explains the engineering reason 30 fps is the hard ceiling on this
project, tied to PAL 50 Hz refresh:

> "I would stick to 25fps by default if you're having issues with 30fps
> mode. Reason being this targets PAL (50Hz) machines, 30fps effectively has
> to be able to support 50fps, as it has to be capable of rendering a frame
> per screen refresh."
(itch.io comments, reassembler reply)

Store-page per-machine performance table:

| Hardware | Accelerator | Result |
|---|---|---|
| A1200 | TerribleFire TF1230 (030) | "Minimum playable spec" |
| A1200 | Blizzard 1230-III + FPU | ~25% faster than TF1230 |
| CD32 | TerribleFire TF030 | On par with A1200/TF1230 |
| A4000 | Cyberstorm Mk2 060 | "Full speed @ 25 fps" |
| A500 Mini / A600GS | (ARM-emulated) | "Full speed @ 30 fps" |
| A1200 | PiStorm32 Lite Model 4 | "Full speed @ 30 fps" |
| A1200 | Vampire 1200 V2 | "Full speed @ 30 fps" |

Real-user reports from the itch.io comment thread (none of these are
reassembler's own benchmark numbers, just player-submitted anecdotes)
skew considerably lower than the marketing table on genuine 68030/68040
hardware:

- A1200 + TF1230 (030): "average of about 16FPS ... sometimes gets to 25,
  and sometimes to 12" (user PaulKitching, on a listed A1200/040 — later
  clarified as an Apollo 1240 @ 25 MHz, 32 MB "Power-Burstmode-FastRAM" at
  29.3 MB/s per SysInfo)
- TF1260LC @ 50 MHz (060-class): "flys ... at 25fps. Seems to run slower if
  setting the game to 30fps. If i oc my TF1260 to @94Mhz it hits 30fps with
  a few dips."
- A600GS (ARM emulation): "ran flawlessly at 25fps, at 30fps I noticed a
  little slowdown"
- Rev 6 '060 @ 50 MHz (TF4060): "runs at 20fps and when I overclock it to
  100mhz it runs slower at 17fps"
- A1200/060 @ 50 MHz: "just excellent" (jotd666, no fps given)

A German-language reviewer's written summary on the comments page sums up
the actual ceiling bluntly: "It's just a shame that it only has 25 or 30 fps
instead of 50 or 60."  **This directly contradicts the "60 fps" premise of
the research question** — no such figure appears anywhere in the primary
material, and a player explicitly calls out that it does *not* reach 60.

## 3. Sprite renderer and 68040-specific architecture

Written detail here is thinner than I'd like — see the gap note in §6 — but
what's confirmed from primary sources:

**Sprite scaling implementation** (from the "Making Of OutRun Chapter 3:
Sprite Scaling" video, https://www.youtube.com/watch?v=O_QP2qQJuXc, and
search-indexed summaries of its content — I could not pull the full spoken
transcript, see §6): the sprite scaler is built as **34 specialised
functions instead of one general-purpose scaler**, eliminating bounds checks
where they can be proven unnecessary, with **separate code paths for
growing vs. shrinking, clipping, horizontal flipping, shadow/priority
rendering, and non-scaled sprites**, using fast integer arithmetic
throughout. The tutorial source code for this stage
("OutRun_Tutorial_Part_02_Sprite_Zooming.zip", 437 kB) was released alongside
the video (see §4).

**68040-specific build** (from reassembler's own itch.io comment reply,
itch.io post https://itch.io/post/13960550, in the "0.91 Released" comments
thread — this is the single richest primary-source technical quote found):

> "The 040 build uses a faster C2P algorithm for that architecture and some
> limited use of the 040 move16 instruction to move data about in the
> codebase. I reworked some of the original sub/road CPU code to be more
> register efficient with less reliance on memory access. I also changed
> the road rendering to minimise drawing of the secondary road layer (at
> the expense of a small amount of additional computation elsewhere). I
> also found a dumb bug where during certain situations I was clearing
> parts of the screen that didn't need to be."

Note this quote is about the **road renderer and chunky-to-planar (C2P)
conversion**, not specifically the sprite engine — but it's the clearest
statement of concrete 68040 optimisation technique (MOVE16 for burst block
copies, a CPU-specific C2P routine, register-allocation rework) that exists
in public form. He explicitly declined to give hard numbers when asked
directly:

> "I tend to remove the granular measurements from the release builds
> themselves, as the extra code probably adds a bit of bloat/speed loss in
> itself." (same reply)

**Chunky-to-planar / blitter:** the port does its own C2P conversion in
software (with a 68040-tuned variant, per above), plus optional hardware
assist via the CD32's Akiko chip:

> "Support for the CD32 Akiko Custom Chip ... in short it assists with
> chunky to planar conversion of the graphics. Unfortunately, due to the
> hardware architecture of the CD32, the increase in performance isn't as
> awesome as it could be." (v0.91 devlog,
> https://reassembler68k.itch.io/outrun-amiga-edition/devlog/1018884/outrun-amiga-edition-091-released)

No explicit statement was found on Amiga hardware-blitter usage for sprite
compositing itself (as opposed to C2P) — the general framing throughout
("ground up rewrite in assembler... tuned for the Amiga's strengths and
quirks") suggests a CPU-driven, per-sprite generated-code approach (consistent
with the "34 specialised functions" description) rather than blitter-composited
sprites, but I found no line explicitly confirming or ruling out blitter use
for sprite blit itself. Chip-RAM-vs-fast-RAM memory-layout tricks, explicit
jump-table dispatch, or "sprite table precomputation" terminology were not
found in any accessible text — likely covered only in the spoken portion of
the Chapter 3 video and/or in the released source zip, neither of which I
could access (see §6).

## 4. Numbers given, and what he says the dominant cost is

No cycle-per-pixel, ms-per-frame, or sprites-per-frame figures were found
anywhere in the accessible material. When directly asked by a user offering
to help benchmark on real hardware, reassembler declined to give numbers and
said he strips instrumentation from release builds (quoted in full in §3).

On what's expensive, his own diagnosis (from the v0.9.1 "Performance
Improvements" devlog entry,
https://reassembler68k.itch.io/outrun-amiga-edition/devlog/1018884/outrun-amiga-edition-091-released):

> "I spent a ridiculous amount of time squeezing extra performance out of
> the engine for those of us (like me) using an 030 processor. After many
> days of optimisation, the gains are marginal at best. That said, certain
> parts of the game do run a little faster now. Please don't expect
> miracles on these minimum-spec systems, the real bottleneck is **the
> sheer amount of data being processed per frame rather than a glaring flaw
> in the code**."

That statement is about the engine broadly (road + sprites + C2P), not
sprites in isolation, but it's the closest thing to a stated "what's the
dominant cost" in his own words.

## 5. Source code availability and file names

**There is no public git repository for this Amiga engine.** When directly
asked "is this not an open source project?" on itch.io, reassembler replied:

> "There's source code provided that fully renders the levels and performs
> the sprite scaling. What are you looking to do/make?"

The follow-up made clear this is *not* a full open-source repo, just
tutorial-scoped drops:

> "I'm not into amiga right now but it is a little disappointing to not see
> a github where the whole process is laid out for anyone to view, reuse,
> and toy with." — user "axunes", itch.io comments

So the actual "source" is distributed as standalone zip attachments on
specific devlog posts, not a browsable repo:

- `OutRun_Tutorial_Part_01_Road.zip` (275 kB) — attached to "The Making Of
  OutRun Chapter 1: The Long Road!"
  (https://reassembler68k.itch.io/outrun-amiga-edition/devlog/1029155/the-making-of-outrun-chapter-1-the-long-road)
  — road/pseudo-3D rendering source.
- `OutRun_Tutorial_Part_02_Sprite_Zooming.zip` (437 kB) — attached to "The
  Making Of OutRun Chapter 3: Sprite Scaling!"
  (https://reassembler68k.itch.io/outrun-amiga-edition/devlog/1105824/the-making-of-outrun-chapter-3-sprite-scaling)
  — the sprite-scaling tutorial source. **This is presumably where the real
  sprite-renderer file/function names live**, but I could not access the zip
  contents (no code-execution/download-and-unzip capability used for this
  web-only research task), so I cannot report actual file names for the
  sprite renderer.
- The main game binary/tools release itself ships a Python
  `amiga_rom_exporter.py` script for converting arcade ROM graphics/audio to
  Amiga format (per multiple comment-thread troubleshooting exchanges about
  Python/librosa dependency issues) — but that's an asset-conversion tool,
  not the sprite renderer.

No YouTube-linked or itch.io-linked GitHub URL for the full engine was found
anywhere in the material searched.

## 6. Gaps — what I could not confirm

Being explicit per the task instructions rather than guessing:

- **Full spoken content of the three "Making Of" YouTube videos**
  (Chapter 1 "The Long Road", Chapter 2 "A Colour Trip", Chapter 3 "Sprite
  Scaling", https://www.youtube.com/watch?v=5PpR-Dm3-nU,
  https://www.youtube.com/watch?v=I-QAhxMhb5Q,
  https://www.youtube.com/watch?v=O_QP2qQJuXc) — YouTube's caption/transcript
  endpoint returned empty responses to unauthenticated fetches in this
  environment, and the video pages themselves only exposed metadata
  (title/description/chapter timestamps) to the fetch tooling, not the
  transcript. The "34 specialised functions" sprite detail in §3 is
  reconstructed from search-engine-indexed summaries of this video's
  content (which line up consistently across several independent search
  results), not from a transcript I read directly — flagging it as
  secondary-sourced rather than a verified direct quote.
- **English Amiga Board (EAB) thread content**
  (`eab.abime.net`, e.g. "Outrun Amiga by re-assembler",
  https://eab.abime.net/showthread.php?s=aa7ec0bf3df9ead21e61d2b648a4e7bb&p=1755909,
  and the older "OutRun: I started porting it!" / "Better Outrun port for
  Amiga" threads) — the site is behind Anubis anti-bot JavaScript
  challenge protection and returned a challenge page to every fetch
  attempt, both via the fetch tool and direct HTTP requests with a browser
  user-agent. No EAB content was accessible.
- **X/Twitter thread** (https://x.com/djyt/status/1771244347201049024,
  the March 2024 tech-demo announcement) — returned HTTP 402 (paywalled)
  to the fetch tool.
- **Bluesky/Mastodon posts** — no search results surfaced any Bluesky or
  Mastodon posts by djyt/reassembler specifically about the sprite engine;
  I found only the auto-generated Bluesky share-link embedded in the
  itch.io devlog page template (not an actual authored post).
- **Exact resolution and colour-depth numbers** for the shipping game (vs.
  the general "AGA, PAL" statement) were not stated in any fetched page.
- **Actual sprite-renderer source file names** — unknown, since the tutorial
  zip contents weren't downloaded/inspected (out of scope for a web-research
  task without code execution against the archive).
- **Any explicit comparison to CannonBall's C++ `hwsprites.cpp`** — none
  found. Every mention of "Cannonball" in the material (by White or by his
  commenters) is about distinguishing this project *from* CannonBall/
  Cannonball-C as an unrelated codebase, never a code-level comparison of
  the sprite renderers.
- **"60 fps sprite generation in low resolution"** — actively contradicted
  by every source found (see §2); I did not find this claim anywhere and
  recommend treating it as a mistaken premise rather than something to
  keep searching for.

## Sources

- Store page: https://reassembler68k.itch.io/outrun-amiga-edition
- Devlogs: 
  https://reassembler68k.itch.io/outrun-amiga-edition/devlog/1018884/outrun-amiga-edition-091-released ,
  https://reassembler68k.itch.io/outrun-amiga-edition/devlog/1037487/outrun-amiga-edition-092-released ,
  https://reassembler68k.itch.io/outrun-amiga-edition/devlog/1029155/the-making-of-outrun-chapter-1-the-long-road ,
  https://reassembler68k.itch.io/outrun-amiga-edition/devlog/1067826/the-making-of-outrun-chapter-2-a-colour-trip ,
  https://reassembler68k.itch.io/outrun-amiga-edition/devlog/1105824/the-making-of-outrun-chapter-3-sprite-scaling
- Comments: https://reassembler68k.itch.io/outrun-amiga-edition/comments ,
  direct-linked reply https://itch.io/post/13960550
- YouTube: https://www.youtube.com/watch?v=5PpR-Dm3-nU (Ch.1),
  https://www.youtube.com/watch?v=I-QAhxMhb5Q (Ch.2),
  https://www.youtube.com/watch?v=O_QP2qQJuXc (Ch.3, sprite scaling)
- djyt GitHub profile: https://github.com/djyt
- Indie Retro News coverage:
  https://www.indieretronews.com/2025/07/outrun-amiga-edition-vastly-superior.html
