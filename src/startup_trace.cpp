// MGPU Bridge - R313: StartupTrace + the stop/death record. See startup_trace.hpp.
//
// Rules kept here:
//   - KEY ABSENT = one relaxed load per entry point, nothing else.
//   - Nothing on a game thread but a QPC read, relaxed stores and one queue post.
//   - Every line of the record is written *_direct (not through the queue) so a
//     death that empties the queue still leaves it; the same text goes to
//     mgpu\startup_trace.last with WriteFile (no CRT lock).
//   - The module table for the snapshot is refreshed on the bridge thread at
//     known-good moments (bridge start, first neural frame) and NEVER at the
//     record: EnumProcessModules can take the loader lock, and a frozen game
//     thread may hold it (R295's rule).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dwmapi.h>   // R313a: DWM_TIMING_INFO only (the functions are resolved with GetProcAddress; no link change)

#include "startup_trace.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"
#include "mgpu_ini_parser.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mgpu::strace
{
namespace
{
    // ---- keys ----
    std::atomic<int>      g_level{-1};          // -1 not read, 0 off, 1, 2
    std::atomic<unsigned> g_stall_ms{3000u};
    std::atomic<int>      g_safe{0};
    std::atomic<unsigned> g_quiet_ms{1000u};
    std::atomic<unsigned> g_fallback_ms{10000u};

    // ---- clock ----
    LARGE_INTEGER g_freq{};
    LARGE_INTEGER g_t0{};

    inline long long qpc() { LARGE_INTEGER c{}; QueryPerformanceCounter(&c); return c.QuadPart; }
    inline double ms_since_t0(long long q) { return g_freq.QuadPart ? (double)(q - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart : 0.0; }

    // ---- event ring ----
    struct event_t { double t; unsigned tid; int role; char step[32]; char detail[96]; unsigned hr; };
    constexpr unsigned EV_N = 128u;
    event_t g_ev[EV_N];
    std::atomic<unsigned> g_ev_w{0};   // writers claim an index with fetch_add; the ring is read only at the record

    // ---- sample ring (bridge thread writes, bridge thread reads) ----
    struct sample_t { double t; unsigned long long game; unsigned long long bridge; unsigned status; unsigned long long dwm; double present_ms; };
    constexpr unsigned SM_N = 256u;
    sample_t g_sm[SM_N];
    unsigned g_sm_w = 0;
    unsigned g_sm_n = 0;

    // ---- counters and milestones ----
    std::atomic<unsigned long long> g_game_presents{0};
    std::atomic<unsigned long long> g_bridge_presents{0};
    std::atomic<unsigned>           g_last_status{0};
    std::atomic<unsigned>           g_tid[R_COUNT] = {};
    std::atomic<void *>             g_game_dev{nullptr};
    std::atomic<void *>             g_game_chain{nullptr};
    std::atomic<long long>          g_q_first_present{0};
    std::atomic<long long>          g_q_p50{0};
    std::atomic<long long>          g_q_decision{0};
    std::atomic<int>                g_decision_mode{-1};
    std::atomic<int>                g_decision_role{R_UNKNOWN};
    std::atomic<double>             g_decision_ms{0.0};
    std::atomic<int>                g_chain_kind{-1};     // 1 composition, 0 hwnd
    std::atomic<int>                g_rooted_at_p50{-1};
    std::atomic<long long>          g_q_arm_enter{0};
    std::atomic<long long>          g_q_arm_exit{0};
    std::atomic<unsigned>           g_arm_hr{0};
    std::atomic<unsigned>           g_arm_pending_line{0};   // GT-render stores, BT writes the .last stage
    std::atomic<int>                g_stop_fired{0};
    std::atomic<unsigned long long> g_flat_ms{0};
    std::atomic<int>                g_clean{0};

    // bridge-thread-only state (no atomics needed)
    unsigned long long g_last_game_seen = 0;
    long long          g_q_last_game_change = 0;
    long long          g_q_last_cadence = 0;
    unsigned long long g_cad_game0 = 0, g_cad_bridge0 = 0;
    unsigned long long g_cad_dwm0 = 0;
    bool               g_self_test_done = false;
    bool               g_stop_record_done = false;
    bool               g_resumed_said = true;
    unsigned           g_recorded_mask = 0;   // one-shot per trigger name (bit per name hash & 31)

    // ---- module table for the snapshot ----
    struct mod_t { unsigned long long base, size; char name[48]; };
    constexpr unsigned MODS = 512u;
    mod_t g_mods[MODS];
    unsigned g_nmods = 0;

    void refresh_modules()   // bridge thread, known-good moments only
    {
        HMODULE hm[MODS];
        DWORD need = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), hm, sizeof hm, &need)) return;
        unsigned n = need / sizeof(HMODULE);
        if (n > MODS) n = MODS;
        unsigned k = 0;
        for (unsigned i = 0; i < n; ++i)
        {
            MODULEINFO mi{};
            if (!K32GetModuleInformation(GetCurrentProcess(), hm[i], &mi, sizeof mi)) continue;
            g_mods[k].base = (unsigned long long)mi.lpBaseOfDll;
            g_mods[k].size = mi.SizeOfImage;
            if (!K32GetModuleBaseNameA(GetCurrentProcess(), hm[i], g_mods[k].name, sizeof g_mods[k].name))
                g_mods[k].name[0] = '\0';
            ++k;
        }
        g_nmods = k;
    }
    const mod_t *find_mod(unsigned long long a)
    {
        for (unsigned i = 0; i < g_nmods; ++i)
            if (a >= g_mods[i].base && a < g_mods[i].base + g_mods[i].size) return &g_mods[i];
        return nullptr;
    }

    // ---- dwmapi, resolved once on the bridge thread ----
    typedef HRESULT (WINAPI *pfn_DwmIsCompositionEnabled)(BOOL *);
    // R313a: the SDK's own DWM_TIMING_INFO (dwmapi.h: #pragma pack(1), UINT32 ratios). The R313
    // hand-written copy had neither, so cbSize was wrong and the call returned nothing (DWM frames 0.0/s in T1).
    typedef HRESULT (WINAPI *pfn_DwmGetCompositionTimingInfo)(HWND, DWM_TIMING_INFO *);
    pfn_DwmIsCompositionEnabled     g_DwmIsCompositionEnabled = nullptr;
    pfn_DwmGetCompositionTimingInfo g_DwmGetCompositionTimingInfo = nullptr;
    bool g_dwm_resolved = false;
    void resolve_dwm()
    {
        if (g_dwm_resolved) return;
        g_dwm_resolved = true;
        HMODULE h = LoadLibraryW(L"dwmapi.dll");
        if (h == nullptr) return;
        g_DwmIsCompositionEnabled = (pfn_DwmIsCompositionEnabled)GetProcAddress(h, "DwmIsCompositionEnabled");
        g_DwmGetCompositionTimingInfo = (pfn_DwmGetCompositionTimingInfo)GetProcAddress(h, "DwmGetCompositionTimingInfo");
    }
    unsigned long long dwm_frames()   // cFrame: frames DWM has composed. 0 when unreadable.
    {
        if (g_DwmGetCompositionTimingInfo == nullptr) return 0ull;
        DWM_TIMING_INFO ti{}; ti.cbSize = sizeof ti;
        if (FAILED(g_DwmGetCompositionTimingInfo(nullptr, &ti))) return 0ull;
        return ti.cFrame;
    }
    int dwm_enabled()   // 1, 0, -1 unreadable
    {
        if (g_DwmIsCompositionEnabled == nullptr) return -1;
        BOOL b = FALSE;
        return SUCCEEDED(g_DwmIsCompositionEnabled(&b)) ? (b ? 1 : 0) : -1;
    }
    unsigned display_paths()
    {
        UINT np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return 0u;
        return (unsigned)np;
    }

    // ---- files ----
    bool addon_dir(wchar_t *out, size_t n)
    {
        static const int anchor = 0;
        HMODULE hm = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&anchor), &hm))
            return false;
        const DWORD len = GetModuleFileNameW(hm, out, (DWORD)n);
        if (len == 0 || len >= n) return false;
        wchar_t *slash = wcsrchr(out, L'\\');
        if (slash == nullptr) return false;
        slash[1] = L'\0';
        return true;
    }
    bool last_path(wchar_t *out, size_t n)
    {
        wchar_t dir[MAX_PATH];
        if (!addon_dir(dir, MAX_PATH)) return false;
        wchar_t sub[MAX_PATH];
        _snwprintf_s(sub, MAX_PATH, _TRUNCATE, L"%lsmgpu", dir);
        CreateDirectoryW(sub, nullptr);
        _snwprintf_s(out, n, _TRUNCATE, L"%lsmgpu\\startup_trace.last", dir);
        return true;
    }
    // Overwrite the .last file with one stage line (bridge thread or teardown only).
    void write_last(const char *stage, const char *extra)
    {
        wchar_t p[MAX_PATH];
        if (!last_path(p, MAX_PATH)) return;
        HANDLE f = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        char b[1024];
        SYSTEMTIME st{}; GetLocalTime(&st);
        const int n = snprintf(b, sizeof b, "Stage=%s\r\nWhen=%04u-%02u-%02u %02u:%02u:%02u\r\nT=%.1f\r\n%s",
                               stage, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                               ms_since_t0(qpc()), extra ? extra : "");
        DWORD w = 0;
        if (n > 0) WriteFile(f, b, (DWORD)n, &w, nullptr);
        CloseHandle(f);
    }
    // Append text to the .last file (the record).
    void append_last(const char *text)
    {
        wchar_t p[MAX_PATH];
        if (!last_path(p, MAX_PATH)) return;
        HANDLE f = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD w = 0;
        WriteFile(f, text, (DWORD)strlen(text), &w, nullptr);
        WriteFile(f, "\r\n", 2, &w, nullptr);
        CloseHandle(f);
    }
    // Read the previous launch's .last (at init). Returns the LAST Stage= value or empty,
    // and whether that session wrote a record. R313a: the whole file (it grows with a
    // record and appended stage lines; R313 read only 2 KB and missed the last stage).
    void read_previous(char *stage, size_t n, char *when, size_t wn, bool *had_record)
    {
        stage[0] = 0; when[0] = 0; if (had_record) *had_record = false;
        wchar_t p[MAX_PATH];
        if (!last_path(p, MAX_PATH)) return;
        FILE *f = nullptr;
        if (_wfopen_s(&f, p, L"rb") != 0 || f == nullptr) return;
        // Heap, freed below: this runs on the bridge thread only (note_bridge_thread), never on a
        // game thread, and only with the trace on. Nothing of it exists with the key absent.
        const size_t CAP = 256u * 1024u;
        char *buf = (char *)malloc(CAP);
        if (buf == nullptr) { fclose(f); return; }
        const size_t got = fread(buf, 1, CAP - 1, f);
        fclose(f);
        buf[got] = 0;
        if (had_record) *had_record = (strstr(buf, "===== RECORD at") != nullptr);
        const char *s = nullptr;
        for (const char *k = strstr(buf, "Stage="); k != nullptr; k = strstr(k + 6, "Stage=")) s = k;   // the LAST stage line
        if (s != nullptr) { s += 6; size_t i = 0; while (s[i] && s[i] != '\r' && s[i] != '\n' && i + 1 < n) { stage[i] = s[i]; ++i; } stage[i] = 0; }
        const char *w = nullptr;
        for (const char *k = strstr(buf, "When="); k != nullptr; k = strstr(k + 5, "When=")) w = k;
        if (w != nullptr) { w += 5; size_t i = 0; while (w[i] && w[i] != '\r' && w[i] != '\n' && i + 1 < wn) { when[i] = w[i]; ++i; } when[i] = 0; }
        free(buf);
    }

    const char *role_name(int r)
    {
        switch (r)
        {
        case R_GT_SWAP:    return "GT-swap";
        case R_GT_RENDER:  return "GT-render";
        case R_GT_PRESENT: return "GT-present";
        case R_GT_OVERLAY: return "GT-overlay";
        case R_BT:         return "BT";
        default:           return "?";
        }
    }

    // One record line: *_direct to ReShade's log AND appended to the .last file.
    void out(const char *line)
    {
        mgpu::diag::error_direct(line);
        append_last(line);
    }

    // Where is that thread? Suspend, copy the context and the top of its stack,
    // resume; name the addresses afterwards from the cached table. Once per
    // thread per record. The same shape as stall_watch's where() (R295 applied
    // once, at the record, instead of on a stall timer).
    void snapshot_thread(const char *label, unsigned tid)
    {
        char l[1200];
        if (tid == 0u)
        {
            snprintf(l, sizeof l, "[MGPU][R313] RECORD thread %s: id unknown (no trace point ran on it)", label);
            out(l);
            return;
        }
        if (tid == GetCurrentThreadId())
        {
            snprintf(l, sizeof l, "[MGPU][R313] RECORD thread %s tid=0x%X: this thread (the writer), alive", label, tid);
            out(l);
            return;
        }
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (th == nullptr)
        {
            snprintf(l, sizeof l, "[MGPU][R313] RECORD thread %s tid=0x%X: OpenThread failed (GetLastError=%lu) - gone?", label, tid, (unsigned long)GetLastError());
            out(l);
            return;
        }
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        unsigned long long stack[128];
        SIZE_T got = 0;
        bool ok = false;
        DWORD exit_code = 0;
        GetExitCodeThread(th, &exit_code);
        if (exit_code == STILL_ACTIVE && SuspendThread(th) != (DWORD)-1)
        {
            if (GetThreadContext(th, &ctx))
            {
                ok = true;
                ReadProcessMemory(GetCurrentProcess(), (const void *)ctx.Rsp, stack, sizeof stack, &got);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
        if (exit_code != STILL_ACTIVE)
        {
            snprintf(l, sizeof l, "[MGPU][R313] RECORD thread %s tid=0x%X: EXITED (code %lu)", label, tid, (unsigned long)exit_code);
            out(l);
            return;
        }
        if (!ok)
        {
            snprintf(l, sizeof l, "[MGPU][R313] RECORD thread %s tid=0x%X: context not read", label, tid);
            out(l);
            return;
        }
        int at = snprintf(l, sizeof l, "[MGPU][R313] RECORD thread %s tid=0x%X rip=", label, tid);
        const mod_t *m = find_mod(ctx.Rip);
        if (m != nullptr) at += snprintf(l + at, sizeof l - (size_t)at, "%s+0x%llX", m->name, (unsigned long long)(ctx.Rip - m->base));
        else              at += snprintf(l + at, sizeof l - (size_t)at, "0x%llX", (unsigned long long)ctx.Rip);
        at += snprintf(l + at, sizeof l - (size_t)at, " | code addresses on the stack, top first:");
        unsigned shown = 0;
        for (SIZE_T i = 0; i < got / sizeof(unsigned long long) && shown < 16u && at > 0 && at < (int)sizeof l - 80; ++i)
        {
            const mod_t *mm = find_mod(stack[i]);
            if (mm == nullptr) continue;
            at += snprintf(l + at, sizeof l - (size_t)at, " %s+0x%llX", mm->name, (unsigned long long)(stack[i] - mm->base));
            ++shown;
        }
        if (shown == 0) snprintf(l + at, sizeof l - (size_t)at, " (none in a known module within the first 1 KB)");
        out(l);
    }

    void game_window_line(char *l, size_t n, const char *when)
    {
        HWND g = (HWND)mgpu::gpu1::dcomp_game_hwnd();
        if (g == nullptr) { snprintf(l, n, "[MGPU][R313] %s game window: not known", when); return; }
        RECT rc{}; GetWindowRect(g, &rc);
        HMONITOR mon = MonitorFromWindow(g, MONITOR_DEFAULTTONULL);
        const HWND fg = GetForegroundWindow();
        snprintf(l, n, "[MGPU][R313] %s game window 0x%p: visible=%d iconic=%d foreground=%d rect=(%ld,%ld,%ld,%ld) monitor=0x%p | DWM composition=%d dwm frames=%llu | active display paths=%u",
                 when, (void *)g, IsWindowVisible(g) ? 1 : 0, IsIconic(g) ? 1 : 0, (fg == g) ? 1 : 0,
                 rc.left, rc.top, rc.right, rc.bottom, (void *)mon, dwm_enabled(), dwm_frames(), display_paths());
    }

    unsigned name_bit(const char *s) { unsigned h = 5381u; while (*s) h = h * 33u + (unsigned char)*s++; return 1u << (h & 31u); }
}   // namespace

// ---------------------------------------------------------------------------
void init()
{
    int expect = -1;
    if (!g_level.compare_exchange_strong(expect, 0)) return;   // once

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);

    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!addon_dir(dir, MAX_PATH)) return;
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", dir);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return;
    static char buf[mgpu::config::MAX_BYTES + 2u];
    const size_t got = fread(buf, 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return;
    buf[got] = '\0';

    auto num = [&](const char *k, int d) { const char *v = mgpu::config::find(buf, got, k); return v ? atoi(v) : d; };
    int lvl = num("StartupTrace", 0);
    if (lvl < 0) lvl = 0; if (lvl > 2) lvl = 2;
    int stall = num("StartupTraceStallMs", 3000);
    if (stall < 500) stall = 500; if (stall > 60000) stall = 60000;
    int safe = num("DcompSafeMode", 0);
    // R313b: value 2 (composition on the game's adapter) was removed - it changed which
    // chain the add-on takes for the game's. 2 or any other value reads as 0 (off).
    const int safe_raw = safe;
    if (safe != 1 && safe != 3) safe = 0;
    int quiet = num("DcompSafeQuietMs", 1000);
    if (quiet < 0) quiet = 0; if (quiet > 30000) quiet = 30000;
    int fb = num("DcompSafeFallbackMs", 10000);
    if (fb < 2000) fb = 2000; if (fb > 120000) fb = 120000;

    g_stall_ms.store((unsigned)stall, std::memory_order_relaxed);
    g_safe.store(safe, std::memory_order_relaxed);
    g_quiet_ms.store((unsigned)quiet, std::memory_order_relaxed);
    g_fallback_ms.store((unsigned)fb, std::memory_order_relaxed);

    if (safe != 0)
    {
        char l[400];
        snprintf(l, sizeof l,
                 "[MGPU][R313] DcompSafeMode=%d: %s. Key absent = the composition mode exactly as shipped.",
                 safe,
                 safe == 1 ? "the composition decision and the overlay root toggle run on the bridge thread only"
                           : "same GPU 1 target, serialized (WaitForCommitCompletion after every Commit), resize done unrooted, root waits for a quiet game chain; if the game's presents stop after the first neural frame the add-on writes DcompOverlay=0 (the bridge's own window) for the next launch");
        mgpu::diag::warn(l);
    }
    else if (safe_raw != 0 && mgpu::config::find(buf, got, "DcompSafeMode") != nullptr)
    {
        char l[200];
        snprintf(l, sizeof l, "[MGPU][R313] DcompSafeMode=%d is not a value (1 or 3) - read as off; the composition mode runs as shipped.", safe_raw);
        mgpu::diag::warn(l);
    }

    if (lvl == 0) return;
    g_level.store(lvl, std::memory_order_release);

    char l[600];
    snprintf(l, sizeof l,
             "[MGPU][R313] StartupTrace=%d ON: [TRACE] lines carry their own time (t=ms since this line) and thread; "
             "the game's present cadence is sampled per bridge frame; a record is written when the game's presents "
             "stop for %u ms after the first neural frame (window up, bridge alive) or when a device is lost%s. "
             "The previous session's file (mgpu\\startup_trace.last) is reported when the bridge thread starts.",
             lvl, (unsigned)stall, lvl == 2 ? "; =2 also fires the record once 5 s after the first neural frame as a self-test" : "");
    mgpu::diag::warn(l);
    // R313a: no file read and no file write here - this is the game's swap-chain thread.
    // The previous session's file is read, reported and reset on the bridge thread
    // (note_bridge_thread). Nothing decides anything from it; it only has to be read
    // before this session overwrites it, and the only writer before that is the bridge.
    const unsigned tid = GetCurrentThreadId();
    g_tid[R_GT_SWAP].store(tid, std::memory_order_relaxed);
    ev(R_GT_SWAP, "trace-init", "on_init_swapchain entry");
}

bool     on()               { return g_level.load(std::memory_order_relaxed) > 0; }
int      level()            { const int v = g_level.load(std::memory_order_relaxed); return v < 0 ? 0 : v; }
unsigned stall_ms()         { return g_stall_ms.load(std::memory_order_relaxed); }
int      safe_mode()        { return g_safe.load(std::memory_order_relaxed); }
unsigned safe_quiet_ms()    { return g_quiet_ms.load(std::memory_order_relaxed); }
unsigned safe_fallback_ms() { return g_fallback_ms.load(std::memory_order_relaxed); }
double   now_ms()           { return on() ? ms_since_t0(qpc()) : 0.0; }

void ev(role r, const char *step, const char *detail, unsigned hr)
{
    if (g_level.load(std::memory_order_relaxed) <= 0) return;
    const double t = ms_since_t0(qpc());
    const unsigned tid = GetCurrentThreadId();
    if (r > R_UNKNOWN && r < R_COUNT && g_tid[r].load(std::memory_order_relaxed) == 0u)
        g_tid[r].store(tid, std::memory_order_relaxed);
    const unsigned i = g_ev_w.fetch_add(1u, std::memory_order_relaxed) % EV_N;
    event_t &e = g_ev[i];
    e.t = t; e.tid = tid; e.role = r; e.hr = hr;
    strncpy_s(e.step, step ? step : "", _TRUNCATE);
    strncpy_s(e.detail, detail ? detail : "", _TRUNCATE);
    char l[360];
    snprintf(l, sizeof l, "[MGPU][TRACE] t=%.1f tid=0x%X role=%s step=%s%s%s hr=0x%08X",
             t, tid, role_name(r), step ? step : "", detail ? " | " : "", detail ? detail : "", hr);
    mgpu::diag::info(l);
}

void note_game_chain(void *chain) { if (chain != nullptr) g_game_chain.store(chain, std::memory_order_relaxed); }

void note_game_present(void *chain)
{
    if (g_level.load(std::memory_order_relaxed) <= 0) return;
    if (chain == nullptr || chain != g_game_chain.load(std::memory_order_relaxed)) return;
    g_game_presents.fetch_add(1ull, std::memory_order_relaxed);
    if (g_tid[R_GT_PRESENT].load(std::memory_order_relaxed) == 0u)
        g_tid[R_GT_PRESENT].store(GetCurrentThreadId(), std::memory_order_relaxed);
}

void note_game_device(void *d) { if (d != nullptr) g_game_dev.store(d, std::memory_order_relaxed); }

void note_bridge_thread()
{
    g_tid[R_BT].store(GetCurrentThreadId(), std::memory_order_relaxed);   // always: DcompSafeMode reads it with the trace off
    if (!on()) return;
    // R313a: the previous session, read BEFORE this session's first write to the file.
    // Bridge thread: the heap buffer and the file I/O are off every game thread.
    {
        char prev_stage[64], prev_when[64];
        bool prev_record = false;
        read_previous(prev_stage, sizeof prev_stage, prev_when, sizeof prev_when, &prev_record);
        char l[500];
        snprintf(l, sizeof l, "[MGPU][R313] previous session (mgpu\\startup_trace.last): %s%s %s%s.",
                 prev_stage[0] ? "ended at Stage=" : "no file",
                 prev_stage[0] ? prev_stage : "", prev_when[0] ? prev_when : "",
                 prev_record ? " - that session wrote a RECORD (kept in the file until this line; overwritten now)" : "");
        if (prev_stage[0] && strcmp(prev_stage, "clean") != 0)
        {
            snprintf(l, sizeof l,
                     "[MGPU][R313] THE PREVIOUS SESSION DID NOT END CLEANLY: its last stage was \"%s\" at %s%s. "
                     "The file is overwritten now.",
                     prev_stage, prev_when, prev_record ? "; it wrote a RECORD (see that session's ReShade.log)" : "");
            mgpu::diag::error(l);
        }
        else
            mgpu::diag::info(l);
        write_last("init", nullptr);
    }
    resolve_dwm();
    refresh_modules();
    ev(R_BT, "bridge-thread", "started");
}

void note_first_present()
{
    if (!on()) return;
    g_q_first_present.store(qpc(), std::memory_order_relaxed);
    ev(R_BT, "first-present", "bridge chain");
    write_last("first-present", nullptr);
}

void note_bridge_present(unsigned st, double ms_inside)
{
    if (!on()) return;
    g_bridge_presents.fetch_add(1ull, std::memory_order_relaxed);
    g_last_status.store(st, std::memory_order_relaxed);
    // the sample ring is written by bt_tick; here only the slow presents get a line
    if (ms_inside >= 50.0)
    {
        char d[96];
        snprintf(d, sizeof d, "Present took %.1f ms status=0x%08X", ms_inside, st);
        ev(R_BT, "present-slow", d, st);
    }
    // the first ten presents, each
    const unsigned long long n = g_bridge_presents.load(std::memory_order_relaxed);
    if (n <= 10ull)
    {
        char d[96];
        snprintf(d, sizeof d, "#%llu status=0x%08X %.2f ms", n, st, ms_inside);
        ev(R_BT, "present", d, st);
    }
}

void note_first_neural_frame(bool rooted)
{
    if (!on()) return;
    g_q_p50.store(qpc(), std::memory_order_relaxed);
    g_rooted_at_p50.store(rooted ? 1 : 0, std::memory_order_relaxed);
    g_q_last_game_change = qpc();
    g_last_game_seen = g_game_presents.load(std::memory_order_relaxed);
    g_q_last_cadence = qpc();
    g_cad_game0 = g_last_game_seen;
    g_cad_bridge0 = g_bridge_presents.load(std::memory_order_relaxed);
    g_cad_dwm0 = dwm_frames();
    refresh_modules();   // known-good moment, bridge thread
    char d[96];
    snprintf(d, sizeof d, "rooted=%d since first present %.1f ms", rooted ? 1 : 0,
             g_q_first_present.load(std::memory_order_relaxed) ? ms_since_t0(qpc()) - ms_since_t0(g_q_first_present.load(std::memory_order_relaxed)) : 0.0);
    ev(R_BT, "first-neural-frame", d);
    char w[400];
    game_window_line(w, sizeof w, "at first neural frame:");
    mgpu::diag::info(w);
    write_last("first-neural-frame", nullptr);
}

void note_chain_kind(bool composition)
{
    if (!on()) return;
    g_chain_kind.store(composition ? 1 : 0, std::memory_order_relaxed);
}

void note_decision(int mode, double elapsed, role decided_on)
{
    if (!on()) return;
    g_q_decision.store(qpc(), std::memory_order_relaxed);
    g_decision_mode.store(mode, std::memory_order_relaxed);
    g_decision_ms.store(elapsed, std::memory_order_relaxed);
    g_decision_role.store(decided_on, std::memory_order_relaxed);
    char d[96];
    snprintf(d, sizeof d, "mode=%d decided in %.2f ms on %s", mode, elapsed, role_name(decided_on));
    ev(decided_on, "composition-decision", d);
}

void note_arm_enter()
{
    if (g_level.load(std::memory_order_relaxed) <= 0) return;
    if (g_q_arm_enter.load(std::memory_order_relaxed) != 0) return;   // the arm can re-enter next frame after a failed allocation: first entry only
    g_q_arm_enter.store(qpc(), std::memory_order_relaxed);
    if (g_tid[R_GT_RENDER].load(std::memory_order_relaxed) == 0u)
        g_tid[R_GT_RENDER].store(GetCurrentThreadId(), std::memory_order_relaxed);
    ev(R_GT_RENDER, "arm-enter", "under s.cs");
}

void note_arm_exit(unsigned hr)
{
    if (g_level.load(std::memory_order_relaxed) <= 0) return;
    if (g_q_arm_exit.load(std::memory_order_relaxed) != 0) return;
    const long long q = qpc();
    g_q_arm_exit.store(q, std::memory_order_relaxed);
    g_arm_hr.store(hr, std::memory_order_relaxed);
    g_arm_pending_line.store(1u, std::memory_order_relaxed);
    char d[96];
    const long long e = g_q_arm_enter.load(std::memory_order_relaxed);
    snprintf(d, sizeof d, "%.1f ms under s.cs", e ? ms_since_t0(q) - ms_since_t0(e) : 0.0);
    ev(R_GT_RENDER, "arm-exit", d, hr);
}

bool is_bridge_thread()
{
    const unsigned b = g_tid[R_BT].load(std::memory_order_relaxed);
    return b != 0u && b == GetCurrentThreadId();
}
bool stop_fired() { return g_stop_fired.load(std::memory_order_relaxed) != 0; }
unsigned long long game_flat_ms() { return g_flat_ms.load(std::memory_order_relaxed); }

void bt_tick()
{
    if (g_level.load(std::memory_order_relaxed) <= 0) return;
    const long long q = qpc();
    const unsigned long long gp = g_game_presents.load(std::memory_order_relaxed);
    const unsigned long long bp = g_bridge_presents.load(std::memory_order_relaxed);
    const unsigned st = g_last_status.load(std::memory_order_relaxed);

    // the arm's .last stage, written here so no file write happens on GT-render
    if (g_arm_pending_line.exchange(0u, std::memory_order_relaxed) != 0u)
        write_last("armed", nullptr);

    // sample ring (bridge thread only)
    {
        sample_t &s = g_sm[g_sm_w];
        s.t = ms_since_t0(q); s.game = gp; s.bridge = bp; s.status = st; s.dwm = dwm_frames(); s.present_ms = 0.0;
        g_sm_w = (g_sm_w + 1u) % SM_N;
        if (g_sm_n < SM_N) ++g_sm_n;
    }

    const long long p50 = g_q_p50.load(std::memory_order_relaxed);
    if (p50 == 0) return;   // the cadence, the stop signature and the self-test start at the first neural frame

    if (gp != g_last_game_seen)
    {
        g_last_game_seen = gp;
        g_q_last_game_change = q;
        g_flat_ms.store(0ull, std::memory_order_relaxed);
        if (!g_resumed_said)
        {
            g_resumed_said = true;
            ev(R_BT, "game-presents-resumed", "after the stop signature");
        }
    }
    const double flat = ms_since_t0(q) - ms_since_t0(g_q_last_game_change);
    g_flat_ms.store((unsigned long long)(flat > 0.0 ? flat : 0.0), std::memory_order_relaxed);

    // cadence line every 2 s
    if (ms_since_t0(q) - ms_since_t0(g_q_last_cadence) >= 2000.0)
    {
        const unsigned long long dwm = dwm_frames();
        const double dt = (ms_since_t0(q) - ms_since_t0(g_q_last_cadence)) / 1000.0;
        char l[420];
        snprintf(l, sizeof l,
                 "[MGPU][R313] CADENCE since first neural frame %.1f s: game presents %.1f/s, bridge presents %.1f/s, DWM frames %.1f/s | last bridge Present status=0x%08X | game presents flat for %.0f ms",
                 (ms_since_t0(q) - ms_since_t0(p50)) / 1000.0,
                 dt > 0.0 ? (double)(gp - g_cad_game0) / dt : 0.0,
                 dt > 0.0 ? (double)(bp - g_cad_bridge0) / dt : 0.0,
                 (dt > 0.0 && dwm >= g_cad_dwm0) ? (double)(dwm - g_cad_dwm0) / dt : 0.0,
                 st, flat);
        mgpu::diag::info(l);
        g_q_last_cadence = q; g_cad_game0 = gp; g_cad_bridge0 = bp; g_cad_dwm0 = dwm;
    }

    // the stop signature: presents flat for stall_ms, our presents not occluded, window up
    if (!g_stop_record_done && flat >= (double)g_stall_ms.load(std::memory_order_relaxed))
    {
        HWND g = (HWND)mgpu::gpu1::dcomp_game_hwnd();
        const bool window_up = (g == nullptr) || (IsWindowVisible(g) && !IsIconic(g) && GetForegroundWindow() == g);
        const bool occluded = (st == 0x087A0001u);   // DXGI_STATUS_OCCLUDED
        if (window_up && !occluded)
        {
            g_stop_record_done = true;
            g_resumed_said = false;
            g_stop_fired.store(1, std::memory_order_relaxed);
            char why[160];
            snprintf(why, sizeof why, "GAME PRESENTS STOPPED: flat for %.0f ms after the first neural frame, bridge alive, game window up", flat);
            record(why, 0u);
        }
    }

    // self-test (StartupTrace=2): the record once, 5 s after the first neural frame
    if (!g_self_test_done && g_level.load(std::memory_order_relaxed) == 2 && ms_since_t0(q) - ms_since_t0(p50) >= 5000.0)
    {
        g_self_test_done = true;
        record("SELF-TEST (StartupTrace=2): nothing is wrong - this exercises the record path 5 s after the first neural frame", 0u);
    }
}

void record(const char *why, unsigned gpu1_reason)
{
    if (!on()) return;
    const unsigned bit = name_bit(why ? why : "");
    if (g_recorded_mask & bit) return;   // one-shot per trigger
    g_recorded_mask |= bit;

    const long long q = qpc();
    const double t = ms_since_t0(q);
    char l[1200];

    // reasons and LUIDs
    unsigned r1 = gpu1_reason, r0 = 0u;
    LUID l1{}, l0{};
    {
        mgpu::adapter::selection_result sel;
        mgpu::adapter::get_selection(sel);
        l1 = sel.selected_luid; l0 = sel.game_luid;
    }
    if (r1 == 0u)
    {
        HRESULT h = S_OK;
        if (mgpu::gpu1::device_removed_reason(h)) r1 = (unsigned)h;
    }
    const char *r0s = "no pointer (pre-arm and not seen at init_swapchain)";
    if (ID3D12Device *gd = (ID3D12Device *)g_game_dev.load(std::memory_order_relaxed))
    {
        r0 = (unsigned)gd->GetDeviceRemovedReason();
        r0s = (r0 == 0u) ? "S_OK (alive)" : (r0 == 0x887A0005u ? "DEVICE_REMOVED" : r0 == 0x887A0006u ? "DEVICE_HUNG" : r0 == 0x887A0007u ? "DEVICE_RESET" : r0 == 0x887A0020u ? "DRIVER_INTERNAL_ERROR" : "removed (other)");
    }

    const long long fp = g_q_first_present.load(std::memory_order_relaxed);
    const long long p50 = g_q_p50.load(std::memory_order_relaxed);
    const long long dq = g_q_decision.load(std::memory_order_relaxed);
    const int kind = g_chain_kind.load(std::memory_order_relaxed);

    snprintf(l, sizeof l, "[MGPU][R313] ===== RECORD at t=%.1f ms (tid=0x%X): %s =====", t, (unsigned)GetCurrentThreadId(), why ? why : "");
    out(l);
    snprintf(l, sizeof l,
             "[MGPU][R313] RECORD devices: GPU 1 (bridge) luid=0x%08X-0x%08X GetDeviceRemovedReason=0x%08X | GPU 0 (game) luid=0x%08X-0x%08X GetDeviceRemovedReason=0x%08X %s",
             (unsigned)l1.HighPart, (unsigned)l1.LowPart, r1, (unsigned)l0.HighPart, (unsigned)l0.LowPart, r0, r0s);
    out(l);
    snprintf(l, sizeof l,
             "[MGPU][R313] RECORD timeline: first bridge present %s | first neural frame %s (rooted then=%d) | composition decision %s mode=%d on %s in %.2f ms | chain=%s | DcompOverlay now=%d rooted now=%d | DcompSafeMode=%d | game presents=%llu bridge presents=%llu last bridge Present status=0x%08X | game presents flat %llu ms",
             fp ? "yes" : "never", p50 ? "yes" : "never", g_rooted_at_p50.load(std::memory_order_relaxed),
             dq ? "made" : "not made", g_decision_mode.load(std::memory_order_relaxed), role_name(g_decision_role.load(std::memory_order_relaxed)), g_decision_ms.load(std::memory_order_relaxed),
             kind == 1 ? "ForComposition" : kind == 0 ? "ForHwnd" : "none",
             mgpu::gpu1::dcomp_overlay_mode() ? 1 : 0, mgpu::gpu1::dcomp_visual_rooted() ? 1 : 0, safe_mode(),
             g_game_presents.load(std::memory_order_relaxed), g_bridge_presents.load(std::memory_order_relaxed), g_last_status.load(std::memory_order_relaxed),
             g_flat_ms.load(std::memory_order_relaxed));
    out(l);
    snprintf(l, sizeof l, "[MGPU][R313] RECORD elapsed: since first present %.1f ms | since first neural frame %.1f ms | since decision %.1f ms | since arm exit %.1f ms",
             fp ? t - ms_since_t0(fp) : -1.0, p50 ? t - ms_since_t0(p50) : -1.0, dq ? t - ms_since_t0(dq) : -1.0,
             g_q_arm_exit.load(std::memory_order_relaxed) ? t - ms_since_t0(g_q_arm_exit.load(std::memory_order_relaxed)) : -1.0);
    out(l);
    game_window_line(l, sizeof l, "RECORD");
    out(l);

    // the event ring, oldest first
    {
        const unsigned w = g_ev_w.load(std::memory_order_relaxed);
        const unsigned n = w < EV_N ? w : EV_N;
        const unsigned start = w < EV_N ? 0u : w % EV_N;
        snprintf(l, sizeof l, "[MGPU][R313] RECORD trace ring: %u event(s), oldest first", n);
        out(l);
        for (unsigned i = 0; i < n; ++i)
        {
            const event_t &e = g_ev[(start + i) % EV_N];
            snprintf(l, sizeof l, "[MGPU][R313]   t=%.1f tid=0x%X %s %s%s%s hr=0x%08X", e.t, e.tid, role_name(e.role), e.step, e.detail[0] ? " | " : "", e.detail, e.hr);
            out(l);
        }
    }
    // the last 40 samples (per bridge loop pass), oldest first
    {
        const unsigned n = g_sm_n < 40u ? g_sm_n : 40u;
        snprintf(l, sizeof l, "[MGPU][R313] RECORD cadence ring (last %u bridge loop passes: t, game presents, bridge presents, bridge Present status, DWM frames)", n);
        out(l);
        char line2[1100]; int at = 0; unsigned k = 0;
        for (unsigned i = 0; i < n; ++i)
        {
            const unsigned idx = (g_sm_w + SM_N - n + i) % SM_N;
            const sample_t &s = g_sm[idx];
            at += snprintf(line2 + at, sizeof line2 - (size_t)at, " [%.0f g=%llu b=%llu s=%X d=%llu]", s.t, s.game, s.bridge, s.status, s.dwm);
            if (++k == 8u || i + 1u == n) { snprintf(l, sizeof l, "[MGPU][R313]  %s", line2); out(l); at = 0; k = 0; line2[0] = 0; }
        }
    }
    // the threads
    snapshot_thread("GT-swap",    g_tid[R_GT_SWAP].load(std::memory_order_relaxed));
    snapshot_thread("GT-render",  g_tid[R_GT_RENDER].load(std::memory_order_relaxed));
    snapshot_thread("GT-present", g_tid[R_GT_PRESENT].load(std::memory_order_relaxed));
    snapshot_thread("GT-overlay", g_tid[R_GT_OVERLAY].load(std::memory_order_relaxed));
    snapshot_thread("BT",         g_tid[R_BT].load(std::memory_order_relaxed));
    snprintf(l, sizeof l, "[MGPU][R313] ===== RECORD END (also in mgpu\\startup_trace.last) =====");
    out(l);
    {
        SYSTEMTIME st{}; GetLocalTime(&st);
        snprintf(l, sizeof l, "Stage=record\r\nWhen=%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        append_last(l);   // appended, so the record text above it is kept for the next launch
    }
}

// R313a: the clean stage is APPENDED, never overwrites the file - a session that wrote a
// record and was then closed normally keeps its record for the next launch to report.
static void append_clean(const char *how)
{
    SYSTEMTIME st{}; GetLocalTime(&st);
    char l[160];
    snprintf(l, sizeof l, "Stage=clean\r\nWhen=%04u-%02u-%02u %02u:%02u:%02u\r\nHow=%s",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, how);
    append_last(l);
}
void mark_clean_exit()
{
    if (!on()) return;
    if (g_clean.exchange(1, std::memory_order_relaxed) != 0) return;
    ev(R_GT_SWAP, "destroy-device", "clean exit");
    append_clean("destroy_device");
}
// R313a: DLL_PROCESS_DETACH (loader lock held). File only: no log line, no queue, no
// lock of ours - CreateFileW / WriteFile / CloseHandle, as the journal's session close
// does at the same point. Cyberpunk exits without destroy_device, so without this every
// next launch reported "did not end cleanly". A task-manager kill skips DETACH, so a
// freeze-and-kill still reads as not clean - the signal this file exists for.
void mark_clean_detach(bool process_exit)
{
    if (!on()) return;
    if (g_clean.exchange(1, std::memory_order_relaxed) != 0) return;
    append_clean(process_exit ? "DLL_PROCESS_DETACH, process exit" : "DLL_PROCESS_DETACH, FreeLibrary unload");
}
}   // namespace mgpu::strace
