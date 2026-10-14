// MGPU Bridge - T1.19 contributed paths (Dom / LordZaxxon), revision R300. See docs/R300_T119.md.
//
// THE ONE PLACE THESE mgpu.ini KEYS ARE READ. Every key is ABSENT = OFF, and with
// every key absent the host code takes exactly the live 0.3.1 path: each call
// site tests one of these flags before it does anything at all - no extra
// command, no extra log line, no extra lock.
//
// Parsed from probe::mode_from_ini, the ini read that already runs at swapchain
// init, so the flags exist before the first NGX CreateFeature the calibrator
// hooks. Read again when the swapchain is re-created, like every probe key.
//
// The instruments themselves (temporal_diag, perf_diag, feature_diag,
// producer_diag, producer_gpu_timers) are Dom's files, unchanged. Their own
// defaults describe the instrument when it is ON; whether it is on is decided
// here.
//
// Log id [R300]: one line, only when at least one of these keys is set.
#pragma once

#include "temporal_diag.hpp"
#include "perf_diag.hpp"
#include "producer_diag.hpp"

#include <cstddef>

namespace mgpu::t119
{
    // probe::mode_from_ini hands over the buffer it already read.
    void load(const char *buf, std::size_t n);

    bool scene_handle_set();     // SceneHandleSet=1          R300b  calibrator takes any latched scene handle
    bool feature_diag();         // FeatureDiag=1             R300f1 + R300f2
    bool temporal_active();      // any temporal / jitter key on (see t119_keys.cpp)
    mgpu::tdiag::config temporal_config();
    bool perf_active();          // Perf11=1                  R300e1-R300e6 (PerfPrefetch=1 needs it)
    bool prefetch_active();      // Perf11=1 and PerfPrefetch=1
    mgpu::perf::config perf_config();
    bool producer_active();      // ProducerDiag=1 or Gpu0Timers=1   R300g1-R300g6
    bool gpu0_timers();          // Gpu0Timers=1              R300h
    mgpu::pdiag::config producer_config();
    bool sltags_mvec();          // SLTagsMvec=1              R300c (his R220) - needs SLTags
    int  sltags_capture();       // SLTagsCapture=1 or 2      R300d (his R221) - needs SLTags
    bool better_console_guard(); // BetterConsoleGuard=1      R300j
    bool dcomp_no_fallback();    // DcompNoFallback=1         R300k
    bool panel_auto_height();    // PanelAutoHeight=1         UI4K1
    bool panel_stats();          // PanelStats=1              R300l + derived readouts
    bool panel_persist();        // PanelPersist=1            T1.17-PERSIST
    bool frame_cap_present();    // a FrameCap line exists    T1.1
    int  frame_cap();            // its value (0 = off)
    void set_frame_cap(int v);   // the panel's live value
}
