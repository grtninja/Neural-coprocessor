// MGPU Bridge - D2.0: our own Reflex for case-B titles (0.3.0 R&D branch).
//
// WHAT. Low latency for titles where nobody sleeps while frame gen is off
// (case B, e.g. Dawnwalker): we set the driver's low-latency mode and call
// NvAPI_D3D_Sleep once per frame ourselves. It never acts where the game's
// own Reflex loop runs (case A, e.g. Cyberpunk, Requiem) and steps aside
// when frame gen comes on. This is NEW work; the old Reflex / ReflexSleep code
// (V21-V43) is not used and not changed.
//
// THE DECISION (read only, from the scouting runs, LATENCY_PROBE_LEDGER
// sections 9-22): NV_GET_SLEEP_STATUS.bUseGameSleep is 1 whenever the game's
// own loop sleeps (Reflex on or off on Cyberpunk and Requiem) and 0 when
// nobody does (Dawnwalker, frame gen off). After a settle time at launch
// (Cyberpunk's first read was 0 before its loop began), we engage only if
// bUseGameSleep=0 AND fgMultiplier<=1; otherwise we stay aside for the
// session and touch nothing.
//
// WHILE ENGAGED our own Sleep makes bUseGameSleep read 1, so it cannot tell us
// whether someone else started sleeping. So every OwnReflexRecheckSec seconds
// we skip our Sleep for OwnReflexGapFrames frames ("the gap") and read the
// status at its end: 1 -> someone else sleeps now -> step aside; 0 -> resume.
// fgMultiplier>1 at any read -> step aside at once. NVIDIA: "It is OK to start
// (or stop) using this function at any time."
//
// KEYS (all off by default; off = the old path exactly):
//   OwnReflex=0/1
//   OwnReflexAnchor=finish | present   where our Sleep runs: in finish_present
//       (after the game's Present returns; the DEFAULT since D2.0-4, T2 beat T1
//       on Dawnwalker) or in the present event (before the game's Present).
//       Automatic fallback to present, once and journaled, if finish_present
//       does not fire on the game chain (none in the settle time, or none in
//       120 game presents while engaged).
//   OwnReflexMarkers=game | own        ride the game's frame markers, or send
//       our own (SIMULATION_START, PRESENT_START, PRESENT_END, frame ids ours).
//       R262: ABSENT = auto - at the engage, NvAPI_D3D_GetLatency's frame
//       reports say whether the game stamps markers; none -> own, any -> game.
//       R281: on a Streamline title whose driver holds NO frame reports at
//       the engage, auto waits for evidence before deciding: the game's scene
//       (the calibrator's count of its DLSS evaluates) then the settle again,
//       or - while no scene has been seen - a look at 2 min and then every
//       5 min that finds frame reports. While it waits our Sleep stays off
//       and no NvAPI call is made. A look that finds nothing stays undecided.
//       Titles without Streamline, an explicit OwnReflexMarkers, and reports
//       already there: decided at the settle as before.
//       R283: a title running its own NGX without Streamline (the calibrator
//       saw the game resolve NGX's evaluate - Tomb Raider DX12 observed; RDR2
//       expected, not yet observed) waits the same way. With DLSS off it
//       never sees a scene and stays undecided: no Reflex (with DLSS off
//       nothing shows the threads, and engaging blind halved Tomb Raider).
//   OwnReflexPipelineGuard (absent = 1). R276, where the game sends markers:
//       its stamps show whether the next frame starts on another thread while
//       it presents -> step aside. R283, where it sends none: the thread that
//       presents (our Sleep runs on it) against the thread of the game's
//       latest DLSS evaluate - different -> step aside;
//       the same -> engage, then watched every 30 frames (it leaves the
//       present thread or changes thread -> step aside, once); none seen ->
//       engage as before (RE4, Skyrim), and the first evaluate that shows up
//       later gets the same check. 0 = engage anyway (both).
//   OwnReflexSettleMs (5000), OwnReflexRecheckSec (10, 0 = never),
//   OwnReflexGapFrames (60)
// Refused (and said) if the old Reflex key is non-zero.
//
// TRACKED. Every NvAPI call it makes goes to the journal (what, when, why):
// the first of each kind in full, every change and every failure in full,
// and per-kind counts every 10 s. Every decision is journaled with its inputs.
// Stages are marked for the stall watch.
//
// TEARDOWN. When the game's swap chain is destroyed for good (resize=false),
// if we set the mode, we put lowLatency back to 0 (the mode is per device in
// the driver and outlives the process - residue). Not from DllMain: no vendor
// call under the loader lock. If we stepped aside because the game started
// sleeping, the mode is the game's and is left alone.
//
// Ledger: Claude outputs\0.3.0-Discovery\D2_0_LEDGER.md.
#pragma once

namespace mgpu::own_reflex
{
    // init_swapchain, ONLY for the chain dllmain proved to be the game's.
    // The first call reads the keys.
    void note_game_chain(void *device, void *chain);

    // R261 (DEBUG): OwnReflexDevice=game (default, as before) | gpu1 | both.
    //   game  every NvAPI_D3D_* call goes to the game's device (note_game_chain).
    //   gpu1  every call goes to OUR GPU 1 device instead; the game's device
    //         is only the chain identity.
    //   both  the engage decision reads the game's device; SetSleepMode,
    //         Sleep and the markers go to both devices, game first.
    // Called by dllmain right after note_game_chain with an AddRef'd
    // reference to our GPU 1 device (gpu1::device_ref_for_reflex); released
    // at on_chain_destroyed(resize=false). Ignored unless the key is set.
    // The journal and the log print, for every device in use, its API
    // (D3D11/D3D12) and adapter LUID: the proof of where Reflex runs.
    void note_gpu1_device(void *device_ref);
    int  device_mode();   // 0 game, 1 gpu1, 2 both (after the keys are read)

    // present event (before the game's Present) and finish_present (after it
    // returns), every chain; only the game's chain is acted on.
    void on_present(void *chain);
    void on_finish_present(void *chain);

    // destroy_swapchain. resize=false on the game chain = teardown restore.
    void on_chain_destroyed(void *chain, bool resize);
}
