// nrcheck.exe - runs the add-on's self-healing arm outside a game, to find
// which retry shape brings DLSS-NR up on a machine where the first arm fails.
//
// Run with no arguments. For every NVIDIA GPU as the neural GPU it runs one
// fresh process shaped like a game: the other GPU's device first and kept
// alive, then the neural device, then up to eight arms, each changing one
// thing, stopping at the first success. See nrcheck_core.cpp for the ladder.
// Output goes to the console and to nrcheck_report.txt beside the exe.
//
// Internal: nrcheck.exe --case N --neural I --other J --snippet P --log L
// is how the parent starts each child. Not meant to be typed by hand.

#include <windows.h>
#include <dxgi1_4.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

// ---- SYNTHETIC FORWARDER ----
// Called by nvngx.dll_nrcheck.dll for synthetic attempts only. Its whole job
// is to be the module NGX sees as the caller: nrcheck.exe has no "nvngx.dll"
// in its path, so nvngx_dlssnr.dll refuses the call with 0xBAD00002. The
// result goes through a volatile on purpose: a tail call would make the
// return address the DLL's again and the refusal would not happen.
typedef unsigned (*ngx_create_raw)(void *cl, int feat, void *params, void **h);
extern "C" __declspec(dllexport) __declspec(noinline)
unsigned nrcheck_foreign_create(void *fn, void *cl, int feat, void *params, void **h)
{
    volatile unsigned r = ((ngx_create_raw)fn)(cl, feat, params, h);
    return r;
}

namespace
{
    std::wstring g_dir;        // exe directory, with trailing backslash
    std::wstring g_report;     // report path
    std::wstring g_details;    // where the children write; folded into the report at the end
    std::string  g_head;       // header + summary: the top of the report
    int          g_synthetic = 0;   // --synthetic N: first N arms made to fail by NGX itself

    // The report is written ONCE, at the end, summary first and the NGX
    // return codes after it under their own heading. A reader - or an AI fed
    // the file - meets the verdict before any start-up code, and the codes
    // are labelled as details rather than as findings.
    void say(const char *fmt, ...)
    {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        fputs(buf, stdout); fputc('\n', stdout); fflush(stdout);
        g_head += buf;
        g_head += "\n";
    }

    void write_report()
    {
        std::string details;
        FILE *f = nullptr;
        if (_wfopen_s(&f, g_details.c_str(), L"rb") == 0 && f != nullptr)
        {
            char b[4096]; size_t n;
            while ((n = fread(b, 1, sizeof b, f)) > 0) details.append(b, n);
            fclose(f);
        }
        DeleteFileW(g_details.c_str());
        if (_wfopen_s(&f, g_report.c_str(), L"wb") == 0 && f != nullptr)
        {
            fputs(g_head.c_str(), f);
            fputs("\n==================== DETAILS ====================\n"
                  "Everything below is the raw record of each try, including NGX start-up\n"
                  "codes such as FAIL_OutOfDate. They are NOT the verdict - the summary\n"
                  "above is. Only tries that reached CreateFeature count.\n\n", f);
            fputs(details.c_str(), f);
            fclose(f);
        }
    }

    bool exists(const std::wstring &p)
    {
        const DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::string file_version(const std::wstring &p)
    {
        DWORD h = 0, n = GetFileVersionInfoSizeW(p.c_str(), &h);
        if (n == 0) return "unknown";
        std::vector<BYTE> b(n);
        if (!GetFileVersionInfoW(p.c_str(), 0, n, b.data())) return "unknown";
        VS_FIXEDFILEINFO *fi = nullptr; UINT l = 0;
        if (!VerQueryValueW(b.data(), L"\\", (void **)&fi, &l) || fi == nullptr) return "unknown";
        char s[64];
        snprintf(s, sizeof s, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                 HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        return s;
    }

    long long file_size(const std::wstring &p)
    {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return -1;
        return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    }

    struct gpu { UINT index; std::wstring name; std::string driver; LUID luid; };

    // 32.0.16.1714 -> 617.14
    std::string nv_driver(IDXGIAdapter1 *a)
    {
        LARGE_INTEGER v{};
        if (FAILED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) return "unknown";
        const unsigned c = HIWORD(v.LowPart), d = LOWORD(v.LowPart);
        const unsigned n = (c % 10) * 10000 + d;
        char s[64];
        snprintf(s, sizeof s, "%u.%02u (%u.%u.%u.%u)", n / 100, n % 100,
                 HIWORD(v.HighPart), LOWORD(v.HighPart), c, d);
        return s;
    }

    std::vector<gpu> nvidia_gpus()
    {
        std::vector<gpu> out;
        IDXGIFactory1 *f = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return out;
        IDXGIAdapter1 *a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; ++i)
        {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                out.push_back({ i, d.Description, nv_driver(a), d.AdapterLuid });
            a->Release();
        }
        f->Release();
        return out;
    }

    int run_child(int c, UINT neural, UINT other, const std::wstring &snip)
    {
        wchar_t exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --case " + std::to_wstring(c) +
            L" --neural " + std::to_wstring(neural) + L" --other " + std::to_wstring(other) +
            L" --snippet \"" + snip + L"\" --log \"" + g_details + L"\"" +
            L" --synthetic " + std::to_wstring(g_synthetic);
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');
        STARTUPINFOW si{}; si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            g_dir.c_str(), &si, &pi))
            return -1;
        DWORD code = 0xFFFFFFFF;
        if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_TIMEOUT)
        {
            TerminateProcess(pi.hProcess, 0xDEAD);
            code = 0xDEAD;
        }
        else GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return (int)code;
    }

    // NGX writes nvngx.log and nvngx_dlssnr_<ver>.log into the data path and
    // rewrites them in every process, so without this only the last case's
    // logs would survive. Each case's pair is renamed after it finishes.
    void keep_ngx_logs(int c, UINT adapter)
    {
        const std::wstring tag = L"_start" + std::to_wstring(c) + L"_adapter" + std::to_wstring(adapter);
        const std::wstring core = g_dir + L"nvngx.log";
        if (exists(core))
            MoveFileExW(core.c_str(), (g_dir + L"nvngx" + tag + L".log").c_str(), MOVEFILE_REPLACE_EXISTING);
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((g_dir + L"nvngx_dlssnr_*.log").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            std::wstring n = fd.cFileName;
            if (n.find(L"_start") != std::wstring::npos) continue;   // already kept
            std::wstring stem = n.substr(0, n.size() - 4);
            MoveFileExW((g_dir + n).c_str(), (g_dir + stem + tag + L".log").c_str(), MOVEFILE_REPLACE_EXISTING);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    const char *word(int code)
    {
        switch (code)
        {
        case 0:  return "PASS";
        case 1:  return "FAIL";
        case 2:  return "CRASH";
        case 3:  return "SKIPPED";
        case 4:  return "NO PARAMETERS";
        case 0xDEAD: return "TIMEOUT";
        default: return "PROCESS DIED";
        }
    }

    int child_main(int argc, wchar_t **argv)
    {
        int c = 1; int n = 0; int o = 0; std::wstring snip, log;
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (!wcscmp(argv[i], L"--case"))    c = _wtoi(argv[++i]);
            else if (!wcscmp(argv[i], L"--neural"))  n = _wtoi(argv[++i]);
            else if (!wcscmp(argv[i], L"--other"))   o = _wtoi(argv[++i]);
            else if (!wcscmp(argv[i], L"--snippet")) snip = argv[++i];
            else if (!wcscmp(argv[i], L"--log"))     log = argv[++i];
            else if (!wcscmp(argv[i], L"--synthetic")) g_synthetic = _wtoi(argv[++i]);
        }
        HMODULE m = LoadLibraryW((g_dir + L"nvngx.dll_nrcheck.dll").c_str());
        if (m == nullptr) { printf("nvngx.dll_nrcheck.dll not found beside nrcheck.exe\n"); return 3; }
        typedef int (*run_fn)(int, int, int, const wchar_t *, const wchar_t *, int);
        run_fn run = (run_fn)GetProcAddress(m, "nrcheck_run");
        if (run == nullptr) return 3;
        return run(c, n, o, snip.c_str(), log.c_str(), g_synthetic);
    }
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    g_dir = exe;
    g_dir.erase(g_dir.find_last_of(L'\\') + 1);
    g_report = g_dir + L"nrcheck_report.txt";
    g_details = g_dir + L"nrcheck_details.tmp";

    for (int i = 1; i < argc; ++i)
        if (!wcscmp(argv[i], L"--case")) return child_main(argc, argv);

    // R268: --bench. The launcher's test, in this process (no ladder, no child):
    //   nrcheck.exe --bench --neural <adapter index> [--other <index>] --res WxH
    //               [--passes 1|2] [--frames N] [--snippet path] [--out file]
    // Writes nrbench_result.ini beside the exe unless --out says otherwise.
    for (int i = 1; i < argc; ++i)
        if (!wcscmp(argv[i], L"--bench"))
        {
            int n = -1, o = -1; unsigned w = 0, hh = 0, passes = 1, frames = 300;
            std::wstring snip, outp = g_dir + L"nrbench_result.ini", log = g_dir + L"nrbench_log.txt";
            for (int j = 1; j + 1 < argc; ++j)
            {
                if (!wcscmp(argv[j], L"--neural")) n = _wtoi(argv[++j]);
                else if (!wcscmp(argv[j], L"--other")) o = _wtoi(argv[++j]);
                else if (!wcscmp(argv[j], L"--res")) swscanf_s(argv[++j], L"%ux%u", &w, &hh);
                else if (!wcscmp(argv[j], L"--passes")) passes = (unsigned)_wtoi(argv[++j]);
                else if (!wcscmp(argv[j], L"--frames")) frames = (unsigned)_wtoi(argv[++j]);
                else if (!wcscmp(argv[j], L"--snippet")) snip = argv[++j];
                else if (!wcscmp(argv[j], L"--out")) outp = argv[++j];
            }
            if (snip.empty())
            {
                if (exists(g_dir + L"mgpu\\nvngx_dlssnr.dll")) snip = g_dir + L"mgpu\\nvngx_dlssnr.dll";
                else if (exists(g_dir + L"nvngx_dlssnr.dll")) snip = g_dir + L"nvngx_dlssnr.dll";
            }
            if (n < 0 || w == 0 || hh == 0 || snip.empty())
            {
                printf("usage: nrcheck.exe --bench --neural <adapter index> [--other <index>] --res WxH [--passes 1|2] [--frames N] [--snippet path] [--out file]\n");
                return 3;
            }
            DeleteFileW(log.c_str());
            HMODULE m = LoadLibraryW((g_dir + L"nvngx.dll_nrcheck.dll").c_str());
            if (m == nullptr) { printf("nvngx.dll_nrcheck.dll not found beside nrcheck.exe\n"); return 3; }
            typedef int (*bench_fn)(int, int, const wchar_t *, const wchar_t *, unsigned, unsigned, unsigned, unsigned, const wchar_t *);
            bench_fn run = (bench_fn)GetProcAddress(m, "nrbench_run");
            if (run == nullptr) { printf("nrbench_run not exported\n"); return 3; }
            return run(n, o, snip.c_str(), log.c_str(), w, hh, passes, frames, outp.c_str());
        }

    std::wstring snip;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--snippet")) snip = argv[i + 1];
        if (!wcscmp(argv[i], L"--synthetic")) g_synthetic = _wtoi(argv[i + 1]);
    }
    if (g_synthetic < 0) g_synthetic = 0;
    if (g_synthetic > 7) g_synthetic = 7;   // attempt 8 must stay real
    if (snip.empty())
    {
        if (exists(g_dir + L"mgpu\\nvngx_dlssnr.dll")) snip = g_dir + L"mgpu\\nvngx_dlssnr.dll";
        else if (exists(g_dir + L"nvngx_dlssnr.dll")) snip = g_dir + L"nvngx_dlssnr.dll";
    }

    DeleteFileW(g_report.c_str());
    DeleteFileW(g_details.c_str());
    SYSTEMTIME t; GetLocalTime(&t);
    say("MGPU Bridge NR check 3.2 - %04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);

    if (snip.empty())
    {
        say("nvngx_dlssnr.dll NOT FOUND. Put nrcheck.exe in the game folder beside the mgpu\\ "
            "folder, or run: nrcheck.exe --snippet \"C:\\path\\to\\nvngx_dlssnr.dll\"");
        write_report();
        return 3;
    }
    say("nvngx_dlssnr.dll: %ls | version %s | %lld bytes", snip.c_str(),
        file_version(snip).c_str(), file_size(snip));

    const std::vector<gpu> g = nvidia_gpus();
    for (const gpu &x : g)
        say("GPU adapter[%u] %ls | driver %s | luid %08lX-%08lX", x.index, x.name.c_str(),
            x.driver.c_str(), (unsigned long)x.luid.HighPart, (unsigned long)x.luid.LowPart);
    if (g.empty()) { say("No NVIDIA GPU found."); write_report(); return 3; }

    struct row { UINT idx; std::wstring name; int code; int setups; };
    std::vector<row> rows;
    const bool multi = g.size() > 1;
    const int kSetupTries = 10;
    for (size_t i = 0; i < g.size(); ++i)
    {
        const UINT other = multi ? g[(i + 1) % g.size()].index : g[i].index;
        int code = 5, tries = 0;
        // Code 5 means NGX was not usable in that process and nothing reached
        // CreateFeature - a fact about the check's start-up, not about this
        // machine. A fresh process is the thing that has cleared it.
        while (code == 5 && tries < kSetupTries)
        {
            ++tries;
            printf("\n");
            code = run_child(multi ? 1 : 3, g[i].index, other, snip);
            keep_ngx_logs(tries, g[i].index);
        }
        rows.push_back({ g[i].index, g[i].name, code, tries });
    }

    static const char *const kWhat[9] = {
        "", "first try",
        "same NGX session, 2 s later",
        "cleaned up and started NGX again",
        "cleaned up and started NGX again, after 2 s",
        "cleaned up and started NGX again, after 4 s",
        "cleaned up and started NGX again, after 8 s",
        "new device on this GPU",
        "old devices released, new device on this GPU" };

    say("");
    say("==================== SUMMARY ====================");
    say("Only tries that reached DLSS-NR creation (CreateFeature) count here.");
    if (g_synthetic > 0)
        say("SYNTHETIC RUN: tries 1-%d were refused on purpose by NGX (0xBAD00002). "
            "A healthy PC should read HEALED on try %d.", g_synthetic, g_synthetic + 1);
    for (const row &r : rows)
    {
        if (r.code == 0)
            say("adapter[%u] %ls: PASS - DLSS-NR was created on the first try.", r.idx, r.name.c_str());
        else if (r.code >= 12 && r.code <= 18)
            say("adapter[%u] %ls: HEALED - DLSS-NR was created on try %d (%s).", r.idx, r.name.c_str(),
                r.code - 10, kWhat[r.code - 10]);
        else if (r.code == 1)
            say("adapter[%u] %ls: NOT HEALED - DLSS-NR creation was reached and failed on every try.",
                r.idx, r.name.c_str());
        else if (r.code == 2)
            say("adapter[%u] %ls: CRASH - NGX crashed during DLSS-NR creation. The check stopped.",
                r.idx, r.name.c_str());
        else if (r.code == 6)
            say("adapter[%u] %ls: SYNTHETIC INVALID - NGX did not refuse the synthetic try as expected. "
                "This reference run cannot be used. See the details.", r.idx, r.name.c_str());
        else if (r.code == 5)
            say("adapter[%u] %ls: NOT REACHED - the check could not get far enough to try DLSS-NR "
                "creation (%d attempts). This result says nothing about this PC.",
                r.idx, r.name.c_str(), r.setups);
        else
            say("adapter[%u] %ls: %s - the check could not run. See the details.",
                r.idx, r.name.c_str(), word(r.code));
        if (r.setups > 1 && r.code != 5)
            say("  (the check needed %d start-ups to reach DLSS-NR creation; that is about the check, not this PC)",
                r.setups);
    }
    say("Please attach nrcheck_results.zip (or nrcheck_report.txt) to your GitHub issue.");
    write_report();
    return 0;
}
