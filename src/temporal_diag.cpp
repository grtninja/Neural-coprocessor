// MGPU Bridge 0.2.6-dev - T1.9 temporal diagnostic. See temporal_diag.hpp.
#include "temporal_diag.hpp"
#include "diag.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace mgpu::tdiag
{
namespace
{
    // Producer-written stamps, one per ring slot, relaxed atomics: the producer
    // is the game thread, the reader is the GPU1 thread, and a torn read of a
    // 64-bit value is the only thing to avoid.
    std::atomic<unsigned long long> g_mvec_id[MAXSLOT];     // sealed id the MVec copy was recorded for
    std::atomic<unsigned long long> g_seal_id[MAXSLOT];     // sealed id the seal carried
    std::atomic<unsigned long long> g_seal_gf[MAXSLOT];     // game frame at seal
    std::atomic<unsigned char>      g_seal_mv[MAXSLOT];     // mvec_valid at seal
    std::atomic<unsigned char>      g_seal_dp[MAXSLOT];     // depth_valid at seal

    // Jitter stamps. Seqlock by sealed id: id is zeroed, fields written, id set last.
    struct jslot
    {
        std::atomic<unsigned long long> id{0};
        std::atomic<float> jx{0.f}, jy{0.f};
        std::atomic<unsigned> mvj{0}, mw{0}, mh{0};
    };
    jslot g_js[MAXSLOT];

    // R300a7/R300a8 consumer state (under g_mx).
    bool  g_pe_have = false;                 // previous EVALUATED frame's slot jitter known
    float g_pe_jx = 0.f, g_pe_jy = 0.f;
    unsigned long long g_pe_gf = 0;
    float g_r104_px = 0.f, g_r104_py = 0.f;  // reconstruction of the author's g_jpx/g_jpy (starts 0 like the original)
    struct lagstat { unsigned long long n = 0; double sum = 0.0, mx = 0.0; };
    lagstat g_lag[6];                        // queue lag 0,1,2,3,4,5+
    unsigned long long g_jit_lines = 0;

    std::mutex g_mx;
    config     g_cfg;
    unsigned long long g_eval_id = 0;
    unsigned long long g_next_report = 300;
    unsigned long long g_sum_elapsed = 0;
    unsigned long long g_hist_headroom_min = 99;
    unsigned long long g_wprev_evals = 0, g_wprev_present = 0, g_wprev_coh = 0;
    float g_last_sx = 0.f, g_last_sy = 0.f;
    unsigned g_last_rw = 0, g_last_rh = 0;
    counters g_c{};

    unsigned bucket8(unsigned long long v) { return (unsigned)(v >= 7ull ? 7ull : v); }
    unsigned long long sub0(unsigned long long a, unsigned long long b) { return a > b ? a - b : 0ull; }
    float absf(float v) { return v < 0.f ? -v : v; }

    const char *policy_name(int p) { return p == MV_SINGLE ? "single" : "multiply"; }

    void say(const char *s) { mgpu::diag::info(s); }
}

void configure(const config &c)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_cfg = c;
    if (c.jitter_slot && c.jitter_mode == 0)
        say("[MGPU][R300a4] JitterSlot=1 but JitterComp=0: nothing is applied (JitterComp gates both the author's R104 and the slot path).");
    char b[640];
    std::snprintf(b, sizeof b,
        "[MGPU][R300a4] TEMPORAL T1.9 CONFIG: TemporalDiag=%d TemporalMV=%s TemporalResetK=%u "
        "TemporalRequireTuple=%d lines=%u then 1/%u JitterSlotStamp=%d JitterDiag=%d JitterSlot=%d JitterMode=%d. Defaults (multiply, K=0, tuple off) are the T1.8 "
        "behaviour byte for byte; only TemporalDiag adds output. NGX does not know how many game "
        "frames separated two evaluates - the application either represents that displacement or "
        "resets. These keys let one run test each choice without a rebuild.",
        c.diag ? 1 : 0, policy_name(c.mv_policy), c.reset_k, c.require_tuple ? 1 : 0,
        c.line_budget, c.line_every ? c.line_every : 1u,
        c.jitter_stamp ? 1 : 0, c.jitter_diag ? 1 : 0, c.jitter_slot ? 1 : 0, c.jitter_mode);
    say(b);
}

config current() { std::lock_guard<std::mutex> lk(g_mx); return g_cfg; }

void note_mvec_copy(unsigned slot, unsigned long long sealed_id)
{
    g_mvec_id[slot % MAXSLOT].store(sealed_id, std::memory_order_release);
}

void note_seal(unsigned slot, unsigned long long sealed_id, unsigned long long game_frame,
               bool mvec_valid, bool depth_valid)
{
    const unsigned i = slot % MAXSLOT;
    g_seal_gf[i].store(game_frame, std::memory_order_relaxed);
    g_seal_mv[i].store(mvec_valid ? 1 : 0, std::memory_order_relaxed);
    g_seal_dp[i].store(depth_valid ? 1 : 0, std::memory_order_relaxed);
    g_seal_id[i].store(sealed_id, std::memory_order_release);
}

void note_jitter(unsigned slot, unsigned long long sealed_id, float jx, float jy,
                 unsigned mv_jittered, unsigned mvec_w, unsigned mvec_h)
{
    jslot &j = g_js[slot % MAXSLOT];
    j.id.store(0ull, std::memory_order_release);          // invalidate first
    j.jx.store(jx, std::memory_order_relaxed); j.jy.store(jy, std::memory_order_relaxed);
    j.mvj.store(mv_jittered, std::memory_order_relaxed);
    j.mw.store(mvec_w, std::memory_order_relaxed); j.mh.store(mvec_h, std::memory_order_relaxed);
    j.id.store(sealed_id, std::memory_order_release);     // valid last
}

jitter_out jitter_decide(const jitter_in &in)
{
    jitter_out o;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_cfg.jitter_stamp && !g_cfg.jitter_diag && !g_cfg.jitter_slot) return o;
    ++g_c.jit_evals;

    // ---- the slot's own jitter, read consistently ----
    jslot &j = g_js[in.slot % MAXSLOT];
    const unsigned long long id1 = j.id.load(std::memory_order_acquire);
    const float sjx = j.jx.load(std::memory_order_relaxed), sjy = j.jy.load(std::memory_order_relaxed);
    const unsigned smvj = j.mvj.load(std::memory_order_relaxed);
    const unsigned long long id2 = j.id.load(std::memory_order_acquire);
    const bool known = (id1 == in.sealed_id && id2 == in.sealed_id);
    if (!known) ++g_c.jit_slot_unknown;

    const float g  = (g_cfg.jitter_mode < 0) ? -1.0f : 1.0f;
    const float ax = (in.scale_x != 0.f) ? in.scale_x : 1.0f;
    const float ay = (in.scale_y != 0.f) ? in.scale_y : 1.0f;

    // ---- slot delta: this slot's jitter minus the previous EVALUATED slot's ----
    float dsx = 0.f, dsy = 0.f;                       // pixels
    if (known && g_pe_have) { dsx = sjx - g_pe_jx; dsy = sjy - g_pe_jy; }

    // ---- reconstruction of R104: newest-at-call minus newest-at-previous-call ----
    float drx = 0.f, dry = 0.f;                       // pixels
    const float r104_prev_x = g_r104_px, r104_prev_y = g_r104_py;
    if (in.newest_have && in.newest_mvj == 0u)
    {
        drx = in.newest_jx - g_r104_px; dry = in.newest_jy - g_r104_py;
        g_r104_px = in.newest_jx; g_r104_py = in.newest_jy;
    }
    const float ex = drx - dsx, ey = dry - dsy;
    const double err = std::sqrt((double)ex * ex + (double)ey * ey);
    const unsigned lb = in.queue_lag >= 5ull ? 5u : (unsigned)in.queue_lag;
    if (known && g_pe_have) { ++g_lag[lb].n; g_lag[lb].sum += err; if (err > g_lag[lb].mx) g_lag[lb].mx = err; }

    // ---- decision ----
    // JitterSlot takes over from R104 ONLY when there is a real delta to apply: the slot is known, its vectors
    // are un-jittered, a previous EVALUATED slot exists, and NR history is not being restarted. Every other case
    // leaves the author's path running, so run C differs from run B only on evaluates that have a true baseline.
    const bool baseline = known && g_pe_have && !in.nr_reset;
    if (g_cfg.jitter_slot && g_cfg.jitter_mode != 0)
    {
        if (known && smvj == 0u && baseline)
        {
            o.apply = true; o.ox = g * dsx / ax; o.oy = g * dsy / ay;
            ++g_c.jit_slot_applied;
        }
        else if (known && smvj == 0u) ++g_c.jit_no_baseline;   // first evaluate, or history reset
        else ++g_c.jit_fallback;                               // slot recycled, or MVJittered=1

        // AUDIT (T1.11): the author's apply_jitter_offset keeps g_jpx/g_jpy only when IT runs. While SLOT applies, it
        // is skipped, so its state goes stale; the next fallback to it then computes (newest - stale), a delta that
        // spans every SLOT evaluate since. Off by default (author = T1.10a). sync: keep its state current.
        // zero: no reliable baseline -> apply nothing instead of that delta.
        if (g_cfg.jitter_fallback == 1 && o.apply) o.sync = true;
        else if (g_cfg.jitter_fallback == 2)
        {
            if (o.apply) o.sync = true;
            else { o.apply = true; o.ox = 0.f; o.oy = 0.f; o.sync = true; ++g_c.jit_zero_fallback; }
        }
    }

    if (g_cfg.jitter_diag)
    {
        const bool show = (g_jit_lines < 200ull) || ((g_c.jit_evals % 60ull) == 0ull);
        if (show)
        {
            ++g_jit_lines;
            char b[800];
            std::snprintf(b, sizeof b,
                "[MGPU][R300a7] JITTER eval_n=%llu f=%llu slot=%u queue_lag=%llu known=%d | slot.gf=%llu "
                "slot.jitter=(%.4f,%.4f) mvj=%u | prev_eval.gf=%llu prev_eval.jitter=(%.4f,%.4f) have=%d | "
                "slot_delta_px=(%.4f,%.4f) | r104.newest=(%.4f,%.4f) r104.prev=(%.4f,%.4f) r104_delta_px=(%.4f,%.4f) | "
                "delta_error_px=(%.4f,%.4f) |err|=%.4f | mode=%d baseline=%d fallback_mode=%d path=%s offset_vec=(%.6f,%.6f)",
                g_c.jit_evals, in.sealed_id, in.slot, in.queue_lag, known ? 1 : 0, in.game_frame,
                (double)sjx, (double)sjy, smvj, g_pe_gf, (double)g_pe_jx, (double)g_pe_jy, g_pe_have ? 1 : 0,
                (double)dsx, (double)dsy, (double)in.newest_jx, (double)in.newest_jy,
                (double)r104_prev_x, (double)r104_prev_y,
                (double)drx, (double)dry, (double)ex, (double)ey, err, g_cfg.jitter_mode, baseline ? 1 : 0, g_cfg.jitter_fallback,
                o.apply ? "SLOT" : "author-R104", (double)o.ox, (double)o.oy);
            say(b);
        }
    }

    // The baseline for the NEXT delta is the frame NR just evaluated. If its jitter is unknown (slot recycled),
    // the next delta would span an unknown number of evaluates: drop the baseline instead of reusing a stale one.
    if (known) { g_pe_have = true; g_pe_jx = sjx; g_pe_jy = sjy; g_pe_gf = in.game_frame; }
    else       { g_pe_have = false; }
    return o;
}

bool seal_mvec_valid(unsigned slot, unsigned long long f, bool &known)
{
    const unsigned i = slot % MAXSLOT;
    known = (g_seal_id[i].load(std::memory_order_acquire) == f);
    return known && g_seal_mv[i].load(std::memory_order_relaxed) != 0;
}

decision decide(const eval_in &in)
{
    decision d;
    std::lock_guard<std::mutex> lk(g_mx);
    ++g_eval_id;
    ++g_c.evals;

    const unsigned i = in.slot % MAXSLOT;
    const unsigned long long mvec_stamp = g_mvec_id[i].load(std::memory_order_acquire);

    // ---- tuple coherence: does the vector in this slot belong to the colour frame we evaluate? ----
    if (!in.seal_known) ++g_c.seal_lost;
    const bool have_mv   = in.mvec_valid_seal;
    const bool stamp_ok  = have_mv && in.mvec_valid_live && (mvec_stamp == in.sealed_id);
    if (!have_mv)                       ++g_c.mvec_missing;
    else if (mvec_stamp != in.sealed_id) ++g_c.mvec_overwritten;    // slot recycled under us
    if (in.mvec_valid_seal != in.mvec_valid_live) ++g_c.flag_disagree;
    if (stamp_ok) ++g_c.coherent; else ++g_c.incoherent;
    if (have_mv && in.mvec_valid_live) { ++g_c.present; if (stamp_ok) ++g_c.present_coherent; }
    d.coherent = stamp_ok ? 1u : 0u;

    // ---- ages, in game frames ----
    const unsigned long long cage = sub0(in.cur_game_frame, in.game_frame);
    ++g_c.age_color[bucket8(cage)];
    ++g_c.age_mvec[stamp_ok ? bucket8(cage) : 7u];   // an incoherent vector has no honest age: bucket 7
    const unsigned el = in.elapsed ? in.elapsed : 1u;
    ++g_c.delta_hist[el >= 8u ? 7u : el - 1u];
    g_sum_elapsed += el;

    const unsigned long long lag = sub0(in.produced, in.sealed_id);
    const unsigned long long headroom = sub0((unsigned long long)in.ring, lag);
    if (headroom < g_hist_headroom_min) g_hist_headroom_min = headroom;

    // ---- scale / extent sanity ----
    if (in.scale_x == 0.f || in.scale_y == 0.f) ++g_c.scale_zero;
    if (g_last_sx != 0.f && (absf(in.scale_x - g_last_sx) > 0.5f || absf(in.scale_y - g_last_sy) > 0.5f))
        ++g_c.scale_changes;
    if (g_last_rw != 0 && (in.render_w != g_last_rw || in.render_h != g_last_rh)) ++g_c.extent_changes;
    g_last_sx = in.scale_x; g_last_sy = in.scale_y; g_last_rw = in.render_w; g_last_rh = in.render_h;

    // ---- policy (T1.10 experiment, off by default) ----
    d.mv_k = (g_cfg.mv_policy == MV_SINGLE) ? 1.0f : (float)el;
    d.reset = (g_cfg.reset_k != 0u && el >= g_cfg.reset_k);
    if (d.reset) ++g_c.resets_forced;
    d.bind_mvec = true;
    if (g_cfg.require_tuple && in.mvec_valid_live && !stamp_ok)
    {
        d.bind_mvec = false;
        ++g_c.unbound_by_policy;
        if (g_c.rejects_logged < 20ull)   // R300a6: proof the policy acted, rate-limited
        {
            ++g_c.rejects_logged;
            char r[300];
            std::snprintf(r, sizeof r, "[MGPU][R300a6] REQUIRE-TUPLE REJECT eval=%llu color_gf=%llu sealed_id=%llu mvec_id=%llu bind_mvec=0",
                          g_eval_id, in.game_frame, in.sealed_id, mvec_stamp);
            say(r);
        }
    }

    // ---- [R300a1] ----
    if (g_cfg.diag)
    {
        const bool show = (g_eval_id <= g_cfg.line_budget) ||
                          (g_cfg.line_every != 0u && (g_eval_id % g_cfg.line_every) == 0ull);
        if (show)
        {
            char b[900];
            std::snprintf(b, sizeof b,
                "[MGPU][R300a1] eval=%llu f=%llu slot=%u color_gf=%llu mvec_id=%llu same_tuple=%u "
                "color_age=%llu elapsed=%u prev_eval_gf=%llu delta_hist=%u | mv_valid seal=%d live=%d "
                "depth=%d | scale_ngx=%.3f,%.3f k=%.2f scale_passed=%.3f,%.3f mv_jittered=%u "
                "jitter_newest=%.4f,%.4f | render=%ux%u mvec=%ux%u | queue=%llu headroom=%llu ring=%u "
                "| reset=%d bind_mvec=%d",
                g_eval_id, in.sealed_id, in.slot, in.game_frame, mvec_stamp, d.coherent,
                cage, el, in.prev_eval_gf, el,
                in.mvec_valid_seal ? 1 : 0, in.mvec_valid_live ? 1 : 0, in.depth_valid ? 1 : 0,
                (double)in.scale_x, (double)in.scale_y, (double)d.mv_k,
                (double)(in.scale_x * d.mv_k), (double)(in.scale_y * d.mv_k), in.mv_jittered,
                (double)in.jitter_x, (double)in.jitter_y, in.render_w, in.render_h, in.mvec_w, in.mvec_h,
                lag, headroom, in.ring, d.reset ? 1 : 0, d.bind_mvec ? 1 : 0);
            say(b);
        }
    }
    return d;
}

static void emit_summary_locked(unsigned long long gf, bool final_)
{
    if (!g_cfg.diag || g_c.evals == 0) return;
    const double n = (double)g_c.evals;
    const unsigned long long wn = g_c.evals - g_wprev_evals;
    const double win_present = wn ? 100.0 * (double)(g_c.present - g_wprev_present) / (double)wn : 0.0;
    const double win_ok = (g_c.present - g_wprev_present) ? 100.0 * (double)(g_c.present_coherent - g_wprev_coh) / (double)(g_c.present - g_wprev_present) : 100.0;
    g_wprev_evals = g_c.evals; g_wprev_present = g_c.present; g_wprev_coh = g_c.present_coherent;
    char wl[420];
    std::snprintf(wl, sizeof wl,
        "[MGPU][R300a3] TEMPORAL WINDOW (evaluates since the previous report) gf=%llu | evals=%llu vector_present=%.2f%% tuple_ok_when_present=%.2f%% "
        "| cumulative vector_present=%.2f%% hides phases: a game with no DLSS (features released) reads as 0%% here and as a slow decline there.",
        gf, wn, win_present, win_ok, 100.0 * (double)g_c.present / n);
    say(wl);
    char b[1400];
    std::snprintf(b, sizeof b,
        "[MGPU][R300a2] TEMPORAL %s gf=%llu | evals=%llu mean_elapsed=%.2f | vector_present=%.2f%% tuple_ok_when_present=%.2f%% same_tuple_all=%.2f%% "
        "(missing=%llu overwritten=%llu flag_disagree=%llu) | color_age 0/1/2/3+: %llu/%llu/%llu/%llu | "
        "elapsed 1/2/3/4/5/6/7/8+: %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu | min_ring_headroom=%llu | "
        "seal_lost=%llu scale_zero=%llu scale_changes=%llu extent_changes=%llu | policy=%s resets_forced=%llu "
        "unbound_by_policy=%llu",
        final_ ? "FINAL" : "SAMPLE", gf, g_c.evals, (double)g_sum_elapsed / n,
        100.0 * (double)g_c.present / n,
        g_c.present ? 100.0 * (double)g_c.present_coherent / (double)g_c.present : 100.0,
        100.0 * (double)g_c.coherent / n, g_c.mvec_missing, g_c.mvec_overwritten, g_c.flag_disagree,
        g_c.age_color[0], g_c.age_color[1], g_c.age_color[2],
        g_c.age_color[3] + g_c.age_color[4] + g_c.age_color[5] + g_c.age_color[6] + g_c.age_color[7],
        g_c.delta_hist[0], g_c.delta_hist[1], g_c.delta_hist[2], g_c.delta_hist[3],
        g_c.delta_hist[4], g_c.delta_hist[5], g_c.delta_hist[6], g_c.delta_hist[7],
        g_hist_headroom_min, g_c.seal_lost, g_c.scale_zero, g_c.scale_changes, g_c.extent_changes,
        policy_name(g_cfg.mv_policy), g_c.resets_forced, g_c.unbound_by_policy);
    say(b);

    if (g_cfg.jitter_diag && g_c.jit_evals != 0)
    {
        char jb[900]; int w = std::snprintf(jb, sizeof jb,
            "[MGPU][R300a8] JITTER DELTA ERROR BY QUEUE LAG (author R104 reconstruction vs slot delta, pixels) | evals=%llu "
            "slot_unknown=%llu slot_applied=%llu no_baseline=%llu fallback=%llu zero_fallback=%llu |", g_c.jit_evals, g_c.jit_slot_unknown,
            g_c.jit_slot_applied, g_c.jit_no_baseline, g_c.jit_fallback, g_c.jit_zero_fallback);
        for (unsigned i = 0; i < 6 && w > 0 && (size_t)w < sizeof jb; ++i)
            w += std::snprintf(jb + w, sizeof jb - (size_t)w, " lag%s%u: n=%llu mean=%.4f max=%.4f |",
                               i == 5 ? ">=" : "=", i, g_lag[i].n,
                               g_lag[i].n ? g_lag[i].sum / (double)g_lag[i].n : 0.0, g_lag[i].mx);
        say(jb);
    }

    // [R300a5] rule-based reading, numbers only. "same_tuple_all" counts a frame with NO vector as
    // incoherent; that is a capture gap, not a tuple mismatch, so the two are judged separately.
    const double pres = 100.0 * (double)g_c.present / n;
    const double tok  = g_c.present ? 100.0 * (double)g_c.present_coherent / (double)g_c.present : 100.0;
    const double mean_el = (double)g_sum_elapsed / n;
    char f[1000];
    const char *verdict;
    if (g_c.scale_zero != 0)       verdict = "SCALE ZERO seen on an evaluate: NR was handed no usable scale.";
    else if (tok < 99.0 || g_c.mvec_overwritten != 0) verdict = "TUPLE MISMATCH: a vector that IS present belongs to another frame - identity/lifetime bug.";
    else if (pres < 95.0)          verdict = "VECTOR MISSING on a share of evaluates (no vector captured for that frame): capture-side gap, NOT an identity mismatch; those frames already reach NR with MVec unbound.";
    else if (mean_el >= 1.5)       verdict = "TUPLE COHERENT, SPAN > 1: identity is right; what is left is the temporal span NR is not told about.";
    else                           verdict = "TUPLE COHERENT, SPAN ~ 1.";
    std::snprintf(f, sizeof f,
        "[MGPU][R300a5] TEMPORAL READING: vector_present=%.2f%% tuple_ok_when_present=%.2f%% mean_elapsed=%.2f "
        "scale_changes=%llu extent_changes=%llu -> %s (jitter_newest in [R300a1] is the calibrator's newest "
        "game evaluate, not this frame's; DLSS-NR takes no jitter parameter here.)",
        pres, tok, mean_el, g_c.scale_changes, g_c.extent_changes, verdict);
    say(f);
}

void periodic(unsigned long long game_frame)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (game_frame < g_next_report) return;
    g_next_report = game_frame + 300ull;
    emit_summary_locked(game_frame, false);
}

void final_summary()
{
    std::lock_guard<std::mutex> lk(g_mx);
    emit_summary_locked(0ull, true);
}

counters snapshot() { std::lock_guard<std::mutex> lk(g_mx); return g_c; }

void reset_all()
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_c = counters{}; g_wprev_evals = g_wprev_present = g_wprev_coh = 0;
    g_eval_id = 0; g_next_report = 300; g_sum_elapsed = 0; g_hist_headroom_min = 99;
    g_last_sx = g_last_sy = 0.f; g_last_rw = g_last_rh = 0;
    g_pe_have = false; g_pe_jx = g_pe_jy = 0.f; g_pe_gf = 0; g_r104_px = g_r104_py = 0.f; g_jit_lines = 0;
    for (auto &l : g_lag) l = lagstat{};
    for (unsigned i = 0; i < MAXSLOT; ++i)
    {
        g_js[i].id = 0; g_mvec_id[i] = 0; g_seal_id[i] = 0; g_seal_gf[i] = 0; g_seal_mv[i] = 0; g_seal_dp[i] = 0; }
}
} // namespace mgpu::tdiag
