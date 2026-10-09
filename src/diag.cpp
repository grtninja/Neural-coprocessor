// MGPU Bridge - structured diagnostic logging (brief, section 08)
//
// D2.0-5 (0.3.0 R&D): NO LOG WRITE ON A HOT THREAD.
//
// Requiem, 2026-10-05: at one spot of the game (loading into gameplay) a
// ReShade log line took ~1 s to write instead of ~1 ms (C6: 14:42:21-25). Our
// code wrote log lines from the game's render thread and, on the bridge
// thread, while holding s.cs - the mutex the game's render thread takes every
// frame. A slow line therefore held the GAME: 49 s freezes (R1, C2, C4, C5), a
// 7 s slowdown (C6), and the ~1 s stall at every stream arm on three titles.
// This is DEFECT E (Starfield, R166): a log write under the mutex the render
// thread takes. R166 fixed it in the present gate only; this fixes it for
// every line, at the one place every line passes through.
//
// info / warn / error now POST the line to the log queue (log_queue.hpp): a
// lock-free copy into a ring, no wait, no I/O. The queue's worker thread writes
// it to ReShade's log. A slow disk now delays our log, never a frame; a full
// ring drops the line and counts it.
//
// When the worker is not running - DllMain, ReShade's add-on load / unload
// cycles before the first swap chain, after the game device is destroyed -
// the line is written directly, as before. Those windows are not on a game
// frame, and no thread of ours may be started in them.
//
// *_direct always writes now; the queue's worker uses them.
#include <reshade.hpp>

#include "diag.hpp"
#include "log_queue.hpp"

namespace mgpu::diag
{
void info_direct (const char *line) { reshade::log::message(reshade::log::level::info, line); }
void warn_direct (const char *line) { reshade::log::message(reshade::log::level::warning, line); }
void error_direct(const char *line) { reshade::log::message(reshade::log::level::error, line); }

void info(const char *line)
{
    if (mgpu::lq::running()) mgpu::lq::post(mgpu::lq::TO_INFO, line);
    else info_direct(line);
}

void warn(const char *line)
{
    if (mgpu::lq::running()) mgpu::lq::post(mgpu::lq::TO_WARN, line);
    else warn_direct(line);
}

void error(const char *line)
{
    if (mgpu::lq::running()) mgpu::lq::post(mgpu::lq::TO_ERROR, line);
    else error_direct(line);
}
}
