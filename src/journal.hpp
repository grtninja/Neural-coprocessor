// MGPU Bridge - D2.0: the journal - WHAT our code did to the game's
// subsystems, WHEN, and WHY (0.3.0 R&D branch).
//
// WHY. Requiem, 2026-10-05: with Reflex OFF in the game, something the
// scouting build did made the game freeze at the loading screen, and the
// session stayed poisoned (black screen on the next launch). Which of our
// interactions the game did not like was not knowable, because nothing
// recorded them as one list. The journal is that list.
//
// WHAT IT RECORDS. One line per interaction with the game's subsystems:
// NvAPI calls (function, device, arguments, result), writes into another
// module's memory, step-aside / step-in decisions with their inputs. Each
// line: local wall time, QPC, thread id, WHAT, and WHY (the key and the
// rule that made it happen). Per-frame calls are summarised by their caller
// (first call, every change, a count per window); the journal itself does
// no rate limiting.
//
// SESSION MARKERS. init() writes "SESSION OPEN" with the build and the keys;
// session_close_direct() writes "SESSION CLOSE" at DLL_PROCESS_DETACH. At the
// next init(), a previous SESSION OPEN with no SESSION CLOSE after it is
// reported, with that session's last 20 lines, in the journal and in
// ReShade's log: a poisoned launch carries its own cause.
//
// FILE. <add-on folder>\mgpu\journal.log, appended (FILE_APPEND_DATA, so
// every line is one atomic append), our own handle: no ReShade log lock.
// Rotated to journal.prev.log above 8 MB, after the previous-session check.
//
// OFF BY DEFAULT. Journal=0 (or absent): init() reads the key once and
// returns; event() is one relaxed load.
//
// Ledger: Claude outputs\0.3.0-Discovery\D2_0_LEDGER.md.
#pragma once

namespace mgpu::journal
{
    // Once, from on_init_swapchain (never from DllMain): reads Journal,
    // opens the file, checks the previous session, writes SESSION OPEN.
    void init(const char *build);

    bool on();

    // Formats one line and posts it through the log queue (never writes
    // from the calling thread). what / why are plain text, no newline.
    void event(const char *what, const char *why);

    // Teardown markers, so the next launch can tell "the session ended after
    // its teardown began" (exit without DLL detach, e.g. TerminateProcess -
    // not a poison signal) from "it ended in the middle" (poison suspect).
    void note_game_chain(void *chain);
    void on_chain_destroyed(void *chain, bool resize);
    void on_game_device_destroyed();

    // DLL_PROCESS_DETACH only: formats and writes now, on the calling thread
    // (kernel32 only), for events that happen after the queue has stopped.
    void event_direct(const char *what, const char *why);

    // The log queue's worker calls this: one line, written now.
    void write_raw(const char *line);

    // DLL_PROCESS_DETACH: writes SESSION CLOSE directly (kernel32 only, no
    // CRT formatting - safe under the loader lock). Lines still in the queue may be lost; the
    // close line says how many were posted and how many written.
    void session_close_direct(const char *how);
}
