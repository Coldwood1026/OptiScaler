# XeFG Multi-Frame Generation (MFG) Unlock — Change Notes

> **English** ｜ [中文](README-XeFG-MFG-Unlock.zh-CN.md)
>
> This is a **locally modified build** of OptiScaler that adds one capability to
> the `framegen/xefg` path: it makes OptiScaler's **MFG multiplier combo box
> appear and switch in real time on non-Intel adapters** (2X and up; the ceiling
> is `XeFG\MaxInterpolatedFrames`, which no longer clamps by default).
>
> **Modified: 2026-09-11** ｜ Based on upstream OptiScaler (GPL-3.0)
> Full reverse-engineering write-up:
> [`optiscaler xemfg version/`](../XeSSMFG-Universal-v0.2.0.1/optiscaler%20xemfg%20version/README.md)

---

## ⚠️ Disclaimer

- This project is **not affiliated with, endorsed by, or authorised by Intel**
  in any way. "Intel", "XeSS" and "XeFG" are used **nominatively**, only to
  identify what this is compatible with. No endorsement is implied.
- This modification **contains, distributes and repackages no Intel binaries**.
  It patches **in-memory, at runtime**, a `libxess_fg.dll` that the user must
  already legitimately own. **The file on disk is never modified — not one byte.**
- Users are responsible for holding a valid licence to the software involved,
  and use this at their own risk.
- Provided for **interoperability research and study** only. **No warranty of
  any kind.**
- Nothing here is legal advice.

---

## 1. The Problem

Intel gates MFG behind an "**am I the `igxess_fg.dll` build?**" check rather
than a real hardware capability query. On a non-Intel adapter that means:

| Symptom | Cause |
|---|---|
| The MFG multiplier combo box **does not appear at all** | The provider reports `maxSupportedInterpolations = 1`, and OptiScaler's gate is `maxInterpolationCount > 1` — **a reported 1 means the whole control is not drawn** |
| Stuck at 2X, cannot go higher | The init path downgrades the MFG model (17→15 / 18→16) |
| Changing the multiplier tends to crash (in similar mods) | "Unlocking" is coupled to "switching" through model weight rebuilding |

This modification removes those gates **on the provider side**, which cleanly
**separates unlocking from switching**: the switching path performs no model
rebuild at all — it just changes an integer.

---

## 2. What Changed

Everything lives under **`OptiScaler/`** — **4 source files + 2 project files +
1 config template**.

### 2.1 Added

| File | Notes |
|---|---|
| [`OptiScaler/proxies/XeFGUnlock.h`](OptiScaler/proxies/XeFGUnlock.h) | **New** (~290 lines). The patch engine: byte tables for all five patches, `.text` bounds checks, per-byte comparison, write-read-back verification, and whole-set rollback on any failure. Header-only — no new translation unit. |

### 2.2 Modified

| File:line | Change |
|---|---|
| [`OptiScaler/proxies/XeFG_Proxy.h:7`](OptiScaler/proxies/XeFG_Proxy.h#L7) | `#include "XeFGUnlock.h"` |
| [`OptiScaler/proxies/XeFG_Proxy.h:180`](OptiScaler/proxies/XeFG_Proxy.h#L180) | Calls `XeFGUnlock::Apply(_dll);` inside `HookXeFG()`, **after** `_dll = libxefgModule;` — it must run after the DLL is mapped and before any `xefgSwapChain*` call |
| [`OptiScaler/Config.h:594-595`](OptiScaler/Config.h#L594-L595) | Two new config fields: `FGXeFGUnlockEnabled{true}`, `FGXeFGMaxInterpolatedFrames{31}` |
| [`OptiScaler/Config.cpp:216-226`](OptiScaler/Config.cpp#L216-L226) | Reads `XeFG\UnlockMFG` / `XeFG\MaxInterpolatedFrames`; the latter falls back to the default when outside 1..31 |
| [`OptiScaler/Config.cpp:1005-1008`](OptiScaler/Config.cpp#L1005-L1008) | Writes both back to the ini — otherwise an edited value is lost on the next launch |
| [`OptiScaler/Config.cpp:219`](OptiScaler/Config.cpp#L219) | **Also fixes an upstream bug**: the `FGXeFGInterpolationCount` clamp reads `> 3`, inconsistent with the **5** used elsewhere in the same file (the combo box lists 2X..6X). Without the fix, selecting 5X/6X is silently `reset()` on the next launch |
| [`OptiScaler.vcxproj:483`](OptiScaler.vcxproj#L483) | Registers the new header |
| [`OptiScaler.vcxproj.filters:500`](OptiScaler.vcxproj.filters#L500) | Same, in the filters file |
| [`OptiScaler.ini:236-260`](OptiScaler.ini#L236-L260) | **Default config template** gains the two new keys, and the now-stale `1 = 2X \| 2 = 3X \| 3 = 4X` note on `InterpolationCount` is completed up to 6X |

### 2.3 Deliberately **not** changed

- **`framegen/xefg/XeFG_Dx12.cpp` is untouched.** Once the reported value is
  corrected, OptiScaler's existing `_maxInterpolationCount` /
  `SetNumInterpolatedFrames()` logic simply starts working — UI and switching
  are stock upstream code.
- **No "frame pacing" compensation was ported.** XeSSMFG needs it because it is
  a `dxgi.dll` proxy that takes over Present and must supply its own cadence.
  OptiScaler goes through the provider's public `xefgSwapChain*` API and lets
  the provider schedule (OptiScaler contains no XeFG pacing code at all).
  Porting it would mean hooking **non-exported internal addresses of a DLL whose
  build we do not control** — precisely the fragility that makes that mod crash.
  See `08-PACING.md`.
- **No Intel binary is modified or redistributed** — not `libxess_fg.dll`, not
  `libxell.dll`, nothing.

---

## 3. What the Five Patches Do

All applied to the **already-mapped memory image** of `libxess_fg.dll`
(`file offset = RVA − 0xC00`). `N` = `XeFG\MaxInterpolatedFrames` (default 31,
valid range 1..31); when `N == 1` **not a single byte is changed**.

| # | RVA | Old → New | Effect |
|---|---|---|---|
| U1 | `0x20DA4F` | `0F 85 CC 00 00 00` → `E9 CD 00 00 00 90` | The per-frame resolver no longer falls back to 2X on "XeLL version too old" |
| U2 | `0x1A5DE4` | `74 09` → `EB 06` | `Settings::mfgAllowed()` is always true, so init stops downgrading the MFG model 17→15 / 18→16 |
| U3 | `0x1A517D` | `BB 03 00 00 00` → `BB N …` | Raises the default interpolation ceiling (`max(floor, override)`) |
| U4 | `0x1A45C2` | `C7 87 6C 01 00 00 01 00 00 00` → `… N …` | When the provider decides MFG is unavailable it no longer pins the override to 1 |
| U5 | `0x20973B` | `B8 01 00 00 00` → `B8 N 00 00 00` | `xefgSwapChainGetProperties` no longer hardcodes a report of 1 — **this is the one that makes the combo box appear** |

> ⚠️ **U4 / U5 must be applied by RVA with a full-instruction byte comparison.**
> `B8 01 00 00 00` occurs **1669 times** in `.text`, and
> `C7 87 6C 01 00 00 01 00 00 00` occurs **twice** — any pattern-replace style
> implementation will corrupt the DLL.

With all five applied, **all four** of the provider's reporting paths converge on `N`.

---

## 4. New Configuration Keys

In the `[XeFG]` section of `OptiScaler.ini`:

```ini
[XeFG]
; Unlock toggle. Default true
UnlockMFG=true

; Reported ceiling on interpolated frames, N. The multiplier shown in the
; menu is N+1: 1 = 2X ... 31 = 32X. Default 31 (no ceiling of our own).
MaxInterpolatedFrames=31
```

Both can be changed at any time; a restart applies them.
`MaxInterpolatedFrames=1` is equivalent to turning the unlock off entirely.

**Note the distinction between two keys**: `MaxInterpolatedFrames` decides
*how high you can go*, `InterpolationCount` decides *which mode you start in*.
They are independent.

> ⚠️ This value is **not only a menu bound**. `XeFG_Dx12.cpp:366-385` reads the
> provider's reported maximum back into
> `xefg_swapchain_d3d12_init_params_t::maxInterpolatedFrames` at swapchain init,
> so raising it declares the same number to the provider on every launch.
> 1..6 is what has been tested; higher is untested, and if the provider sizes
> anything from this number the symptom would be a failed init or exhausted
> VRAM. Setting it back to 5 is the way out.

---

## 5. Building

Same as upstream, no extra dependencies:

```powershell
msbuild OptiScaler.sln /p:Configuration=Release /p:Platform=x64
```

Output: `x64\Release\OptiScaler.dll`.

---

## 6. Disabling / Reverting

Any one of three, at your convenience:

1. **Config only** (no files touched) — set `XeFG\UnlockMFG=false`, or set
   `XeFG\MaxInterpolatedFrames=1`. The latter means **no patch is applied at
   all**, and the provider keeps its factory behaviour.
2. **Back to stock** — overwrite with an upstream build. The Intel DLL on disk
   was never modified, so there is nothing to clean up.
3. **Safe on partial failure** — if any single patch fails its byte comparison
   (e.g. a different provider build), the engine **rolls the whole set back**
   and the provider runs as shipped. **Failure means "no MFG", never a crash.**
   On a build mismatch the log shows `unrecognised provider build` followed by
   `rolled back`.

---

## 7. Verification Status

**Verified in-game on 2026-09-11 in Cyberpunk 2077** (RTX 2060, `libxell.dll` 1.3.0):

- All five patches applied: `5 of 5 patches applied (0 skipped)`
- Reported value went from **1 to 5**: `Max supported interpolations: 5`
- The multiplier combo box appears, 2X..6X selectable
- **Six consecutive switches (`3↔5↔4`) in 90 seconds, with no crash**

**Not yet done**: long-run image-quality and stability testing above 4X, and
anything at all above 6X — the default ceiling was raised from 5 to 31 after
that verification, so 5X+ is still exactly as unverified as it was then.
If a higher multiplier turns out to be unstable, set `MaxInterpolatedFrames=3`
(4X — the mode with the most exercised history).

---

## 8. GPL-3.0 Modification Notice

Per **GNU GPL v3 §5(a)**:
*"The work must carry prominent notices stating that you modified it, and giving a relevant date."*

> This repository is a modified version of
> [OptiScaler](https://github.com/optiscaler/OptiScaler).
> **Date of modification: 2026-09-11.** The full list of changes is in §2 of
> this document (added `OptiScaler/proxies/XeFGUnlock.h`; modified
> `XeFG_Proxy.h`, `Config.h`, `Config.cpp`, `OptiScaler.vcxproj`,
> `OptiScaler.vcxproj.filters`, `OptiScaler.ini`).
>
> Copyright in the original work remains with the OptiScaler authors, licensed
> under **GPL-3.0**. This modified version is likewise licensed under
> **GPL-3.0**; the full licence text is in [`LICENSE`](LICENSE).
> The complete corresponding source for the modifications is this repository.

---

## 9. Further Reading

The full reverse-engineering analysis, per-patch safety arguments and raw
in-game logs live in:

```
..\XeSSMFG-Universal-v0.2.0.1\optiscaler xemfg version\
```

| Document | Contents |
|---|---|
| `README.md` | Overview and conclusions |
| `01-THE-LOCK.md` | The full gate chain and call graph |
| `02-UNLOCK-PATCH.md` | Patch byte table + per-patch safety arguments |
| `07-WHY-NO-COMBO.md` | Why the combo box never appeared (where U4/U5 came from) |
| `08-PACING.md` | What pacing is, and why it was not ported |
| `09-CHANGELOG-U4U5.md` | This round's changes and the in-game evidence |
| `RVA-MAP.md` | RVA / file-offset quick reference |
