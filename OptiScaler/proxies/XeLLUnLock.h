#pragma once

// Raises the *second* interpolation ceiling - the one inside Intel's libxell.dll.
//
// Unlocking MFG in libxess_fg.dll (see XeFGUnlock.h) is not sufficient on its
// own. The provider has two independent gates and they have to agree:
//
//   libxess_fg.dll   xefgSwapChainGetProperties -> maxSupportedInterpolations
//                    the number OptiScaler reads to build its MFG combo
//   libxell.dll      xellSetGeneratedFramesCount -> framesCount argument check
//                    the number the provider hands back to XeLL for the burst
//
// Both ship with a ceiling of 3 interpolated frames (3 = 4X) and both are
// derived from the same config value, so this file writes the same N that U3
// and U5 write into libxess_fg.
//
// The gate is a plain argument validation:
//
//   cmp  ebx, 3
//   jbe  0xD1D5          ; framesCount <= 3 -> proceed
//   mov  eax, -4         ; XELL_RESULT_ERROR_INVALID_ARGUMENT
//   jmp  0xD20B
//
// This patch rewrites the immediate, not the branch:
//
//   83 FB 03 76 07  ->  83 FB N 76 07
//
// so the validation survives and simply accepts a larger count. Turning the
// `jbe` into an unconditional `jmp` (as the first cut of this patch did) would
// delete the check outright and let a caller pass framesCount = 0 or garbage
// straight through to the provider.
//
// Note that the immediate here is *imm8*, unlike the imm32 sites in
// libxess_fg, so N has to fit in a signed byte - 127 is the ceiling, which is
// far above the 2..8 this is meant for.
//
// The dll on disk is never modified: we patch the already mapped image, verify
// the write by read-back, and undo it if any step fails. A failed unlock is not
// fatal - the provider just behaves as it always did.

#include "SysUtils.h"
#include "Logger.h"
#include "Config.h"

class XeLLUnlock
{
  public:
    static bool Applied() { return _applied; }

    // Returns true when the patch was applied. Safe to call more than once.
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

        // The gate is at a fixed RVA, and that RVA moves between provider
        // builds - the two libxell.dll on this machine differ by 0x2F0. An
        // unrecognised build is left alone rather than guessed at.
        const Build* build = nullptr;

        for (const auto& candidate : KnownBuilds)
        {
            if (nt->FileHeader.TimeDateStamp == candidate.stamp &&
                nt->OptionalHeader.SizeOfImage == candidate.sizeOfImage)
            {
                build = &candidate;
                break;
            }
        }

        if (build == nullptr)
        {
            LOG_WARN("XeLL unlock: unrecognised provider build {:#010x}/{:#x}, skipping",
                     nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
            return false;
        }

        int32_t maxInterp = Config::Instance()->FGXeFGMaxInterpolatedFrames.value_or_default();

        if (maxInterp < 1 || maxInterp > MaxAllowedInterpolations)
            maxInterp = MaxAllowedInterpolations;

        if (maxInterp > Imm8Ceiling)
            maxInterp = Imm8Ceiling;

        // Only ever raise the gate. Writing a smaller N would make the provider
        // stricter than it ships, which could break a 4X that used to work.
        const bool unlock = maxInterp > build->stockCeiling;

        LOG_INFO("XeLL unlock: recognised provider build {:#010x}, gate at {:#x}, {} -> {}", build->stamp,
                 build->cmpRva, build->stockCeiling, unlock ? maxInterp : build->stockCeiling);

        static const uint8_t u1Old[] = { 0x83, 0xFB, 0x03, 0x76, 0x07 };
        uint8_t u1New[] = { 0x83, 0xFB, 0x03, 0x76, 0x07 };

        // Byte 2 of `83 FB imm8` is the immediate being compared against.
        u1New[2] = static_cast<uint8_t>(unlock ? maxInterp : build->stockCeiling);

        const Patch patches[] = {
            { build->cmpRva, u1Old, u1New, sizeof(u1Old), unlock, "U1/generated-frames-count" },
        };

        constexpr int32_t PatchCount = static_cast<int32_t>(sizeof(patches) / sizeof(patches[0]));

        Edited applied[PatchCount] {};
        int32_t appliedCount = 0;

        for (const auto& patch : patches)
        {
            if (!patch.enabled)
                continue;

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

        if (appliedCount == 0)
            return false;

        _applied = true;

        LOG_INFO("XeLL unlock: {} of {} patch(es) applied, generated frame count allowed up to {}X", appliedCount,
                 PatchCount, maxInterp + 1);

        return true;
    }

  private:
    // Same shared sanity bound libxess_fg uses; the two ceilings are read from
    // the same config value and must not disagree.
    static constexpr int32_t MaxAllowedInterpolations = Config::XeFGMaxInterpolations;

    // `83 FB imm8` sign-extends, so the immediate is a signed byte.
    static constexpr int32_t Imm8Ceiling = 127;

    // Build identity and gate location. stockCeiling is the immediate the build
    // ships with, read out of the disassembly rather than assumed.
    struct Build
    {
        uint32_t stamp;
        uint32_t sizeOfImage;
        uint32_t cmpRva;
        int32_t stockCeiling;
    };

    static constexpr Build KnownBuilds[] = {
        // XeSS MFG Universal package, the provider set this project ships.
        { 0x6A561284, 0x0006A000, 0x00D1C9, 3 },
        // The copy in OptiScaler's own external/xess - same gate, different RVA.
        { 0x69A6C659, 0x00069000, 0x00CED9, 3 },
    };

    struct Patch
    {
        uint32_t rva;
        const uint8_t* expected;
        const uint8_t* replacement;
        uint32_t size;
        bool enabled;
        const char* name;
    };

    struct Edited
    {
        uint8_t* dst;
        const uint8_t* original;
        uint32_t size;
    };

    inline static bool _applied = false;

    static IMAGE_NT_HEADERS* NtHeaders(uint8_t* base)
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);

        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return nullptr;

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

        return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
    }

    static const IMAGE_SECTION_HEADER* FindSection(uint8_t* base, const char* name)
    {
        auto* nt = NtHeaders(base);

        if (nt == nullptr)
            return nullptr;

        auto* section = IMAGE_FIRST_SECTION(nt);

        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++)
        {
            if (strncmp(reinterpret_cast<const char*>(section->Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0)
                return section;
        }

        return nullptr;
    }

    static bool Contains(const IMAGE_SECTION_HEADER* section, uint32_t rva, uint32_t size)
    {
        uint32_t start = section->VirtualAddress;
        uint32_t end = start + section->Misc.VirtualSize;

        return rva >= start && rva + size <= end;
    }

    static void WriteRaw(uint8_t* dst, const uint8_t* bytes, uint32_t size)
    {
        DWORD oldProtect = 0;

        if (!VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &oldProtect))
            return;

        memcpy(dst, bytes, size);
        FlushInstructionCache(GetCurrentProcess(), dst, size);

        DWORD ignored = 0;
        VirtualProtect(dst, size, oldProtect, &ignored);
    }

    static bool WriteVerified(uint8_t* dst, const uint8_t* bytes, uint32_t size)
    {
        WriteRaw(dst, bytes, size);
        return memcmp(dst, bytes, size) == 0;
    }

    static void Rollback(const Edited* applied, int32_t count)
    {
        if (count == 0)
            return;

        // Undo newest first so the image ends up exactly as the provider shipped it.
        for (int32_t i = count - 1; i >= 0; i--)
            WriteRaw(applied[i].dst, applied[i].original, applied[i].size);

        LOG_INFO("XeLL unlock: rolled back {} patch(es)", count);
    }

    static std::string ToHex(const uint8_t* bytes, uint32_t size)
    {
        static const char* digits = "0123456789ABCDEF";

        std::string out;
        out.reserve(size * 3);

        for (uint32_t i = 0; i < size; i++)
        {
            if (i > 0)
                out.push_back(' ');

            out.push_back(digits[bytes[i] >> 4]);
            out.push_back(digits[bytes[i] & 0xF]);
        }

        return out;
    }
};
