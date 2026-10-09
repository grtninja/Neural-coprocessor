// MGPU Bridge - D2.0: our own Reflex for case-B titles. See own_reflex.hpp.
//
// NVAPI FACTS (nvapi_interface.h / nvapi.h in dlss5\0.3.0 POTENTIAL\nvapi-main;
// the same ids discovery.cpp and latency_probe.cpp run with):
//   0xAC1CA9E0 NvAPI_D3D_SetSleepMode (IUnknown *, NV_SET_SLEEP_MODE_PARAMS_V1 *)
//     44 bytes: version @0, bLowLatencyMode @4, bLowLatencyBoost @5,
//     minimumIntervalUs @8, bUseMarkersToOptimize @12, bUseMinQueueTime @13.
//   0x852CD1D2 NvAPI_D3D_Sleep (IUnknown *)
//   0xAEF96CA1 NvAPI_D3D_GetSleepStatus (IUnknown *, 136 bytes; bLowLatencyMode
//     @4, sleepIntervalUs @8, bUseGameSleep @12, fgMultiplier @14)
//   0xD9984C05 NvAPI_D3D_SetLatencyMarker (IUnknown *, 88 bytes: version @0,
//     frameID @8, markerType @16). Types: 0 SIMULATION_START, 4 PRESENT_START,
//     5 PRESENT_END.
// nvapi64.dll is found with GetModuleHandleW, never loaded. NvAPI_Initialize
// and NvAPI_Unload are never called.
#include <windows.h>
#include <unknwn.h>     // IUnknown. WIN32_LEAN_AND_MEAN (set on the command line) leaves it out of windows.h
#include <d3d11.h>      // R261: identify a device (QueryInterface only; no call is made on the interface)
#include <d3d12.h>
#include <dxgi.h>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "own_reflex.hpp"
#include "gpu1_context.hpp"   // R261b: device_ref_for_reflex, fetched lazily
#include "journal.hpp"
#include "log_queue.hpp"
#include "stall_watch.hpp"
#include "mgpu_ini_parser.hpp"
#include "calibrator.hpp"     // R281: scene_captures(), installed(); R283: game_ngx(), eval_threads() - loads, lock-free
#include "sl_probe.hpp"       // R281: interposer_resident(), once per game chain (never per present)

namespace mgpu::own_reflex
{
namespace
{
// ---- NvAPI ----
constexpr unsigned ID_SET_SLEEP  = 0xAC1CA9E0u;
constexpr unsigned ID_SLEEP      = 0x852CD1D2u;
constexpr unsigned ID_GET_STATUS = 0xAEF96CA1u;
constexpr unsigned ID_MARKER     = 0xD9984C05u;

typedef void *(__cdecl *pf_qi)(unsigned int);
typedef int   (__cdecl *pf_dev_p)(IUnknown *, void *);
typedef int   (__cdecl *pf_dev)(IUnknown *);

struct set_params
{
    unsigned int  version;            // 0
    unsigned char low_latency;        // 4
    unsigned char boost;              // 5
    unsigned char pad[2];             // 6
    unsigned int  min_interval_us;    // 8
    unsigned char use_markers;        // 12
    unsigned char use_min_queue;      // 13
    unsigned char rsvd[30];           // 14..43
};
struct status_params
{
    unsigned int  version;            // 0
    unsigned char low_latency;        // 4
    unsigned char fs_vrr;             // 5
    unsigned char cpl_vsync;          // 6
    unsigned char pad7;               // 7
    unsigned int  sleep_interval_us;  // 8
    unsigned char use_game_sleep;     // 12
    unsigned char iflip;              // 13
    unsigned char fg_mult;            // 14
    unsigned char dfg;                // 15
    unsigned int  dfg_target_us;      // 16
    unsigned char rsvd[114];          // 20..133
};
struct marker_params
{
    unsigned int       version;       // 0
    unsigned int       pad;           // 4
    unsigned long long frame_id;      // 8
    unsigned int       type;          // 16
    unsigned int       pad2;          // 20
    unsigned long long rsvd0;         // 24
    unsigned char      rsvd[56];      // 32..87
};
static_assert(sizeof(set_params) == 44, "NV_SET_SLEEP_MODE_PARAMS_V1 must be 44 bytes");
static_assert(offsetof(set_params, min_interval_us) == 8, "minimumIntervalUs @8");
static_assert(offsetof(set_params, use_markers) == 12, "bUseMarkersToOptimize @12");
static_assert(sizeof(status_params) == 136, "NV_GET_SLEEP_STATUS_PARAMS_V1 must be 136 bytes");
static_assert(offsetof(status_params, use_game_sleep) == 12, "bUseGameSleep @12");
static_assert(offsetof(status_params, fg_mult) == 14, "fgMultiplier @14");
static_assert(sizeof(marker_params) == 88, "NV_LATENCY_MARKER_PARAMS_V1 must be 88 bytes");
static_assert(offsetof(marker_params, frame_id) == 8, "frameID @8");
static_assert(offsetof(marker_params, type) == 16, "markerType @16");
unsigned ver(size_t sz, unsigned n) { return (unsigned)sz | (n << 16); }

constexpr unsigned M_SIM_START = 0u, M_PRESENT_START = 4u, M_PRESENT_END = 5u;

pf_dev_p g_set = nullptr, g_get = nullptr, g_mark = nullptr;
pf_dev_p g_lat = nullptr;                   // R262: NvAPI_D3D_GetLatency, for the markers decision
pf_dev   g_sleep = nullptr;
bool     g_resolved = false;               // tried (game present thread only)

// ---- keys ----
std::atomic<int> g_mode{-1};               // -1 not read, 0 off, 1 on
// D2.0-4 (operator, 13:37: T2 beat T1): finish is the default; present is the
// option, and the automatic fallback when finish_present does not fire.
std::atomic<int> g_anchor_finish{1};         // 0 present, 1 finish (read on two threads)
std::atomic<unsigned long long> g_finish_mark{0};   // finish_present count at the last check
bool g_fallback_done = false;               // under g_busy
int g_markers_own = -1;                     // R262: -1 auto (decided at the engage), 0 game, 1 own
// R276: recheck 0 by default. The recheck gap paused our Sleep for 60
// frames every 10 s (DW-1: 23 gaps, the driver's sleep interval re-converging
// after each - 8.9 -> 11.0 -> 10.9 ms - and the consumed rate dipping around
// them). A pacing hole every 10 s is a hitch by construction; the frame-gen
// check every 30 frames is a status read and never pauses. The key stays
// for anyone who wants the old behaviour.
int g_settle_ms = 5000, g_recheck_s = 0, g_gap_frames = 60;
int g_pipeline_guard = 1;   // R276: OwnReflexPipelineGuard, absent = 1

// ---- game chain ----
std::atomic<void *> g_dev{nullptr};
std::atomic<void *> g_chain{nullptr};
// R261: which device the calls go to, and our GPU 1 device (AddRef'd, ours to release).
int g_dev_sel = 0;                          // 0 game, 1 gpu1, 2 both
std::atomic<void *> g_gpu1{nullptr};

// ---- state ----
enum st : int { ST_SETTLE = 0, ST_ENGAGED, ST_GAP, ST_ASIDE, ST_DONE };
std::atomic<int> g_state{ST_SETTLE};
std::atomic<bool> g_mode_set_by_us{false};
unsigned g_prior_ll = 0;                     // lowLatency read just before we engaged: what teardown restores
std::atomic<bool> g_busy{false};            // the state machine runs on one thread at a time
LARGE_INTEGER g_freq{};
long long g_t0 = 0;
unsigned long long g_presents = 0;          // game chain, under g_busy
long long g_next_recheck = 0;
unsigned g_gap_left = 0;
std::atomic<unsigned long long> g_frame{1};  // our frame id (own markers), starts at 1, increments
std::atomic<unsigned long long> g_finish_seen{0};

// ---- R281: THE STREAMLINE WAIT ----
// R262 decides the markers once, at the engage, from the driver's frame
// reports; R276 needs the game's own stamps to see the engine's thread shape.
// That read must come BEFORE our first marker (after it the reports carry our
// stamps too), but it does not have to come at 5 s. Dawnwalker (Streamline,
// Unreal), the five launches: the game's stamps were there at the 5 s engage
// on DW-1, DW-4 and DW-R2 (first DLSS evaluate at +2.8 / +3.0 / +3.0 s) and
// the driver held NO frame reports at all on DW-R1 and DW-R3 (first evaluate
// at +11.3 / +11.8 s). On DW-R3 R262 then sent our own markers, R276 never
// ran, and our Sleep paced the wrong thread for the whole session.
// So on a Streamline title whose driver holds no frame reports yet, the
// decision waits for evidence:
//   - the game's scene: the calibrator's count of the game's DLSS evaluates
//     moves -> the settle restarts there (OwnReflexSettleMs, the same key) ->
//     the decision, whatever the reports say then;
//   - no scene: a look at 2 min, then every 5 min (Marcelo, 2026-10-08: a
//     30 s timer lands on menus, as on RE4). A look that finds frame reports
//     decides; one that finds none stays undecided ("I'd rather have some
//     people without Reflex than the race; worst case we set profiles").
//     Once the scene is seen the looks stop: only its settle decides.
// Once decided, it is final for the session, engaged or aside, as before.
// While waiting: no Sleep, no NvAPI call; one counter load per present
// (wait_due). Everything else - every title without Streamline, an explicit
// OwnReflexMarkers, a title whose reports are there at 5 s - decides at 5 s
// exactly as before, with the same calls.
// Present thread, under g_busy; reset with g_t0 on a chain rebuild.
std::atomic<bool> g_sl{false};              // sl.interposer.dll resident when the game chain was noted
bool      g_wait_on = false;                // the wait is running
long long g_wait_t0 = 0;                    // when it started (qpc)
long long g_wait_scene_t0 = 0;              // the game's scene first seen while waiting (0 = not yet)
long long g_wait_next_look = 0;             // the next timed look (qpc)
unsigned  g_wait_looks = 0;                 // timed looks taken that found nothing
constexpr long long WAIT_FIRST_LOOK_S = 120;
constexpr long long WAIT_LOOK_EVERY_S = 300;
// R262's read when wait_hold took it, so the decision never reads twice.
int       g_rd_r = -1;
unsigned  g_rd_rep = 0, g_rd_stamp = 0;
bool      g_rd_have = false;

// ---- R283: THE THREAD GUARD ----
// Tomb Raider DX12, the A/B (mv63, same spot): our Sleep took the frame rate
// from ~61 to ~32 (13.97-14.08 ms inside per call). The game sends no
// markers, so R276 could not see the engine's shape. The NR1 bursts showed
// it: the game's DLSS evaluate runs on one thread (24160), Present on
// another (16004), one after the other every frame. Our Sleep runs on the
// present thread after Present returns, so it holds the render thread's
// next frame, not the start of the game's frame - and the thread that starts
// the game's frames makes no D3D call, so no callback of ours ever runs on
// it: there is nothing to latch. So:
//   - a title running its own NGX without Streamline (the calibrator saw
//     the game resolve NGX's evaluate: game_ngx()) waits for its scene the
//     way R281 waits on Streamline titles, when the driver holds no frame
//     reports at the settle;
//   - at the decision, where the game sends no markers (R262 -> own): the
//     present thread (where our Sleep runs) against the thread(s) of the
//     game's DLSS evaluates. Different -> step aside. The same -> engage.
//     No evaluate seen yet -> engage as before (RE4, Skyrim: no game DLSS);
//   - engaged that way, the first evaluate that shows up later (a title
//     whose NGX goes live after the decision) gets the same comparison on
//     the 30-frame status cadence; different -> step aside and put the mode
//     back;
//   - R283b (review): after a "same thread" verdict the watch goes on, on
//     the same cadence, while we stay engaged on our own markers: the
//     evaluate leaves the present thread, or changes thread at all (a menu
//     rendered on the present thread, the game on a render thread) -> step
//     aside and put the mode back. One way: it never re-engages;
//   - the decision uses the latest evaluate's thread (first / changes are
//     printed for the reader and feed the watch), so a chain rebuilt with a
//     new render+present thread is judged on the new thread once it
//     evaluates. R283c: no per-chain baseline - with one, a Tomb Raider
//     chain rebuilt during a loading pause read "none seen" and engaged
//     blind (second review, RV5); without it the old chain's evaluate
//     thread still stands for the engine.
// Where the game's markers exist, R276 decides as before. The same key
// turns both off: OwnReflexPipelineGuard=0 engages anyway.
// Present thread, under g_busy; reset on a chain rebuild.
bool          g_thread_known = false;       // the comparison was made (an evaluate had been seen)
unsigned long g_present_tid = 0;            // the present thread at the verdict (printed by the watch)
unsigned      g_changes_at_verdict = 0;     // the calibrator's change count at the verdict
const char *const R283_ASIDE_LINE =
    "[MGPU][D20][RFX][R283] STEPPING ASIDE: the game renders its frames (its DLSS evaluate) on another "
    "thread than the one that presents. Our Sleep runs after Present, so it would hold the render thread's "
    "next frame instead of the start of the game's frame (Tomb Raider DX12: 61 -> 32 fps), and no callback of "
    "ours runs on the thread that starts the game's frames. OwnReflexPipelineGuard=0 engages anyway.";

// ---- counts for the journal window ----
std::atomic<unsigned long long> c_sleep{0}, c_sleep_fail{0}, c_status{0}, c_marker{0},
                                c_marker_fail{0};
std::atomic<long long> c_sleep_qpc{0};      // QPC spent inside our Sleep calls
long long g_next_window = 0;
std::atomic<int> g_last_sleep_r{0}, g_last_marker_r{0};

long long qpc() { LARGE_INTEGER c{}; QueryPerformanceCounter(&c); return c.QuadPart; }

const char *state_name(int s)
{
    switch (s)
    {
    case ST_SETTLE:  return "settle";
    case ST_ENGAGED: return "engaged";
    case ST_GAP:     return "gap";
    case ST_ASIDE:   return "aside";
    default:         return "done";
    }
}

void jl(const char *what, const char *why) { mgpu::journal::event(what, why); }
void say(const char *line) { mgpu::lq::post(mgpu::lq::TO_INFO, line); }

// ---- keys ----
bool addon_dir(wchar_t *out, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    const DWORD len = GetModuleFileNameW(hm, out, (DWORD)n);
    if (len == 0 || len >= n) return false;
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    return true;
}

int read_keys()
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!addon_dir(dir, MAX_PATH)) return 0;
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", dir);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return 0;
    static char buf[mgpu::config::MAX_BYTES + 2u];
    const size_t got = fread(buf, 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return 0;
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf, got, "OwnReflex");
    if (v == nullptr || atoi(v) != 1) return 0;
    const char *old = mgpu::config::find(buf, got, "Reflex");
    if (old != nullptr && atoi(old) != 0)
    {
        mgpu::lq::post(mgpu::lq::TO_WARN, "[MGPU][D20][RFX] OwnReflex=1 REFUSED: the old Reflex key is "
                       "non-zero. Two of our own Reflex setters must never run together; set Reflex=0.");
        jl("OwnReflex refused", "OwnReflex=1 with the old Reflex key non-zero");
        return 0;
    }
    if ((v = mgpu::config::find(buf, got, "OwnReflexAnchor")) != nullptr)
        g_anchor_finish.store((strncmp(v, "present", 7) == 0) ? 0 : 1);   // absent or anything else: finish
    if ((v = mgpu::config::find(buf, got, "OwnReflexPipelineGuard")) != nullptr)   // R276
        g_pipeline_guard = (*v == '0') ? 0 : 1;
    if ((v = mgpu::config::find(buf, got, "OwnReflexMarkers")) != nullptr)   // R262: absent = auto
        g_markers_own = (strncmp(v, "own", 3) == 0) ? 1 : (strncmp(v, "game", 4) == 0) ? 0 : -1;
    if ((v = mgpu::config::find(buf, got, "OwnReflexDevice")) != nullptr)   // R261
        g_dev_sel = (strncmp(v, "both", 4) == 0) ? 2 : (strncmp(v, "gpu1", 4) == 0) ? 1 : 0;
    if ((v = mgpu::config::find(buf, got, "OwnReflexSettleMs")) != nullptr)
        { const int x = atoi(v); g_settle_ms = x < 500 ? 500 : (x > 60000 ? 60000 : x); }
    if ((v = mgpu::config::find(buf, got, "OwnReflexRecheckSec")) != nullptr)
        { const int x = atoi(v); g_recheck_s = x < 0 ? 0 : (x > 600 ? 600 : x); }
    if ((v = mgpu::config::find(buf, got, "OwnReflexGapFrames")) != nullptr)
        { const int x = atoi(v); g_gap_frames = x < 5 ? 5 : (x > 2000 ? 2000 : x); }
    return 1;
}

// ---- NvAPI, every call journaled by kind ----
bool resolve()
{
    if (g_resolved) return g_set != nullptr && g_sleep != nullptr && g_get != nullptr;
    HMODULE nv = GetModuleHandleW(L"nvapi64.dll");
    if (nv == nullptr) return false;           // not resident yet: try again next present
    g_resolved = true;
    pf_qi q = (pf_qi)(void *)GetProcAddress(nv, "nvapi_QueryInterface");
    if (q == nullptr)
    {
        jl("resolve: nvapi_QueryInterface not exported", "OwnReflex needs NvAPI; it stays off");
        return false;
    }
    g_set   = (pf_dev_p)q(ID_SET_SLEEP);
    g_sleep = (pf_dev)  q(ID_SLEEP);
    g_get   = (pf_dev_p)q(ID_GET_STATUS);
    g_mark  = (pf_dev_p)q(ID_MARKER);
    g_lat   = (pf_dev_p)q(0x1A587F9Cu);   // R262: NvAPI_D3D_GetLatency (latency_probe.cpp's id and layout)
    char w[300];
    snprintf(w, sizeof w, "resolve: nvapi_QueryInterface -> SetSleepMode=%p Sleep=%p GetSleepStatus=%p "
             "SetLatencyMarker=%p", (void *)g_set, (void *)g_sleep, (void *)g_get, (void *)g_mark);
    jl(w, "OwnReflex=1: first game present after the settle start");
    return g_set != nullptr && g_sleep != nullptr && g_get != nullptr;
}

bool read_status(IUnknown *dev, status_params &out, const char *why)
{
    mgpu::stallwatch::scope s(mgpu::stallwatch::S_RFX_STATUS);
    memset(&out, 0, sizeof out);
    out.version = ver(sizeof(status_params), 1u);
    const int r = g_get(dev, &out);
    c_status.fetch_add(1, std::memory_order_relaxed);
    static int last_r = 1234, last_ugs = -1, last_fg = -1, last_ll = -1;
    if (r != last_r || out.use_game_sleep != last_ugs || out.fg_mult != last_fg ||
        out.low_latency != last_ll)
    {
        char w[300];
        snprintf(w, sizeof w, "GetSleepStatus(dev=%p) -> %d | lowLatency=%u useGameSleep=%u fgMult=%u "
                 "sleepIntervalUs=%u (logged on change)", (void *)dev, r, out.low_latency,
                 out.use_game_sleep, out.fg_mult, out.sleep_interval_us);
        jl(w, why);
        last_r = r; last_ugs = out.use_game_sleep; last_fg = out.fg_mult; last_ll = out.low_latency;
    }
    return r == 0;
}

// R261: who is this device. QueryInterface only; the interface is released at once.
void describe_device(IUnknown *dev, char *out, size_t n)
{
    if (dev == nullptr) { snprintf(out, n, "none"); return; }
    ID3D12Device *d12 = nullptr; ID3D11Device *d11 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d12))) && d12 != nullptr)
    {
        const LUID l = d12->GetAdapterLuid();
        snprintf(out, n, "%p D3D12 adapter luid=0x%08X-0x%08X", (void *)dev, (unsigned)l.HighPart, (unsigned)l.LowPart);
        d12->Release();
        return;
    }
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d11))) && d11 != nullptr)
    {
        LUID l{}; bool got = false;
        IDXGIDevice *dx = nullptr; IDXGIAdapter *ad = nullptr;
        if (SUCCEEDED(d11->QueryInterface(IID_PPV_ARGS(&dx))) && dx != nullptr)
        {
            if (SUCCEEDED(dx->GetAdapter(&ad)) && ad != nullptr)
            {
                DXGI_ADAPTER_DESC d{};
                if (SUCCEEDED(ad->GetDesc(&d))) { l = d.AdapterLuid; got = true; }
                ad->Release();
            }
            dx->Release();
        }
        snprintf(out, n, "%p D3D11 fl=0x%04X adapter luid=%s0x%08X-0x%08X", (void *)dev,
                 (unsigned)d11->GetFeatureLevel(), got ? "" : "(not read) ", (unsigned)l.HighPart, (unsigned)l.LowPart);
        d11->Release();
        return;
    }
    snprintf(out, n, "%p (neither D3D11 nor D3D12)", (void *)dev);
}

// R261: the devices the calls go to, in order. game | gpu1 | both.
unsigned call_devices(IUnknown *game, IUnknown **out2)
{
    IUnknown *g1 = reinterpret_cast<IUnknown *>(g_gpu1.load(std::memory_order_acquire));
    unsigned n = 0;
    if (g_dev_sel == 1) { if (g1 != nullptr) out2[n++] = g1; }   // R261b: never the game's device in gpu1 mode
    else { if (game != nullptr) out2[n++] = game; if (g_dev_sel == 2 && g1 != nullptr) out2[n++] = g1; }
    return n;
}

// R262: markers auto. The game's own markers, if it sends any, show up in
// NvAPI_D3D_GetLatency's frame reports (simStartTime / presentStartTime
// non-zero). Read once at the engage, on the game's device: reports with
// stamps -> ride the game's markers (0); none -> send our own (1). If the
// call cannot be made, 0: never two marker streams on one device.
// Layout: NV_LATENCY_RESULT_PARAMS_V1, 15400 bytes, frameReport[64] @8,
// 240 bytes each: frameID @0, simStartTime @16, presentStartTime @48.
// R276. THE PIPELINE SHAPE. Our Sleep runs on the thread that called Present
// (anchor=finish: right after Present returns). NvAPI's Sleep is meant for the
// thread that STARTS the next frame - in a single-threaded renderer (Skyrim)
// that is the same thread; in an engine with a separate render/RHI thread
// (Unreal: Dawnwalker) it is not, and sleeping the thread that submits GPU
// work holds the GPU's next frame back instead of the CPU's. DW-1 vs DW-2
// (2026-10-07, same scene): equal average frame rate, ours engaged 14 ms
// inside Sleep per frame with the driver's interval climbing, and the run
// without our Sleep felt smoother - the hallmark of pacing the wrong thread.
// The game's own markers say which shape the engine has: if the simulation of
// frame n+1 starts BEFORE frame n's Present starts, the frame-start thread is
// not the present thread. Read from the same GetLatency buffer R262 reads,
// at the engage. Overlap on most pairs -> step aside and say so; the game's
// own Reflex setting is the right loop for that engine. No marker stamps
// (a title without a contract) -> unknown shape -> engage as before: those
// are the single-threaded titles this feature exists for.
// Layout: frameReport[64] @8, 240 bytes each: frameID @0, simStartTime @16,
// presentStartTime @48.
int pipeline_overlap(IUnknown *dev, unsigned *pairs_out, unsigned *overlap_out)
{
    static unsigned char buf[15400];
    *pairs_out = 0; *overlap_out = 0;
    if (g_lat == nullptr) return -1;
    memset(buf, 0, sizeof buf);
    *reinterpret_cast<unsigned *>(buf) = ver(sizeof buf, 1u);
    const int r = g_lat(dev, buf);
    if (r != 0) return r;
    unsigned long long id[64], sim[64], pre[64];
    for (unsigned i = 0; i < 64u; ++i)
    {
        const unsigned char *f = buf + 8 + i * 240u;
        id[i]  = *reinterpret_cast<const unsigned long long *>(f);
        sim[i] = *reinterpret_cast<const unsigned long long *>(f + 16);
        pre[i] = *reinterpret_cast<const unsigned long long *>(f + 48);
    }
    for (unsigned i = 0; i < 64u; ++i)
        for (unsigned j = 0; j < 64u; ++j)
        {
            if (id[i] == 0ull || id[j] != id[i] + 1ull) continue;
            if (sim[j] == 0ull || pre[i] == 0ull) continue;
            ++*pairs_out;
            if (sim[j] < pre[i]) ++*overlap_out;
        }
    return 0;
}

// R281: R262's read, moved verbatim out of decide_markers so the Streamline
// wait can look at the same counts without a second call. reports = frame
// reports with a frame id; stamped = with a simulation or present stamp.
// The caller checks g_lat.
int report_counts(IUnknown *dev, unsigned *reports_out, unsigned *stamped_out)
{
    static unsigned char buf[15400];
    memset(buf, 0, sizeof buf);
    *reinterpret_cast<unsigned *>(buf) = ver(sizeof buf, 1u);
    const int r = g_lat(dev, buf);
    unsigned reports = 0, stamped = 0;
    if (r == 0)
        for (unsigned i = 0; i < 64u; ++i)
        {
            const unsigned char *f = buf + 8 + i * 240u;
            const unsigned long long id = *reinterpret_cast<const unsigned long long *>(f);
            const unsigned long long sim = *reinterpret_cast<const unsigned long long *>(f + 16);
            const unsigned long long pre = *reinterpret_cast<const unsigned long long *>(f + 48);
            if (id != 0ull) ++reports;
            if (sim != 0ull || pre != 0ull) ++stamped;
        }
    *reports_out = reports;
    *stamped_out = stamped;
    return r;
}

// R262's decision and its lines, from counts already read (R281: unchanged).
int decide_markers_from(IUnknown *dev, int r, unsigned reports, unsigned stamped)
{
    const int own = (r == 0 && stamped == 0) ? 1 : 0;
    char w[300];
    snprintf(w, sizeof w, "markers auto -> %s: GetLatency(dev=%p) -> %d, frame reports=%u with marker stamps=%u",
             own ? "own (the game sends no markers)" : "game (the game sends markers, or the read failed)",
             (void *)dev, r, reports, stamped);
    jl(w, "R262: OwnReflexMarkers absent - decided from the driver's frame reports at the engage");
    char l[400];
    snprintf(l, sizeof l, "[MGPU][D20][RFX][R262] %s", w);
    say(l);
    return own;
}

int decide_markers(IUnknown *dev)
{
    if (g_lat == nullptr) { jl("markers auto -> game: GetLatency not resolved", "R262"); return 0; }
    unsigned reports = 0, stamped = 0;
    const int r = report_counts(dev, &reports, &stamped);
    return decide_markers_from(dev, r, reports, stamped);
}

// ---- R281: the wait (see the state block above) ----

// Markers on auto, GetLatency resolved, the calibrator's evaluate tap live
// (it is what says the scene started), and a Streamline title - or, R283, a
// title running its own NGX (the calibrator saw the game resolve NGX's
// evaluate; read here, at the decision). Any of them missing -> the decision
// at 5 s, as before.
bool wait_applies()
{
    return g_markers_own < 0 && g_lat != nullptr && mgpu::calibrator::installed() &&
           (g_sl.load(std::memory_order_relaxed) || mgpu::calibrator::game_ngx());
}

bool wait_scene_settled(long long now)
{
    return g_wait_scene_t0 != 0 && now - g_wait_scene_t0 >= g_freq.QuadPart * g_settle_ms / 1000;
}

// Every present while waiting: is the decision due? One counter load until
// the scene is seen, then compares only. No NvAPI call. Once the scene is
// seen the timed looks stop: only its settle decides (review finding: a
// look landing inside that settle could decide on a handful of reports,
// too few for R276's 8 pairs - the DW-R3 outcome again).
bool wait_due(long long now)
{
    if (g_wait_scene_t0 == 0 && mgpu::calibrator::scene_captures() != 0ull)
    {
        g_wait_scene_t0 = now;
        char l[300];
        snprintf(l, sizeof l, "[MGPU][D20][RFX][R281] the game's scene started %.1f s into the wait (its first "
                 "DLSS evaluate): the settle restarts here, the decision comes in %d ms.",
                 (double)(now - g_wait_t0) / (double)g_freq.QuadPart, g_settle_ms);
        say(l);
        jl(l + 17, "R281: the calibrator's count of the game's DLSS evaluates moved");
    }
    return wait_scene_settled(now) || (g_wait_scene_t0 == 0 && now >= g_wait_next_look);
}

void wait_end(long long now, const char *why)
{
    g_wait_on = false;
    char l[400];
    snprintf(l, sizeof l, "[MGPU][D20][RFX][R281] the wait ends after %.1f s and %u empty look(s): %s. "
             "The engage decision runs now.",
             (double)(now - g_wait_t0) / (double)g_freq.QuadPart, g_wait_looks, why);
    say(l);
    jl(l + 17, "R281");
}

// At the engage decision, after the status checks passed (nobody sleeps,
// frame gen off). True = the decision waits; nothing else happens this
// present. The read is R262's own read, kept in g_rd_* so the decision that
// follows never reads twice. The decision the scene triggers never waits.
bool wait_hold(IUnknown *dev, long long now)
{
    g_rd_have = false;
    if (g_wait_on && wait_scene_settled(now))
    {
        wait_end(now, "the game's scene started and the settle passed");
        return false;   // decide_markers reads the reports fresh
    }
    if (!g_wait_on && !wait_applies()) return false;   // as before: R262 reads at 5 s
    g_rd_r = report_counts(dev, &g_rd_rep, &g_rd_stamp);
    g_rd_have = true;
    if (g_rd_r != 0 || g_rd_rep != 0u)
    {
        if (g_wait_on)
        {
            char why[160];
            snprintf(why, sizeof why, "a timed look found GetLatency -> %d, frame reports=%u with marker stamps=%u",
                     g_rd_r, g_rd_rep, g_rd_stamp);
            wait_end(now, why);
        }
        return false;   // reports there (or the read failed): R262 decides from this read
    }
    char l[700];
    if (!g_wait_on)
    {
        g_wait_on = true;
        g_wait_t0 = now;
        g_wait_scene_t0 = 0;
        g_wait_looks = 0;
        g_wait_next_look = now + g_freq.QuadPart * WAIT_FIRST_LOOK_S;
        snprintf(l, sizeof l, "[MGPU][D20][RFX][R281] WAITING: %s, and the driver holds no frame "
                 "reports from the game yet (GetLatency -> 0, frame reports=0). The markers decision waits for the "
                 "game's scene (its first DLSS evaluate, then the settle) or a look at %lld s, then every %lld s. "
                 "Until then our Sleep stays off and no NvAPI call is made.",
                 g_sl.load(std::memory_order_relaxed) ? "a Streamline title"
                     : "a title running its own NGX without Streamline (R283: the calibrator saw the game resolve NGX's evaluate)",
                 WAIT_FIRST_LOOK_S, WAIT_LOOK_EVERY_S);
    }
    else
    {
        ++g_wait_looks;
        g_wait_next_look = now + g_freq.QuadPart * WAIT_LOOK_EVERY_S;
        snprintf(l, sizeof l, "[MGPU][D20][RFX][R281] look %u at %.0f s: no frame reports and no scene yet - "
                 "still undecided, next look in %lld s.",
                 g_wait_looks, (double)(now - g_wait_t0) / (double)g_freq.QuadPart, WAIT_LOOK_EVERY_S);
    }
    say(l);
    jl(l + 17, "R281: no frame reports - the markers decision must come before our first marker");
    g_rd_have = false;
    return true;
}

// R283: the present thread (this call runs on it, in the present event; our
// Sleep runs on it after Present returns) against the thread of the game's
// latest DLSS evaluate (the calibrator's captures). true = step aside.
// None seen yet -> false: at the decision one line says so, with its inputs
// (the engage is as before); while engaged (late) nothing is written until
// one shows up. Sets g_thread_known and the watch's reference once compared.
bool thread_guard_blocks(bool late)
{
    unsigned long ef = 0ul, el = 0ul; unsigned ch = 0u;
    mgpu::calibrator::eval_threads(&ef, &el, &ch);
    const unsigned long long caps = mgpu::calibrator::scene_captures();
    const unsigned long pt = (unsigned long)GetCurrentThreadId();
    char l[700];
    if (el == 0ul)
    {
        if (late) return false;
        snprintf(l, sizeof l, "[MGPU][D20][RFX][R283] threads: present %lu (our Sleep) | the game's DLSS evaluate: "
                 "none seen (calibrator installed=%s, the game runs its own NGX=%s, evaluates read %llu) -> the "
                 "shape is unknown, engaging as before; the first evaluate, if one comes, is checked then.",
                 pt, mgpu::calibrator::installed() ? "yes" : "no",
                 mgpu::calibrator::game_ngx() ? "yes" : "no", caps);
        say(l);
        jl(l + 17, "R283: at the engage decision, the game sends no markers");
        return false;
    }
    g_thread_known = true;
    g_present_tid = pt;
    g_changes_at_verdict = ch;
    const bool differs = (el != pt);
    snprintf(l, sizeof l, "[MGPU][D20][RFX][R283] threads: present %lu (our Sleep) | the game's DLSS evaluate: "
             "latest %lu (it decides) | first %lu, %u change(s) so far -> %s", pt, el, ef, ch,
             differs ? "the game renders on another thread: stepping aside"
                     : late ? "the same thread renders and presents: staying engaged, watched every 30 frames"
                            : "the same thread renders and presents: engaging as before, watched every 30 frames");
    say(l);
    jl(l + 17, late ? "R283: the late check while engaged - the first game DLSS evaluate came after the engage"
                    : "R283: at the engage decision, the game sends no markers");
    return differs;
}

// R283b: the watch after a "same thread" verdict, every 30 frames while
// engaged on our own markers. true = the evaluate is no longer on the thread
// that presents, or it changed thread since the verdict -> step aside.
// Three loads and one thread-id read; a line only when it fires.
bool thread_moved()
{
    unsigned long ef = 0ul, el = 0ul; unsigned ch = 0u;
    mgpu::calibrator::eval_threads(&ef, &el, &ch);
    const unsigned long pt = (unsigned long)GetCurrentThreadId();
    if (el == pt && ch == g_changes_at_verdict) return false;
    char l[700];
    if (el != pt)
        snprintf(l, sizeof l, "[MGPU][D20][RFX][R283] threads moved: present %lu (%lu at the verdict) | the game's "
                 "DLSS evaluate: latest %lu, %u change(s) (%u at the verdict) -> it renders on another thread than "
                 "the one that presents: stepping aside", pt, g_present_tid, el, ch, g_changes_at_verdict);
    else
        snprintf(l, sizeof l, "[MGPU][D20][RFX][R283] threads moved: present %lu (%lu at the verdict) | the game's "
                 "DLSS evaluate: latest %lu (the present thread now), but it changed thread since the verdict "
                 "(%u -> %u change(s)) -> it does not stay on the thread that presents: stepping aside",
                 pt, g_present_tid, el, g_changes_at_verdict, ch);
    say(l);
    jl(l + 17, "R283b: the watch after a same-thread verdict, every 30 frames while engaged on our own markers");
    return true;
}

int set_mode_one(IUnknown *dev, unsigned ll, unsigned markers, const char *why)
{
    mgpu::stallwatch::scope s(mgpu::stallwatch::S_RFX_SETMODE);
    set_params p{};
    p.version = ver(sizeof(set_params), 1u);
    p.low_latency = (unsigned char)ll;
    p.use_markers = (unsigned char)markers;
    const int r = g_set(dev, &p);
    char w[240];
    snprintf(w, sizeof w, "SetSleepMode(dev=%p, lowLatency=%u, boost=0, minIntervalUs=0, markers=%u) -> %d",
             (void *)dev, ll, markers, r);
    jl(w, why);
    return r;
}

void do_sleep_one(IUnknown *dev)
{
    mgpu::stallwatch::scope s(mgpu::stallwatch::S_RFX_SLEEP);
    const long long a = qpc();
    const int r = g_sleep(dev);
    c_sleep_qpc.fetch_add(qpc() - a, std::memory_order_relaxed);
    const unsigned long long n = c_sleep.fetch_add(1, std::memory_order_relaxed);
    if (r != 0) c_sleep_fail.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || r != g_last_sleep_r.exchange(r, std::memory_order_relaxed))
    {
        char w[160];
        snprintf(w, sizeof w, "NvAPI_D3D_Sleep(dev=%p) -> %d (%s)", (void *)dev, r,
                 n == 0 ? "first call" : "result changed");
        jl(w, g_anchor_finish.load() ? "OwnReflex engaged, anchor=finish: once per frame after the game's "
                                "Present returns" : "OwnReflex engaged, anchor=present: once per frame "
                                "before the game's Present");
    }
}

void marker_one(IUnknown *dev, unsigned type, unsigned long long fid)
{
    if (g_mark == nullptr) return;
    mgpu::stallwatch::scope s(mgpu::stallwatch::S_RFX_MARKER);
    marker_params p{};
    p.version = ver(sizeof(marker_params), 1u);
    p.frame_id = fid;
    p.type = type;
    const int r = g_mark(dev, &p);
    const unsigned long long n = c_marker.fetch_add(1, std::memory_order_relaxed);
    if (r != 0) c_marker_fail.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || r != g_last_marker_r.exchange(r, std::memory_order_relaxed))
    {
        char w[180];
        snprintf(w, sizeof w, "SetLatencyMarker(dev=%p, type=%u, frameID=%llu) -> %d (%s)", (void *)dev,
                 type, fid, r, n == 0 ? "first call" : "result changed");
        jl(w, "OwnReflexMarkers=own: our markers, frame ids ours");
    }
}

void window_line(long long now)
{
    if (now < g_next_window) return;
    g_next_window = now + g_freq.QuadPart * 10;
    const unsigned long long sl = c_sleep.exchange(0), slf = c_sleep_fail.exchange(0),
                             stt = c_status.exchange(0), mk = c_marker.exchange(0),
                             mkf = c_marker_fail.exchange(0);
    const long long sq = c_sleep_qpc.exchange(0);
    const double avg_us = sl ? (double)sq * 1e6 / (double)g_freq.QuadPart / (double)sl : 0.0;
    char w[400];
    snprintf(w, sizeof w, "window 10 s: state=%s anchor=%s markers=%s | Sleep calls=%llu (failed %llu, "
             "avg %.0f us inside) | SetLatencyMarker=%llu (failed %llu) | GetSleepStatus=%llu | "
             "finish_present seen=%llu", state_name(g_state.load()), g_anchor_finish.load() ? "finish" : "present",
             g_markers_own < 0 ? "auto" : g_markers_own ? "own" : "game", sl, slf, avg_us, mk, mkf, stt,
             g_finish_seen.load(std::memory_order_relaxed));
    jl(w, "OwnReflex window counts");
    char l[460];
    snprintf(l, sizeof l, "[MGPU][D20][RFX] %s", w);
    say(l);
}

// D2.0-4: finish_present is the default anchor. If it does not fire on the
// game chain while presents do, our Sleep would never run: fall back to the
// present anchor, once, journaled. Called under g_busy.
void fallback_to_present(const char *why)
{
    if (g_fallback_done || g_anchor_finish.load() == 0) return;
    g_fallback_done = true;
    g_anchor_finish.store(0);
    jl("ANCHOR FALLBACK finish -> present", why);
    char l[300];
    snprintf(l, sizeof l, "[MGPU][D20][RFX] ANCHOR FALLBACK: finish_present is not firing on the game chain "
                          "(%s). Our Sleep moves to the present event (OwnReflexAnchor=present).", why);
    mgpu::lq::post(mgpu::lq::TO_WARN, l);
}

// R261: the fan-out. `dev` is the game's device (the chain identity); the
// key decides which device(s) the call actually goes to. Returns the first
// device's result (the one the state machine acts on).
int set_mode(IUnknown *dev, unsigned ll, unsigned markers, const char *why)
{
    IUnknown *ds[2]; const unsigned n = call_devices(dev, ds);
    int r = -1;
    for (unsigned i = 0; i < n; ++i) { const int ri = set_mode_one(ds[i], ll, markers, why); if (i == 0) r = ri; }
    return r;
}
void do_sleep(IUnknown *dev)
{
    IUnknown *ds[2]; const unsigned n = call_devices(dev, ds);
    for (unsigned i = 0; i < n; ++i) do_sleep_one(ds[i]);
}
void marker(IUnknown *dev, unsigned type, unsigned long long fid)
{
    IUnknown *ds[2]; const unsigned n = call_devices(dev, ds);
    for (unsigned i = 0; i < n; ++i) marker_one(ds[i], type, fid);
}

void step_aside(IUnknown *dev, const status_params &s, const char *why, bool restore_mode)
{
    g_state.store(ST_ASIDE, std::memory_order_release);
    char w[300];
    snprintf(w, sizeof w, "DECISION step aside | inputs: useGameSleep=%u fgMult=%u lowLatency=%u",
             s.use_game_sleep, s.fg_mult, s.low_latency);
    jl(w, why);
    char l[400];
    snprintf(l, sizeof l, "[MGPU][D20][RFX] STEP ASIDE: %s (useGameSleep=%u fgMult=%u). Our Sleep stops.",
             why, s.use_game_sleep, s.fg_mult);
    say(l);
    if (restore_mode)
    {
        if (g_mode_set_by_us.exchange(false))
            set_mode(dev, g_prior_ll, 0, "stepping aside with nobody else owning the mode: put "
                                         "lowLatency back to what it was before we engaged");
    }
    else if (g_mode_set_by_us.exchange(false))
        // Someone else sleeps now (the game's loop or frame gen): the mode is
        // theirs from here. We never write it again, teardown included.
        jl("the mode is left to the game from now on", "stepped aside because someone else sleeps: "
           "never act where the game sleeps (teardown will not restore it either)");
}
}   // namespace

void note_game_chain(void *device, void *chain)
{
    if (g_mode.load(std::memory_order_acquire) < 0)
    {
        const int m = read_keys();
        QueryPerformanceFrequency(&g_freq);
        int expect = -1;
        if (g_mode.compare_exchange_strong(expect, m, std::memory_order_acq_rel) && m == 1)
        {
            char l[400];
            snprintf(l, sizeof l, "[MGPU][D20][RFX] OWN REFLEX ON (OwnReflex=1) anchor=%s markers=%s "
                     "settle=%d ms recheck=%d s gap=%d frames. Engages only if nobody sleeps and frame "
                     "gen is off after the settle time; every NvAPI call is journaled.",
                     g_anchor_finish.load() ? "finish" : "present",
                     g_markers_own < 0 ? "auto (R262: decided at the engage)" : g_markers_own ? "own" : "game",
                     g_settle_ms, g_recheck_s, g_gap_frames);
            mgpu::lq::post(mgpu::lq::TO_WARN, l);
            jl(l + 17, "the keys in mgpu.ini");
        }
    }
    if (g_mode.load(std::memory_order_relaxed) != 1 || device == nullptr || chain == nullptr) return;
    // R281: a Streamline title? One module lookup per game chain, here at the
    // chain's creation - never on the present path. Stored before g_chain's
    // release store, so the present thread sees it with the chain.
    g_sl.store(mgpu::slprobe::interposer_resident(), std::memory_order_relaxed);
    g_chain.store(nullptr, std::memory_order_release);
    g_dev.store(device, std::memory_order_release);
    g_chain.store(chain, std::memory_order_release);
    {   // R261: the proof of where Reflex runs
        char d[200]; describe_device(reinterpret_cast<IUnknown *>(device), d, sizeof d);
        char l[400];
        snprintf(l, sizeof l, "[MGPU][D20][RFX][R261] game device: %s | OwnReflexDevice=%s", d,
                 g_dev_sel == 2 ? "both" : g_dev_sel == 1 ? "gpu1" : "game");
        say(l);
        jl(l + 17, "R261: the device(s) the NvAPI_D3D_* calls go to");
    }
}

void note_gpu1_device(void *device_ref)
{
    if (g_mode.load(std::memory_order_relaxed) != 1 || device_ref == nullptr) return;
    if (g_dev_sel == 0)
    {   // not asked for: give the reference back at once
        reinterpret_cast<IUnknown *>(device_ref)->Release();
        return;
    }
    void *old = g_gpu1.exchange(device_ref, std::memory_order_acq_rel);
    if (old != nullptr) reinterpret_cast<IUnknown *>(old)->Release();
    char d[200]; describe_device(reinterpret_cast<IUnknown *>(device_ref), d, sizeof d);
    char l[400];
    snprintf(l, sizeof l, "[MGPU][D20][RFX][R261] OUR GPU 1 device: %s | OwnReflexDevice=%s -> %s", d,
             g_dev_sel == 2 ? "both" : "gpu1",
             g_dev_sel == 2 ? "SetSleepMode, Sleep and the markers go to the game's device AND this one"
                            : "SetSleepMode, Sleep and the markers go to THIS device only");
    say(l);
    jl(l + 17, "R261");
}

int device_mode() { return g_dev_sel; }

void on_present(void *chain)
{
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    if (chain == nullptr || chain != g_chain.load(std::memory_order_acquire)) return;
    if (g_state.load(std::memory_order_acquire) >= ST_ASIDE) return;
    IUnknown *dev = reinterpret_cast<IUnknown *>(g_dev.load(std::memory_order_acquire));
    if (dev == nullptr) return;
    mgpu::stallwatch::scope sc(mgpu::stallwatch::S_RFX);
    if (g_busy.exchange(true, std::memory_order_acquire)) return;   // another present thread has it

    const long long now = qpc();
    if (g_t0 == 0) { g_t0 = now; g_next_window = now + g_freq.QuadPart * 10; }
    ++g_presents;
    int s = g_state.load(std::memory_order_relaxed);
    bool sleep_now = false;

    if (s == ST_SETTLE)
    {
        // R261b: SK-38 - our GPU 1 device did not exist yet when dllmain noted
        // the game chain (it is created later, by the worker), so the hand-over
        // never happened and OwnReflexDevice=gpu1 silently fell back to the
        // game's device. Fetch it here, every present of the settle, until it
        // exists; in gpu1/both mode the engage waits for it and says so.
        if (g_dev_sel != 0 && g_gpu1.load(std::memory_order_acquire) == nullptr)
        {
            void *g1 = nullptr;
            if (mgpu::gpu1::device_ref_for_reflex(&g1) && g1 != nullptr) note_gpu1_device(g1);
        }
        if (g_dev_sel != 0 && g_gpu1.load(std::memory_order_acquire) == nullptr)
        {
            if ((g_presents % 300ull) == 0ull)
                jl("engage waits: OwnReflexDevice=gpu1|both and our GPU 1 device does not exist yet",
                   "R261b: no silent fallback to the game's device");
        }
        else if (g_wait_on && !wait_due(now))
        {
            // R281: waiting for the game's scene or the next look. Nothing is
            // called; wait_due made one counter load.
        }
        else if (resolve() && now - g_t0 >= g_freq.QuadPart * g_settle_ms / 1000 && g_presents >= 60)
        {
            status_params st{};
            if (!read_status(dev, st, "end of the settle time: the engage decision"))
                step_aside(dev, st, "GetSleepStatus failed at the decision: we do not act blind", false);
            else if (st.use_game_sleep != 0)
                step_aside(dev, st, "the game's own loop sleeps (case A): never act here", false);
            else if (st.fg_mult > 1)
                step_aside(dev, st, "frame gen is on: its own Reflex loop owns sleep", false);
            else if (wait_hold(dev, now))
            {
                // R281: a Streamline title with no frame reports yet - the
                // decision waits (started now, or a look that found nothing).
            }
            else
            {
                // D2.0-4: 60+ game presents so far; none of them raised
                // finish_present on this chain -> the default anchor cannot run.
                if (g_anchor_finish.load() && g_finish_seen.load(std::memory_order_relaxed) == 0ull)
                    fallback_to_present("no finish_present in the settle time, 60+ game presents");
                g_finish_mark.store(g_finish_seen.load(std::memory_order_relaxed));
                char w[200];
                snprintf(w, sizeof w, "DECISION engage | inputs: useGameSleep=0 fgMult=%u lowLatency=%u",
                         st.fg_mult, st.low_latency);
                jl(w, "OwnReflex=1 and nobody sleeps with frame gen off (case B)");
                g_prior_ll = st.low_latency;
                if (g_markers_own < 0)   // R262 (R281: from wait_hold's read when it took one - never a second call)
                    g_markers_own = g_rd_have ? decide_markers_from(dev, g_rd_r, g_rd_rep, g_rd_stamp)
                                              : decide_markers(dev);
                g_rd_have = false;
                // R276: the pipeline guard, only where the game's markers can
                // show the shape (a title without stamps engages as before).
                bool shape_blocks = false;
                if (g_pipeline_guard != 0 && g_markers_own == 0)
                {
                    unsigned pairs = 0, overlap = 0;
                    const int pr = pipeline_overlap(dev, &pairs, &overlap);
                    char w[360];
                    if (pr == 0 && pairs >= 8u && overlap * 2u > pairs)
                    {
                        shape_blocks = true;
                        snprintf(w, sizeof w, "pipeline: the simulation of frame n+1 started BEFORE frame n's Present in %u of %u "
                                              "consecutive frame pairs - the thread that starts frames is not the thread that presents, "
                                              "so our Sleep (after Present) would hold the GPU's next frame back, not the CPU's",
                                 overlap, pairs);
                        jl(w, "R276: GetLatency frame reports at the engage");
                    }
                    else
                    {
                        snprintf(w, sizeof w, "pipeline: GetLatency -> %d, %u consecutive pairs, %u with the next simulation before "
                                              "this Present -> %s", pr, pairs, overlap,
                                 (pr != 0 || pairs < 8u) ? "unknown shape, engaging as before" : "the present thread starts frames, engaging");
                        jl(w, "R276: GetLatency frame reports at the engage");
                    }
                }
                // R283: the thread guard, where the game sends no markers
                // (R276 above needs its stamps). See thread_guard_blocks.
                const bool thread_blocks = !shape_blocks && g_pipeline_guard != 0 && g_markers_own == 1 &&
                                           thread_guard_blocks(false);
                if (shape_blocks)
                {
                    say("[MGPU][D20][RFX][R276] STEPPING ASIDE: this engine starts the next frame on another thread "
                        "while it presents (its markers say so). Our Sleep after Present would pace the wrong thread. "
                        "Use the game's own Reflex setting on this title - that loop sleeps on the right thread. "
                        "OwnReflexPipelineGuard=0 engages anyway.");
                    step_aside(dev, st, "R276: the engine's frame-start thread is not the present thread", false);
                    s = ST_ASIDE;
                }
                else if (thread_blocks)
                {
                    say(R283_ASIDE_LINE);
                    step_aside(dev, st, "R283: the game renders its frames on another thread than the one that presents",
                               false);
                    s = ST_ASIDE;
                }
                else if (set_mode(dev, 1, 1, "engage: low latency on, markers allowed (ours or the game's)") != 0)
                {
                    step_aside(dev, st, "SetSleepMode failed at the engage: we do not run a half-set "
                                        "Reflex", false);
                    s = ST_ASIDE;
                }
                else
                {
                    g_mode_set_by_us.store(true);
                    g_state.store(ST_ENGAGED, std::memory_order_release);
                    g_next_recheck = g_recheck_s > 0 ? now + g_freq.QuadPart * g_recheck_s : 0;
                    say("[MGPU][D20][RFX] ENGAGED: low latency set, our Sleep runs once per frame.");
                    s = ST_ENGAGED;
                }
            }
        }
    }
    else if (s == ST_ENGAGED)
    {
        // D2.0-4: finish_present must keep pace while the finish anchor runs.
        if (g_anchor_finish.load() && (g_presents % 120ull) == 0ull)
        {
            const unsigned long long fs = g_finish_seen.load(std::memory_order_relaxed);
            if (fs == g_finish_mark.load()) fallback_to_present("no finish_present in the last 120 game presents");
            g_finish_mark.store(fs);
        }
        if ((g_presents % 30ull) == 0ull)
        {
            status_params st{};
            if (read_status(dev, st, "engaged: frame gen check every 30 frames") && st.fg_mult > 1)
            {
                step_aside(dev, st, "frame gen came on: its own Reflex loop owns sleep", false);
                s = ST_ASIDE;
            }
            // R283: engaged on our own markers. No verdict yet (no game DLSS
            // evaluate seen at the decision: its NGX went live later,
            // or its scene had not started): the first evaluate gets the
            // comparison. R283b: after a "same thread" verdict, the watch.
            // A few loads per 30 frames; one way.
            else if (g_pipeline_guard != 0 && g_markers_own == 1)
            {
                const bool known = g_thread_known;
                if (known ? thread_moved() : thread_guard_blocks(true))
                {
                    say(R283_ASIDE_LINE);
                    step_aside(dev, st, known ? "R283b (watch): the game's DLSS evaluate left the thread that presents "
                                                "- our mode goes back"
                                              : "R283 (late): the game renders its frames on another thread than the one "
                                                "that presents - our mode goes back", true);
                    s = ST_ASIDE;
                }
            }
        }
        if (s == ST_ENGAGED && g_next_recheck != 0 && now >= g_next_recheck)
        {
            g_state.store(ST_GAP, std::memory_order_release);
            g_gap_left = (unsigned)g_gap_frames;
            jl("recheck gap starts: our Sleep pauses", "OwnReflexRecheckSec: while we sleep, "
               "useGameSleep cannot show whether someone else sleeps");
            s = ST_GAP;
        }
        if (s == ST_ENGAGED) sleep_now = (g_anchor_finish.load() == 0);
    }
    else if (s == ST_GAP)
    {
        if (g_gap_left > 0) --g_gap_left;
        if (g_gap_left == 0)
        {
            status_params st{};
            if (!read_status(dev, st, "end of the recheck gap"))
            {
                step_aside(dev, st, "GetSleepStatus failed after the gap: we do not act blind", true);
                s = ST_ASIDE;
            }
            else if (st.use_game_sleep != 0)
            {
                step_aside(dev, st, "after the gap someone else still sleeps (the game's loop started)", false);
                s = ST_ASIDE;
            }
            else if (st.fg_mult > 1)
            {
                step_aside(dev, st, "frame gen came on", false);
                s = ST_ASIDE;
            }
            else
            {
                jl("recheck gap ends: nobody else sleeps, our Sleep resumes", "useGameSleep=0 after the gap");
                g_state.store(ST_ENGAGED, std::memory_order_release);
                g_next_recheck = now + g_freq.QuadPart * g_recheck_s;
                s = ST_ENGAGED;
            }
        }
    }
    window_line(now);
    g_busy.store(false, std::memory_order_release);

    // Our markers: SIMULATION_START always right after our Sleep (Reflex wants
    // Sleep before it), PRESENT_START here, PRESENT_END in finish_present.
    // With anchor=present the Sleep is here, so SIMULATION_START of the NEXT
    // frame is sent here too; with anchor=finish both are in finish_present.
    if (sleep_now)
    {
        do_sleep(dev);
        if (g_markers_own == 1) marker(dev, M_SIM_START, g_frame.load(std::memory_order_relaxed) + 1ull);
    }
    if (g_markers_own == 1 && (s == ST_ENGAGED))
        marker(dev, M_PRESENT_START, g_frame.load(std::memory_order_relaxed));
}

void on_finish_present(void *chain)
{
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    if (chain == nullptr || chain != g_chain.load(std::memory_order_acquire)) return;
    g_finish_seen.fetch_add(1, std::memory_order_relaxed);
    if (g_state.load(std::memory_order_acquire) != ST_ENGAGED) return;
    IUnknown *dev = reinterpret_cast<IUnknown *>(g_dev.load(std::memory_order_acquire));
    if (dev == nullptr) return;
    mgpu::stallwatch::scope sc(mgpu::stallwatch::S_RFX);
    unsigned long long f = g_frame.load(std::memory_order_relaxed);
    if (g_markers_own == 1) marker(dev, M_PRESENT_END, f);
    f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1ull;
    if (g_anchor_finish.load())
    {
        do_sleep(dev);
        if (g_markers_own == 1) marker(dev, M_SIM_START, f);
    }
}

void on_chain_destroyed(void *chain, bool resize)
{
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    if (chain == nullptr || chain != g_chain.load(std::memory_order_acquire)) return;
    if (resize) return;
    IUnknown *dev = reinterpret_cast<IUnknown *>(g_dev.load(std::memory_order_acquire));
    // The present thread may be inside the settle decision: wait for it (it
    // holds g_busy only for a status read and a set), at most ~50 ms.
    bool got = false;
    for (int i = 0; i < 500 && !(got = !g_busy.exchange(true, std::memory_order_acquire)); ++i)
        YieldProcessor(), (i % 50 == 49 ? Sleep(1) : (void)0);
    const int was = g_state.exchange(ST_DONE, std::memory_order_acq_rel);
    g_chain.store(nullptr, std::memory_order_release);
    char w[200];
    snprintf(w, sizeof w, "game swap chain destroyed (resize=0) in state %s", state_name(was));
    jl(w, "teardown or a game-side chain rebuild");
    if (dev != nullptr && g_set != nullptr && g_mode_set_by_us.exchange(false))
        set_mode(dev, g_prior_ll, 0, "teardown restore: the mode is per device in the driver and "
                                     "outlives the process (residue); we set it, so it goes back to "
                                     "what it was before we engaged");
    if (!got) jl("chain destroy proceeded without the state lock after ~50 ms", "the present thread held it");
    if (void *g1 = g_gpu1.exchange(nullptr, std::memory_order_acq_rel))   // R261: our reference goes back
        reinterpret_cast<IUnknown *>(g1)->Release();
    // A rebuilt chain gets a new note_game_chain; start over with a fresh settle.
    g_t0 = 0;
    g_presents = 0;
    // R281: and a fresh wait - the new settle decides again whether it waits.
    if (g_wait_on && was == ST_SETTLE) jl("the R281 wait was running: it starts over with the new chain's settle", "R281");
    g_wait_on = false;
    g_wait_scene_t0 = 0;
    g_wait_next_look = 0;
    g_wait_looks = 0;
    g_rd_have = false;
    g_thread_known = false;   // R283: the new chain's decision compares again
    g_present_tid = 0;
    g_changes_at_verdict = 0;
    g_state.store(was == ST_ASIDE ? ST_ASIDE : ST_SETTLE, std::memory_order_release);
    if (got) g_busy.store(false, std::memory_order_release);
}
}   // namespace mgpu::own_reflex
