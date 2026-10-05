// MGPU Bridge - D1.5a: the read-only latency probe (0.3.0 R&D branch only).
//
// WHAT IT IS FOR. Gives the HOW for our own Reflex on case-B titles
// (Dawnwalker: no Reflex setting, Reflex only with frame gen): where in the
// frame the game's own Reflex loop sits when frame gen is on, whether the
// driver keeps any latency report when frame gen is off, and the driver's own
// sleep status (bUseGameSleep, fgMultiplier) as a read-only A/B signal.
// Second use: whether the reports' present times sit on the same clock as our
// present timestamps, which would label each present rendered or generated.
//
// HOW. Read only. It takes the game's ID3D12Device and swap chain from the
// init_swapchain event (only for the chain dllmain has proved, by LUID, to be
// the game's), finds nvapi64.dll with GetModuleHandleW (never loads it),
// resolves NvAPI_D3D_GetSleepStatus and NvAPI_D3D_GetLatency through
// nvapi_QueryInterface, and calls them from our present event on the game chain every
// 2 s. Nothing is swapped, hooked or written anywhere outside this file.
// It never calls NvAPI_Initialize, NvAPI_Unload, SetSleepMode, Sleep or
// SetLatencyMarker.
//
// OFF BY DEFAULT. LatencyProbe=0 (or absent) in mgpu.ini: on_present and
// on_chain_destroyed return after one relaxed load; note_game_chain reads the
// key once (a file read at the game chain's first init) and returns.
//
// Ledger: Claude outputs\0.3.0-Discovery\DISCOVERY_LEDGER.md, entry D1.5a.
// Log tag [MGPU][LAT]. Scouting: never ships.
#pragma once

namespace mgpu::latprobe
{
    // init_swapchain, ONLY for the game's chain (dllmain's sc_is_game). The
    // first call reads LatencyProbe from mgpu.ini. `device` is the game's
    // ID3D12Device, `chain` its native swap chain. No reference is taken.
    void note_game_chain(void *device, void *chain);

    // destroy_swapchain. If it is the game's chain, sampling stops until the
    // next note_game_chain, so the device is never used past its chain.
    void on_chain_destroyed(void *chain);

    // Every ReShade present. Only the game's chain is counted; on it, our
    // QPC timestamp goes into a 16-entry ring, and every 2 s (after 300 game
    // presents) one sample is taken and logged.
    void on_present(void *chain);
}
