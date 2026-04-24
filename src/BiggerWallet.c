#include "BiggerWallet.h"
#include "mewjector.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static const bool ENABLE_DEBUG_LOGS = false;

static MewjectorAPI g_mj;
static HMODULE g_hModule = NULL;
static fn_battle_coin_adjust_safe g_origBattleCoinAdjustSafe = NULL;
static volatile void* g_lastBattleOwner = NULL;
static volatile LONG64 g_lastBattleSeenTick = 0;
static volatile LONG g_runtimeInstallStarted = 0;
static volatile LONG g_runtimeInstalled = 0;
static volatile LONG g_loggedBattleHookSeen = 0;
static HANDLE g_bootstrapTimerQueue = NULL;
static HANDLE g_bootstrapTimer = NULL;
static volatile LONG g_bootstrapTimerStarted = 0;
static volatile LONG g_bootstrapPollCount = 0;

static void EnsureRuntimeInstalled(void);

static void Log(const char* fmt, ...)
{
    char buffer[512];
    va_list ap;

    if (!ENABLE_DEBUG_LOGS || !g_mj.Log)
    {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    g_mj.Log(MOD_NAME, "%s", buffer);
}

static UINT_PTR GetGameBase(void)
{
    if (!g_mj.GetGameBase)
    {
        return 0U;
    }

    return g_mj.GetGameBase();
}

static void* GetAdventureState(void)
{
    UINT_PTR gameBase;
    UINT_PTR singletonAddress;
    void* state;

    gameBase = GetGameBase();

    if (!gameBase)
    {
        return NULL;
    }

    singletonAddress = gameBase + (UINT_PTR)RVA_ADVENTURE_STATE_SINGLETON_PTR;
    state = NULL;

    __try
    {
        state = *(void**)singletonAddress;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        state = NULL;
    }

    return state;
}

static void WriteInt32Rva(UINT_PTR rva, int32_t value)
{
    UINT_PTR gameBase;
    DWORD oldProtect;
    int32_t* target;

    gameBase = GetGameBase();

    if (!gameBase)
    {
        Log("WriteInt32Rva failed: Game base unavailable for RVA 0x%X", (unsigned int)rva);
        return;
    }

    target = (int32_t*)(gameBase + rva);
    oldProtect = 0U;

    if (!VirtualProtect(target, sizeof(*target), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("WriteInt32Rva failed: VirtualProtect failed for RVA 0x%X", (unsigned int)rva);
        return;
    }

    *target = value;
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(*target));
    VirtualProtect(target, sizeof(*target), oldProtect, &oldProtect);
    Log("Patched RVA 0x%X => %d", (unsigned int)rva, value);
}

static void ApplyCoinCapPatches(void)
{
    WriteInt32Rva(RVA_PATCH_COIN_CAP_1, COIN_MAX_VALUE);
    WriteInt32Rva(RVA_PATCH_COIN_CAP_2, COIN_MAX_VALUE);
    WriteInt32Rva(RVA_PATCH_COIN_CAP_3, COIN_MAX_VALUE);
    WriteInt32Rva(RVA_PATCH_COIN_CAP_4, COIN_MAX_VALUE);
    WriteInt32Rva(RVA_PATCH_COIN_CAP_5, COIN_MAX_VALUE);
    WriteInt32Rva(RVA_PATCH_COIN_CAP_6, COIN_MAX_VALUE);
    WriteInt32Rva(RVA_PATCH_COIN_CAP_7, COIN_MAX_VALUE);
    WriteInt32Rva(RVA_PATCH_COIN_CAP_8, COIN_MAX_VALUE);
    Log("Patched all known adventure coin caps to %d", COIN_MAX_VALUE);
}

static int TryReadAdventureCoins(int* outValue)
{
    uint8_t* state;

    if (!outValue)
    {
        return 0;
    }

    state = (uint8_t*)GetAdventureState();

    if (!state)
    {
        Log("TryReadAdventureCoins failed: No adventure state!!!");
        return 0;
    }

    __try
    {
        *outValue = *(int*)(state + ADVENTURE_COINS_OFFSET);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("TryReadAdventureCoins exception: State=%p", state);
        return 0;
    }
}

static int TryWriteAdventureCoins(int value)
{
    uint8_t* state;
    state = (uint8_t*)GetAdventureState();

    if (!state)
    {
        Log("TryWriteAdventureCoins failed: No adventure state!");
        return 0;
    }

    __try
    {
        *(int*)(state + ADVENTURE_COINS_OFFSET) = value;
        Log("TryWriteAdventureCoins: %d", value);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("TryWriteAdventureCoins exception: State=%p", state);
        return 0;
    }
}

static int TryReadHeldCoins(int* outValue)
{
    uint8_t* owner;

    if (!outValue)
    {
        return 0;
    }

    owner = (uint8_t*)InterlockedCompareExchangePointer((PVOID volatile*)&g_lastBattleOwner, NULL, NULL);

    if (!owner)
    {
        Log("TryReadHeldCoins failed: No battle owner!");
        return 0;
    }

    __try
    {
        *outValue = *(int*)(owner + HELD_COINS_OFFSET);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("TryReadHeldCoins exception: Owner=%p", owner);
        return 0;
    }
}

static int TryWriteHeldCoins(int value)
{
    uint8_t* owner;
    owner = (uint8_t*)InterlockedCompareExchangePointer((PVOID volatile*)&g_lastBattleOwner, NULL, NULL);

    if (!owner)
    {
        Log("TryWriteHeldCoins failed: No battle owner");
        return 0;
    }

    __try
    {
        *(int*)(owner + HELD_COINS_OFFSET) = value;
        Log("TryWriteHeldCoins: %d", value);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("TryWriteHeldCoins exception: Owner=%p", owner);
        return 0;
    }
}

static int IsBattleLikelyActive(void)
{
    void* owner;
    ULONGLONG nowTick;
    ULONGLONG battleTick;

    owner = InterlockedCompareExchangePointer((PVOID volatile*)&g_lastBattleOwner, NULL, NULL);

    if (!owner)
    {
        return 0;
    }

    nowTick = GetTickCount64();
    battleTick = (ULONGLONG)InterlockedCompareExchange64((volatile LONG64*)&g_lastBattleSeenTick, 0, 0);

    if ((nowTick - battleTick) > BATTLE_ACTIVE_WINDOW_MS)
    {
        return 0;
    }

    return 1;
}

static void __fastcall HookBattleCoinAdjustSafe(void* owner, int delta)
{
    InterlockedExchangePointer((PVOID volatile*)&g_lastBattleOwner, owner);
    InterlockedExchange64((volatile LONG64*)&g_lastBattleSeenTick, (LONG64)GetTickCount64());

    if (InterlockedCompareExchange(&g_loggedBattleHookSeen, 1, 0) == 0)
    {
        __try
        {
            Log("Battle safe hook hit: owner=%p delta=%d mode=%u held=%d", owner, delta, (unsigned int)*(uint8_t*)((uint8_t*)owner + HELD_COINS_MODE_FLAG_OFFSET), *(int*)((uint8_t*)owner + HELD_COINS_OFFSET));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("Battle safe hook hit: owner=%p delta=%d, owner field read failed", owner, delta);
        }
    }

    if (g_origBattleCoinAdjustSafe)
    {
        g_origBattleCoinAdjustSafe(owner, delta);
    }
}

static void EnsureRuntimeInstalled(void)
{
    void* battleTrampoline;
    void* adventureState;

    if (InterlockedCompareExchange(&g_runtimeInstalled, 0, 0) != 0)
    {
        return;
    }

    adventureState = GetAdventureState();

    if (!adventureState)
    {
        LONG pollCount;

        pollCount = InterlockedIncrement(&g_bootstrapPollCount);

        if ((pollCount % 5) == 1)
        {
            Log("Bootstrap waiting for adventure state: poll=%ld gameBase=%p", pollCount, (void*)GetGameBase());
        }

        return;
    }

    if (InterlockedCompareExchange(&g_runtimeInstallStarted, 1, 0) != 0)
    {
        return;
    }

    battleTrampoline = NULL;

    Log("Runtime install begin: adventureState=%p", adventureState);
    ApplyCoinCapPatches();

    if (g_mj.InstallHook(RVA_BATTLE_COIN_ADJUST_SAFE, BATTLE_COIN_HOOK_STOLEN_BYTES, (void*)HookBattleCoinAdjustSafe, &battleTrampoline, 20, MOD_NAME))
    {
        g_origBattleCoinAdjustSafe = (fn_battle_coin_adjust_safe)battleTrampoline;
        Log("Hooked battle coin adjust safe at RVA 0x%X, trampoline=%p", RVA_BATTLE_COIN_ADJUST_SAFE, battleTrampoline);
    }
    else
    {
        Log("Failed to hook battle coin adjust safe at RVA 0x%X", RVA_BATTLE_COIN_ADJUST_SAFE);
        InterlockedExchange(&g_runtimeInstallStarted, 0);
        return;
    }

    InterlockedExchange(&g_runtimeInstalled, 1);
    Log("Runtime install end!");
}

static VOID CALLBACK BootstrapTimerProc(PVOID parameter, BOOLEAN timerOrWaitFired)
{
    (void)parameter;
    (void)timerOrWaitFired;

    if (!g_mj.GetGameBase)
    {
        MJ_Resolve(&g_mj);
    }

    EnsureRuntimeInstalled();
}

static void StartBootstrapTimer(void)
{
    if (InterlockedCompareExchange(&g_bootstrapTimerStarted, 1, 0) != 0)
    {
        return;
    }

    g_bootstrapTimerQueue = CreateTimerQueue();

    if (!g_bootstrapTimerQueue)
    {
        InterlockedExchange(&g_bootstrapTimerStarted, 0);
        return;
    }

    if (!CreateTimerQueueTimer(&g_bootstrapTimer, g_bootstrapTimerQueue, BootstrapTimerProc, NULL, BOOTSTRAP_POLL_INTERVAL_MS, BOOTSTRAP_POLL_INTERVAL_MS, WT_EXECUTEDEFAULT))
    {
        DeleteTimerQueue(g_bootstrapTimerQueue);
        g_bootstrapTimerQueue = NULL;
        InterlockedExchange(&g_bootstrapTimerStarted, 0);
        return;
    }

    Log("Bootstrap timer started: polling every %u ms", (unsigned int)BOOTSTRAP_POLL_INTERVAL_MS);
}

static void StopBootstrapTimer(void)
{
    if (g_bootstrapTimerQueue)
    {
        DeleteTimerQueueEx(g_bootstrapTimerQueue, INVALID_HANDLE_VALUE);
        g_bootstrapTimerQueue = NULL;
        g_bootstrapTimer = NULL;
    }

    InterlockedExchange(&g_bootstrapTimerStarted, 0);
}

static void Initialize(void)
{
    if (!MJ_Resolve(&g_mj))
    {
        return;
    }

    Log("Initialize begin: gameBase=%p adventureState=%p", (void*)GetGameBase(), GetAdventureState());
    Log("Wait for live adventure state!");
    StartBootstrapTimer();
    Log("Initialize end!");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);

        if (MJ_Resolve(&g_mj) && g_mj.Log)
        {
            g_mj.Log(MOD_NAME, "Loading!");
        }

        Initialize();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        StopBootstrapTimer();

        if (MJ_Resolve(&g_mj) && g_mj.Log)
        {
            g_mj.Log(MOD_NAME, "Unloading!");
        }
    }

    return TRUE;
}