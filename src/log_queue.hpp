// MGPU Bridge - D2.0: the log queue (0.3.0 R&D branch).
//
// WHY. R1 (Requiem, 2026-10-05): the game's present thread stood still for
// 49 s INSIDE our present callback, between two of the latency probe's log
// calls. Whatever the cause, a log write on the game's present thread is a
// wait we add inside the game's Present. D2.0 never writes a log line from
// the present thread: it posts the line here, and a worker thread of ours
// writes it.
//
// HOW. A fixed ring of preformatted lines (no allocation on post). Posting is
// lock free: claim a position with one compare-exchange, copy, publish. The worker
// drains in order every 20 ms and hands each line to its destination: the
// ReShade log (info / warning) or the journal file. A post that finds the ring
// full is dropped and counted; the count is logged by the worker.
//
// THREADS. The worker starts with the first posted line (so it exists only
// when a D2.0 feature is on). Every poster is a D2.0 module initialised from
// on_init_swapchain, never from DllMain (the project's rule: no thread of ours
// alive in an add-on unload window).
//
// D2.0-5: mgpu::diag::info / warn / error post here whenever the worker
// runs, so NO line of this add-on is written on a game or bridge thread while
// it runs. dllmain starts the worker at every on_init_swapchain (after
// ReShade's add-on load / unload cycles) so the stream's lines are covered
// from the first game swap chain on.
//
// Ledger: Claude outputs\0.3.0-Discovery\D2_0_LEDGER.md.
#pragma once

namespace mgpu::lq
{
    enum dest : unsigned { TO_INFO = 0, TO_WARN = 1, TO_JOURNAL = 2, TO_ERROR = 3 };

    // Copies the line (truncated to the slot size) and returns at once.
    // Safe from any thread, including the game's present thread.
    void post(dest d, const char *line);

    // Starts the worker once. Idempotent. post() calls it. Refused after
    // signal_stop() / stop() until rearm().
    void start();

    // True while a worker is running and not told to stop.
    bool running();

    // The game's device is going (destroy_device): tell the worker to leave
    // its loop now (it drains first), so no thread of ours is inside this
    // image if ReShade unloads the add-on next. Never waits.
    void signal_stop();

    // on_init_swapchain: after a signal_stop whose worker has left its loop,
    // allow a new worker (a new game device in the same load).
    void rearm();

    // DLL_PROCESS_DETACH. Signals the worker. On the FreeLibrary path
    // (process alive) waits up to 300 ms for it to leave its loop, so no
    // thread of ours is executing this image when it unmaps; at process exit
    // the worker is already gone and nothing waits.
    void stop(bool process_exit);
}
