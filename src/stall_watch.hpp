// MGPU Bridge - D2.0: the stall watch (0.3.0 R&D branch).
//
// WHY. Requiem, 2026-10-05 (R1): the game's present thread stood still for
// 49 s inside our present callback, and nothing could say where. The
// operator: "make sure D2.0 is able to debug this case".
//
// HOW. Every callback of ours that runs on a game thread opens a scope with a
// stage id; nested scopes name the step inside it (the probe's NvAPI calls,
// our Sleep, ...). A watchdog thread of ours checks every 100 ms. A scope open
// longer than StallWatch ms is reported to <add-on folder>\mgpu\stall.log -
// our own file, written with WriteFile and our own number formatting, so a
// stuck ReShade log or a stuck CRT lock cannot stop the report. The report
// names the stage, the time in it, the thread, and WHERE that thread is: it is
// suspended for a moment, its instruction pointer and the code addresses found
// on its stack are copied, it is resumed, and the addresses are named as
// module + offset from a module table cached while nothing was stalled. It
// repeats every 5 s while the stall lasts and says when it ends.
// No scope open but no game present for StallWatch ms either: "stall outside
// our callback" (game or driver), and when presents resume.
//
// OFF BY DEFAULT. StallWatch=0 (or absent): scope costs one relaxed load.
//
// Ledger: Claude outputs\0.3.0-Discovery\D2_0_LEDGER.md.
#pragma once

namespace mgpu::stallwatch
{
    enum stage : unsigned
    {
        S_NONE = 0,
        S_PRESENT_CB,       // dllmain on_present, whole callback
        S_FINISH_CB,        // dllmain on_finish_present, whole callback
        S_DISCOVERY,        // discovery::on_present
        S_PROBE,            // latprobe::on_present
        S_PROBE_NVAPI,      // the probe's GetSleepStatus + GetLatency
        S_PROBE_LOG,        // the probe's log lines
        S_STREAM,           // gpu1::stream_on_present
        S_RFX,              // own_reflex callback
        S_RFX_STATUS,       // own_reflex GetSleepStatus
        S_RFX_SLEEP,        // own_reflex NvAPI_D3D_Sleep
        S_RFX_SETMODE,      // own_reflex SetSleepMode
        S_RFX_MARKER,       // own_reflex SetLatencyMarker
        S_SWAPCHAIN,        // swap chain init / destroy callbacks
        S_COUNT
    };

    // Once, from on_init_swapchain: reads StallWatch, opens mgpu\stall.log,
    // starts the watchdog. Idempotent.
    void init();

    // The game's swap chain (from dllmain's LUID-proved site) and every
    // present on it, for the "outside our callback" check.
    void note_game_chain(void *chain);
    void on_present(void *chain);

    // RAII stage marker. The outermost scope on a thread claims a slot; inner
    // scopes change its stage and put it back on exit.
    class scope
    {
    public:
        explicit scope(stage s);
        ~scope();
        scope(const scope &) = delete;
        scope &operator=(const scope &) = delete;
    private:
        bool     active_ = false;
        unsigned prev_   = 0;
        long long prev_q_ = 0;
    };

    // The game's device is going (destroy_device): the watchdog leaves its
    // loop within 100 ms, so no thread of ours is inside this image if
    // ReShade unloads the add-on next. init() re-arms it for a new device.
    // Consequence: what happens after the game device is destroyed is not
    // watched (teardown after that point).
    void signal_stop();

    // DLL_PROCESS_DETACH. Signals the watchdog; on the FreeLibrary path
    // (process alive) waits up to 300 ms for it to leave its loop, so no
    // thread of ours is executing this image when it unmaps.
    void stop(bool process_exit);
}
