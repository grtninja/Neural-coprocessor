// MGPU Bridge - T1.19 contributed keys. See t119_keys.hpp.
#include "t119_keys.hpp"
#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace mgpu::t119
{
namespace
{
    std::atomic<bool> g_shs{false}, g_fdiag{false}, g_tact{false}, g_perf{false}, g_pf{false},
                      g_prod{false}, g_t0{false}, g_slm{false}, g_bcg{false}, g_nofb{false},
                      g_pah{false}, g_pst{false}, g_pp{false}, g_fcp{false};
    std::atomic<int>  g_slc{0}, g_fc{0};
    std::mutex g_mx;
    mgpu::tdiag::config g_tc;
    mgpu::perf::config  g_pc;
    mgpu::pdiag::config g_dc;
    bool g_said = false;

    const char *val(const char *b, std::size_t n, const char *k) { return mgpu::config::find(b, n, k); }
    bool is1(const char *b, std::size_t n, const char *k) { const char *v = val(b, n, k); return v != nullptr && *v == '1'; }
    int  num(const char *b, std::size_t n, const char *k, int d) { const char *v = val(b, n, k); return v != nullptr ? atoi(v) : d; }
}

void load(const char *b, std::size_t n)
{
    if (b == nullptr) return;
    g_shs.store(is1(b, n, "SceneHandleSet"), std::memory_order_relaxed);
    g_fdiag.store(is1(b, n, "FeatureDiag"), std::memory_order_relaxed);

    // ---- temporal / jitter (Dom's T1.9-T1.11). Same meanings as his keys. ----
    mgpu::tdiag::config tc;
    const char *mv = val(b, n, "TemporalMV");
    const bool mv_single = (mv != nullptr && strncmp(mv, "single", 6) == 0);
    const int  rk = num(b, n, "TemporalResetK", 0);
    const bool tup = is1(b, n, "TemporalRequireTuple");
    const bool diag = is1(b, n, "TemporalDiag");
    const char *js = val(b, n, "JitterSlotStamp");
    const char *jd = val(b, n, "JitterDiag");
    const bool jslot = is1(b, n, "JitterSlot");
    tc.diag = diag;
    tc.mv_policy = mv_single ? mgpu::tdiag::MV_SINGLE : mgpu::tdiag::MV_MULTIPLY;
    tc.reset_k = (rk > 0 && rk < 64) ? (unsigned)rk : 0u;
    tc.require_tuple = tup;
    tc.jitter_stamp = (js != nullptr) ? (*js != '0') : diag;   // his default (on) follows TemporalDiag here
    tc.jitter_diag  = (jd != nullptr) ? (*jd != '0') : diag;
    tc.jitter_slot  = jslot;
    {   // the same reading as probe's R104 keys, so the slot path obeys them
        const char *jk = val(b, n, "JitterComp");
        const char *ik = val(b, n, "InvertJitter");
        int jv = (jk != nullptr && atoi(jk) != 0) ? 1 : 0;
        if (jv != 0 && ik != nullptr && atoi(ik) != 0) jv = -1;
        tc.jitter_mode = jv;
    }
    {
        const char *fk = val(b, n, "JitterFallback");
        if (fk != nullptr && strncmp(fk, "sync", 4) == 0) tc.jitter_fallback = 1;
        else if (fk != nullptr && strncmp(fk, "zero", 4) == 0) tc.jitter_fallback = 2;
    }
    { const int v = num(b, n, "TemporalDiagLines", -1); if (v >= 0 && v <= 100000) tc.line_budget = (unsigned)v; }
    const bool tact = diag || mv_single || tc.reset_k != 0u || tup || jslot || tc.jitter_stamp || tc.jitter_diag;

    // ---- performance (T1.11-T1.13) ----
    mgpu::perf::config pc;
    const bool perf = is1(b, n, "Perf11");
    pc.enabled = perf;
    { const int v = num(b, n, "PerfLines", -1); if (v >= 0 && v <= 100000) pc.line_budget = (unsigned)v; }
    pc.prefetch = is1(b, n, "PerfPrefetch");
    { const int v = num(b, n, "PerfPrefetchMax", -1); if (v >= 0 && v <= 4) pc.prefetch_max = (unsigned)v; }

    // ---- producer cost (T1.16) ----
    mgpu::pdiag::config dc;
    const bool t0 = is1(b, n, "Gpu0Timers");
    const bool prod = is1(b, n, "ProducerDiag") || t0;
    dc.gpu0_timers = t0;
    { const int v = num(b, n, "ProducerReport", -1); if (v >= 60 && v <= 100000) dc.report_every = (unsigned)v; }

    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_tc = tc; g_pc = pc; g_dc = dc;
    }
    g_tact.store(tact, std::memory_order_relaxed);
    g_perf.store(perf, std::memory_order_relaxed);
    g_pf.store(perf && pc.prefetch, std::memory_order_relaxed);
    g_prod.store(prod, std::memory_order_relaxed);
    g_t0.store(t0, std::memory_order_relaxed);

    // ---- Streamline tags, display, panel ----
    g_slm.store(is1(b, n, "SLTagsMvec"), std::memory_order_relaxed);
    { const int v = num(b, n, "SLTagsCapture", 0); g_slc.store((v == 1 || v == 2) ? v : 0, std::memory_order_relaxed); }
    g_bcg.store(is1(b, n, "BetterConsoleGuard"), std::memory_order_relaxed);
    g_nofb.store(is1(b, n, "DcompNoFallback"), std::memory_order_relaxed);
    g_pah.store(is1(b, n, "PanelAutoHeight"), std::memory_order_relaxed);
    g_pst.store(is1(b, n, "PanelStats"), std::memory_order_relaxed);
    g_pp.store(is1(b, n, "PanelPersist"), std::memory_order_relaxed);
    {
        const char *fc = val(b, n, "FrameCap");
        g_fcp.store(fc != nullptr, std::memory_order_relaxed);
        const int v = fc != nullptr ? atoi(fc) : 0;
        g_fc.store((v >= 20 && v <= 500) ? v : 0, std::memory_order_relaxed);
    }

    const bool any = g_shs || g_fdiag || tact || perf || prod || g_slm || g_slc.load() != 0 || g_bcg || g_nofb ||
                     g_pah || g_pst || g_pp || g_fcp;
    if (any && !g_said)
    {
        g_said = true;
        char l[900];
        snprintf(l, sizeof l,
            "[MGPU][R300] T1.19 CONTRIBUTED KEYS ON: SceneHandleSet=%d FeatureDiag=%d temporal=%d "
            "(TemporalDiag=%d MV=%s ResetK=%u Tuple=%d JitterSlot=%d) Perf11=%d PerfPrefetch=%d ProducerDiag=%d Gpu0Timers=%d "
            "SLTagsMvec=%d SLTagsCapture=%d BetterConsoleGuard=%d DcompNoFallback=%d PanelAutoHeight=%d PanelStats=%d "
            "PanelPersist=%d FrameCap=%s%d. Every key not listed as 1 here runs the live path.",
            g_shs ? 1 : 0, g_fdiag ? 1 : 0, tact ? 1 : 0, diag ? 1 : 0, mv_single ? "single" : "multiply", tc.reset_k,
            tup ? 1 : 0, jslot ? 1 : 0, perf ? 1 : 0, (perf && pc.prefetch) ? 1 : 0, prod ? 1 : 0, t0 ? 1 : 0,
            g_slm ? 1 : 0, g_slc.load(), g_bcg ? 1 : 0, g_nofb ? 1 : 0, g_pah ? 1 : 0, g_pst ? 1 : 0, g_pp ? 1 : 0,
            g_fcp ? "" : "absent/", g_fc.load());
        mgpu::diag::info(l);
    }
}

bool scene_handle_set()     { return g_shs.load(std::memory_order_relaxed); }
bool feature_diag()         { return g_fdiag.load(std::memory_order_relaxed); }
bool temporal_active()      { return g_tact.load(std::memory_order_relaxed); }
mgpu::tdiag::config temporal_config() { std::lock_guard<std::mutex> lk(g_mx); return g_tc; }
bool perf_active()          { return g_perf.load(std::memory_order_relaxed); }
bool prefetch_active()      { return g_pf.load(std::memory_order_relaxed); }
mgpu::perf::config perf_config() { std::lock_guard<std::mutex> lk(g_mx); return g_pc; }
bool producer_active()      { return g_prod.load(std::memory_order_relaxed); }
bool gpu0_timers()          { return g_t0.load(std::memory_order_relaxed); }
mgpu::pdiag::config producer_config() { std::lock_guard<std::mutex> lk(g_mx); return g_dc; }
bool sltags_mvec()          { return g_slm.load(std::memory_order_relaxed); }
int  sltags_capture()       { return g_slc.load(std::memory_order_relaxed); }
bool better_console_guard() { return g_bcg.load(std::memory_order_relaxed); }
bool dcomp_no_fallback()    { return g_nofb.load(std::memory_order_relaxed); }
bool panel_auto_height()    { return g_pah.load(std::memory_order_relaxed); }
bool panel_stats()          { return g_pst.load(std::memory_order_relaxed); }
bool panel_persist()        { return g_pp.load(std::memory_order_relaxed); }
bool frame_cap_present()    { return g_fcp.load(std::memory_order_relaxed); }
int  frame_cap()            { return g_fc.load(std::memory_order_relaxed); }
void set_frame_cap(int v)   { g_fc.store((v >= 20 && v <= 500) ? v : 0, std::memory_order_relaxed); }
}
