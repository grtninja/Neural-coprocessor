// MGPU Bridge - D1: the discovery calibrator (0.3.0 R&D branch only).
//
// WHAT IT IS FOR. A scouting path, destructive on purpose (operator,
// 2026-10-04). It maps what the game itself has switched on - who sets the
// Reflex sleep mode, who calls sleep every frame, which NvAPI functions the
// game asks for, when the swap chain is created or destroyed - so the real
// 0.3.0 can be shaped from evidence and then rebuilt under the regular
// calibrator's rules. The old path (calibrator, stream, our own Reflex block)
// is not touched by this file.
//
// HOW. R102's technique: learn the real address of a function, scan every
// module's writable data for 8-byte words equal to it, swap them for a thunk
// that records and calls through with the arguments untouched. Late install,
// rescans to catch pointers cached later. nvapi_QueryInterface itself is
// wrapped the same way, so a function resolved after the install is wrapped
// when it is handed out.
//
// OFF BY DEFAULT. Discovery=0 (or absent) in mgpu.ini: the two entry points
// below return after one relaxed load. Nothing is scanned or swapped.
//
// Ledger: Claude outputs\0.3.0-Discovery\DISCOVERY_LEDGER.md. Log tag
// [MGPU][DSC].
#pragma once

namespace mgpu::discovery
{
    // Every ReShade present, on the thread that presents it (the game's render
    // thread for the game's chain, the bridge thread for ours). `chain` is the
    // native swap chain. Counts presents per chain, installs late, schedules
    // rescans and writes the periodic window line.
    void on_present(void *chain);

    // init_swapchain (created = true) and destroy_swapchain (created = false).
    // The first call reads Discovery from mgpu.ini. A chain created after the
    // first one is the menu-toggle signature this map exists to catch, so it
    // is logged and schedules a rescan.
    void on_swapchain(void *chain, bool created, bool resize);

    // DLL_PROCESS_DETACH, before this image goes away: every word holding one
    // of our thunks, in every module of the last scan's list, is put back to
    // the real NvAPI address - the swapped slots and the pointers handed out
    // by the QueryInterface thunk alike. Same reason as calibrator::uninstall
    // (R101c): slots aimed at unmapped memory crash the next caller. A thunk
    // pointer held only in a register or on a stack cannot be reached.
    void uninstall();
}
