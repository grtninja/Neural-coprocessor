// MGPU Bridge 0.2.6-dev - T1.15 DLSS-silence detector. See feature_diag.hpp.
#include "feature_diag.hpp"
#include "diag.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>

namespace mgpu::feature_diag
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

    std::mutex g_mx;
    unsigned long long g_step = 30;              // look every 30 game frames
    unsigned long long g_last_gf = 0, g_last_evals = 0, g_last_all = 0;
    double g_last_t = 0.0;
    bool g_seen_eval = false;                    // never "silent" before the first evaluate of the run
    unsigned long long g_quiet_obs = 0;          // consecutive observations with no evaluate
    bool g_silent = false;
    unsigned long long g_silence_gf0 = 0, g_silence_creates0 = 0, g_silence_releases0 = 0, g_silence_all0 = 0;
    double g_silence_t0 = 0.0;
    counters g_c{};
    unsigned long long g_last_creates = 0, g_last_releases = 0;
    const unsigned long long NEED_QUIET_OBS = 2; // two looks in a row = ~60 presents without a single evaluate

    unsigned long long live_of(unsigned long long c, unsigned long long r) { return c > r ? c - r : 0ull; }
    void say(const char *s) { mgpu::diag::info(s); }
}

void observe(unsigned long long gf, unsigned long long evals, unsigned long long all_evals,
             unsigned long long creates, unsigned long long releases)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_last_creates = creates; g_last_releases = releases;
    if (g_last_gf == 0) { g_last_gf = gf; g_last_evals = evals; g_last_all = all_evals; g_last_t = now_s(); if (evals > 0) g_seen_eval = true; return; }
    if (gf < g_last_gf + g_step) return;

    const double t = now_s();
    const double dt = t - g_last_t;
    const double fps = dt > 0.0 ? (double)(gf - g_last_gf) / dt : 0.0;
    const bool evaluated = evals > g_last_evals;
    if (evaluated) g_seen_eval = true;

    if (evaluated)
    {
        g_quiet_obs = 0;
        if (g_silent)
        {
            g_silent = false; ++g_c.resumed;
            char b[820];
            const unsigned long long all_during = all_evals - g_silence_all0;
            std::snprintf(b, sizeof b,
                "[MGPU][R300f1] SCENE-FEATURE EVALUATES RESUMED after %llu presents (%.1f s). During that stretch the hook saw %llu Evaluate call(s) on ALL features (%s). "
                "Features now: creates=%llu releases=%llu live=%llu (at the start: creates=%llu releases=%llu live=%llu).",
                gf - g_silence_gf0, t - g_silence_t0, all_during,
                all_during ? "another feature kept evaluating" : "nothing evaluated at all",
                creates, releases, live_of(creates, releases),
                g_silence_creates0, g_silence_releases0, live_of(g_silence_creates0, g_silence_releases0));
            say(b);
        }
    }
    else if (g_seen_eval)
    {
        ++g_quiet_obs;
        if (!g_silent && g_quiet_obs >= NEED_QUIET_OBS)
        {
            g_silent = true; ++g_c.silences;
            g_silence_gf0 = g_last_gf; g_silence_t0 = g_last_t;     // it began no later than the last observation that still saw evaluates
            g_silence_creates0 = creates; g_silence_releases0 = releases;
            char b[900];
            g_silence_all0 = g_last_all;
            const bool others = all_evals > g_last_all;
            std::snprintf(b, sizeof b,
                "[MGPU][R300f1] NO SCENE-FEATURE EVALUATE for %llu presents while the game keeps presenting at ~%.0f fps (game frame %llu). "
                "Other features: %s. Features: creates=%llu releases=%llu live=%llu. This line follows the SCENE feature only - the one whose vectors the bridge copies; "
                "it does NOT say DLSS stopped. While it lasts the bridge keeps running NR on frames with no scene vectors, so [R300a3] vector_present reads 0%%.",
                gf - g_last_gf + (g_quiet_obs - 1) * g_step, fps, gf,
                others ? "STILL EVALUATING in the last observation" : "NOTHING evaluated in the last observation either",
                creates, releases, live_of(creates, releases));
            say(b);
        }
    }
    g_last_gf = gf; g_last_evals = evals; g_last_all = all_evals; g_last_t = t;
}

void final_summary()
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_c.silences == 0 && !g_silent) return;
    char b[520];
    std::snprintf(b, sizeof b,
        "[MGPU][R300f1] SCENE-FEATURE SILENCE SUMMARY: %llu stretch(es) without a scene-feature Evaluate, %llu resumed%s. Features at the end: creates=%llu releases=%llu live=%llu.",
        g_c.silences, g_c.resumed, g_silent ? " - THE RUN ENDED WHILE NO SCENE-FEATURE EVALUATE WAS ARRIVING" : "",
        g_last_creates, g_last_releases, live_of(g_last_creates, g_last_releases));
    say(b);
}

void set_clock_for_test(double (*fn)()) { g_clock_fn = fn; }
void set_step_for_test(unsigned long long frames) { g_step = frames ? frames : 1ull; }
counters snapshot() { std::lock_guard<std::mutex> lk(g_mx); counters c = g_c; c.silent = g_silent; return c; }
void reset_all()
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_last_gf = g_last_evals = g_last_all = 0; g_last_t = 0.0; g_seen_eval = false; g_quiet_obs = 0; g_silent = false;
    g_silence_gf0 = g_silence_creates0 = g_silence_releases0 = g_silence_all0 = 0; g_silence_t0 = 0.0; g_c = counters{};
    g_last_creates = g_last_releases = 0;
}
} // namespace mgpu::feature_diag
