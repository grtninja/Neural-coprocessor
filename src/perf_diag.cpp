// MGPU Bridge 0.2.6-dev - T1.11 performance measurement. See perf_diag.hpp.
#include "perf_diag.hpp"
#include "diag.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

namespace mgpu::perf
{
namespace
{
    double (*g_clock_fn)() = nullptr;
    double now_ms()
    {
        if (g_clock_fn) return g_clock_fn();
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    std::atomic<double> g_seal_ms[MAXSLOT];              // producer-written, consumer-read
    std::atomic<unsigned long long> g_seal_id[MAXSLOT];

    struct stat
    {
        unsigned long long n = 0; double sum = 0.0, mn = 1e30, mx = 0.0;
        void add(double v) { ++n; sum += v; if (v < mn) mn = v; if (v > mx) mx = v; }
        double mean() const { return n ? sum / (double)n : 0.0; }
        double lo() const { return n ? mn : 0.0; }
    };

    std::mutex g_mx;
    config g_cfg;
    counters g_c{};
    unsigned long long g_lines = 0, g_next_report = 600;

    // current job
    bool   j_open = false, j_eval = false;
    double j_begin = 0, j_rec = 0, j_exec = 0, j_done = 0;
    bool   j_cq = false, j_have_gpu = false, j_calib = false;
    double j_pre = 0, j_ngx = 0, j_post = 0;
    unsigned long long j_f = 0, j_completed = 0;
    unsigned j_slot = 0; double j_age_start = 0;
    unsigned long long j_gf_age = 0;
    double g_prev_done = -1.0;                           // for the idle gap
    bool g_armed_pending = false;
    double j_gap = -1.0;

    stat s_record, s_stall, s_wall, s_gpu_total, s_pre, s_ngx, s_post, s_outside, s_gap, s_age_start, s_age_done;
    stat s_outside_cq, s_outside_nocq;
    stat s_cqcopy, s_resid, s_bw, s_out_armed, s_out_cold, s_wall_armed, s_wall_cold;
    bool j_armed = false; double j_cqms = -1.0, j_cqbytes = 0.0;
    // T1.13: per-window statistics. Cumulative numbers hide phases (a menu, DLSS switched off by the game, a scene
    // change); a window is the difference between two reports.
    struct wsnap { stat wall, outside, age_s, age_d, cqcopy; unsigned long long jobs = 0, cq_used = 0, armed = 0,
                   hit = 0, catchup = 0, wait = 0, issued = 0, fsum = 0, fn = 0; unsigned long long first_f = 0, last_f = 0; };
    wsnap g_prev; unsigned long long g_win_first_f = 0, g_win_last_f = 0;
    std::vector<double> g_ngx_samples;                   // capped, for the tail
    double g_busy_ms = 0.0, g_span_first = -1.0, g_span_last = 0.0;

    double pct(std::vector<double> v, double p)
    {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        size_t i = (size_t)(p * (double)(v.size() - 1) + 0.5);
        return v[i < v.size() ? i : v.size() - 1];
    }
    void say(const char *s) { mgpu::diag::info(s); }
}

void configure(const config &c)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_cfg = c;
    char b[1000];
    std::snprintf(b, sizeof b,
        "[MGPU][R300e4] PERF T1.12 CONFIG: Perf11=%d PerfLines=%u then 1/%u PerfPrefetch=%d PerfPrefetchMax=%u. With PerfPrefetch=0 this is MEASUREMENT ONLY - no scheduling, queue, "
        "fence, barrier or NGX parameter is changed; Perf11=0 stops the instrument. GPU numbers are the author's GPU1 "
        "timestamps (mark0->2 seal copy+unpack, 2->3 NGX evaluate, 3->4 output); wall is CPU submit->fence done; "
        "outside = wall - (mark4-mark0) is time spent outside the job's own GPU timeline.",
        c.enabled ? 1 : 0, c.line_budget, c.line_every ? c.line_every : 1u, c.prefetch ? 1 : 0, c.prefetch_max);
    say(b);
}

config current() { std::lock_guard<std::mutex> lk(g_mx); return g_cfg; }

void note_seal(unsigned slot, unsigned long long sealed_id)
{
    const unsigned i = slot % MAXSLOT;
    g_seal_ms[i].store(now_ms(), std::memory_order_relaxed);
    g_seal_id[i].store(sealed_id, std::memory_order_release);
}

bool prefetch_active() { std::lock_guard<std::mutex> lk(g_mx); return g_cfg.enabled && g_cfg.prefetch; }

unsigned long long sched_target(unsigned long long completed, unsigned long long consumed,
                                unsigned long long cq_issued, bool gated, bool draining)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_cfg.enabled || !g_cfg.prefetch || !gated || draining) return completed;
    // The armed frame has not arrived yet. Its copy sits behind a GPU-side wait on the game's fence, and the copy queue
    // is a FIFO: copying any other frame now would queue BEHIND that wait and gain nothing. Do nothing this poll; the
    // next present asks again and the armed frame will be there.
    if (cq_issued > completed && cq_issued > consumed) { ++g_c.pol_wait; return consumed; }
    // An armed frame is usable only if it has ARRIVED (<= completed) and has not been consumed yet.
    if (cq_issued > consumed && cq_issued <= completed)
    {
        if (completed - cq_issued <= (unsigned long long)g_cfg.prefetch_max) { ++g_c.pol_hit; return cq_issued; }
        ++g_c.pol_catchup;                       // armed frame too old: jump to the newest, as T1.11 does
        return completed;
    }
    ++g_c.pol_none;                              // nothing armed that has arrived: newest, as T1.11 does
    return completed;
}

unsigned long long arm_target(unsigned long long completed, unsigned long long f)
{
    // The first frame that will arrive after now. The copy for frame n lands in tex_in[n & 1]; the evaluate that is
    // about to run reads tex_in[f & 1], so n must have the other parity or the copy would overwrite its input.
    unsigned long long n = completed + 1ull;
    if (n < f + 1ull) n = f + 1ull;
    if ((n & 1ull) == (f & 1ull)) ++n;
    return n;
}

void note_cq_state(bool armed) { std::lock_guard<std::mutex> lk(g_mx); g_armed_pending = armed; }
void note_cq_issue() { std::lock_guard<std::mutex> lk(g_mx); ++g_c.cq_issued_total; }
void note_cq_copy(double ms, double bytes) { std::lock_guard<std::mutex> lk(g_mx); if (j_open) { j_cqms = ms; j_cqbytes = bytes; } }

void job_begin(unsigned long long f, unsigned slot, unsigned long long completed,
               bool run_nr, bool newest, unsigned long long game_frame_now,
               unsigned long long game_frame_slot)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_cfg.enabled) { j_open = false; return; }
    if (!newest) ++g_c.skipped_older;
    if (!run_nr) { j_open = false; return; }

    j_open = true; j_eval = true; j_cq = false; j_have_gpu = false; j_calib = false;
    j_armed = g_armed_pending; g_armed_pending = false; j_cqms = -1.0; j_cqbytes = 0.0;
    j_begin = now_ms(); j_rec = j_exec = j_done = 0.0;
    j_f = f; j_completed = completed; j_slot = slot;
    const unsigned i = slot % MAXSLOT;
    j_age_start = (g_seal_id[i].load(std::memory_order_acquire) == f)
                      ? j_begin - g_seal_ms[i].load(std::memory_order_relaxed) : -1.0;
    j_gf_age = game_frame_now > game_frame_slot ? game_frame_now - game_frame_slot : 0ull;
    if (j_gf_age >= 1) ++g_c.shadow_obs1;
    if (j_gf_age >= 2) ++g_c.shadow_obs2;
    if (j_gf_age >= 3) ++g_c.shadow_obs3;
    j_gap = (g_prev_done >= 0.0) ? j_begin - g_prev_done : -1.0;
    if (j_gap >= 0.0) s_gap.add(j_gap);                          // GPU1 idle + CPU between jobs
    if (g_span_first < 0.0) g_span_first = j_begin;
}

void job_recorded() { std::lock_guard<std::mutex> lk(g_mx); if (j_open) j_rec = now_ms(); }
void job_exec(bool cq) { std::lock_guard<std::mutex> lk(g_mx); if (j_open) { j_exec = now_ms(); j_cq = cq; } }
void job_done() { std::lock_guard<std::mutex> lk(g_mx); if (j_open) j_done = now_ms(); }
void job_abort() { std::lock_guard<std::mutex> lk(g_mx); if (j_open) { ++g_c.aborted; j_open = false; } }

void note_gpu(double pre, double ngx, double post, bool calib)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!j_open) return;
    j_pre = pre; j_ngx = ngx; j_post = post; j_have_gpu = true; j_calib = calib;
}

void job_end(unsigned long long produced_now)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!j_open) return;
    j_open = false;
    if (j_done == 0.0) return;                           // no fence wait happened

    if (j_calib) { ++g_c.calib; g_prev_done = j_done; return; }
    ++g_c.jobs;
    if (g_win_first_f == 0) g_win_first_f = j_f;
    g_win_last_f = j_f;
    if (j_cq) ++g_c.used_cq;

    const double record = (j_rec > 0.0) ? j_rec - j_begin : 0.0;
    const double stall  = (j_rec > 0.0 && j_exec > 0.0) ? j_exec - j_rec : 0.0;  // incl. queue Wait + Execute + Signal CPU cost
    const double wall   = j_done - j_exec;
    s_record.add(record); s_stall.add(stall); s_wall.add(wall);
    g_busy_ms += wall;
    g_span_last = j_done;

    double total = 0.0, outside = wall;
    if (j_have_gpu)
    {
        total = j_pre + j_ngx + j_post;
        outside = wall > total ? wall - total : 0.0;
        s_pre.add(j_pre); s_ngx.add(j_ngx); s_post.add(j_post); s_gpu_total.add(total); s_outside.add(outside);
        (j_cq ? s_outside_cq : s_outside_nocq).add(outside);
        if (g_ngx_samples.size() < 4096) g_ngx_samples.push_back(j_ngx);
        if (j_cq)
        {
            (j_armed ? s_out_armed : s_out_cold).add(outside);
            (j_armed ? s_wall_armed : s_wall_cold).add(wall);
            if (j_cqms >= 0.0)
            {
                s_cqcopy.add(j_cqms);
                s_resid.add(outside > j_cqms ? outside - j_cqms : 0.0);
                if (j_cqms > 0.0 && j_cqbytes > 0.0) s_bw.add(j_cqbytes / (j_cqms * 1.0e6));   // GB/s
            }
        }
    }
    if (j_cq) { ++g_c.cq_used; if (j_armed) ++g_c.armed_jobs; }
    else ++g_c.no_gpu_ts;

    const double age_done = (j_age_start >= 0.0) ? j_done - (j_begin - j_age_start) : -1.0;
    if (j_age_start >= 0.0) { s_age_start.add(j_age_start); s_age_done.add(age_done); }

    const unsigned long long newer = produced_now > j_f ? produced_now - j_f : 0ull;
    ++g_c.done_lag[newer >= 4ull ? 4u : (unsigned)newer];

    if (g_cfg.enabled)
    {
        const bool show = (g_c.jobs <= g_cfg.line_budget) ||
                          (g_cfg.line_every != 0u && (g_c.jobs % g_cfg.line_every) == 0ull);
        if (show)
        {
            char b[960];
            std::snprintf(b, sizeof b,
                "[MGPU][R300e1] JOB n=%llu f=%llu slot=%u cq=%d | gpu1_ms pre=%.3f ngx=%.3f post=%.3f total=%.3f | "
                "wall_ms=%.3f outside=%.3f | cpu_ms record=%.3f submit=%.3f | gap_before_ms=%.3f | age_ms start=%.2f done=%.2f | "
                "game_frames_old_at_start=%llu newer_sealed_at_done=%llu | armed_before_start=%d cq_copy_ms=%.3f",
                g_c.jobs, j_f, j_slot, j_cq ? 1 : 0, j_have_gpu ? j_pre : -1.0, j_have_gpu ? j_ngx : -1.0,
                j_have_gpu ? j_post : -1.0, j_have_gpu ? total : -1.0, wall, j_have_gpu ? outside : -1.0,
                record, stall, j_gap,
                j_age_start, age_done, j_gf_age, newer, j_armed ? 1 : 0, j_cqms);
            say(b);
        }
    }
    g_prev_done = j_done;
}

static double wmean(const stat &now, const stat &prev)
{ return now.n > prev.n ? (now.sum - prev.sum) / (double)(now.n - prev.n) : 0.0; }

static void emit_locked(unsigned long long consumed, bool final_)
{
    if (!g_cfg.enabled || g_c.jobs == 0) return;
    if (!final_)
    {   // [R300e6] THIS WINDOW ONLY (jobs since the previous report)
        const unsigned long long dj = g_c.jobs - g_prev.jobs, dcq = g_c.cq_used - g_prev.cq_used;
        char w[900];
        const double frames_per_job = dj ? (double)(g_win_last_f - g_prev.last_f) / (double)dj : 0.0;
        std::snprintf(w, sizeof w,
            "[MGPU][R300e6] PERF WINDOW consumed=%llu | jobs=%llu armed=%.1f%% | wall=%.3f outside=%.3f cq_copy=%.3f | age_ms start=%.2f done=%.2f | "
            "sealed frames advanced per job=%.2f | policy hit/catch-up/wait-polls=%llu/%llu/%llu | copies issued=%llu used=%llu | "
            "READ ONLY THE WINDOWS WHEN A RUN HAS PHASES: a game that releases its DLSS features, a menu, or a scene change shifts every cumulative figure.",
            consumed, dj, dcq ? 100.0 * (double)(g_c.armed_jobs - g_prev.armed) / (double)dcq : 0.0,
            wmean(s_wall, g_prev.wall), wmean(s_outside, g_prev.outside), wmean(s_cqcopy, g_prev.cqcopy),
            wmean(s_age_start, g_prev.age_s), wmean(s_age_done, g_prev.age_d), frames_per_job,
            g_c.pol_hit - g_prev.hit, g_c.pol_catchup - g_prev.catchup, g_c.pol_wait - g_prev.wait,
            g_c.cq_issued_total - g_prev.issued, dcq);
        say(w);
        g_prev.wall = s_wall; g_prev.outside = s_outside; g_prev.age_s = s_age_start; g_prev.age_d = s_age_done; g_prev.cqcopy = s_cqcopy;
        g_prev.jobs = g_c.jobs; g_prev.cq_used = g_c.cq_used; g_prev.armed = g_c.armed_jobs; g_prev.hit = g_c.pol_hit;
        g_prev.catchup = g_c.pol_catchup; g_prev.wait = g_c.pol_wait; g_prev.issued = g_c.cq_issued_total; g_prev.last_f = g_win_last_f;
    }
    const double span = (g_span_last > g_span_first && g_span_first >= 0.0) ? g_span_last - g_span_first : 0.0;
    const double busy_pct = span > 0.0 ? 100.0 * g_busy_ms / span : 0.0;
    const double p50 = pct(g_ngx_samples, 0.50), p95 = pct(g_ngx_samples, 0.95), fl = s_ngx.lo();

    char b[1500];
    std::snprintf(b, sizeof b,
        "[MGPU][R300e2] PERF %s consumed=%llu | jobs=%llu skipped_older=%llu aborted=%llu calib=%llu no_gpu_ts=%llu | "
        "GPU1 ms (mean/min/max): pre=%.3f/%.3f/%.3f NGX=%.3f/%.3f/%.3f post=%.3f/%.3f/%.3f total=%.3f | "
        "wall(submit->done)=%.3f/%.3f/%.3f OUTSIDE-GPU-TIMELINE=%.3f (cq=%.3f n=%llu, no-cq=%.3f n=%llu) | "
        "cpu record=%.3f submit=%.3f | idle gap between jobs=%.3f/%.3f/%.3f GPU1-busy=%.1f%% | NGX p50=%.3f p95=%.3f floor=%.3f",
        final_ ? "FINAL" : "SAMPLE", consumed, g_c.jobs, g_c.skipped_older, g_c.aborted, g_c.calib, g_c.no_gpu_ts,
        s_pre.mean(), s_pre.lo(), s_pre.mx, s_ngx.mean(), s_ngx.lo(), s_ngx.mx, s_post.mean(), s_post.lo(), s_post.mx,
        s_gpu_total.mean(), s_wall.mean(), s_wall.lo(), s_wall.mx, s_outside.mean(),
        s_outside_cq.mean(), s_outside_cq.n, s_outside_nocq.mean(), s_outside_nocq.n,
        s_record.mean(), s_stall.mean(), s_gap.mean(), s_gap.lo(), s_gap.mx, busy_pct, p50, p95, fl);
    say(b);

    char a[900];
    std::snprintf(a, sizeof a,
        "[MGPU][R300e3] PERF AGE | age_ms since seal: at job start mean=%.2f max=%.2f, at job done mean=%.2f max=%.2f | "
        "newer sealed frames at completion 0/1/2/3/4+: %llu/%llu/%llu/%llu/%llu | SHADOW (nothing skipped) jobs that were "
        ">=1 / >=2 / >=3 game frames old at START: %llu / %llu / %llu of %llu",
        s_age_start.mean(), s_age_start.mx, s_age_done.mean(), s_age_done.mx,
        g_c.done_lag[0], g_c.done_lag[1], g_c.done_lag[2], g_c.done_lag[3], g_c.done_lag[4],
        g_c.shadow_obs1, g_c.shadow_obs2, g_c.shadow_obs3, g_c.jobs);
    say(a);

    {   // R300e5: the copy-queue decomposition of OUTSIDE
        const double copies_wasted = g_c.cq_issued_total > g_c.cq_used ? (double)(g_c.cq_issued_total - g_c.cq_used) : 0.0;
        char q[1200];
        std::snprintf(q, sizeof q,
            "[MGPU][R300e5] CQ DECOMPOSITION | jobs on the copy queue=%llu (armed before start=%llu = %.1f%%) | "
            "copy-queue GPU time mean/min/max=%.3f/%.3f/%.3f ms -> %.2f GB/s mean | OUTSIDE mean=%.3f of which copy=%.3f, "
            "residual(wait for arrival gate / queue / completion)=%.3f | OUTSIDE when armed=%.3f (n=%llu, wall %.3f) vs cold=%.3f "
            "(n=%llu, wall %.3f) | copies issued=%llu used=%llu never used=%.0f | policy hit/catch-up/none/wait=%llu/%llu/%llu/%llu",
            g_c.cq_used, g_c.armed_jobs, g_c.cq_used ? 100.0 * (double)g_c.armed_jobs / (double)g_c.cq_used : 0.0,
            s_cqcopy.mean(), s_cqcopy.lo(), s_cqcopy.mx, s_bw.mean(), s_outside_cq.mean(), s_cqcopy.mean(), s_resid.mean(),
            s_out_armed.mean(), s_out_armed.n, s_wall_armed.mean(), s_out_cold.mean(), s_out_cold.n, s_wall_cold.mean(),
            g_c.cq_issued_total, g_c.cq_used, copies_wasted, g_c.pol_hit, g_c.pol_catchup, g_c.pol_none, g_c.pol_wait);
        say(q);
        const char *cv;
        if (s_cqcopy.n == 0) cv = "NO COPY-QUEUE TIMESTAMPS: the copy's own cost cannot be stated.";
        else if (s_cqcopy.mean() >= 0.85 * s_outside_cq.mean() && s_outside_cq.mean() > 0.0)
            cv = "OUTSIDE IS THE TRANSFER ITSELF: the copy queue's own GPU time accounts for >= 85% of it. It is bandwidth, not a scheduling wait - overlap it with NGX or shrink what is copied.";
        else if (s_resid.mean() >= 0.30 * s_outside_cq.mean())
            cv = "A LARGE RESIDUAL beyond the copy's own time: the job waits for something other than the transfer (arrival gate, queue ordering, completion latency).";
        else cv = "Copy time explains most of OUTSIDE with a modest residual.";
        char z[700];
        if (s_out_armed.n > 0 && s_out_cold.n > 0)
            std::snprintf(z, sizeof z, "[MGPU][R300e5] CQ READING: %s | armed jobs wait %.3f ms outside vs %.3f cold - %s", cv,
                          s_out_armed.mean(), s_out_cold.mean(),
                          s_out_armed.mean() < 0.5 * s_out_cold.mean() ? "OVERLAP WORKS WHEN THE FRAME IS ARMED." : "arming does not remove the wait.");
        else
            std::snprintf(z, sizeof z, "[MGPU][R300e5] CQ READING: %s | no armed jobs to compare (the consumer evaluates the newest frame, so the armed f+1 copy is not the one used).", cv);
        say(z);
    }

    // Rule-based reading, numbers only.
    const double ngx = s_ngx.mean(), tot = s_gpu_total.mean(), wall = s_wall.mean(), out = s_outside.mean();
    const char *v1, *v2;
    if (s_ngx.n == 0)                         v1 = "NO GPU TIMESTAMPS: the NGX share cannot be stated.";
    else if (wall > 0.0 && ngx / wall >= 0.80 && fl > 0.0 && p95 / fl < 1.20)
        v1 = "NGX COMPUTE IS THE JOB AND IT IS STABLE (p95 within 20% of the floor): incompressible on this card at this resolution - optimise WHICH frames are evaluated, not NR itself.";
    else if (fl > 0.0 && p95 / fl >= 1.20)
        v1 = "NGX TIME HAS A TAIL (p95 >= 1.2x floor): something other than NR arithmetic stretches some evaluates - clocks, contention on GPU1's queue, or memory.";
    else
        v1 = "NGX is not the dominant part of the job: read pre/post/outside before touching NR.";
    if (wall > 0.0 && out / wall >= 0.25)
        v2 = "A QUARTER OR MORE OF THE WALL TIME IS OUTSIDE THE JOB'S GPU TIMELINE: queue-level wait / idle before mark 0 / completion latency - the critical path is not NR.";
    else if (s_gap.n && s_gap.mean() >= 0.25 * (wall > 0.0 ? wall : 1.0))
        v2 = "GPU1 SITS IDLE BETWEEN JOBS (gap >= 25% of a job): the loop, not the card, limits throughput - pipelining or earlier submission is the lever.";
    else
        v2 = "GPU1 is close to continuously busy and waits are small: gains must come from doing less work per frame or choosing which frames to evaluate.";
    std::snprintf(a, sizeof a, "[MGPU][R300e4] PERF READING: NGX=%.3f of total=%.3f wall=%.3f outside=%.3f busy=%.1f%% -> %s | %s",
                  ngx, tot, wall, out, busy_pct, v1, v2);
    say(a);
}

void periodic(unsigned long long consumed)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (consumed < g_next_report) return;
    g_next_report = consumed + 600ull;
    emit_locked(consumed, false);
}

void final_summary()
{
    std::lock_guard<std::mutex> lk(g_mx);
    emit_locked(0ull, true);
}

counters snapshot() { std::lock_guard<std::mutex> lk(g_mx); return g_c; }

void reset_all()
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_c = counters{}; g_lines = 0; g_next_report = 600; j_open = false; g_prev_done = -1.0;
    s_record = s_stall = s_wall = s_gpu_total = s_pre = s_ngx = s_post = s_outside = s_gap = s_age_start = s_age_done = stat{};
    s_outside_cq = s_outside_nocq = stat{};
    s_cqcopy = s_resid = s_bw = s_out_armed = s_out_cold = s_wall_armed = s_wall_cold = stat{};
    g_armed_pending = false; j_armed = false; g_prev = wsnap{}; g_win_first_f = g_win_last_f = 0;
    g_ngx_samples.clear(); g_busy_ms = 0.0; g_span_first = -1.0; g_span_last = 0.0;
    for (unsigned i = 0; i < MAXSLOT; ++i) { g_seal_id[i] = 0; g_seal_ms[i] = 0.0; }
}

void set_clock_for_test(double (*fn)()) { g_clock_fn = fn; }
} // namespace mgpu::perf
