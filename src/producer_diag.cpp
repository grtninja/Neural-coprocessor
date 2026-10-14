// MGPU Bridge 0.2.6-dev - T1.16 producer (GPU 0) cost measurement. See producer_diag.hpp.
#include "producer_diag.hpp"
#include "diag.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

namespace mgpu::pdiag
{
namespace
{
    double (*g_clock_fn)() = nullptr;
    double now_s()
    {
        if (g_clock_fn) return g_clock_fn();
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    struct stat
    {
        unsigned long long n = 0; double sum = 0.0, mn = 1e30, mx = 0.0;
        void add(double v) { ++n; sum += v; if (v < mn) mn = v; if (v > mx) mx = v; }
        double mean() const { return n ? sum / (double)n : 0.0; }
        double lo() const { return n ? mn : 0.0; }
    };
    double wmean(const stat &a, const stat &p) { return a.n > p.n ? (a.sum - p.sum) / (double)(a.n - p.n) : 0.0; }
    double pct(std::vector<double> v, double p)
    {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        size_t i = (size_t)(p * (double)(v.size() - 1) + 0.5);
        return v[i < v.size() ? i : v.size() - 1];
    }

    std::mutex g_mx;
    config g_cfg;
    stat g_gpu[5];
    stat p_gpu[5];                                    // previous report, for windows
    unsigned long long g_gpu_invalid[5] = {0, 0, 0, 0, 0};
    std::vector<double> g_gpu_samples[5];             // capped, for p95

    // ---- LOCK-FREE, because the GAME'S RENDER THREAD calls note_cpu_record() and note_sealed() every frame. ----
    // The bridge thread holds g_mx while it formats and writes a report; a lock the render thread could wait on is exactly the
    // class of fault the author recorded as DEFECT E (60 -> 49.8 fps). Everything the game's thread touches below is a relaxed
    // atomic; the bridge thread only reads.
    std::atomic<unsigned long long> a_cpu_n[2], a_cpu_sum_ns[2], a_cpu_max_ns[2];
    std::atomic<unsigned long long> a_ring_id[64], a_ring_bytes[64];
    std::atomic<unsigned long long> a_sealed_frames{0}, a_sealed_bytes{0}, a_mvec_frames{0};
    // volume, bridge-thread side
    unsigned long long g_eval_frames = 0, g_eval_bytes = 0;
    unsigned long long p_sealed_frames = 0, p_sealed_bytes = 0, p_eval_bytes = 0, p_eval_frames = 0;
    unsigned long long p_mvec_frames = 0;
    double cpu_mean_ms(int b) { const unsigned long long n = a_cpu_n[b].load(std::memory_order_relaxed); return n ? (double)a_cpu_sum_ns[b].load(std::memory_order_relaxed) / 1.0e6 / (double)n : 0.0; }
    double cpu_max_ms(int b) { return (double)a_cpu_max_ns[b].load(std::memory_order_relaxed) / 1.0e6; }
    double p_t = -1.0;
    unsigned long long g_next = 600;
    backlog p_back{};
    bool g_have_back = false;
    backlog g_last_back{};

    void say(const char *s) { mgpu::diag::info(s); }
    double mb(unsigned long long b) { return (double)b / 1048576.0; }
}

void configure(const config &c)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_cfg = c;
    if (g_cfg.report_every == 0) g_cfg.report_every = 600;
    g_next = g_cfg.report_every;
    char b[760];
    std::snprintf(b, sizeof b,
        "[MGPU][R300g6] PRODUCER COST T1.16 CONFIG: Gpu0Timers=%d ProducerReport=%u. MEASUREMENT ONLY - no scheduling, queue, fence, "
        "barrier or parameter changes. With Gpu0Timers=1 two GPU timestamps bracket the producer's colour+depth+seal block and two "
        "bracket the MVec copy, on GPU 0, plus one ResolveQueryData per frame into a readback buffer that only the BRIDGE thread reads, "
        "after the game's fence says the frame is done: the game thread never waits for it. Gpu0Timers=0 inserts nothing, so a run with "
        "1 and a run with 0 on the same scene measure the instrument's own cost.",
        g_cfg.gpu0_timers ? 1 : 0, g_cfg.report_every);
    say(b);
}

config current() { std::lock_guard<std::mutex> lk(g_mx); return g_cfg; }

void note_cpu_record(int block, double ms)
{
    if (block < 0 || block > 1 || ms < 0.0) return;
    const unsigned long long ns = (unsigned long long)(ms * 1.0e6);
    a_cpu_n[block].fetch_add(1ull, std::memory_order_relaxed);
    a_cpu_sum_ns[block].fetch_add(ns, std::memory_order_relaxed);
    unsigned long long m = a_cpu_max_ns[block].load(std::memory_order_relaxed);
    while (ns > m && !a_cpu_max_ns[block].compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
}

void note_gpu_block(int block, unsigned long long t0, unsigned long long t1, double hz)
{
    if (block < 0 || block > 4) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (t0 == 0ull || t1 == 0ull || t1 < t0 || hz <= 0.0) { ++g_gpu_invalid[block]; return; }
    const double ms = (double)(t1 - t0) * 1000.0 / hz;
    if (ms > 100.0) { ++g_gpu_invalid[block]; return; }       // a garbage pair, not a copy that took 100 ms
    g_gpu[block].add(ms);
    if (g_gpu_samples[block].size() < 4096) g_gpu_samples[block].push_back(ms);
}

void note_gpu_invalid(int block)
{
    if (block < 0 || block > 4) return;
    std::lock_guard<std::mutex> lk(g_mx);
    ++g_gpu_invalid[block];
}

void note_sealed(unsigned long long frame, unsigned long long cd, unsigned long long mv)
{
    const unsigned i = (unsigned)(frame % 64ull);
    a_ring_bytes[i].store(cd + mv, std::memory_order_relaxed);
    a_ring_id[i].store(frame, std::memory_order_release);          // id last: a reader that sees the id sees the bytes
    a_sealed_frames.fetch_add(1ull, std::memory_order_relaxed);
    a_sealed_bytes.fetch_add(cd + mv, std::memory_order_relaxed);
    if (mv != 0ull) a_mvec_frames.fetch_add(1ull, std::memory_order_relaxed);
}

void note_evaluated(unsigned long long frame)
{
    std::lock_guard<std::mutex> lk(g_mx);                           // bridge thread only
    const unsigned i = (unsigned)(frame % 64ull);
    if (a_ring_id[i].load(std::memory_order_acquire) != frame) return;   // the entry was recycled: not counted rather than guessed
    ++g_eval_frames; g_eval_bytes += a_ring_bytes[i].load(std::memory_order_relaxed);
}

static void emit_locked(unsigned long long sealed, const backlog &b, bool final_)
{
    const double t = now_s();
    const double dt = (p_t >= 0.0) ? t - p_t : 0.0;
    const unsigned long long g_sealed_frames = a_sealed_frames.load(std::memory_order_relaxed);
    const unsigned long long g_sealed_bytes = a_sealed_bytes.load(std::memory_order_relaxed);
    const unsigned long long g_mvec_frames = a_mvec_frames.load(std::memory_order_relaxed);
    char l[1500];

    // [R300g1] GPU 0 and CPU cost of the producer blocks
    std::snprintf(l, sizeof l,
        "[MGPU][R300g1] PRODUCER GPU0 COST %s sealed=%llu | WHOLE end-of-frame block (seal+barrier+colour+depth), GPU0 ms: window mean=%.3f | cumulative mean/min/max/p95=%.3f/%.3f/%.3f/%.3f (n=%llu, invalid=%llu) | "
        "MVec copy, GPU0 ms: window mean=%.3f | cumulative mean/min/max/p95=%.3f/%.3f/%.3f/%.3f (n=%llu, invalid=%llu) | "
        "CPU RECORDING ms (QPC; recording only, nothing waits): block0 mean/max=%.4f/%.4f (n=%llu), mvec mean/max=%.4f/%.4f (n=%llu)",
        final_ ? "FINAL" : "WINDOW", sealed,
        wmean(g_gpu[0], p_gpu[0]), g_gpu[0].mean(), g_gpu[0].lo(), g_gpu[0].mx, pct(g_gpu_samples[0], 0.95), g_gpu[0].n, g_gpu_invalid[0],
        wmean(g_gpu[1], p_gpu[1]), g_gpu[1].mean(), g_gpu[1].lo(), g_gpu[1].mx, pct(g_gpu_samples[1], 0.95), g_gpu[1].n, g_gpu_invalid[1],
        cpu_mean_ms(0), cpu_max_ms(0), a_cpu_n[0].load(std::memory_order_relaxed), cpu_mean_ms(1), cpu_max_ms(1), a_cpu_n[1].load(std::memory_order_relaxed));
    say(l);
    // [R300g2] the same block, split. The first piece contains a barrier that waits for the GAME's own rendering to finish: it is the game's
    // work, not the bridge's, and only the next two pieces are copies the bridge asked for.
    std::snprintf(l, sizeof l,
        "[MGPU][R300g1] PRODUCER BLOCK SPLIT %s | seal copy + barrier + DRAIN of the game's rendering (game's own work, not bridge work): window mean=%.3f cumulative mean/p95=%.3f/%.3f ms | "
        "COLOUR copy: window mean=%.3f cumulative mean/min/max/p95=%.3f/%.3f/%.3f/%.3f ms | DEPTH copy: window mean=%.3f cumulative mean/min/max/p95=%.3f/%.3f/%.3f/%.3f ms | "
        "(colour + depth copies = %.3f ms on average; invalid readings: pre=%llu colour=%llu depth=%llu)",
        final_ ? "FINAL" : "WINDOW",
        wmean(g_gpu[2], p_gpu[2]), g_gpu[2].mean(), pct(g_gpu_samples[2], 0.95),
        wmean(g_gpu[3], p_gpu[3]), g_gpu[3].mean(), g_gpu[3].lo(), g_gpu[3].mx, pct(g_gpu_samples[3], 0.95),
        wmean(g_gpu[4], p_gpu[4]), g_gpu[4].mean(), g_gpu[4].lo(), g_gpu[4].mx, pct(g_gpu_samples[4], 0.95),
        g_gpu[3].mean() + g_gpu[4].mean(), g_gpu_invalid[2], g_gpu_invalid[3], g_gpu_invalid[4]);
    say(l);

    // [R300g3] volume sealed vs volume that reached a NR job
    const unsigned long long ds = g_sealed_bytes - p_sealed_bytes, de = g_eval_bytes - p_eval_bytes;
    const unsigned long long fs = g_sealed_frames - p_sealed_frames, fe = g_eval_frames - p_eval_frames;
    std::snprintf(l, sizeof l,
        "[MGPU][R300g3] PRODUCER VOLUME %s | window: sealed %llu frames = %.0f MB (%.2f GB/s over %.1f s), evaluated %llu frames = %.0f MB, "
        "NEVER EVALUATED %.1f%% of the sealed bytes | cumulative: sealed %llu frames = %.0f MB, evaluated %llu frames = %.0f MB, NEVER EVALUATED %.1f%% | "
        "frames carrying vectors: %.1f%%",
        final_ ? "FINAL" : "WINDOW", fs, mb(ds), dt > 0.0 ? mb(ds) / 1024.0 / dt : 0.0, dt, fe, mb(de),
        ds ? 100.0 * (1.0 - (double)de / (double)ds) : 0.0,
        g_sealed_frames, mb(g_sealed_bytes), g_eval_frames, mb(g_eval_bytes),
        g_sealed_bytes ? 100.0 * (1.0 - (double)g_eval_bytes / (double)g_sealed_bytes) : 0.0,
        g_sealed_frames ? 100.0 * (double)g_mvec_frames / (double)g_sealed_frames : 0.0);
    say(l);

    // [R300g4] the author's backlog counters (cumulative q_hist/q_sum/q_n/q_max), window deltas
    if (g_have_back)
    {
        const unsigned long long dn = b.n - p_back.n;
        int w = std::snprintf(l, sizeof l,
            "[MGPU][R300g4] PRODUCER BACKLOG %s (frames whose copies are NOT yet complete on GPU0 when the next frame's signal is issued; the author's q_hist, unchanged) | "
            "window mean=%.3f | cumulative mean=%.3f max=%llu | window histogram 0/1/2/3/4/5/6/7+:",
            final_ ? "FINAL" : "WINDOW", dn ? (double)(b.sum - p_back.sum) / (double)dn : 0.0,
            b.n ? (double)b.sum / (double)b.n : 0.0, b.max);
        for (int i = 0; i < 8 && w > 0 && (size_t)w < sizeof l; ++i)
            w += std::snprintf(l + w, sizeof l - (size_t)w, " %llu", b.hist[i] - p_back.hist[i]);
        say(l);
    }

    // [R300g5] reading, numbers only
    const double cd = g_gpu[3].mean() + g_gpu[4].mean(), mv = g_gpu[1].mean();   // the copies the bridge asked for; the drain wait is the game's own
    const double presence = g_sealed_frames ? (double)g_mvec_frames / (double)g_sealed_frames : 0.0;
    const double per_frame = cd + mv * presence;
    const double never = g_sealed_bytes ? 100.0 * (1.0 - (double)g_eval_bytes / (double)g_sealed_bytes) : 0.0;
    const double period_ms = (dt > 0.0 && fs > 0) ? 1000.0 * dt / (double)fs : 0.0;
    char v[900];
    if (g_gpu[3].n == 0)
        std::snprintf(v, sizeof v, "[MGPU][R300g5] PRODUCER READING: no GPU0 timestamp readings yet (Gpu0Timers=%d). Volume and CPU recording cost above are still valid; "
                      "%.1f%% of the sealed bytes never reached a NR job.", g_cfg.gpu0_timers ? 1 : 0, never);
    else
        std::snprintf(v, sizeof v,
            "[MGPU][R300g5] PRODUCER READING: GPU0 time of the bridge's own copies per sealed frame = %.3f ms (colour + depth) + %.3f ms x %.0f%% frames with vectors (MVec) = %.3f ms on average, NOT counting the barrier/drain wait on the game's own rendering; "
            "%.1f%% of the sealed bytes never reached a NR job. Compare %.3f ms with the frame-time gap you see between the game with and without the bridge: "
            "this is the GPU0 execution time of the producer's own commands, NOT proof that it explains the gap (bus contention and the tap pass are not in it).",
            cd, mv, 100.0 * presence, per_frame, never, per_frame);
    say(v);
    (void)period_ms;

    // roll the window
    for (int k = 0; k < 5; ++k) p_gpu[k] = g_gpu[k];
    p_sealed_bytes = g_sealed_bytes; p_eval_bytes = g_eval_bytes; p_sealed_frames = g_sealed_frames; p_eval_frames = g_eval_frames;
    p_back = b; p_t = t;
}

void periodic(unsigned long long sealed, const backlog &b)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_last_back = b; g_have_back = true;
    if (p_t < 0.0) { p_t = now_s(); }                  // first call: anchor the window clock
    if (sealed < g_next) return;
    g_next = sealed + (unsigned long long)g_cfg.report_every;
    emit_locked(sealed, b, false);
}

void final_summary()
{
    std::lock_guard<std::mutex> lk(g_mx);
    const unsigned long long sf = a_sealed_frames.load(std::memory_order_relaxed);
    if (sf == 0) return;
    emit_locked(sf, g_have_back ? g_last_back : backlog{}, true);
}

void set_clock_for_test(double (*fn)()) { g_clock_fn = fn; }
counters snapshot()
{
    std::lock_guard<std::mutex> lk(g_mx);
    counters c{};
    for (int i = 0; i < 5; ++i) { c.gpu_n[i] = g_gpu[i].n; c.gpu_invalid[i] = g_gpu_invalid[i]; }
    for (int i = 0; i < 2; ++i) c.cpu_n[i] = a_cpu_n[i].load();
    c.sealed_frames = a_sealed_frames.load(); c.sealed_bytes = a_sealed_bytes.load(); c.evaluated_frames = g_eval_frames; c.evaluated_bytes = g_eval_bytes;
    return c;
}
void reset_all()
{
    std::lock_guard<std::mutex> lk(g_mx);
    for (int i = 0; i < 5; ++i) { g_gpu[i] = stat{}; p_gpu[i] = stat{}; g_gpu_invalid[i] = 0; g_gpu_samples[i].clear(); }
    for (int i = 0; i < 2; ++i) { a_cpu_n[i] = 0; a_cpu_sum_ns[i] = 0; a_cpu_max_ns[i] = 0; }
    for (auto &e : a_ring_id) e = 0;
    for (auto &e : a_ring_bytes) e = 0;
    a_sealed_frames = 0; a_sealed_bytes = 0; a_mvec_frames = 0;
    g_eval_frames = g_eval_bytes = 0;
    p_sealed_frames = p_sealed_bytes = p_eval_bytes = p_eval_frames = 0;
    p_mvec_frames = 0; p_t = -1.0; g_next = g_cfg.report_every ? g_cfg.report_every : 600;
    p_back = backlog{}; g_last_back = backlog{}; g_have_back = false;
}
} // namespace mgpu::pdiag
