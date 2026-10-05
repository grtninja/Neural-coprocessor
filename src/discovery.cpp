// MGPU Bridge - D1: the discovery calibrator. See discovery.hpp.
//
// R&D ONLY. Destructive on purpose: it swaps pointers inside other modules'
// data while they run. Every write is logged before it is made, every call
// into NvAPI is preceded by its own step line, because a wrong function id
// crashes with no return code (roadmap rule, ROADMAP_TENTATIVE C0).
//
// THE NVAPI FACTS USED HERE, AND WHERE THEY CAME FROM. All of them are the
// ones the old Reflex block in gpu1_context.cpp already runs with, measured
// and fixed in V21:
//   nvapi_QueryInterface   exported by nvapi64.dll
//   0xAC1CA9E0             NvAPI_D3D_SetSleepMode   (IUnknown *, params *)
//   0xAEF96CA1             NvAPI_D3D_GetSleepStatus (IUnknown *, params *)
//   0x852CD1D2             NvAPI_D3D_Sleep          (IUnknown *)
//   0x1A587F9C             NvAPI_D3D_GetLatency     (IUnknown *, params *)
//   NV_SET_SLEEP_MODE_PARAMS_V1, 44 bytes: version @0 (size in the low 16
//     bits), bLowLatencyMode @4, bLowLatencyBoost @5, minimumIntervalUs @8,
//     bUseMarkersToOptimize @12. NvBool is one byte.
//   NV_GET_SLEEP_STATUS_PARAMS_V1, 136 bytes: bLowLatencyMode @4,
//     fgMultiplier @14 (the gpu1_context layout names it fg_multiplier).
// No field is read past the size the caller's own version word declares.
//
// WHAT IT CANNOT SEE. A caller that resolves a function and keeps it only in a
// register, or calls GetProcAddress("nvapi_QueryInterface") and uses the
// result at once without storing it, is not wrapped. Its calls are simply
// missing from the counts; nothing breaks.

#include <windows.h>
#include <unknwn.h>     // IUnknown. WIN32_LEAN_AND_MEAN (set on the command line) leaves it out of windows.h
#include <tlhelp32.h>
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "discovery.hpp"
#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

#pragma intrinsic(_ReturnAddress)

namespace mgpu::discovery
{
namespace
{
// ---- the key ----
std::atomic<int> g_mode{-1};           // -1 not read yet, 0 off, 1 on

// ---- NvAPI ----
constexpr unsigned ID_SET_SLEEP  = 0xAC1CA9E0u;
constexpr unsigned ID_GET_STATUS = 0xAEF96CA1u;
constexpr unsigned ID_SLEEP      = 0x852CD1D2u;
constexpr unsigned ID_LATENCY    = 0x1A587F9Cu;

typedef void *(__cdecl *pf_qi)(unsigned int);
typedef int   (__cdecl *pf_dev_p)(IUnknown *, void *);
typedef int   (__cdecl *pf_dev)(IUnknown *);

// Set once, before any thunk is reachable, never changed after.
pf_qi    g_real_qi    = nullptr;
pf_dev_p g_real_set   = nullptr;
pf_dev_p g_real_get   = nullptr;
pf_dev   g_real_sleep = nullptr;
pf_dev_p g_real_lat   = nullptr;
HMODULE  g_nvapi      = nullptr;
HMODULE  g_self       = nullptr;

// ---- time and presents ----
LARGE_INTEGER g_freq{};
std::atomic<long long> g_t0{0};        // QPC at the first present
double now_s()
{
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    const long long t0 = g_t0.load(std::memory_order_relaxed);
    if (t0 == 0 || g_freq.QuadPart == 0) return 0.0;
    return (double)(c.QuadPart - t0) / (double)g_freq.QuadPart;
}

constexpr unsigned CHAINS = 6u;
struct chain_ent
{
    std::atomic<void *> chain{nullptr};
    std::atomic<unsigned long long> presents{0};
    std::atomic<unsigned long long> win_at{0};   // presents at the last window line
    std::atomic<bool> destroyed{false};
};
chain_ent g_chains[CHAINS];
std::atomic<unsigned long long> g_presents_all{0};
std::atomic<unsigned> g_chain_inits{0};

// ---- callers, by module ----
enum kind { K_SET = 0, K_SLEEP, K_GET, K_LAT, K_QI, K_KINDS };
constexpr unsigned CALLERS = 12u;
struct caller_ent
{
    std::atomic<void *> mod{nullptr};
    std::atomic<unsigned long long> n[K_KINDS];
    std::atomic<unsigned long long> win[K_KINDS];   // n at the last window line
};
caller_ent g_callers[CALLERS];
std::atomic<unsigned long long> g_callers_full{0};

HMODULE module_of(const void *addr)
{
    HMODULE hm = nullptr;
    if (addr == nullptr) return nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &hm))
        return nullptr;
    return hm;
}

void mod_name(HMODULE m, char *out, size_t n)
{
    if (n == 0) return;
    out[0] = '\0';
    if (m == nullptr) { snprintf(out, n, "(no module)"); return; }
    char path[MAX_PATH];
    const DWORD len = GetModuleFileNameA(m, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) { snprintf(out, n, "%p", (void *)m); return; }
    const char *b = strrchr(path, '\\');
    snprintf(out, n, "%s", b != nullptr ? b + 1 : path);
}

// Index of the caller's module in the table; -1 when the table is full.
int caller_index(HMODULE m)
{
    for (unsigned i = 0; i < CALLERS; ++i)
    {
        void *cur = g_callers[i].mod.load(std::memory_order_acquire);
        if (cur == (void *)m) return (int)i;
        if (cur == nullptr)
        {
            void *expect = nullptr;
            if (g_callers[i].mod.compare_exchange_strong(expect, (void *)m,
                                                         std::memory_order_acq_rel))
                return (int)i;
            if (expect == (void *)m) return (int)i;
        }
    }
    g_callers_full.fetch_add(1, std::memory_order_relaxed);
    return -1;
}

// Counts one call; true the first time this module makes this kind of call.
bool count_call(HMODULE m, kind k)
{
    const int i = caller_index(m);
    if (i < 0) return false;
    return g_callers[i].n[k].fetch_add(1, std::memory_order_relaxed) == 0ull;
}

std::atomic<unsigned> g_lines{0};       // change lines written, capped
constexpr unsigned LINE_CAP = 400u;
bool line_budget() { return g_lines.fetch_add(1, std::memory_order_relaxed) < LINE_CAP; }

// ---- the thunks ----
std::atomic<unsigned long long> g_last_set{~0ull};
std::atomic<void *> g_last_set_dev{nullptr};

__declspec(noinline) int __cdecl th_set(IUnknown *dev, void *p)
{
    void *ra = _ReturnAddress();
    unsigned ver = 0, mi = 0;
    unsigned ll = 0xFFu, bo = 0xFFu, mk = 0xFFu;
    if (p != nullptr)
    {
        const unsigned char *b = (const unsigned char *)p;
        memcpy(&ver, b, 4);
        const unsigned size = ver & 0xFFFFu;
        if (size >= 6u)  { ll = b[4]; bo = b[5]; }
        if (size >= 12u) memcpy(&mi, b + 8, 4);
        if (size >= 13u) mk = b[12];
    }
    const int r = (g_real_set != nullptr) ? g_real_set(dev, p) : -1;
    const HMODULE m = module_of(ra);
    const bool first = count_call(m, K_SET);
    const unsigned long long key = ((unsigned long long)ll << 56) | ((unsigned long long)bo << 48) |
                                   ((unsigned long long)mk << 40) | (unsigned long long)mi;
    const unsigned long long was = g_last_set.exchange(key, std::memory_order_relaxed);
    void *was_dev = g_last_set_dev.exchange((void *)dev, std::memory_order_relaxed);
    if ((first || was != key || was_dev != (void *)dev) && line_budget())
    {
        char mn[96]; mod_name(m, mn, sizeof mn);
        char l[400];
        snprintf(l, sizeof l,
                 "[MGPU][DSC] SetSleepMode t=%.2fs P=%llu by %s dev=%p ver=0x%08X lowLatency=%u "
                 "boost=%u minIntervalUs=%u markers=%u -> %d%s",
                 now_s(), g_presents_all.load(std::memory_order_relaxed), mn, (void *)dev, ver,
                 ll, bo, mi, mk, r, was_dev != nullptr && was_dev != (void *)dev
                                      ? " | DEVICE CHANGED" : "");
        mgpu::diag::info(l);
    }
    return r;
}

std::atomic<void *> g_last_sleep_dev{nullptr};

__declspec(noinline) int __cdecl th_sleep(IUnknown *dev)
{
    void *ra = _ReturnAddress();
    const int r = (g_real_sleep != nullptr) ? g_real_sleep(dev) : -1;
    const HMODULE m = module_of(ra);
    const bool first = count_call(m, K_SLEEP);
    void *was_dev = g_last_sleep_dev.exchange((void *)dev, std::memory_order_relaxed);
    if ((first || (was_dev != nullptr && was_dev != (void *)dev)) && line_budget())
    {
        char mn[96]; mod_name(m, mn, sizeof mn);
        char l[300];
        snprintf(l, sizeof l, "[MGPU][DSC] Sleep t=%.2fs P=%llu by %s dev=%p -> %d%s",
                 now_s(), g_presents_all.load(std::memory_order_relaxed), mn, (void *)dev, r,
                 first ? " | FIRST from this module" : " | DEVICE CHANGED");
        mgpu::diag::info(l);
    }
    return r;
}

std::atomic<unsigned> g_last_status{0xFFFFFFFFu};

__declspec(noinline) int __cdecl th_get(IUnknown *dev, void *p)
{
    void *ra = _ReturnAddress();
    const int r = (g_real_get != nullptr) ? g_real_get(dev, p) : -1;
    const HMODULE m = module_of(ra);
    const bool first = count_call(m, K_GET);
    unsigned ll = 0xFFu, fg = 0xFFu;
    if (r == 0 && p != nullptr)
    {
        const unsigned char *b = (const unsigned char *)p;
        unsigned ver = 0; memcpy(&ver, b, 4);
        const unsigned size = ver & 0xFFFFu;
        if (size >= 5u)  ll = b[4];
        if (size >= 15u) fg = b[14];
    }
    const unsigned key = (ll << 8) | fg;
    const unsigned was = g_last_status.exchange(key, std::memory_order_relaxed);
    if ((first || was != key) && line_budget())
    {
        char mn[96]; mod_name(m, mn, sizeof mn);
        char l[360];
        snprintf(l, sizeof l,
                 "[MGPU][DSC] GetSleepStatus t=%.2fs P=%llu by %s dev=%p -> %d | reads "
                 "lowLatency=%u fgMultiplier=%u (the driver's read-back - the ledger records it "
                 "disagreeing with reality; L1 is the proof)",
                 now_s(), g_presents_all.load(std::memory_order_relaxed), mn, (void *)dev, r,
                 ll, fg);
        mgpu::diag::info(l);
    }
    return r;
}

__declspec(noinline) int __cdecl th_lat(IUnknown *dev, void *p)
{
    void *ra = _ReturnAddress();
    const int r = (g_real_lat != nullptr) ? g_real_lat(dev, p) : -1;
    const HMODULE m = module_of(ra);
    if (count_call(m, K_LAT) && line_budget())
    {
        char mn[96]; mod_name(m, mn, sizeof mn);
        char l[240];
        snprintf(l, sizeof l, "[MGPU][DSC] GetLatency t=%.2fs by %s -> %d | FIRST from this module",
                 now_s(), mn, r);
        mgpu::diag::info(l);
    }
    return r;
}

// Distinct (id, module) pairs seen through QueryInterface, logged once each.
constexpr unsigned QI_SEEN = 128u;
std::atomic<unsigned long long> g_qi_seen[QI_SEEN];   // (id << 32) | module index
std::atomic<unsigned> g_qi_seen_n{0};

bool qi_first(unsigned id, int ci)
{
    const unsigned long long key = ((unsigned long long)id << 32) | (unsigned)(ci + 1);
    const unsigned n = g_qi_seen_n.load(std::memory_order_acquire);
    for (unsigned i = 0; i < n && i < QI_SEEN; ++i)
        if (g_qi_seen[i].load(std::memory_order_relaxed) == key) return false;
    const unsigned slot = g_qi_seen_n.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= QI_SEEN) return false;
    g_qi_seen[slot].store(key, std::memory_order_release);
    return true;   // a racing duplicate can log twice; harmless
}

__declspec(noinline) void *__cdecl th_qi(unsigned int id)
{
    void *ra = _ReturnAddress();
    void *r = (g_real_qi != nullptr) ? g_real_qi(id) : nullptr;
    const HMODULE m = module_of(ra);
    const int ci = caller_index(m);
    if (ci >= 0) g_callers[ci].n[K_QI].fetch_add(1, std::memory_order_relaxed);

    // Hand out the thunk only when the driver handed out the address we
    // learned at install. Anything else passes through untouched and is said.
    void *out = r;
    const char *wrapped = "";
    if (r != nullptr)
    {
        if (id == ID_SET_SLEEP && r == (void *)g_real_set)     { out = (void *)&th_set;   wrapped = " -> WRAPPED"; }
        else if (id == ID_SLEEP && r == (void *)g_real_sleep)  { out = (void *)&th_sleep; wrapped = " -> WRAPPED"; }
        else if (id == ID_GET_STATUS && r == (void *)g_real_get) { out = (void *)&th_get; wrapped = " -> WRAPPED"; }
        else if (id == ID_LATENCY && r == (void *)g_real_lat)  { out = (void *)&th_lat;   wrapped = " -> WRAPPED"; }
        else if ((id == ID_SET_SLEEP || id == ID_SLEEP || id == ID_GET_STATUS || id == ID_LATENCY))
            wrapped = " -> NOT WRAPPED: the driver returned a different address than at install";
    }
    if (qi_first(id, ci) && line_budget())
    {
        char mn[96]; mod_name(m, mn, sizeof mn);
        char l[300];
        snprintf(l, sizeof l, "[MGPU][DSC] QueryInterface t=%.2fs id=0x%08X by %s -> %p%s",
                 now_s(), id, mn, r, wrapped);
        mgpu::diag::info(l);
    }
    return out;
}

// ---- the data scan (R102's technique, its own copy) ----
//
// POD only inside the structured handler. Hits are recorded first and logged
// by the caller afterwards.
struct hit
{
    HMODULE  mod;
    char     section[9];
    unsigned rva;
    unsigned what;           // index into the pair table
};
constexpr unsigned HIT_CAP = 64u;
hit g_hits[HIT_CAP];
unsigned g_hit_n = 0;
std::atomic<unsigned long long> g_hits_total{0};

struct pair_ent { void *find; void *repl; const char *name; };

unsigned scan_module(HMODULE mod, const pair_ent *pairs, unsigned npairs)
{
    if (mod == nullptr || mod == g_self || mod == g_nvapi) return 0;
    BYTE *base = (BYTE *)mod;
    unsigned hits = 0;
    __try
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        const unsigned nsec = nt->FileHeader.NumberOfSections;
        for (unsigned i = 0; i < nsec; ++i, ++sec)
        {
            if ((sec->Characteristics & IMAGE_SCN_MEM_WRITE) == 0) continue;
            if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) continue;
            BYTE *p = base + sec->VirtualAddress;
            SIZE_T len = (SIZE_T)sec->Misc.VirtualSize;
            if (len < sizeof(void *)) continue;
            len -= sizeof(void *);
            for (SIZE_T off = 0; off <= len; off += sizeof(void *))
            {
                void **slot = (void **)(p + off);
                void *v = *slot;
                for (unsigned k = 0; k < npairs; ++k)
                {
                    if (v != pairs[k].find) continue;
                    if (g_hit_n < HIT_CAP)
                    {
                        hit &h = g_hits[g_hit_n++];
                        h.mod = mod;
                        for (unsigned c = 0; c < 8u; ++c) h.section[c] = (char)sec->Name[c];
                        h.section[8] = '\0';
                        h.rva = (unsigned)(sec->VirtualAddress + (unsigned)off);
                        h.what = k;
                    }
                    DWORD old = 0;
                    if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
                    {
                        *slot = pairs[k].repl;
                        VirtualProtect(slot, sizeof(void *), old, &old);
                        ++hits;
                    }
                    break;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return hits;
    }
    return hits;
}

constexpr unsigned MOD_CAP = 1024u;
HMODULE g_mods[MOD_CAP];
unsigned g_mods_n = 0;   // the last scan's list, reused by uninstall

unsigned list_modules()
{
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 5 && snap == INVALID_HANDLE_VALUE; ++tries)
    {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snap == INVALID_HANDLE_VALUE) return 0;
    unsigned n = 0;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    if (Module32FirstW(snap, &me))
    {
        do { if (n < MOD_CAP) g_mods[n++] = me.hModule; } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return n;
}

// ---- install and rescan, on their own thread ----
std::atomic<bool> g_scanning{false};
std::atomic<bool> g_installed{false};
std::atomic<unsigned> g_scans{0};
std::atomic<long long> g_next_scan_qpc{0};
std::atomic<bool> g_said_no_nvapi{false};

bool learn_nvapi()
{
    HMODULE m = GetModuleHandleW(L"nvapi64.dll");
    if (m == nullptr)
    {
        if (!g_said_no_nvapi.exchange(true))
            mgpu::diag::info("[MGPU][DSC] nvapi64.dll is not resident yet - nothing in this process "
                             "has loaded NvAPI. Not loading it: discovery only observes. Retried at "
                             "every rescan.");
        return false;
    }
    mgpu::diag::info("[MGPU][DSC] step 1/3: nvapi64.dll resident, GetProcAddress(nvapi_QueryInterface)");
    pf_qi q = (pf_qi)(void *)GetProcAddress(m, "nvapi_QueryInterface");
    if (q == nullptr)
    {
        mgpu::diag::warn("[MGPU][DSC] nvapi_QueryInterface not exported - D1 stops here.");
        return false;
    }
    mgpu::diag::info("[MGPU][DSC] step 2/3: QueryInterface for SetSleepMode, Sleep, "
                     "GetSleepStatus, GetLatency (ids from the V21 Reflex block)");
    g_real_set   = (pf_dev_p)q(ID_SET_SLEEP);
    g_real_sleep = (pf_dev)  q(ID_SLEEP);
    g_real_get   = (pf_dev_p)q(ID_GET_STATUS);
    g_real_lat   = (pf_dev_p)q(ID_LATENCY);
    g_real_qi    = q;
    g_nvapi      = m;
    char l[300];
    snprintf(l, sizeof l, "[MGPU][DSC] step 3/3: QueryInterface=%p SetSleepMode=%p Sleep=%p "
             "GetSleepStatus=%p GetLatency=%p", (void *)q, (void *)g_real_set,
             (void *)g_real_sleep, (void *)g_real_get, (void *)g_real_lat);
    mgpu::diag::info(l);
    return true;
}

DWORD WINAPI scan_thread(LPVOID)
{
    if (!g_installed.load(std::memory_order_acquire))
    {
        if (!learn_nvapi()) { g_scanning.store(false, std::memory_order_release); return 0; }
        g_installed.store(true, std::memory_order_release);
    }
    pair_ent pairs[5];
    unsigned np = 0;
    if (g_real_qi)    pairs[np++] = { (void *)g_real_qi,    (void *)&th_qi,    "nvapi_QueryInterface" };
    if (g_real_set)   pairs[np++] = { (void *)g_real_set,   (void *)&th_set,   "NvAPI_D3D_SetSleepMode" };
    if (g_real_sleep) pairs[np++] = { (void *)g_real_sleep, (void *)&th_sleep, "NvAPI_D3D_Sleep" };
    if (g_real_get)   pairs[np++] = { (void *)g_real_get,   (void *)&th_get,   "NvAPI_D3D_GetSleepStatus" };
    if (g_real_lat)   pairs[np++] = { (void *)g_real_lat,   (void *)&th_lat,   "NvAPI_D3D_GetLatency" };

    const unsigned scan_no = g_scans.fetch_add(1, std::memory_order_relaxed) + 1u;
    const unsigned nm = list_modules();
    g_mods_n = nm;
    g_hit_n = 0;
    unsigned total = 0;
    for (unsigned i = 0; i < nm; ++i) total += scan_module(g_mods[i], pairs, np);
    g_hits_total.fetch_add(total, std::memory_order_relaxed);

    char l[400];
    snprintf(l, sizeof l, "[MGPU][DSC] scan %u t=%.2fs: %u module(s), %u word(s) swapped this scan, "
             "%llu in total.", scan_no, now_s(), nm, total,
             g_hits_total.load(std::memory_order_relaxed));
    mgpu::diag::info(l);
    for (unsigned i = 0; i < g_hit_n; ++i)
    {
        char mn[96]; mod_name(g_hits[i].mod, mn, sizeof mn);
        snprintf(l, sizeof l, "[MGPU][DSC]   swapped %s in %s section %s +0x%X",
                 pairs[g_hits[i].what].name, mn, g_hits[i].section, g_hits[i].rva);
        mgpu::diag::info(l);
    }
    g_scanning.store(false, std::memory_order_release);
    return 0;
}

// Rescan cadence: every 10 s for the first 5 minutes, then every 60 s.
long long scan_interval_qpc()
{
    const unsigned n = g_scans.load(std::memory_order_relaxed);
    return g_freq.QuadPart * (n < 30u ? 10 : 60);
}

void start_scan()
{
    bool expect = false;
    if (!g_scanning.compare_exchange_strong(expect, true, std::memory_order_acq_rel)) return;
    HANDLE h = CreateThread(nullptr, 0, &scan_thread, nullptr, 0, nullptr);
    if (h == nullptr) { g_scanning.store(false, std::memory_order_release); return; }
    CloseHandle(h);
}

// ---- the window line ----
std::atomic<long long> g_next_win_qpc{0};
std::atomic<unsigned> g_wins{0};

void window_line()
{
    char l[1600];
    int w = snprintf(l, sizeof l, "[MGPU][DSC] window t=%.1fs | presents per chain:", now_s());
    for (unsigned i = 0; i < CHAINS && w < (int)sizeof l - 80; ++i)
    {
        void *c = g_chains[i].chain.load(std::memory_order_acquire);
        if (c == nullptr) continue;
        const unsigned long long p = g_chains[i].presents.load(std::memory_order_relaxed);
        const unsigned long long a = g_chains[i].win_at.exchange(p, std::memory_order_relaxed);
        w += snprintf(l + w, sizeof l - (size_t)w, " %p=%llu%s", c, p - a,
                      g_chains[i].destroyed.load(std::memory_order_relaxed) ? "(destroyed)" : "");
    }
    w += snprintf(l + w, sizeof l - (size_t)w, " | NvAPI calls this window by module:");
    bool any = false;
    for (unsigned i = 0; i < CALLERS && w < (int)sizeof l - 160; ++i)
    {
        void *m = g_callers[i].mod.load(std::memory_order_acquire);
        if (m == nullptr) continue;
        unsigned long long d[K_KINDS];
        unsigned long long sum = 0;
        for (unsigned k = 0; k < K_KINDS; ++k)
        {
            const unsigned long long n = g_callers[i].n[k].load(std::memory_order_relaxed);
            d[k] = n - g_callers[i].win[k].exchange(n, std::memory_order_relaxed);
            sum += d[k];
        }
        if (sum == 0) continue;
        any = true;
        char mn[96]; mod_name((HMODULE)m, mn, sizeof mn);
        w += snprintf(l + w, sizeof l - (size_t)w, " %s[set=%llu sleep=%llu status=%llu lat=%llu qi=%llu]",
                      mn, d[K_SET], d[K_SLEEP], d[K_GET], d[K_LAT], d[K_QI]);
    }
    if (!any) w += snprintf(l + w, sizeof l - (size_t)w, " none");
    const unsigned long long full = g_callers_full.load(std::memory_order_relaxed);
    if (full != 0 && w < (int)sizeof l - 60)
        snprintf(l + w, sizeof l - (size_t)w, " | %llu call(s) from modules past the table", full);
    mgpu::diag::info(l);
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
    g_self = hm;
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
    const char *v = mgpu::config::find(buf, got, "Discovery");
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
        mgpu::diag::warn("[MGPU][DSC] DISCOVERY ON (Discovery=1). R&D scouting build: it swaps "
                         "NvAPI pointers inside other modules' data while they run, and it can "
                         "crash the game. Keep Reflex=0 so the map shows only the game. Installs "
                         "after 300 presents; rescans every 10 s for 5 minutes, then every 60 s.");
}
}   // namespace

void on_present(void *chain)
{
    if (g_mode.load(std::memory_order_relaxed) != 1) return;

    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    long long z = 0;
    g_t0.compare_exchange_strong(z, c.QuadPart, std::memory_order_relaxed);
    const unsigned long long all = g_presents_all.fetch_add(1, std::memory_order_relaxed) + 1ull;

    // Per chain. A chain first seen here rather than at init is said.
    bool found = false;
    for (unsigned i = 0; i < CHAINS && !found; ++i)
    {
        void *cur = g_chains[i].chain.load(std::memory_order_acquire);
        if (cur == chain) { g_chains[i].presents.fetch_add(1, std::memory_order_relaxed); found = true; }
        else if (cur == nullptr)
        {
            void *expect = nullptr;
            if (g_chains[i].chain.compare_exchange_strong(expect, chain, std::memory_order_acq_rel))
            {
                g_chains[i].presents.fetch_add(1, std::memory_order_relaxed);
                found = true;
                char l[200];
                snprintf(l, sizeof l, "[MGPU][DSC] chain %p first seen at present (no init seen) "
                         "t=%.2fs P=%llu", chain, now_s(), all);
                mgpu::diag::info(l);
            }
            else if (expect == chain)
            {
                g_chains[i].presents.fetch_add(1, std::memory_order_relaxed);
                found = true;
            }
        }
    }

    // Install at 300 presents, then the rescan cadence.
    if (all >= 300ull)
    {
        const long long next = g_next_scan_qpc.load(std::memory_order_relaxed);
        if (c.QuadPart >= next)
        {
            long long expect = next;
            if (g_next_scan_qpc.compare_exchange_strong(expect, c.QuadPart + scan_interval_qpc(),
                                                        std::memory_order_relaxed))
                start_scan();
        }
    }

    // Window line: every 10 s for the first 60 lines, then every 60 s.
    const long long nw = g_next_win_qpc.load(std::memory_order_relaxed);
    if (nw == 0)
    {
        long long expect = 0;
        g_next_win_qpc.compare_exchange_strong(expect, c.QuadPart + g_freq.QuadPart * 10,
                                               std::memory_order_relaxed);
    }
    else if (c.QuadPart >= nw)
    {
        const unsigned wn = g_wins.load(std::memory_order_relaxed);
        long long expect = nw;
        if (g_next_win_qpc.compare_exchange_strong(expect,
                c.QuadPart + g_freq.QuadPart * (wn < 60u ? 10 : 60), std::memory_order_relaxed))
        {
            g_wins.fetch_add(1, std::memory_order_relaxed);
            window_line();
        }
    }
}

void on_swapchain(void *chain, bool created, bool resize)
{
    ensure_mode();
    if (g_mode.load(std::memory_order_relaxed) != 1) return;

    if (created)
    {
        bool known = false;
        for (unsigned i = 0; i < CHAINS && !known; ++i)
        {
            void *cur = g_chains[i].chain.load(std::memory_order_acquire);
            if (cur == chain) { known = true; g_chains[i].destroyed.store(false); }
            else if (cur == nullptr)
            {
                void *expect = nullptr;
                if (g_chains[i].chain.compare_exchange_strong(expect, chain,
                                                              std::memory_order_acq_rel))
                    known = true;
            }
        }
    }
    else
    {
        for (unsigned i = 0; i < CHAINS; ++i)
            if (g_chains[i].chain.load(std::memory_order_acquire) == chain)
                g_chains[i].destroyed.store(true);
    }

    const unsigned inits = created ? g_chain_inits.fetch_add(1) + 1u : g_chain_inits.load();
    char l[300];
    snprintf(l, sizeof l, "[MGPU][DSC] swap chain %s %p resize=%d t=%.2fs P=%llu (init #%u)%s",
             created ? "INIT" : "DESTROY", chain, resize ? 1 : 0, now_s(),
             g_presents_all.load(std::memory_order_relaxed), inits,
             (created && !resize && inits > 1u)
                 ? " | a chain after the first: the bridge's own chain, or the game recreating its "
                   "chain (the frame-gen toggle signature). Rescan in 2 s."
                 : "");
    mgpu::diag::info(l);

    // A new chain may come with newly resolved pointers: rescan soon.
    if (created && g_installed.load(std::memory_order_acquire))
    {
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        g_next_scan_qpc.store(c.QuadPart + g_freq.QuadPart * 2, std::memory_order_relaxed);
    }
}
void uninstall()
{
    if (!g_installed.load(std::memory_order_acquire)) return;
    // No new scan may start, and no new write is made by one. A scan that is
    // already running finishes on its own thread; this does not wait for it
    // (DllMain holds the loader lock).
    g_mode.store(0, std::memory_order_release);
    pair_ent back[5];
    unsigned np = 0;
    if (g_real_qi)    back[np++] = { (void *)&th_qi,    (void *)g_real_qi,    "nvapi_QueryInterface" };
    if (g_real_set)   back[np++] = { (void *)&th_set,   (void *)g_real_set,   "NvAPI_D3D_SetSleepMode" };
    if (g_real_sleep) back[np++] = { (void *)&th_sleep, (void *)g_real_sleep, "NvAPI_D3D_Sleep" };
    if (g_real_get)   back[np++] = { (void *)&th_get,   (void *)g_real_get,   "NvAPI_D3D_GetSleepStatus" };
    if (g_real_lat)   back[np++] = { (void *)&th_lat,   (void *)g_real_lat,   "NvAPI_D3D_GetLatency" };
    // The last scan's module list: no snapshot under the loader lock. A module
    // unloaded since then faults inside scan_module's handler and is skipped.
    g_hit_n = 0;
    unsigned restored = 0;
    for (unsigned i = 0; i < g_mods_n; ++i) restored += scan_module(g_mods[i], back, np);
    char l[200];
    snprintf(l, sizeof l, "[MGPU][DSC] uninstall: %u word(s) put back to the real NvAPI addresses "
             "(%llu swapped by the scans; the rest were handed out by QueryInterface).",
             restored, g_hits_total.load(std::memory_order_relaxed));
    mgpu::diag::info(l);
}
}   // namespace mgpu::discovery
