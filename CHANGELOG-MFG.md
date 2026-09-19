# Multi-Frame Generation — Changelog

Cumulative changelog for this fork's XeFG multi-frame generation work: the unlock, the
>2X pacing fix, the in-game multiplier exposure, and the second ceiling inside `libxell`.

Covers **2026-09-11 → 2026-09-18**. Upstream's own [`Changelog.md`](Changelog.md) is
untouched and still tracks the base OptiScaler releases; everything here sits on top of it.
Per-feature write-ups live in [`README-MFG-Unlock.md`](README-MFG-Unlock.md) and
[`README-MFG-Pacing.md`](README-MFG-Pacing.md) (each with a `.zh-CN.md` companion).

Two things to keep straight while reading — both cost real time to rediscover:

- **The multiplier is the interpolation count plus one.** `XeFG\InterpolationCount: 5`
  means **6X**, and the count the provider forwards to XeLL in that case is `5`.
- **There are two independent 4X ceilings, in two different DLLs.** Unlocking one is
  useless without the other — they are two separate argument checks reached by two
  separate code paths, and nothing cross-checks them.

### Timeline

| Date | Commit | Author | What |
|---|---|---|---|
| 2026-09-11 | `da427e20` | upstream | Config drops its hardcoded 4X bound on `XeFG\InterpolationCount` |
| 2026-09-12 | `5c5e424d` | upstream | Menu builds MFG labels from the provider's reported maximum |
| 2026-09-12 | `5ee53e38` | upstream | Dx11 → Dx12 swapchain description adaptation |
| **2026-09-13** | **`bb1619ec`** | fork | **MFG unlock (U1–U5) + >2X pacing + config + docs** |
| 2026-09-13 | `71d8f7aa` | fork | Repair a stray `)` that stopped `DxgiFactory_Hooks.cpp` compiling |
| 2026-09-13 | `70676c5f` | fork | Custom field gets its own row; ceiling drops 32X → 8X |
| **2026-09-15** | **`9f492c0e`** | fork (PR #1, YiBoF) | **Intel Arc support** |
| 2026-09-15 | `fe830f73` | fork | Merge of PR #1 |
| **2026-09-18** | **`c0ec7979`** | fork | **Expose XeFG's ceiling to the game; follow the game's multiplier** |
| **2026-09-18** | **`abb4bc62`** | fork | **Raise the second ceiling, in `libxell.dll`** |

---

## Baseline: what upstream had already done

Not this fork's work, but the unlock is built on it, so it is listed for context.

**`da427e20` (2026-09-11) — remove the hardcoded 4X bound.** `Config.cpp` validated
`XeFG\InterpolationCount` against `1..3` and reset anything outside that to the default.
The upper bound is gone; only `< 1` is rejected. The provider's reported maximum now
governs the menu instead of a number baked in at the config layer.

**`5c5e424d` (2026-09-12) — dynamic multiplier labels.** `menu_common.cpp` carried
`const char* intModes[] = { "2X", …, "6X" }` arrays, indexed by the current count and
bounded by the array length. Those are replaced with labels generated from
`maxInterpolationCount`, for the DLSSG override combo and both XeFG MFG combos. This is
what lets the menu follow the provider past 6X once the provider stops reporting 1.

**`5ee53e38` (2026-09-12) — Dx11 → Dx12 swapchain descriptions.** A rework of
`PrepareDx12InteropDesc`, `PrepareDx12InteropDesc1` and the new `PrepareDx12FlipFormat` in
`DxgiFactory_Hooks.cpp` (+179 lines). Not MFG work — listed only because it is the same
Dx12 flip-model swapchain plumbing the XeFG backend sits on.

> `5ee53e38` also introduced a stray `)` in `PrepareDx12FlipFormat`'s unsupported-format
> path (`LOG_ERROR(…format);)`) that made the file fail to compile. Repaired in the fork's
> next commit, `71d8f7aa`.

---

## 2026-09-13 — `bb1619ec` · Unlock MFG, and fix pacing above 2X

The foundational commit: 14 files, +2639 lines. Everything after it is refinement.

### Why there was anything to unlock

Intel gates MFG in `libxess_fg.dll` behind an *"am I the `igxess_fg.dll` build?"* check
rather than a hardware capability query. On a non-Intel adapter the provider therefore
reports `maxSupportedInterpolations = 1`, and `menu_common.cpp` gates the entire MFG block
on `> 1` — so the combo box does not merely stay at 2X, it never appears at all.

### `OptiScaler/proxies/XeFGUnlock.h` (new, 309 lines) — five byte patches

Applied to the **already-mapped image** at runtime. The DLL on disk is never modified.
Each patch is located by RVA and verified byte-for-byte before writing; any mismatch rolls
the whole set back. The failure mode is "no MFG", never a corrupted image. All four of the
paths that report a maximum (invalid version struct, version < 1.3.0, override set,
override clear) converge on one value.

| # | RVA | From | To | Effect |
|---|---|---|---|---|
| U1 | `0x20DA4F` | `0F 85 CC 00 00 00` | `E9 CD 00 00 00 90` | Per-frame count resolver no longer falls back to 2X with *"XeLL version x.y.z is too old to support multi-frame generation"* |
| U2 | `0x1A5DE4` | `74 09` | `EB 06` | `Settings::mfgAllowed()` always true → init stops downgrading model 17→15 / 18→16 |
| U3 | `0x1A517D` | `BB 03 00 00 00` | `BB N 00 00 00` | Raises the default interpolation ceiling |
| U4 | `0x1A45C2` | `C7 87 6C 01 00 00 01 …` | `C7 87 6C 01 00 00 N …` | Where the provider decides MFG is unavailable it pins the override to one interpolated frame; pin it to `N` instead |
| U5 | `0x20973B` | `B8 01 00 00 00` | `B8 N 00 00 00` | `xefgSwapChainGetProperties` reports `maxSupportedInterpolations = 1` on those two paths — **this is the one that makes the combo appear** |

`N` is `XeFG\MaxInterpolatedFrames`. A recognised build (`TimeDateStamp 0x69CB0F4D`,
`SizeOfImage 0x015ED000`) logs its stamp; an unrecognised one warns and falls back to
per-byte checks alone. U4 and U5 must be RVA-addressed with full-instruction comparison —
`B8 01 00 00 00` occurs 1669 times and the U4 pattern twice in `.text`, so no
pattern-search approach is safe here.

Why U2 patches a jump and not the flag: `+0x4d` in the `Settings` singleton has no writer
anywhere in the binary (full `.text` scan), so the flag itself is never set.

### `OptiScaler/proxies/XeFGPacing.h` (new, 1022 lines) — spread the burst

Fixing the count alone is not enough. Above 2X the provider hands every generated frame of
a burst to the swapchain back to back, and the spacing it computed is not consumed until
the loop has already finished — the limiter then holds the *last* frame back. The burst
arrives bunched up, which is what the frames-out-of-order complaint above 2X actually was.

- `r12` is computed once per burst at `0x220254` (`duration / (count + 1)`) and only
  consumed afterwards, by the FPS limiter block at `0x220317`, itself reachable only when
  `count > 2` and two other conditions hold.
- Every present in the loop goes through the 5-byte thunk at `0x25C0`
  (`jmp 0x18021f730`), followed by 11 bytes of `int3` padding — 16 bytes containing nothing
  but a jump. That is room for a 14-byte `jmp qword ptr [rip+0]`, so the thunk can be
  redirected anywhere with no instruction relocation and no code cave.
- The thunk has two other callers (`0x220A52`, `0x220DAD`), so the detour filters on
  `_ReturnAddress()` and paces only the two burst sites. Every other present is forwarded
  untouched.
- The last frame of a burst is **not** presented from the loop — it goes out from the call
  at `0x220462`, after the provider has submitted the burst, with `arg7 = 0` where the loop
  passes 1. It needs pacing of its own, or every burst ends with two frames on one
  timestamp.
- The spacing is not invented here: each paced present is handed to the provider's own
  frame scheduler at `0x21EE30` with arguments the present path already has, and the
  provider works out the presentation time itself.
- A latent trap: the provider writes only the *low byte* of the `arg7` stack slot, so the
  comparison must be on `(uint8_t) arg7`, not the whole word.

### Config and menu

Three new keys — `XeFG\UnlockMFG` (default true), `XeFG\MaxInterpolatedFrames` (default at
the top of the range), `XeFG\ExtraPacing` (default true). The menu follows the provider's
reported maximum instead of stopping at 6X.

Note this is not only a menu bound: the same number is declared to the provider as the
swapchain's `maxInterpolatedFrames` at init on every launch.

### Docs

Four files added: `README-MFG-Unlock.md` / `.zh-CN.md` and `README-MFG-Pacing.md` /
`.zh-CN.md` (initially named `README-XeFG-*`, renamed in `70676c5f`).

---

## 2026-09-13 — `70676c5f` · Cap the multiplier at 8X

- The Custom field shared a row with the MFG combo. The menu window is
  `AlwaysAutoResize`, so that one wide field widened every row in the menu; it now has its
  own line.
- The ceiling drops from 32X to 8X: `Config::XeFGMaxInterpolations` becomes `7`
  (`Config.h`), validated in `Config.cpp` by *resetting* an out-of-range value to auto
  rather than clamping it.
- The four README files are renamed `README-XeFG-MFG-Unlock.md` → `README-MFG-Unlock.md`
  and `README-XeFG-Pacing.md` → `README-MFG-Pacing.md`.

---

## 2026-09-15 — `9f492c0e` · Intel Arc support *(PR #1, YiBoF — merged as `fe830f73`)*

Contributed by **YiBoF**, not part of the original unlock.

- `DllNames.h` recognises `igxess_fg` as a provider name, and `LibraryLoad_Hooks.cpp`
  redirects a load of `igxess_fg.dll` to `libxess_fg.dll`.
- On a **native Intel adapter** with no explicit `XeFG\ExtraPacing` setting,
  `ExtraPacing` defaults to **false** (`dllmain.cpp`). The pacing patch exists for the
  non-Intel path where the provider's own pacing does not engage; on genuine Intel
  hardware the extra spacing is not wanted.
- `XeFGUnlock.h` gates `XeFGPacing::Install` on `ExtraPacing` as well as `unlock`
  (previously `unlock` alone).
- Menu: an "Only FG" checkbox, and `XeFG_Dx12.cpp` now re-arms
  `XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION` only when the value changes
  rather than every dispatch.

---

## 2026-09-18 — `c0ec7979` · Expose the in-game multiplier and follow it

Until this commit, XeFG's ceiling was reported to the game as **`1` in both places the
game asks**, which is what collapsed the game's own frame generation setting to a plain
on/off toggle.

- `Streamline_Hooks.cpp/.h`: `slDLSSGGetState`'s `numFramesToGenerateMax`.
- `NVNGX_Parameter.cpp`: the NGX parameter block's `DLSSG.MultiFrameCountMax`.
- **The ceiling has to fall back to `XeFG\MaxInterpolatedFrames`, not to `1`.** The game
  asks for capabilities *before* the XeFG swapchain exists; without that fallback the
  reported value never changes in practice.
- Games that resolve DLSSG through `slGetFeatureFunction` are handed the dummy
  implementations rather than the hooks, so `dummy_slDLSSGSetOptions` now **records** what
  the game asked for instead of discarding it. That is the only copy of the request on
  that path; without it the new Auto entry sits at 2X forever.
- **`Auto` is added to the MFG control and made the default.** It leaves the config value
  unset, which is what the backend reads as "use the game's multiplier"; the label shows
  the multiplier it resolved to, and an unset value is written back to the ini as `auto`.
  An explicit 2X/3X/4X/Custom still wins over the game.
- `XeFG_Dx12.cpp` asks `XeFGPacing` for the frame render time *before* falling back to
  `_ftDelta`. `Upscaler_Inputs_Dx12` fills `_ftDelta` with `lastFGFrameTime`, so trying it
  first made that self-referential present-to-present value always win and
  `RenderTimeMs()` was never reached.

---

## 2026-09-18 — `abb4bc62` · Raise the second ceiling, in `libxell.dll`

Unlocking MFG in `libxess_fg.dll` is not sufficient on its own — the provider has two
independent gates and they have to agree:

| DLL | Where | What it is |
|---|---|---|
| `libxess_fg.dll` | `xefgSwapChainGetProperties` | `maxSupportedInterpolations` — the number OptiScaler reads to build its MFG combo (U5) |
| `libxell.dll` | `xellSetGeneratedFramesCount` | the `framesCount` argument check — the number the provider hands back to XeLL for the burst |

Both ship with a ceiling of 3 interpolated frames (4X), and both are driven from the same
config value.

### `OptiScaler/proxies/XeLLUnLock.h` (new, 305 lines)

The gate is a plain argument validation:

```
cmp  ebx, 3
jbe  0xD1D5          ; framesCount <= 3 -> proceed
mov  eax, -4         ; XELL_RESULT_ERROR_INVALID_ARGUMENT
jmp  0xD20B
```

The patch rewrites the **immediate, not the branch**: `83 FB 03 76 07` → `83 FB N 76 07`.
The validation survives and simply accepts a larger count. (An earlier cut turned the `jbe`
into an unconditional `jmp`, which would delete the check outright and let a caller pass
`framesCount = 0` or garbage straight through.)

The immediate here is **imm8**, unlike the imm32 sites in `libxess_fg`, so `N` must fit in
a signed byte — 127 is the ceiling (`Imm8Ceiling`), far above the 2..8 this is meant for.
The patch is **raise-only** and gated on `XeFG\UnlockMFG`, like the `libxess_fg` set.

Two known builds, both with a byte-identical and unique pattern:

| `TimeDateStamp` | `SizeOfImage` | RVA |
|---|---|---|
| `0x6A561284` | `0x0006A000` | `0x00D1C9` |
| `0x69A6C659` | `0x00069000` | `0x00CED9` |

Wired into `XeLL_Proxy.h::HookXeLL`, right after the module handle is stored.

### Why it is needed — measured, not assumed

`XeFG_Dx12.cpp:101` calls
`XeFGProxy::SetLatencyReduction()(_swapChainContext, fakenvapi::getCurrentContext())`,
handing **OptiScaler's own XeLL context** to the provider. So `libxess_fg.dll` is the
caller of `xellSetGeneratedFramesCount` — confirmed by instrumenting the function and
reading the return address, not inferred.

A read-only logging probe in Cyberpunk 2077 at 6X (config `XeFG\InterpolationCount: 5`)
recorded, over one session:

- **7304 calls, every one `caller=libxess_fg.dll`, `ours=true` (our context), returning `0`**,
  with zero errors anywhere in the log.
- `count=5` in 4606 of them — faithful forwarding of the configured multiplier, not a
  clamped value. The remaining 2698 are `count=0`, only in startup/transition clusters;
  the tail of the run is a pure `count=5` stream.

Read off the on-disk disassembly (the file is never modified, so it still shows
`cmp ebx,3`), the `mov eax, -4` path is **unconditional** for `ebx > 3` and there is no
bypass. So in that run **all 4606 `count=5` calls would have returned `-4` without the
patch.** This is not a no-op safety net; it is what makes anything above 4X work at all.

The probe was scaffolding and has been removed again — the hook is not in the tree.

---

## Current state

### Ceilings

| Ceiling | Where | Shipped | Patched to | Switch |
|---|---|---|---|---|
| Default interpolation ceiling | `libxess_fg.dll` `0x1A517D` (U3) | 3 | `N` | `XeFG\UnlockMFG` |
| `maxSupportedInterpolations` | `libxess_fg.dll` `0x20973B` (U5) | 1 | `N` | `XeFG\UnlockMFG` |
| `xellSetGeneratedFramesCount` arg check | `libxell.dll` | 3 | `N` | `XeFG\UnlockMFG` |
| Multiplier ceiling | `Config::XeFGMaxInterpolations` | — | **7 (8X)** | — |

### Configuration

| Key | Range | Default | Meaning |
|---|---|---|---|
| `XeFG\UnlockMFG` | bool | `true` | Master switch for both DLL patch sets and for pacing |
| `XeFG\InterpolationCount` | 1..7, or `auto` | `auto` | `auto` = follow the game's multiplier; otherwise 2X..8X. Multiplier = value + 1 |
| `XeFG\MaxInterpolatedFrames` | 1..7 | `7` (8X) | `N` written into every patch above **and** declared to the provider as the swapchain's `maxInterpolatedFrames` at init |
| `XeFG\ExtraPacing` | bool | `true` | Per-generated-frame pacing. Defaults to `false` on native Intel adapters |

### What was deliberately not done

- **Injecting a count into XeLL from OptiScaler's side.** It would be a silent no-op: the
  `o_xellSetGeneratedFramesCount` pointer is never assigned on the actually-compiled branch
  of `ll_xell.h`, so the call site short-circuits. It would also be actively harmful —
  `libxell` keys its per-frame accounting by `frame_id` and has a
  *"XeFG missed generation N times. Disabling XeFG temporarily."* punish path, and its FPS
  cap scales with the multiplier, which `ll_xell.cpp` already compensates for with a `/2`.
  The provider forwards the right number on its own; the only correct lever is to stop
  rejecting it.
- **Publishing `SleepParams::fg_multiplier`.** Dead code in this build —
  `LOW_LATENCY_INPUTS` in `SysUtils.h` is commented out, so its only consumer
  (`input_reflex.cpp`) is not compiled, and `NvAPI_D3D_GetSleepStatus` is answered by
  fakenvapi's `low_latency_d3d.cpp`, which copies three fields and not this one.

### Verification status

- **U1–U5** — applied in Cyberpunk 2077: the provider's reported maximum went 1 → 5, the
  multiplier combo appeared, and six live multiplier switches (3↔5↔4) over 90 seconds
  produced no crash.
- **`libxell` gate** — measured as described above: 7304 calls at 6X, all forwarded through
  our context, all returning `0`.
- **Pacing** — the fence-wait variant was built, run and then **removed**: over a 5-second
  window it blocked for 1856.5 ms (**37% of wall clock**), 275 of 516 waits timed out
  (`WaitForSingleObject` rounds a short timeout up to the system timer tick — ≈6.75 ms per
  timeout), and the measured gap between generated frames ran 6.31 ms average against a
  4.65 ms target. `XeFGPacing.h` is now wall-clock only, and the provider's scheduler is
  used where it can be reached.
- **Multipliers** — 2X..6X have been exercised in game. 2X–4X is the range that holds up
  without an external frame limiter. 5X and above are usable but need one.

### The ceiling pacing cannot fix

Above 4X the present rate can exceed the display's vblank rate — measured at 1080p/165 Hz
with a ~54.6 fps real frame, 4X presents **218 times a second against 165 vblanks**. The
panel can only show the newest frame at each vblank, so evenly spaced content is sampled
at 4.58 / 9.15 ms alternately. That is judder by construction; no pacing algorithm removes
it. Vsync does — `Present` then blocks on vblank and the pipeline self-regulates to
`165 / 4 = 41.25` real fps, showing each frame exactly once. Without vsync the only fix is
capping the real frame rate at `refresh / multiplier`.

---

## Credits

- **YiBoF** — Intel Arc support (`9f492c0e`, PR #1).
- **cdozdil**, **FakeMichau** and the upstream OptiScaler contributors — the config bound
  removal and the dynamic menu labels the unlock builds on, and the Dx12 swapchain
  description work.
- Unlock, pacing, multiplier exposure and the `libxell` gate — this fork.
