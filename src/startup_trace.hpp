// MGPU Bridge - R313: StartupTrace + the stop/death record, and the DcompSafeMode keys.
//
// WHY. Two machines failed with the composition overlay (DcompOverlay=1) in ways
// not reproduced here: Dlagonborn #52 (Cyberpunk: the game's presents stop
// 1.5-3 s after the first neural frame is rooted, bridge thread alive) and
// N01950 #47 (KCD2: the bridge's device removed 0.7 s after its first present).
// Their logs are written late, by the log queue thread, with the queue's
// timestamp and thread id, so the ORDER of what the add-on did to the game's
// window across threads - the upstream half - is lost exactly where it
// matters. This module keeps that order: every trace event carries its own
// QPC time and thread id, in a ring that is dumped when the game stops.
//
// WHAT, with StartupTrace=1:
//   - [MGPU][TRACE] lines at the startup steps (dllmain, gpu1_context, worker),
//     each with t=<ms since trace init> tid=<thread> role=<GT-swap|GT-render|
//     GT-present|GT-overlay|BT> step=<name>. Posted through the log queue like
//     every other line, AND kept in a ring of the last 128 events.
//   - the game's present cadence: one relaxed increment per game present
//     (dllmain on_present), sampled by the bridge thread every loop pass into
//     the same ring, with the bridge's own Present status and DWM's compose
//     counter; a cadence line every 2 s after the first neural frame.
//   - the record: written *_direct (survives a dead queue) and to
//     mgpu\startup_trace.last, at the existing failure sites (present failed,
//     device removed, chain creation failed) and at ONE NEW site: the game's
//     presents flat for StartupTraceStallMs (default 3000) after the first
//     neural frame while the bridge thread is alive and the game's window is
//     foreground, visible and not iconic - Dlagonborn's shape. The record
//     carries both devices' removed reasons and LUIDs, elapsed since the
//     first present / first neural frame / the composition decision, rooted
//     flag, mode, chain kind, DWM and window readings, the ring, and one
//     snapshot (rip + stack code addresses as module+offset) of the game's
//     swap-chain, render and present threads and the bridge thread.
//   - next launch: mgpu\startup_trace.last is read at init; a previous
//     session that did not mark itself clean is reported in the first line.
//   StartupTrace=2: everything above, plus the record fires once by itself
//   5 s after the first neural frame (a self-test of the record path).
//
// WHAT TOUCHES A GAME THREAD WITH THE KEY ON (the honest list):
//   1. one mgpu.ini read at on_init_swapchain entry (GT-swap), beside the
//      stall watch's and the journal's - this read happens with the key
//      absent too, as theirs do;
//   2. a QPC read + relaxed stores at each game-thread trace point (inside
//      handlers already running there), and one posted log line per event;
//   3. one relaxed increment per game present;
//   4. SuspendThread / ResumeThread of the three game threads, once, only
//      when the record fires (the game has already stopped presenting for
//      StartupTraceStallMs, or the self-test).
//   No file write, no lock, no message to a game window from a game thread.
//
// KEY ABSENT: every entry point is one relaxed load and a return. No line,
// no counter, no ring, no file. The shipped path is byte for byte the same.
//
// DcompSafeMode (R313, same key family; read here, applied in gpu1_context /
// worker / dllmain):
//   absent/0 = mode 1 exactly as shipped.
//   1 = the composition decision and the overlay root toggle run on the
//       bridge thread, never on a game thread (the two mode-1 things that
//       did run there).
//   (2 was "composition on the game's adapter"; removed in R313b - it made the
//    add-on take its own chain for the game's. Any value other than 1 or 3 is off.)
//   3 = 1 + the same GPU 1 target, serialized and quiet-gated: every Commit
//       followed by WaitForCommitCompletion; the arm's resize done unrooted;
//       the root waits until the game's chain has had no swap-chain event for
//       DcompSafeQuietMs (default 1000). If the stop signature fires after
//       the first neural frame for DcompSafeFallbackMs (default 10000) the
//       add-on writes DcompOverlay=0 into mgpu.ini (the bridge's own window)
//       for the NEXT launch and says so. Only value 3 writes; 1 never does.
//
// Ledger: Claude outputs\CODE_CHANGES_0.4.0_LEDGER.md, R313.
#pragma once

namespace mgpu::strace
{
    // Thread roles. The ReShade handler that is executing IS the role: no
    // new call into any game thread is needed to name it.
    enum role : int
    {
        R_UNKNOWN = 0,
        R_GT_SWAP,      // init_swapchain / create_swapchain / set_fullscreen_state / destroy_device
        R_GT_RENDER,    // reshade_finish_effects (the arm, the seals)
        R_GT_PRESENT,   // present / finish_present of the game's chain
        R_GT_OVERLAY,   // reshade_open_overlay
        R_BT,           // the bridge thread (worker.cpp)
        R_COUNT
    };

    // Once, from on_init_swapchain's entry (before the composition decision).
    // Reads StartupTrace, StartupTraceStallMs, DcompSafeMode, DcompSafeQuietMs,
    // DcompSafeFallbackMs. Idempotent. Key absent: one ini read, then nothing.
    void init();

    bool     on();             // StartupTrace >= 1
    int      level();          // 0, 1, 2
    unsigned stall_ms();       // StartupTraceStallMs (default 3000, 500..60000)

    int      safe_mode();      // DcompSafeMode 0, 1 or 3 (absent / anything else = 0)
    unsigned safe_quiet_ms();  // DcompSafeQuietMs (default 1000)
    unsigned safe_fallback_ms();   // DcompSafeFallbackMs (default 10000)

    // A trace event. One relaxed load when off. `detail` and `hr` are optional.
    // Posts one [MGPU][TRACE] line through the log queue and keeps the event in
    // the ring. Safe from any thread (QPC + relaxed stores + one queue post).
    void ev(role r, const char *step, const char *detail = nullptr, unsigned hr = 0u);

    // Elapsed milliseconds since init (0.0 when off). For "elapsed inside" readings.
    double now_ms();

    // The game's chain (native pointer, from dllmain's LUID-proved site): a store.
    void note_game_chain(void *chain_native);
    // The game's present cadence: called from dllmain's on_present for EVERY
    // chain; one relaxed load when off, one pointer compare + one relaxed
    // increment when the chain is the game's. Nothing else happens there.
    void note_game_present(void *chain_native);

    // The game's device (ReShade's native pointer from init_swapchain, LUID-proved).
    // A store, no reference taken; read only by the record, for its removed reason.
    void note_game_device(void *d3d12_device);

    // Bridge-thread milestones (worker.cpp / gpu1_context.cpp, BT only).
    void note_bridge_thread();                 // call once on BT, first thing: records its id (ALWAYS - DcompSafeMode needs it even with the trace off)
    bool is_bridge_thread();                   // the calling thread is the bridge thread (one relaxed load + a TEB read)
    void note_first_present();                 // first successful present of the bridge chain
    void note_bridge_present(unsigned hr_or_status, double ms_inside);   // every bridge present
    void note_first_neural_frame(bool rooted); // P5.0
    void note_chain_kind(bool composition);    // ForComposition vs ForHwnd
    void note_decision(int mode, double elapsed_ms, role decided_on);   // the composition decision latched

    // Game-thread milestones (relaxed stores only).
    void note_arm_enter();     // GT-render, under s.cs
    void note_arm_exit(unsigned hr);

    // Bridge-thread loop tick: samples the counters into the ring, writes the
    // cadence line every 2 s after P5.0, runs the stop detection and the
    // self-test. One relaxed load when off.
    void bt_tick();

    // The record. `why` names the trigger; gpu1_reason is GetDeviceRemovedReason
    // of the bridge device when the caller has it (0 = read it here). Bridge
    // thread only. Writes *_direct and mgpu\startup_trace.last. One-shot per
    // trigger name.
    void record(const char *why, unsigned gpu1_reason);

    // True once the stop signature has fired (the game's presents flat after
    // P5.0, window up). Read by the DcompSafeMode=3 fallback in worker.cpp.
    bool stop_fired();
    // Milliseconds the game's presents have been flat (0 when they are moving
    // or before P5.0). Read by the value-3 fallback.
    unsigned long long game_flat_ms();

    // on_destroy_device (GT-swap, teardown - the same point mvec_extract writes
    // its ini): marks startup_trace.last as a clean exit (R313a: appended, so a
    // record written earlier in the session is kept).
    void mark_clean_exit();
    // R313a: DLL_PROCESS_DETACH, after the journal's session close. Appends the clean
    // stage to the file and nothing else (loader lock held). One relaxed load when off.
    void mark_clean_detach(bool process_exit);
}
