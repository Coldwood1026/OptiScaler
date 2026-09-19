#pragma once

#include "SysUtils.h"
#include "Logger.h"
#include "Config.h"
#include "UnlockBase.h"

using namespace UnLockBase;

class XeLLUnlock
{
    inline static bool _applied = false;
    inline static uint32_t KnownBuildStamp = 0x6a561284;
    inline static uint32_t KnownSizeOfImage = 0x0006a000;

  public:
    static bool Applied() { return _applied; }

    // Returns true when every patch was applied. Safe to call more than once.
    static bool Apply(HMODULE module)
    {
        if (_applied)
            return true;

        if (module == nullptr)
            return false;

        if (!Config::Instance()->FGXeFGUnlockEnabled.value_or_default())
        {
            LOG_INFO("XeLL unlock: disabled by config (XeFG\\UnlockMFG)");
            return false;
        }

        auto* base = reinterpret_cast<uint8_t*>(module);
        auto* nt = NtHeaders(base);
        auto* text = FindSection(base, ".text");

        if (nt == nullptr || text == nullptr)
        {
            LOG_WARN("XeLL unlock: provider has no usable PE headers, skipping");
            return false;
        }

        if (nt->FileHeader.TimeDateStamp != KnownBuildStamp || nt->OptionalHeader.SizeOfImage != KnownSizeOfImage)
            LOG_WARN("XeLL unlock: unrecognised provider build {:#010x}/{:#x}, relying on per-byte checks",
                     nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        else
            LOG_INFO("XeLL unlock: recognised provider build {:#010x}", KnownBuildStamp);

        static const uint8_t u1Old[] = { 0x76, 0x07 };
        static const uint8_t u1New[] = { 0xEB, 0x07 };

        const Patch patches[] = {
            /*
            83 FB 03  cmp ebx, 3
            76 07  jbe -> EB, 07  jmp
            */
            { 0x00D1CC, u1Old, u1New, sizeof(u1Old), -1, true, "U1/UnlockGenFramesMaxValue" },
        };

        constexpr int32_t PatchCount = static_cast<int32_t>(sizeof(patches) / sizeof(patches[0]));

        Edited applied[PatchCount] {};
        int32_t appliedCount = 0;
        int32_t skipped = 0;

        for (const auto& patch : patches)
        {
            if (!patch.enabled)
            {
                skipped++;
                continue;
            }

            if (!Contains(text, patch.rva, patch.size))
            {
                LOG_WARN("XeLL unlock: {} at {:#x} falls outside .text, aborting", patch.name, patch.rva);
                Rollback(applied, appliedCount);
                return false;
            }

            uint8_t* dst = base + patch.rva;

            if (memcmp(dst, patch.expected, patch.size) != 0)
            {
                LOG_WARN("XeLL unlock: {} at {:#x} has unexpected bytes ({}), aborting", patch.name, patch.rva,
                         ToHex(dst, patch.size));
                Rollback(applied, appliedCount);
                return false;
            }

            if (!WriteVerified(dst, patch.replacement, patch.size))
            {
                LOG_WARN("XeLL unlock: {} at {:#x} failed write verification, aborting", patch.name, patch.rva);
                Rollback(applied, appliedCount);
                return false;
            }

            applied[appliedCount] = { dst, patch.expected, patch.size };
            appliedCount++;

            LOG_INFO("XeLL unlock: {} patched at {:#x} -> {}", patch.name, patch.rva, ToHex(dst, patch.size));
        }

        _applied = true;
        return true;
    };
};
