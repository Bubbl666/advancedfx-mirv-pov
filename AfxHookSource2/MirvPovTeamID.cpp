#include "stdafx.h"

#include "MirvPovTeamID.h"

#include "ClientEntitySystem.h"
#include "MirvPovCore.h"

#include "../shared/AfxConsole.h"
#include "../shared/AfxDetours.h"
#include "../shared/binutils.h"
#include "../deps/release/prop/cs2/sdk_src/public/icvar.h"

#include <Windows.h>
#include <stdint.h>

namespace {

using GetTeamIdContextPlayerFn = CEntityInstance * (__fastcall *)();

uint8_t * g_TeamIdCallSite = nullptr;
uint8_t g_TeamIdOriginalCall[5] = {};
uint8_t * g_TeamIdThunk = nullptr;
GetTeamIdContextPlayerFn g_GetNativeTeamIdContextPlayer = nullptr;
bool g_TeamIdPatched = false;

bool IsSpectatorXrayEnabled()
{
    if(nullptr == SOURCESDK::CS2::g_pCVar) return true;

    static SOURCESDK::CS2::ConVarHandle handle;
    if(!handle.IsValid()) handle = SOURCESDK::CS2::g_pCVar->FindConVar("spec_show_xray", false);
    if(!handle.IsValid()) return true;

    SOURCESDK::CS2::Cvar_s * cvar = SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
    return nullptr == cvar || 0 != cvar->m_Value.m_i32Value;
}

CEntityInstance * __fastcall GetTeamIdContextPlayer()
{
    CEntityInstance * nativePlayer = g_GetNativeTeamIdContextPlayer
        ? g_GetNativeTeamIdContextPlayer()
        : nullptr;

    if(!MirvPov_IsEnabled()) return nativePlayer;

    __try {
        if(IsSpectatorXrayEnabled()) return nativePlayer;

        CEntityInstance * povPlayer = GetCurrentPovPlayerPawn();
        if(nullptr == povPlayer || !povPlayer->IsPlayerPawn()) return nativePlayer;

        return povPlayer;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nativePlayer;
    }
}

bool CalcRel32(uint8_t * fromNext, uint8_t * target, int32_t & result)
{
    intptr_t relative = target - fromNext;
    if(relative < INT32_MIN || INT32_MAX < relative) return false;
    result = static_cast<int32_t>(relative);
    return true;
}

uint8_t * AllocNear(uint8_t * target, size_t size)
{
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    const uintptr_t granularity = systemInfo.dwAllocationGranularity;
    const uintptr_t targetAddress = reinterpret_cast<uintptr_t>(target);
    const uintptr_t minimumAddress = targetAddress > 0x7fff0000
        ? targetAddress - 0x7fff0000
        : 0;
    const uintptr_t maximumAddress = targetAddress + 0x7fff0000;

    for(uintptr_t offset = 0; offset < 0x7fff0000; offset += granularity) {
        if(targetAddress >= offset + granularity) {
            uintptr_t address = (targetAddress - offset) & ~(granularity - 1);
            if(minimumAddress <= address) {
                if(void * result = VirtualAlloc(
                    reinterpret_cast<void *>(address), size,
                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) {
                    return static_cast<uint8_t *>(result);
                }
            }
        }

        uintptr_t address = (targetAddress + offset + granularity - 1) & ~(granularity - 1);
        if(address <= maximumAddress) {
            if(void * result = VirtualAlloc(
                reinterpret_cast<void *>(address), size,
                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) {
                return static_cast<uint8_t *>(result);
            }
        }
    }

    return nullptr;
}

} // namespace

void MirvPovTeamID_ApplyPatches(HMODULE clientDll)
{
    if(g_TeamIdPatched) return;
    if(nullptr == clientDll) {
        advancedfx::Warning("[mirv_pov_teamid] client.dll is not loaded.\n");
        return;
    }

    Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    if(!sections.Eof()) textRange = sections.GetMemRange();

    // TeamID obtains its native context player immediately after its display-state player.
    auto sequence = Afx::BinUtils::FindPatternString(
        textRange,
        "E8 ?? ?? ?? ?? 4C 8B F0 48 89 45 ?? E8 ?? ?? ?? ?? 48 8B F0 48 89 45 ?? E8");
    if(sequence.IsEmpty()) {
        advancedfx::Warning("[mirv_pov_teamid] TeamID context call-site was not found.\n");
        return;
    }

    uint8_t * callSite = reinterpret_cast<uint8_t *>(sequence.Start) + 12;
    if(0xE8 != callSite[0]) {
        advancedfx::Warning("[mirv_pov_teamid] TeamID context call-site has an unexpected opcode.\n");
        return;
    }

    int32_t nativeRelative = 0;
    memcpy(&nativeRelative, callSite + 1, sizeof(nativeRelative));
    g_GetNativeTeamIdContextPlayer = reinterpret_cast<GetTeamIdContextPlayerFn>(
        callSite + 5 + nativeRelative);

    uint8_t * thunk = AllocNear(callSite, 16);
    if(nullptr == thunk) {
        advancedfx::Warning("[mirv_pov_teamid] Could not allocate the TeamID context thunk.\n");
        g_GetNativeTeamIdContextPlayer = nullptr;
        return;
    }

    // mov rax, GetTeamIdContextPlayer; jmp rax
    thunk[0] = 0x48;
    thunk[1] = 0xB8;
    uint64_t wrapperAddress = reinterpret_cast<uint64_t>(&GetTeamIdContextPlayer);
    memcpy(thunk + 2, &wrapperAddress, sizeof(wrapperAddress));
    thunk[10] = 0xFF;
    thunk[11] = 0xE0;
    FlushInstructionCache(GetCurrentProcess(), thunk, 12);

    int32_t thunkRelative = 0;
    if(!CalcRel32(callSite + 5, thunk, thunkRelative)) {
        advancedfx::Warning("[mirv_pov_teamid] TeamID context thunk is out of range.\n");
        VirtualFree(thunk, 0, MEM_RELEASE);
        g_GetNativeTeamIdContextPlayer = nullptr;
        return;
    }

    uint8_t replacement[5] = {0xE8, 0, 0, 0, 0};
    memcpy(replacement + 1, &thunkRelative, sizeof(thunkRelative));
    memcpy(g_TeamIdOriginalCall, callSite, sizeof(g_TeamIdOriginalCall));

    MdtMemBlockInfos memory;
    MdtMemAccessBegin(callSite, sizeof(replacement), &memory);
    memcpy(callSite, replacement, sizeof(replacement));
    FlushInstructionCache(GetCurrentProcess(), callSite, sizeof(replacement));
    MdtMemAccessEnd(&memory);

    g_TeamIdCallSite = callSite;
    g_TeamIdThunk = thunk;
    g_TeamIdPatched = true;
}

void MirvPovTeamID_RemovePatches()
{
    if(!g_TeamIdPatched || nullptr == g_TeamIdCallSite) return;

    MdtMemBlockInfos memory;
    MdtMemAccessBegin(g_TeamIdCallSite, sizeof(g_TeamIdOriginalCall), &memory);
    memcpy(g_TeamIdCallSite, g_TeamIdOriginalCall, sizeof(g_TeamIdOriginalCall));
    bool restored = 0 == memcmp(
        g_TeamIdCallSite, g_TeamIdOriginalCall, sizeof(g_TeamIdOriginalCall))
        && 0 != FlushInstructionCache(
            GetCurrentProcess(), g_TeamIdCallSite, sizeof(g_TeamIdOriginalCall));
    MdtMemAccessEnd(&memory);
    if(!restored) {
        advancedfx::Warning("[mirv_pov_teamid] Could not confirm restoration; keeping the thunk allocated.\n");
        return;
    }

    if(g_TeamIdThunk) VirtualFree(g_TeamIdThunk, 0, MEM_RELEASE);
    g_TeamIdCallSite = nullptr;
    g_TeamIdThunk = nullptr;
    g_GetNativeTeamIdContextPlayer = nullptr;
    g_TeamIdPatched = false;
}
