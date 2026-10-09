// MGPU Bridge - structured diagnostic logging (brief, section 08)
//
// Every line this add-on writes goes through this interface, so the
// format is one place: a fixed "[MGPU][Tn]" prefix carrying the task id,
// and the inputs to each decision, not just the outcome. The only sink
// is ReShade's log - together with the debug window it is the complete
// instrument set for P0.
//
// Called from the game thread (ReShade callbacks) and the bridge thread;
// reshade::log::message is used concurrently by ReShade itself.
//
// D2.0-5: info / warn / error never write on the calling thread while the
// log queue's worker runs - they post the line and return (see diag.cpp). The
// *_direct forms write now; they are for the queue's worker and for windows
// where no worker may run.
#pragma once

namespace mgpu::diag
{
    void info (const char *line);   // reshade::log::level::info, via the queue when it runs
    void warn (const char *line);   // reshade::log::level::warning, via the queue when it runs
    void error(const char *line);   // reshade::log::level::error, via the queue when it runs

    void info_direct (const char *line);   // written now, on this thread
    void warn_direct (const char *line);
    void error_direct(const char *line);
}
