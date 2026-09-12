# XeFG Frame Pacing — Change Notes

> **English** ｜ [中文](README-XeFG-Pacing.zh-CN.md)
>
> This document covers **one capability** of this locally modified OptiScaler
> build: making Intel's generated frames come out **evenly spaced** above 2X,
> instead of as one clump followed by a gap.
>
> **Modified: 2026-09-12** ｜ Based on upstream OptiScaler (GPL-3.0)
> Companion document: [`README-XeFG-MFG-Unlock.md`](README-XeFG-MFG-Unlock.md)
> Full reverse-engineering write-up:
> [`optiscaler xemfg version/`](../XeSSMFG-Universal-v0.2.0.1/optiscaler%20xemfg%20version/README.md)

---

## ⚠️ Disclaimer

Same as the MFG unlock document: **not affiliated with Intel**, **no Intel
binaries redistributed**, all patching is **in-memory at runtime**, and
**nothing on disk is ever modified**. For interoperability research only, no
warranty, not legal advice.

---

## 1. The Problem

The MFG unlock makes 3X and above reachable. Above 2X they do not look right:
frames arrive **bunched together with a gap after them**, which reads as judder
and, at high multipliers, as frames appearing out of order.

The cause is not that the provider has no notion of spacing. It has one, and it
computes the right number — once per burst, at `0x220254`:

```asm
0x220254  mov  r9, [r8+8]        ; count  = generated frames in this burst
0x220258  lea  rcx, [r9+1]       ; count + 1 = the multiplier
0x22025E  mov  rax, [rbp-0x41]   ; duration of the real frame
0x220262  div  rcx
0x220265  mov  r12, rax          ; -> duration / multiplier, per frame
```

The problem is what happens to it. The present loop that follows
(`0x220280..0x22030A`) hands every generated frame to the swapchain **back to
back**, and `r12` is not consumed until the loop is over — by an FPS-limiter
block at `0x220317` that also requires `count > 2` and two further conditions.
So the burst goes out as fast as the swapchain accepts it and the limiter then
holds the **last** frame back. That is the clump.

There is a second, quieter defect. The provider has a real **frame scheduler**
at `0x21EE30` which stamps each frame with a presentation deadline out of
`0x3430` → `0x224B30`:

```
deadline = base
         + min( index * median / (count + 1),          ; nanoseconds
                f * index / (count + 1) * 1000000 )   ; ms -> ns
         - recv * 1000000
```

Two things are wrong with it:

| Defect | Consequence |
|---|---|
| `base` is written in exactly one place — the fence block, which only runs when `gate != 0 && index == 1` | For every other index the deadline is computed from **whatever that stack slot happened to hold**. The provider's own call carries `index == count`, so above 2X it is the only call it makes *and* it computes from garbage. 2X is the one multiplier where its own call has `index == 1` — and 2X is the one multiplier that ever worked. |
| `min()` clamps the step down to `f`, the float at `ring+0x1B8`, clamped to `[0.125, 500.0]` ms | `f` is smaller than a real frame interval — measured at 4X as **8.5 ms against a 20.6 ms frame**. The step came out at **2.13 ms where the burst needed 5.03 ms**. |

And the provider calls that scheduler **only for the last frame of each burst**
(from `0x220411`). Every other generated frame is presented with no
presentation time computed for it at all.

There is also a frame-time feedback path that makes the whole thing
self-locking. `FG_Hooks::FGPresent` samples the clock at entry, *before* any
frame-generation work, and that delta is handed to the provider as
`frameRenderTime`; the provider sizes the burst from it and does not return
until the burst is done. That closes a fixed point:

```
period = renderTime * (count + 1)
```

so the more the burst blocks, the larger the render time the provider is told,
and the slower the next burst is allowed to be.

---

## 2. What Changed

Everything lives under **`OptiScaler/`**. One new header, three modified files,
plus config plumbing.

### 2.1 Added

| File | Notes |
|---|---|
| [`OptiScaler/proxies/XeFGPacing.h`](OptiScaler/proxies/XeFGPacing.h) | **New**, header-only (~1,000 lines, heavily commented). The whole pacing engine: the three thunk hooks, the provider-scheduler binding, the deadline repair, the statistics, and the `Install` entry point. No new translation unit. |

### 2.2 Modified

| File | Change |
|---|---|
| [`OptiScaler/proxies/XeFGUnlock.h`](OptiScaler/proxies/XeFGUnlock.h) | Calls `XeFGPacing::Install(base)` from the unlock's apply pass — the pacing is installed **after** the MFG patches, and only when the unlock is actually being applied. Failure is non-fatal. |
| [`OptiScaler/framegen/xefg/XeFG_Dx12.cpp`](OptiScaler/framegen/xefg/XeFG_Dx12.cpp) | The `frameRenderTime` hand-off. The fallback chain is now `_ftDelta` → `XeFGPacing::RenderTimeMs()` → `state.lastFGFrameTime`, and the value actually handed over is recorded via `NoteFedFrameTime`. |
| [`OptiScaler/menu/menu_common.cpp`](OptiScaler/menu/menu_common.cpp) | The **Extra Pacing** checkbox in the XeFG section, and the MFG multiplier combo reworked (see §4). |
| [`OptiScaler/Config.h`](OptiScaler/Config.h) / [`OptiScaler/Config.cpp`](OptiScaler/Config.cpp) | Two new options: `XeFG\ExtraPacing` (default **true**) and the frame-time source enum behind `FTInput`. |

---

## 3. How the Pacing Works

The design choice worth stating plainly: **no pacing algorithm is invented
here.** The spacing is the provider's own — each generated frame is simply
routed through the provider's own scheduler with the arguments the present path
already holds in its registers. (This is what dashdogy's patch does as well,
except that he has to rebuild those arguments through a ~1500-byte helper
because he hooks without the present's arguments in hand. From inside the
present they are free.)

### 3.1 The three hooks

Every hook site is a 5-byte `jmp` thunk followed by 11 bytes of `int3` padding:
**16 bytes containing nothing but a jump**. That is room for a 14-byte
`jmp qword ptr [rip+0]`, so a thunk can be redirected anywhere in the address
space without relocating a single instruction and without a code cave. Each
thunk's 16 expected bytes are compared before writing, and the write is
verified by read-back.

| # | Thunk | Target | Role |
|---|---|---|---|
| 1 | `0x25C0` | `0x21F730` | **Present.** This is the thunk every generated-frame present goes through. The detour checks `_ReturnAddress()` and paces only the two call sites that matter: `0x2202ED` (the burst loop) and `0x220467` (the burst's last frame). The thunk's two other callers (`0x220A52`, `0x220DAD`) are forwarded untouched. |
| 2 | `0x3100` | `0x21EE30` | **The provider's scheduler.** Each paced frame is handed to it with `arg5` = the frame's index in the burst. All five arguments are derived from the present's own arguments: `arg2` is the burst (`present arg5 - 0x38`), `arg5` is the index the loop is already walking, `arg3` is `burst[0xC0] & 1`, and `arg4` comes from `0x4DA0` given the ring. |
| 3 | `0x3430` | `0x224B30` | **The deadline.** Called once per scheduled frame — ours **and the provider's alike** — and it hands back a writable out pointer, so both defects in §1 are repaired at this single choke point. |

Frames **`1..count-1`** are scheduled explicitly; the provider's own call site
keeps the last one, and the detour mirrors the provider's limiter condition so
that exactly one of the two parties waits on that frame rather than two waits
stacking into one long frame.

### 3.2 Why the last frame of a burst needs its own site

It is not presented from the loop at all. It goes out after the loop and after
the provider has submitted the burst, from `0x220462`, with the same burst in
`arg5` but `arg7 = 0` instead of `1`. Left alone, every burst ends with two
frames carrying the same timestamp — at 4X the delivered pattern is
`0, i, 2i, 2i` instead of `0, i, 2i, 3i`.

### 3.3 The deadline repair

At the `0x3430` hook, `g_nextDeadlineNs` replaces whatever the provider would
have computed:

- the burst's step is `median / (count + 1)`, held **fixed for the whole burst**
  so its frames cannot drift apart from each other;
- the step is rebased off the provider's `base` whenever `base` is not fresh
  (i.e. for every index other than 1);
- the amount `min()` removed is added back.

### 3.4 The gate

The scheduler is not always on, and its entry gate is two bytes of the context
(`0xdbb0` → `0x21ED40`):

```asm
0x21ED40  cmp   byte ptr [rcx + 0x340], 0
0x21ED47  je    0x21ed4c
0x21ED49  xor   al, al                    ; [ctx+0x340] != 0 -> false
0x21ED4B  ret
0x21ED4C  movzx eax, byte ptr [rcx + 0x341] ; else return [ctx+0x341]
```

`0x21EE30` then bails unless that second byte is set too. And `0x340` is the
**same byte the provider's FPS limiter is gated on** — so the scheduler and the
limiter are mutually exclusive by construction: when the limiter owns the
pacing, the scheduler is off, and calling into it is a silent no-op.

`SchedulerUsable()` transcribes that condition. Nothing is handed to the
scheduler unless it would actually run, and refusals are counted — because a
refused call and a completed one are indistinguishable from the outside, and
pretending otherwise is exactly what made an earlier build quietly do nothing.

### 3.5 The fallback, and the fence wait that was removed

If the scheduler thunk cannot be hooked (unexpected bytes, hook failure) the
pacing falls back to a wall clock measuring the real frame period over the last
15 frames. It is a fallback, not the design.

**Waiting on the provider's own D3D12 fence was implemented and then removed.**
The provider has a fence in the swapchain context (`ctx+0x328`, event at
`ctx+0x330`) and knows how to wait on it properly. Doing the same from here
failed for a mundane reason: `WaitForSingleObject` rounds a short timeout up to
the system timer tick, so every wait that expired overshot by about **7 ms**;
over half of them expired, and the measured gap between generated frames ended
up **30–40% above target**. The wall clock is what the channel is left with.

### 3.6 The frame time handed to the provider

To break the self-lock described at the end of §1, the value passed to the
provider is no longer the measured present-to-present delta. The blocking the
current burst has cost so far (`g_burstBlockQpc`) is subtracted from the period
and *that* is handed over, via `RenderTimeMs()`. `XeFG_Dx12.cpp` records it with
`NoteFedFrameTime`, and the pacing report prints it next to the period it came
out of — that pairing is the entire point of the change, since it is what makes
"did the estimate actually differ from the measured period?" a number rather
than an opinion.

The old behaviour is kept as a runtime A/B switch, so the two can be compared
without a rebuild:

| `FTInput` | Behaviour |
|---|---|
| `Input` (**default**) | the render-time estimate: `_ftDelta` → `RenderTimeMs()` → fallback |
| `Opti` | old behaviour — hand over `state.lastFGFrameTime` directly |
| `Zero` | hand over `0.0` |

The multiplier ceiling is not a constant of ours. `XeFGUnlock.h` writes the
configured maximum into the provider's own reported maximum
(`XeFG\MaxInterpolatedFrames`, default **31**), and the menu follows whatever
comes back — so the ceiling *is* that setting, and the menu reaches 32X out of
the box. Nothing in the XeFG path clamps it any more.

That setting is not only a menu bound. `XeFG_Dx12` reads the same reported value
back into `xefg_swapchain_d3d12_init_params_t::maxInterpolatedFrames` at
swapchain init, so declaring a higher ceiling means declaring it to the provider
on every launch. Above 6X is untested: if the provider sizes anything from that
number, the symptom would be a failed init or exhausted VRAM, and the way back is
to set `XeFG\MaxInterpolatedFrames` to 5.

---

## 4. Configuration

| Where | Option | Default | Notes |
|---|---|---|---|
| `OptiScaler.ini`, `[XeFG]` | `ExtraPacing` | `true` | Master switch. Read at install time. |
| in-game menu, XeFG section | **Extra Pacing** | — | Same switch. **Requires a game restart** — the hook rewrites the present thunk at provider init, so it cannot be toggled while the game is running. |
| `OptiScaler.ini`, `[XeFG]` | `InterpolationCount` | `1` | The interpolation count, so the **multiplier is this plus one**. Validated on load to `1..31` — i.e. **2X..32X**. |
| `OptiScaler.ini`, `[XeFG]` | `MaxInterpolatedFrames` | `31` | The ceiling reported to the provider, and therefore the menu's ceiling (multiplier = this plus one). See §3.6. |
| `OptiScaler.ini` | `FTInput` | `Input` | See the table in §3.6. |

### The multiplier menu

The MFG combo lists **2X / 3X / 4X** by name, then **Custom…**. 2X–4X are the
multipliers that hold up on their own; 5X and above are reachable through the
custom slot, which takes a free-form multiplier clamped to the provider's
reported maximum and to nothing else.

**Above 4X the menu shows a warning, and it is not decoration.** At those
multipliers the burst is presented faster than the display refreshes, and
nothing inside the provider can pull that back — it needs the present rate
capped from outside, by **VSync** or a frame-rate cap. Without one, the extra
frames tear and judder.

Selecting the custom slot logs the request at **INFO** level
(`MFG menu: Custom selected, asking for …`), unlike the named entries, which log
at debug level. That is deliberate: the custom slot is the one control whose
value can be rejected downstream, and this line is what separates "the menu
never asked" from "the menu asked and something clamped it".

---

## 5. Limits

- **The MFG unlock is required.** The pacing is installed from the unlock's
  apply pass and only makes sense above 2X, which the unlock is what makes
  reachable. A stock OptiScaler never gets here.
- **Tied to one provider build.** All offsets were derived from
  `libxess_fg.dll` **1.3.1.78** (build identity `0x69CB0F4D`). A different
  provider build means different offsets; the thunk byte comparisons will fail
  and the pacing will decline to install rather than corrupt anything — but the
  offsets themselves would have to be re-derived.
- **Above 4X needs an external cap** (§4).
- **The old fence wait is gone on purpose** (§3.5). Reinstating it means
  re-measuring, not just re-enabling.
- **Nothing here overrides the provider's own hard ceiling**: when the present
  rate exceeds the refresh rate, no amount of intra-burst scheduling fixes it.

---

## 6. Observability and Maintenance

Everything the pacing does is reported through **one INFO line every 5
seconds**:

```
XeFG pacing: {mult}X, real frame {:.2f} ms ({:.1f} fps), target {:.2f} ms/frame;
gap {:.2f} avg / {:.2f} min / {:.2f} max ms over {} frames;
scheduler {} calls, {} refused, {:.2f} ms avg inside;
deadlines {} calls, {} rebased, {} clamped; render-est {:.2f} ms, fed {:.2f} ms
```

Reading it:

| Field | Meaning |
|---|---|
| `real frame` / `target` | the measured period and the period the pacing is aiming for |
| `gap … avg / min / max` | spacing between consecutive **paced** frames — the only direct evidence that they come out evenly. Everything else in the line is a claim about intent. |
| `scheduler … calls / refused` | a refused call and a completed one look identical from outside; this is what separates them |
| `deadlines … calls / rebased / clamped` | how often the provider's own arithmetic had to be corrected |
| `render-est` vs `real frame` | **the decisive pair.** If `render-est` is clearly *below* `real frame`, the self-lock in §1 was real and has been broken. If they are about equal, the measured period is being set by the GPU or the display and the estimate is not what limits anything. |
| `fed` | what was actually handed to the provider. With the default `FTInput` it should track `render-est`; if it tracks `real frame` instead, `RenderTimeMs()` returned 0 and fell through to the fallback. |

### ⚠️ A format-string error here kills the pacing silently

This is the one maintenance hazard that has already cost a full test run, so it
is worth stating in full.

That `LOG_INFO` runs on the **present thread** and has **no `try`/`catch`**
around it. `fmt` ignores surplus arguments, so an over-supplied call is
harmless — but a **type specifier landing on the wrong argument is fatal**: it
throws `fmt::format_error` at runtime, and the throw unwinds straight out of the
hook. Because the statistics are reset *after* the report is emitted, the very
first throw leaves the deadline permanently in the past, so **every subsequent
call throws too and the pacing is dead for the rest of the session**.

The reason it is worth a whole section: it is **silent**. The line that would
have reported the problem is the line that throws, so the log simply stops
mentioning pacing — which is easy to mistake for "the pacing had no effect".
An entire build was tested and produced zero usable data this way.

**Before changing that report line, count the placeholders.** The repository
carries a script for exactly this:

| Script | Purpose |
|---|---|
| `_analysis/check_reportstats_args.py` | counts the format placeholders against the argument list and prints the pairing as the compiler will produce it, flagging a float specifier on a counter |
| `_analysis/probe_strings.py` | decides which build on disk is which round, from its log strings |
| `_analysis/verify_pacing_build.py` | asserts the strings this change is supposed to contain are present, and the ones it removed are absent |

### Telling builds apart

The strings are the reliable marker, not the file size: builds that differ only
by a change to that report line come out **the same size**, because a few
characters either way land inside the section alignment.

---

## 7. Provenance

The pacing was developed over nine measured rounds against a live
`Cyberpunk 2077` session on an RTX 2060, with every round's prediction checked
against the previous round's log before the next change was made. Several
earlier conclusions were **refuted by measurement and withdrawn** — the fence
wait (§3.5), and the assumption that `0x21EE30` was a fence wait at all (it is
the scheduler, which is what made this whole approach possible).

The round-by-round record, including the refutations and the open questions, is
in [`optiscaler xemfg version/08-PACING.md`](../XeSSMFG-Universal-v0.2.0.1/optiscaler%20xemfg%20version/08-PACING.md).
