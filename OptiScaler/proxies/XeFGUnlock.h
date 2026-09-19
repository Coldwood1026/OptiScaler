#pragma once

// Unlocks multi frame generation in Intel's libxess_fg.dll.
//
// Intel gates MFG behind "am I the igxess_fg.dll build?" rather than behind an
// actual hardware capability query, so the whole thing comes down to five
// byte-level patches in the provider's own code:
//
//   U1  0x20DA4F   jne -> jmp    the per-frame frame count resolver no longer
//                                falls back to 2X with "XeLL version x.y.z is
//                                too old to support multi-frame generation"
//   U2  0x1A5DE4   je  -> jmp    Settings::mfgAllowed() always returns true, so
//                                the init path stops downgrading model 17/18
//   U3  0x1A517D   mov ebx,3 -> mov ebx,N
//                                raises the default interpolation ceiling
//   U4  0x1A45C2   mov dword [rdi+0x16c], 1 -> mov dword [rdi+0x16c], N
//                                when the provider decides MFG is unavailable
//                                it pins the override to a single interpolated
//                                frame; pin it to the configured count instead
//   U5  0x20973B   mov eax,1 -> mov eax,N
//                                xefgSwapChainGetProperties reports
//                                maxSupportedInterpolations = 1 whenever the
//                                XeLL version is unknown or older than
//                                1.3.0. OptiScaler hides its MFG combo box
//                                entirely for a reported value of 1, so this
//                                is the one that makes the combo appear.
//
// Unlocking the count is not enough on its own. Above 2X the provider hands
// every generated frame of a burst to the swapchain back to back and only paces
// the burst as a whole, which shows up as the frames arriving bunched up and out
// of order - so the present thunk at 0x25C0 is additionally redirected to pace
// each generated frame individually. See XeFGPacing.h; it is a separate patch
// with its own XeFG\ExtraPacing switch and does not touch the five above.
//
// The dll on disk is never modified: we patch the already mapped image, verify
// every write by read-back, and roll the whole set back if any single step
// fails. A failed unlock is not fatal - the provider just behaves as it always
// did.

#include "SysUtils.h"
#include "Logger.h"
#include "Config.h"
#include "XeFGPacing.h"
#include "UnlockBase.h"

using namespace UnLockBase;

class XeFGUnlock
{
  public:
    static bool Applied() { return _applied; }

    static void ResetApplied() { _applied = false; }

    // Returns true when every patch was applied. Safe to call more than once.
    static bool Apply(HMODULE module)
    {
        if (_applied)
            return true;

        if (module == nullptr)
            return false;

        if (!Config::Instance()->FGXeFGUnlockEnabled.value_or_default())
        {
            LOG_INFO("XeFG unlock: disabled by config (XeFG\\UnlockMFG)");
            return false;
        }

        auto* base = reinterpret_cast<uint8_t*>(module);
        auto* nt = NtHeaders(base);
        auto* text = FindSection(base, ".text");

        if (nt == nullptr || text == nullptr)
        {
            LOG_WARN("XeFG unlock: provider has no usable PE headers, skipping");
            return false;
        }
        bool check = false;
        uint32_t checkIndex = 0;
        for (int i = 0; i < KnownBuildStamp.size(); i++)
        {
            if (nt->FileHeader.TimeDateStamp == KnownBuildStamp[i] &&
                nt->OptionalHeader.SizeOfImage == KnownSizeOfImage[i])
            {
                check = true;
                checkIndex = i;
                break;
            }
        }
        if (check)
            LOG_INFO("XeFG unlock: recognised provider build {:#010x} {:#010x}", KnownBuildStamp[0],
                     KnownBuildStamp[1]);
        else
        {
            LOG_WARN("XeFG unlock: unrecognised provider build {:#010x}/{:#x}, relying on per-byte checks",
                     nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
            return false;
        }

        int32_t maxInterp = Config::Instance()->FGXeFGMaxInterpolatedFrames.value_or_default();

        if (maxInterp < 1 || maxInterp > MaxReportedInterpolations)
            maxInterp = MaxReportedInterpolations;

        // A reported count of 1 is plain 2X, which is what the unpatched
        // provider already does - so at that setting we leave it alone.
        const bool unlock = maxInterp > 1;

        static const uint8_t u1Old[] = { 0x0F, 0x85, 0xCC, 0x00, 0x00, 0x00 };
        static const uint8_t u1New[] = { 0xE9, 0xCD, 0x00, 0x00, 0x00, 0x90 };

        static const uint8_t u2Old[] = { 0x74, 0x09 };
        static const uint8_t u2New[] = { 0xEB, 0x06 };

        static const uint8_t u3Old[] = { 0xBB, 0x03, 0x00, 0x00, 0x00 };
        static const uint8_t u3New[] = { 0xBB, 0x00, 0x00, 0x00, 0x00 };

        static const uint8_t u4Old[] = { 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
        static const uint8_t u4New[] = { 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

        static const uint8_t u5Old[] = { 0xB8, 0x01, 0x00, 0x00, 0x00 };
        static const uint8_t u5New[] = { 0xB8, 0x00, 0x00, 0x00, 0x00 };

        static const uint8_t u2Old2[] = { 0x74, 0x0C };
        static const uint8_t u2New2[] = { 0xEB, 0x09 };

        static const uint8_t u4Old2[] = { 0xC7, 0x87, 0x8C, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
        static const uint8_t u4New2[] = { 0xC7, 0x87, 0x8C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

        const Patch* patches = nullptr;
        int PatchCount = 0;
        if (checkIndex == 0)
        {
            // immOffset is the offset of a little endian imm32 inside `replacement`
            // that gets overwritten with the configured interpolation count, so the
            // count is baked into the bytes we write rather than patched in later.
            // target file version 1.3.1.78
            const Patch patchesSDK[] = {
                { 0x20DA4F, u1Old, u1New, sizeof(u1Old), -1, unlock, "U1/frame-count-fallback" },
                // Allow all gpu switch mode
                { 0x1A5DE4, u2Old, u2New, sizeof(u2Old), -1, false, "U2/model-downgrade" },
                { 0x1A517D, u3Old, u3New, sizeof(u3Old), 1, unlock, "U3/default-ceiling" },
                { 0x1A45C2, u4Old, u4New, sizeof(u4Old), 6, unlock, "U4/override-clamp" },
                { 0x20973B, u5Old, u5New, sizeof(u5Old), 1, unlock, "U5/reported-maximum" },
            };
            patches = patchesSDK;
            PatchCount = sizeof(patchesSDK) / sizeof(Patch);
        }
        else
        {
            // target file version 1.3.3.93，driver version 101.8992
            const Patch patchesDriver[] = {
                // Allow switching to the Intel-specific model (Code name XE 15)
                // { 0x153634, u2Old2, u2New2, sizeof(u2Old2), -1, unlock, "U2/model-downgrade" },
                { 0x152E3D, u3Old, u3New, sizeof(u3Old), 1, unlock, "U3/default-ceiling" },
                { 0x152272, u4Old2, u4New2, sizeof(u4Old2), 6, unlock, "U4/override-clamp" },
                { 0x1B2CBB, u5Old, u5New, sizeof(u5Old), 1, unlock, "U5/reported-maximum" },
            };
            patches = patchesDriver;
            PatchCount = sizeof(patchesDriver) / sizeof(Patch);
        }
        Edited applied[5] {};
        int32_t appliedCount = 0;
        int32_t skipped = 0;

        for (int i = 0; i < PatchCount; i++)
        {
            const auto& patch = patches[i];
            if (!patch.enabled)
            {
                skipped++;
                continue;
            }

            if (!Contains(text, patch.rva, patch.size))
            {
                LOG_WARN("XeFG unlock: {} at {:#x} falls outside .text, aborting", patch.name, patch.rva);
                Rollback(applied, appliedCount);
                return false;
            }

            uint8_t* dst = base + patch.rva;

            if (memcmp(dst, patch.expected, patch.size) != 0)
            {
                LOG_WARN("XeFG unlock: {} at {:#x} has unexpected bytes ({}), aborting", patch.name, patch.rva,
                         ToHex(dst, patch.size));
                Rollback(applied, appliedCount);
                return false;
            }

            uint8_t buffer[16] {};
            const uint8_t* replacement = patch.replacement;

            if (patch.immOffset >= 0)
            {
                memcpy(buffer, patch.replacement, patch.size);
                buffer[patch.immOffset + 0] = static_cast<uint8_t>(maxInterp & 0xFF);
                buffer[patch.immOffset + 1] = static_cast<uint8_t>((maxInterp >> 8) & 0xFF);
                buffer[patch.immOffset + 2] = static_cast<uint8_t>((maxInterp >> 16) & 0xFF);
                buffer[patch.immOffset + 3] = static_cast<uint8_t>((maxInterp >> 24) & 0xFF);
                replacement = buffer;
            }

            if (!WriteVerified(dst, replacement, patch.size))
            {
                LOG_WARN("XeFG unlock: {} at {:#x} failed write verification, aborting", patch.name, patch.rva);
                Rollback(applied, appliedCount);
                return false;
            }

            applied[appliedCount] = { dst, patch.expected, patch.size };
            appliedCount++;

            LOG_INFO("XeFG unlock: {} patched at {:#x} -> {}", patch.name, patch.rva, ToHex(dst, patch.size));
        }

        _applied = true;

        LOG_INFO("XeFG unlock: {} of {} patches applied ({} skipped), MFG enabled up to {}X", appliedCount, PatchCount,
                 skipped, maxInterp + 1);

        // The provider emits every generated frame of a burst back to back above
        // 2X, which is only reachable now that the multi frame path is unlocked.
        // Independent of the patches above, and harmless if it fails.
        if (unlock)
            XeFGPacing::Install(base);

        return true;
    }

  private:
    // The provider reports an interpolation frame count N, where N=1 is plain 2X.
    //
    // This was 5, justified as "the combo box lists 2X..6X" - which was circular,
    // since that combo box is ours. The patches below write N as a 4 byte
    // immediate, so the encoding has never been what limits this. The ceiling is
    // now the shared sanity bound, and what the user actually gets is decided by
    // XeFG\MaxInterpolatedFrames.
    static constexpr int32_t MaxReportedInterpolations = Config::XeFGMaxInterpolations;

    // Build identity of the libxess_fg.dll these offsets were derived from.
    static constexpr std::array<uint32_t, 2> KnownBuildStamp = { 0x69CB0F4D, 0x6A82BCEA };
    static constexpr std::array<uint32_t, 2> KnownSizeOfImage = { 0x015ED000, 0x01184000 };

    inline static bool _applied = false;
};
