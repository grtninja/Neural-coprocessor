// MGPU Bridge - D1.5a: the read-only latency probe. See latency_probe.hpp.
//
// R&D ONLY. Read only: two documented NvAPI getters, called by us on the
// game's device, results logged raw. Every first call into NvAPI is preceded
// by its own step line, because a wrong function id crashes with no return
// code (roadmap rule, ROADMAP_TENTATIVE C0).
//
// THE NVAPI FACTS USED HERE, AND WHERE THEY CAME FROM.
//   nvapi_interface.h (dlss5\0.3.0 POTENTIAL\nvapi-main):
//     NvAPI_D3D_GetSleepStatus 0xaef96ca1, NvAPI_D3D_GetLatency 0x1a587f9c.
//     Same ids as discovery.cpp and the V21 Reflex block in gpu1_context.cpp.
//   nvapi.h, same folder:
//     NV_GET_SLEEP_STATUS_PARAMS_V1, 136 bytes: version @0, bLowLatencyMode @4,
//       bFsVrr @5, bCplVsyncOn @6, sleepIntervalUs @8, bUseGameSleep @12,
//       bFullscreenIFlip @13, fgMultiplier @14, bDfgControl @15,
//       dfgFrameTimeTargetUs @16, rsvd[114]. NvBool is one byte.
//     NV_LATENCY_RESULT_PARAMS_V1, 15400 bytes: version @0, frameReport[64]
//       @8 (240 bytes each, newest at [63]), rsvd[32]. "If not enough frames
//       are valid then all frames are returned with all zeroes." "Requires
//       calling NvAPI_D3D_SetLatencyMarker with incrementing frameID for
//       valid results."
//     MAKE_NVAPI_VERSION(T, n) = sizeof(T) | (n << 16).
//   The layouts below are the same as gpu1_context.cpp's V21 block and carry
//   the same static_asserts.
//
// UNITS. nvapi.h does not state the unit of the NvU64 times. They are logged
// raw beside our QueryPerformanceCounter values and its frequency; the clock
// comparison is done offline from the log, not here.
//
// IF Discovery=1 RUNS ALONGSIDE. D1 swaps cached NvAPI pointers, including
// the ones in nvapi64_impl.dll's own table, so nvapi_QueryInterface may hand
// us D1's call-through thunk. The calls still reach the driver unchanged; D1's
// window lines then count this module as one more caller of GetSleepStatus
// and GetLatency (one each per sample). The step line below names the module
// each resolved address lives in, so that case is visible in the log.

#include <windows.h>
#include <unknwn.h>     // IUnknown. WIN32_LEAN_AND_MEAN (set on the command line) leaves it out of windows.h
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "latency_probe.hpp"
#include "diag.hpp"
#include "journal.hpp"       // D2.0: what / when / why of every NvAPI call (Journal=1)
#include "stall_watch.hpp"   // D2.0: stage marks (StallWatch=N)
#include "mgpu_ini_parser.hpp"

namespace mgpu::latprobe
{
namespace
{
// ---- the key ----
std::atomic<int> g_mode{-1};            // -1 not read yet, 0 off, 1 on

// ---- the game's device and chain (set together, by note_game_chain) ----
std::atomic<void *> g_dev{nullptr};
std::atomic<void *> g_chain{nullptr};

// ---- NvAPI ----
constexpr unsigned ID_GET_STATUS = 0xAEF96CA1u;   // NvAPI_D3D_GetSleepStatus
constexpr unsigned ID_LATENCY    = 0x1A587F9Cu;   // NvAPI_D3D_GetLatency

typedef void *(__cdecl *pf_qi)(unsigned int);
typedef int   (__cdecl *pf_dev_p)(IUnknown *, void *);

pf_dev_p g_get = nullptr;
pf_dev_p g_lat = nullptr;
bool     g_resolved = false;     // resolution finished (success or a final failure)
bool     g_said_absent = false;  // "nvapi64.dll not resident" said once

struct sleep_status
{
    unsigned int  version;                  // 0
    unsigned char low_latency;              // 4
    unsigned char fs_vrr;                   // 5
    unsigned char cpl_vsync_on;             // 6
    unsigned char pad7;                     // 7 (compiler padding in nvapi.h)
    unsigned int  sleep_interval_us;        // 8
    unsigned char use_game_sleep;           // 12
    unsigned char fullscreen_iflip;         // 13
    unsigned char fg_multiplier;            // 14
    unsigned char dfg_control;              // 15
    unsigned int  dfg_frame_time_target_us; // 16
    unsigned char rsvd[114];                // 20..133 (padded to 136)
};

struct frame_report
{
    unsigned long long frame_id;                  // 0
    unsigned long long input_sample_time;         // 8
    unsigned long long sim_start;                 // 16
    unsigned long long sim_end;                   // 24
    unsigned long long render_submit_start;       // 32
    unsigned long long render_submit_end;         // 40
    unsigned long long present_start;             // 48
    unsigned long long present_end;               // 56
    unsigned long long driver_start;              // 64
    unsigned long long driver_end;                // 72
    unsigned long long os_render_queue_start;     // 80
    unsigned long long os_render_queue_end;       // 88
    unsigned long long gpu_render_start;          // 96
    unsigned long long gpu_render_end;            // 104
    unsigned int       gpu_active_render_time_us; // 112
    unsigned int       gpu_frame_time_us;         // 116
    unsigned long long camera_constructed_time;   // 120
    unsigned int       cross_adapter_copy_time_us;// 128
    unsigned int       ai_frame_time_us;          // 132
    unsigned char      rsvd[104];                 // 136..239
};
struct latency_params
{
    unsigned int version;         // 0
    // 4 bytes of padding - frame_report is 8-byte aligned
    frame_report frames[64];      // 8
    unsigned char rsvd[32];       // 15368..15399
};

// The numbers are the driver's contract. A failure means a field type above
// is wrong; do not "fix" it by changing the number.
static_assert(sizeof(sleep_status) == 136, "NV_GET_SLEEP_STATUS_PARAMS_V1 must be 136 bytes");
static_assert(offsetof(sleep_status, sleep_interval_us) == 8, "sleepIntervalUs @8");
static_assert(offsetof(sleep_status, use_game_sleep) == 12, "bUseGameSleep @12");
static_assert(offsetof(sleep_status, fg_multiplier) == 14, "fgMultiplier @14");
static_assert(offsetof(sleep_status, dfg_frame_time_target_us) == 16, "dfgFrameTimeTargetUs @16");
static_assert(sizeof(frame_report) == 240, "NV frame report must be 240 bytes");
static_assert(offsetof(frame_report, gpu_active_render_time_us) == 112, "gpuActiveRenderTimeUs @112");
static_assert(offsetof(frame_report, camera_constructed_time) == 120, "cameraConstructedTime @120");
static_assert(offsetof(frame_report, ai_frame_time_us) == 132, "aiFrameTimeUs @132");
static_assert(offsetof(latency_params, frames) == 8, "frameReport @8");
static_assert(sizeof(latency_params) == 15400, "NV_LATENCY_RESULT_PARAMS_V1 must be 15400 bytes");

unsigned ver(size_t sz, unsigned n) { return (unsigned)sz | (n << 16); }

// Static, not on the game thread's stack: 15 KB of driver-written output.
latency_params g_lat_buf;
sleep_status   g_status_buf;

// ---- sampling ----
// THREADS. Presents on the game chain may arrive on more than one thread
// (with frame gen, generated frames may be presented from another thread).
// So the present count, t0 and the ring are atomics, and a sample is taken
// under a try-lock: a present that finds the lock held skips sampling, it
// never waits. Everything below g_sampling is touched only under the lock.
constexpr unsigned           RING = 16u;
constexpr unsigned long long START_AFTER = 300ull;   // presents; NVIDIA asks for 90+ frames
constexpr unsigned           DENSE_SAMPLES = 60u;    // every 2 s ...
constexpr unsigned           MAX_SAMPLES = 150u;     // ... then every 10 s, to this cap
// One ring slot: the present's number (1-based), its QPC and the id of the
// thread that presented it (D1.5a-fix2: shows whether frame gen presents
// from a second thread). The writer clears the number, stores QPC and
// thread, then stores the number; a reader that sees the same non-zero
// number before and after reading them has a matching set.
struct ring_ent
{
    std::atomic<unsigned long long> n{0};
    std::atomic<long long>          qpc{0};
    std::atomic<unsigned long>      tid{0};
};
ring_ent                        g_ring[RING];
std::atomic<unsigned long long> g_presents{0};
LARGE_INTEGER                   g_freq{};
std::atomic<long long>          g_t0{0};       // QPC at the first counted present
std::atomic_flag                g_sampling = ATOMIC_FLAG_INIT;
std::atomic<unsigned>           g_samples_done{0};   // read outside the lock for the cap check
long long          g_last_sample = 0;  // QPC of the last sample attempt
unsigned           g_samples = 0;
unsigned long long g_prev_presents = 0;
unsigned long long g_prev_newest_id = 0;
bool               g_said_cap = false;

long long qpc()
{
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

double t_s(long long c)
{
    const long long t0 = g_t0.load(std::memory_order_relaxed);
    if (t0 == 0 || g_freq.QuadPart == 0) return 0.0;
    return (double)(c - t0) / (double)g_freq.QuadPart;
}

void mod_name_of(const void *addr, char *out, size_t n)
{
    if (n == 0) return;
    out[0] = '\0';
    HMODULE hm = nullptr;
    if (addr == nullptr ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &hm) ||
        hm == nullptr)
    {
        snprintf(out, n, "(no module)");
        return;
    }
    char path[MAX_PATH];
    const DWORD len = GetModuleFileNameA(hm, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) { snprintf(out, n, "%p", (void *)hm); return; }
    const char *b = strrchr(path, '\\');
    snprintf(out, n, "%s", b != nullptr ? b + 1 : path);
}

// ---- the key ----
bool ini_path(wchar_t *path, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    wchar_t mod[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    _snwprintf_s(path, n, _TRUNCATE, L"%lsmgpu.ini", mod);
    return true;
}

int read_mode()
{
    wchar_t path[MAX_PATH];
    if (!ini_path(path, MAX_PATH)) return 0;
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return 0;
    static char buf[mgpu::config::MAX_BYTES + 2u];
    const size_t got = fread(buf, 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return 0;
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf, got, "LatencyProbe");
    return (v != nullptr && atoi(v) == 1) ? 1 : 0;
}

void ensure_mode()
{
    if (g_mode.load(std::memory_order_acquire) >= 0) return;
    const int m = read_mode();
    QueryPerformanceFrequency(&g_freq);   // before the mode is visible to on_present
    int expect = -1;
    if (!g_mode.compare_exchange_strong(expect, m, std::memory_order_acq_rel)) return;
    if (m == 1)
        mgpu::diag::warn("[MGPU][LAT] LATENCY PROBE ON (LatencyProbe=1). R&D build, read only: "
                         "calls NvAPI_D3D_GetSleepStatus and NvAPI_D3D_GetLatency on the game's "
                         "device and logs what they return. Swaps nothing, sets nothing, never "
                         "loads nvapi64.dll. Starts after 300 game presents; one sample every 2 s "
                         "for 60 samples, then every 10 s, 150 samples at most.");
}

// One-time resolution. Returns true when both getters are usable.
bool resolve()
{
    if (g_resolved) return g_get != nullptr && g_lat != nullptr;

    HMODULE nv = GetModuleHandleW(L"nvapi64.dll");
    if (nv == nullptr)
    {
        if (!g_said_absent)
        {
            g_said_absent = true;
            mgpu::diag::info("[MGPU][LAT] nvapi64.dll is not resident - nothing in this process "
                             "has loaded it. Not loading it; checking again at each sample.");
        }
        return false;   // not final: try again next sample
    }
    g_resolved = true;

    mgpu::diag::info("[MGPU][LAT] step 1/2: nvapi64.dll resident, GetProcAddress(nvapi_QueryInterface)");
    pf_qi q = (pf_qi)(void *)GetProcAddress(nv, "nvapi_QueryInterface");   // same cast as discovery.cpp
    if (q == nullptr)
    {
        mgpu::diag::warn("[MGPU][LAT] nvapi_QueryInterface not exported - the probe stops here.");
        return false;
    }
    mgpu::diag::info("[MGPU][LAT] step 2/2: QueryInterface for GetSleepStatus (0xAEF96CA1) and "
                     "GetLatency (0x1A587F9C)");
    g_get = (pf_dev_p)q(ID_GET_STATUS);
    g_lat = (pf_dev_p)q(ID_LATENCY);

    char m_get[96], m_lat[96];
    mod_name_of((const void *)g_get, m_get, sizeof m_get);
    mod_name_of((const void *)g_lat, m_lat, sizeof m_lat);
    char l[512];
    snprintf(l, sizeof l,
             "[MGPU][LAT] resolved: QueryInterface=%p GetSleepStatus=%p (in %s) GetLatency=%p (in %s)"
             " | QPC frequency %lld per second",
             (void *)q, (void *)g_get, m_get, (void *)g_lat, m_lat, (long long)g_freq.QuadPart);
    mgpu::diag::info(l);
    if (g_get == nullptr || g_lat == nullptr)
        mgpu::diag::warn("[MGPU][LAT] a getter was not returned by QueryInterface - the probe stops here.");
    return g_get != nullptr && g_lat != nullptr;
}

void sample(IUnknown *dev, long long now)
{
    if (!resolve()) return;
    ++g_samples;

    // ---- 1. sleep status ----
    memset(&g_status_buf, 0, sizeof g_status_buf);
    g_status_buf.version = ver(sizeof(sleep_status), 1u);
    if (g_samples == 1u) mgpu::diag::info("[MGPU][LAT] first GetSleepStatus call");
    int rs = 0, rl = 0;
    long long q_before = 0, q_after = 0;
    {
        mgpu::stallwatch::scope sn(mgpu::stallwatch::S_PROBE_NVAPI);   // D2.0
        rs = g_get(dev, &g_status_buf);

        // ---- 2. latency reports ----
        memset(&g_lat_buf, 0, sizeof g_lat_buf);
        g_lat_buf.version = ver(sizeof(latency_params), 1u);
        if (g_samples == 1u) mgpu::diag::info("[MGPU][LAT] first GetLatency call");
        q_before = qpc();
        rl = g_lat(dev, &g_lat_buf);
        q_after = qpc();
    }
    // D2.0 journal: what, when, why of the two calls (queued, not written here).
    // Formatted only when the journal is on: off adds nothing on this thread.
    if (mgpu::journal::on())
    {
        char jw[200];
        snprintf(jw, sizeof jw, "probe s%u: GetSleepStatus(dev=%p) -> %d, GetLatency(dev=%p) -> %d",
                 g_samples, (void *)dev, rs, (void *)dev, rl);
        mgpu::journal::event(jw, "LatencyProbe=1: read-only sample (every 2 s, then every 10 s)");
    }
    mgpu::stallwatch::scope slog(mgpu::stallwatch::S_PROBE_LOG);   // D2.0: the log lines below

    unsigned valid = 0;
    int newest = -1;
    if (rl == 0)
        for (int i = 0; i < 64; ++i)
            if (g_lat_buf.frames[i].frame_id != 0ull) { ++valid; newest = i; }

    const unsigned long long newest_id = (newest >= 0) ? g_lat_buf.frames[newest].frame_id : 0ull;
    const unsigned long long new_ids =
        (g_prev_newest_id != 0ull && newest_id > g_prev_newest_id) ? newest_id - g_prev_newest_id : 0ull;
    const unsigned long long presents_now = g_presents.load(std::memory_order_acquire);
    const unsigned long long presents_since = presents_now - g_prev_presents;
    g_prev_newest_id = newest_id;
    g_prev_presents = presents_now;

    char l[2048];
    snprintf(l, sizeof l,
             "[MGPU][LAT] s%u t=%.2fs P=%llu | status r=%d lowLatency=%u fsVrr=%u cplVsync=%u "
             "sleepIntervalUs=%u useGameSleep=%u iFlip=%u fgMult=%u dfg=%u dfgTargetUs=%u | "
             "latency r=%d valid=%u/64 newest=[%d] id=%llu ids since last sample=%llu, "
             "game presents since last sample=%llu | QPC before call %lld, after %lld",
             g_samples, t_s(now), presents_now, rs,
             (unsigned)g_status_buf.low_latency, (unsigned)g_status_buf.fs_vrr,
             (unsigned)g_status_buf.cpl_vsync_on, g_status_buf.sleep_interval_us,
             (unsigned)g_status_buf.use_game_sleep, (unsigned)g_status_buf.fullscreen_iflip,
             (unsigned)g_status_buf.fg_multiplier, (unsigned)g_status_buf.dfg_control,
             g_status_buf.dfg_frame_time_target_us,
             rl, valid, newest, newest_id, new_ids, presents_since, q_before, q_after);
    mgpu::diag::info(l);

    if (newest >= 0)
    {
        // Every field of the newest report, raw.
        const frame_report &f = g_lat_buf.frames[newest];
        snprintf(l, sizeof l,
                 "[MGPU][LAT] s%u newest report raw: id=%llu inputSample=%llu simStart=%llu "
                 "simEnd=%llu renderSubmitStart=%llu renderSubmitEnd=%llu presentStart=%llu "
                 "presentEnd=%llu driverStart=%llu driverEnd=%llu osQueueStart=%llu "
                 "osQueueEnd=%llu gpuRenderStart=%llu gpuRenderEnd=%llu gpuActiveUs=%u "
                 "gpuFrameUs=%u cameraConstructed=%llu crossAdapterCopyUs=%u aiFrameUs=%u",
                 g_samples, f.frame_id, f.input_sample_time, f.sim_start, f.sim_end,
                 f.render_submit_start, f.render_submit_end, f.present_start, f.present_end,
                 f.driver_start, f.driver_end, f.os_render_queue_start, f.os_render_queue_end,
                 f.gpu_render_start, f.gpu_render_end, f.gpu_active_render_time_us,
                 f.gpu_frame_time_us, f.camera_constructed_time, f.cross_adapter_copy_time_us,
                 f.ai_frame_time_us);
        mgpu::diag::info(l);

        // The 8 newest reports, oldest first: id:simStart/presentStart/presentEnd/gpuRenderEnd/aiUs.
        int w = snprintf(l, sizeof l,
                         "[MGPU][LAT] s%u last reports (id:simStart/presentStart/presentEnd/gpuRenderEnd/aiUs):",
                         g_samples);
        const int first = (newest - 7 > 0) ? newest - 7 : 0;
        for (int i = first; i <= newest && w > 0 && (size_t)w < sizeof l; ++i)
        {
            const frame_report &r = g_lat_buf.frames[i];
            if (r.frame_id == 0ull) continue;
            w += snprintf(l + w, sizeof l - (size_t)w, " %llu:%llu/%llu/%llu/%llu/%u",
                          r.frame_id, r.sim_start, r.present_start, r.present_end,
                          r.gpu_render_end, r.ai_frame_time_us);
        }
        mgpu::diag::info(l);
    }

    // Our own present timestamps on the game chain, oldest first: raw QPC,
    // taken in ReShade's present event (before the game's Present runs).
    {
        int w = snprintf(l, sizeof l, "[MGPU][LAT] s%u our last presents (P:QPC:thread):", g_samples);
        // Each slot carries its own present number, so a slot another thread
        // is rewriting is skipped rather than printed under the wrong number.
        const unsigned long long top = g_presents.load(std::memory_order_acquire);
        const unsigned long long n = top < RING ? top : RING;
        for (unsigned long long k = top - n; k < top && w > 0 && (size_t)w < sizeof l; ++k)
        {
            const ring_ent &e = g_ring[k % RING];
            const unsigned long long n1 = e.n.load(std::memory_order_acquire);
            const long long q = e.qpc.load(std::memory_order_acquire);
            const unsigned long t = e.tid.load(std::memory_order_acquire);
            const unsigned long long n2 = e.n.load(std::memory_order_acquire);
            if (n1 != k + 1ull || n2 != n1) continue;
            w += snprintf(l + w, sizeof l - (size_t)w, " %llu:%lld:%lu", n1, q, t);
        }
        mgpu::diag::info(l);
    }
}
}   // namespace

void note_game_chain(void *device, void *chain)
{
    ensure_mode();
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    if (device == nullptr || chain == nullptr) return;
    void *prev = g_chain.load(std::memory_order_acquire);
    g_chain.store(nullptr, std::memory_order_release);   // never a new chain with the old device
    g_dev.store(device, std::memory_order_release);
    g_chain.store(chain, std::memory_order_release);
    char l[256];
    snprintf(l, sizeof l, "[MGPU][LAT] game chain %p device %p%s", chain, device,
             (prev != nullptr && prev != chain) ? " (replaces the previous game chain)" : "");
    mgpu::diag::info(l);
}

void on_chain_destroyed(void *chain)
{
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    if (chain == nullptr) return;
    void *expect = chain;
    if (g_chain.compare_exchange_strong(expect, nullptr, std::memory_order_acq_rel))
    {
        char l[160];
        snprintf(l, sizeof l, "[MGPU][LAT] game chain %p destroyed - sampling paused until the next "
                              "game chain", chain);
        mgpu::diag::info(l);
    }
}

void on_present(void *chain)
{
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    if (chain == nullptr || chain != g_chain.load(std::memory_order_acquire)) return;

    const long long now = qpc();
    long long zero = 0;
    g_t0.compare_exchange_strong(zero, now, std::memory_order_relaxed);
    const unsigned long long num = g_presents.fetch_add(1ull, std::memory_order_acq_rel) + 1ull;
    ring_ent &e = g_ring[(num - 1ull) % RING];
    e.n.store(0ull, std::memory_order_release);    // slot being rewritten
    e.qpc.store(now, std::memory_order_release);
    e.tid.store((unsigned long)GetCurrentThreadId(), std::memory_order_release);
    e.n.store(num, std::memory_order_release);

    if (num < START_AFTER) return;
    // Past the cap and already said: return without touching the lock.
    if (g_samples_done.load(std::memory_order_relaxed) > MAX_SAMPLES) return;

    // One sampler at a time; anyone else skips.
    if (g_sampling.test_and_set(std::memory_order_acquire)) return;
    if (g_samples >= MAX_SAMPLES)
    {
        if (!g_said_cap)
        {
            g_said_cap = true;
            mgpu::diag::info("[MGPU][LAT] sample cap reached (150) - the probe is silent from here.");
        }
        g_samples_done.store(MAX_SAMPLES + 1u, std::memory_order_relaxed);
    }
    else
    {
        const long long every = g_freq.QuadPart * ((g_samples < DENSE_SAMPLES) ? 2 : 10);
        if (g_last_sample == 0 || now - g_last_sample >= every)
        {
            g_last_sample = now;
            void *dev = g_dev.load(std::memory_order_acquire);
            if (dev != nullptr)
                sample(reinterpret_cast<IUnknown *>(dev), now);
            g_samples_done.store(g_samples, std::memory_order_relaxed);
        }
    }
    g_sampling.clear(std::memory_order_release);
}
}   // namespace mgpu::latprobe
