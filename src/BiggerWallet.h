#ifndef BIGGER_WALLET_H
#define BIGGER_WALLET_H

#include <stdint.h>
#include <windows.h>

#define MOD_NAME "BiggerWallet"
#define COIN_MAX_VALUE 999
#define HOTKEY_POLL_INTERVAL_MS 16U
#define BOOTSTRAP_POLL_INTERVAL_MS 1000U
#define BATTLE_ACTIVE_WINDOW_MS 1500ULL

#define RVA_ADVENTURE_STATE_SINGLETON_PTR 0x13C7560

#define RVA_BATTLE_COIN_ADJUST_SAFE 0x00114E38

// Jesus christ, why can't there just be one universal clamp I can patch? 
// Why DID YOU HARDCODE THE CHECKS mr glaiel??? WHY????
#define RVA_PATCH_COIN_CAP_1 0x00114E7D // Main coin adjust clamp...
#define RVA_PATCH_COIN_CAP_2 0x002E1BD8 // Another generic coin adjust helper...
#define RVA_PATCH_COIN_CAP_3 0x003B9243 // Likely some kinda event/reward path??
#define RVA_PATCH_COIN_CAP_4 0x0079A70C // Likely shop/event spend path...
#define RVA_PATCH_COIN_CAP_5 0x007EC83B // Adds money from an event/reward path, then clamps...
#define RVA_PATCH_COIN_CAP_6 0x0091C844 // Another purchase/payment path...
#define RVA_PATCH_COIN_CAP_7 0x0092BEED // Adds a reward amount to adventure coins, then clamps...
#define RVA_PATCH_COIN_CAP_8 0x0092C2C6 // Another spend/deduct path, then clamps...

#define ADVENTURE_COINS_OFFSET 0x148
#define HELD_COINS_OFFSET 0x0C90
#define HELD_COINS_MODE_FLAG_OFFSET 0x0C94

#define BATTLE_COIN_HOOK_STOLEN_BYTES 17

typedef void (__fastcall *fn_battle_coin_adjust_safe)(void* owner, int delta);

#endif