# Third-Party Notices

This project includes or interfaces with third-party components. Copies of their
licenses are included in this `docs/` directory.

---

## Blargg SNES NTSC Video Filter

- **Component:** `snes_ntsc` (`src/main/sdl2/snes_ntsc.*`)
- **Author:** Shay Green ('Blargg') <gblargg@gmail.com>
- **License:** GNU Lesser General Public License 2.1 — see [LGPL-2.1.txt](LGPL-2.1.txt)
- **Upstream documentation:** `tools/ntsc_bench/scalar_ref/snes_ntsc.txt`, kept alongside
  the unmodified reference copy of the original library

### Notes

This library provides composite-video style NTSC filtering. CannonBall-SE ships a
substantially rewritten, SIMD-accelerated derivative of it; the pristine upstream sources
are retained under `tools/ntsc_bench/scalar_ref/` as the bit-exactness reference.

When statically linking LGPL code into binaries, ensure recipients can relink with a
modified version (for example, by providing relinkable object files); when dynamically
linking, ensure users can swap in a compatible modified library. Preserve copyright and
license notices.

For the full terms, see [LGPL-2.1.txt](LGPL-2.1.txt).

---

## MAME — YM2151 FM Sound Emulation

- **Component:** `src/main/hwaudio/ym2151.*`
- **Author:** Jarek Burczynski, as distributed with the MAME source code
- **Copyright:** © 1997–2002 Jarek Burczynski; © 1997–2025 MAMEDev and contributors
- **License:** GNU General Public License v2 or later — see [license_mame.txt](license_mame.txt)

---

## Atari800 — System 16 Sound Emulation

- **Component:** S16 sound emulation support code
- **Copyright:** © 1995–1998 David Firth; © 1998–2008 Atari800 development team
- **License:** GNU General Public License v2 or later — see [license_atari800.txt](license_atari800.txt)

---

## Sega System 16 Video Emulation

- **Component:** S16 video emulation
- **Copyright:** © 2019–2024 Aaron Giles
- Distributed under the terms of the upstream CannonBall project — see
  [license.txt](license.txt)

---

## Linux Force-Feedback Support

- **Component:** `/dev/hidraw` rumble support
- Based on work © 2020 Ismas and Johan Deneux

---

## CannonBall DX — User Setup Experience

- **Components:** Menu-Driven Input Binding system and Extended Rumble Effects
- **Author:** Based on work by Endprodukt, cannonball-dx (https://github.com/Endprodukt/cannonball-dx),
  a fork of CannonBall-SE
- **License:** the CannonBall-SE terms — see [CannonBall-SE-license.txt](CannonBall-SE-license.txt)

---

## SDL2, TinyXML-2, mpg123, ANGLE

These are external dependencies, linked rather than vendored, and are installed by
`install.sh` (Linux) or vcpkg (Windows). Their licenses are those of their respective
upstream projects: SDL2 (zlib), TinyXML-2 (zlib), mpg123 (LGPL-2.1), ANGLE (BSD-3-Clause;
Windows builds only).
