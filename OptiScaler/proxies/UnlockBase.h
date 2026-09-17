#pragma once

#include "SysUtils.h"
#include "Logger.h"
#include "Config.h"

namespace UnLockBase
{
struct Patch
{
    uint32_t rva;
    const uint8_t* expected;
    const uint8_t* replacement;
    uint32_t size;
    int32_t immOffset;
    bool enabled;
    const char* name;
};

struct Edited
{
    uint8_t* dst;
    const uint8_t* original;
    uint32_t size;
};

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

    LOG_INFO("unlock: rolled back {} patch(es)", count);
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
}; // namespace UnLockBase