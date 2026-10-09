// MGPU Bridge - D2.0: the log queue. See log_queue.hpp.
//
// The ring is a bounded multi-producer queue in the usual sequence-number
// form: every slot carries a sequence. A producer may take position p only
// while its slot's sequence equals p; it then writes and publishes p + 1. The
// single consumer reads position p when the sequence equals p + 1 and frees
// the slot by setting p + SLOTS. A full ring is seen by the producer as a
// sequence behind its position: the line is dropped, no position is consumed,
// so the consumer never waits on a hole.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "log_queue.hpp"
#include "journal.hpp"
#include "diag.hpp"

namespace mgpu::lq
{
namespace
{
// D2.0-5: every mgpu::diag line now passes through here, not only D2.0's.
// The stream's own lines run to ~1.5 KB and the arm's census is ~200 lines in
// one burst: 2048 lines of 2 KB (4 MB, static).
constexpr unsigned long long SLOTS = 2048ull;
constexpr unsigned LINE = 2048u;

struct slot
{
    std::atomic<unsigned long long> seq{0};
    unsigned d = 0;
    char text[LINE];
};
slot g_ring[SLOTS];
std::atomic<bool> g_init{false};
std::atomic<unsigned long long> g_head{0};
unsigned long long g_tail = 0;                  // worker only
std::atomic<unsigned long long> g_dropped{0};
std::atomic<bool> g_started{false};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_loop_left{false};
std::atomic<HANDLE> g_wake{nullptr};

void ring_init()
{
    // Sequences start at their index. Done once, before the first post can
    // read them (post calls this; the flag makes it idempotent and the
    // release/acquire pair publishes the stores).
    static std::atomic<int> state{0};          // 0 not done, 1 in progress, 2 done
    int expect = 0;
    if (state.compare_exchange_strong(expect, 1, std::memory_order_acq_rel))
    {
        for (unsigned long long i = 0; i < SLOTS; ++i) g_ring[i].seq.store(i, std::memory_order_relaxed);
        state.store(2, std::memory_order_release);
        g_init.store(true, std::memory_order_release);
        return;
    }
    while (state.load(std::memory_order_acquire) != 2) YieldProcessor();
}

void deliver(unsigned d, const char *text)
{
    // *_direct: diag::info / warn / error would post back into this queue.
    if (d == TO_JOURNAL)    mgpu::journal::write_raw(text);
    else if (d == TO_WARN)  mgpu::diag::warn_direct(text);
    else if (d == TO_ERROR) mgpu::diag::error_direct(text);
    else                    mgpu::diag::info_direct(text);
}

void drain()
{
    for (;;)
    {
        slot &s = g_ring[g_tail % SLOTS];
        if (s.seq.load(std::memory_order_acquire) != g_tail + 1ull) return;   // nothing published here yet
        deliver(s.d, s.text);
        s.seq.store(g_tail + SLOTS, std::memory_order_release);              // free for the next lap
        ++g_tail;
    }
}

DWORD WINAPI worker(LPVOID)
{
    unsigned long long reported = 0;
    while (!g_stop.load(std::memory_order_acquire))
    {
        HANDLE w = g_wake.load(std::memory_order_acquire);
        if (w != nullptr) WaitForSingleObject(w, 20);
        else Sleep(20);
        drain();
        const unsigned long long dr = g_dropped.load(std::memory_order_relaxed);
        if (dr != reported)
        {
            char l[160];
            snprintf(l, sizeof l, "[MGPU][D20] log queue: %llu line(s) dropped so far (ring full)", dr);
            mgpu::diag::warn_direct(l);
            reported = dr;
        }
    }
    drain();
    g_loop_left.store(true, std::memory_order_release);
    return 0;
}
}   // namespace

void post(dest d, const char *line)
{
    if (line == nullptr) return;
    if (!g_init.load(std::memory_order_acquire)) ring_init();
    // The worker starts with the first line, so it exists only when a D2.0
    // feature is on. Every poster is a D2.0 module initialised from
    // on_init_swapchain, after ReShade's add-on load/unload cycles.
    if (!g_started.load(std::memory_order_acquire)) start();
    unsigned long long pos = g_head.load(std::memory_order_relaxed);
    for (;;)
    {
        slot &s = g_ring[pos % SLOTS];
        const unsigned long long seq = s.seq.load(std::memory_order_acquire);
        const long long diff = (long long)(seq - pos);
        if (diff == 0)
        {
            if (g_head.compare_exchange_weak(pos, pos + 1ull, std::memory_order_relaxed))
            {
                s.d = (unsigned)d;
                size_t n = strlen(line);
                if (n >= LINE) n = LINE - 1u;
                memcpy(s.text, line, n);
                s.text[n] = '\0';
                s.seq.store(pos + 1ull, std::memory_order_release);
                break;
            }
            // pos was reloaded by the failed exchange; try again
        }
        else if (diff < 0)
        {
            g_dropped.fetch_add(1, std::memory_order_relaxed);   // full: drop, consume nothing
            return;
        }
        else
            pos = g_head.load(std::memory_order_relaxed);
    }
    HANDLE w = g_wake.load(std::memory_order_acquire);
    if (w != nullptr) SetEvent(w);
}

void start()
{
    if (g_stop.load(std::memory_order_acquire)) return;   // stopped: only rearm() allows a new worker
    bool expect = false;
    if (!g_started.compare_exchange_strong(expect, true, std::memory_order_acq_rel)) return;
    if (!g_init.load(std::memory_order_acquire)) ring_init();
    g_wake.store(CreateEventW(nullptr, FALSE, FALSE, nullptr), std::memory_order_release);
    HANDLE h = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr);
    if (h != nullptr) CloseHandle(h);
}

bool running()
{
    return g_started.load(std::memory_order_acquire) && !g_stop.load(std::memory_order_acquire);
}

void signal_stop()
{
    g_stop.store(true, std::memory_order_release);
    HANDLE w = g_wake.load(std::memory_order_acquire);
    if (w != nullptr) SetEvent(w);
}

void rearm()
{
    if (!g_stop.load(std::memory_order_acquire)) return;
    if (g_started.load(std::memory_order_acquire) && !g_loop_left.load(std::memory_order_acquire)) return;
    g_loop_left.store(false, std::memory_order_release);
    g_started.store(false, std::memory_order_release);
    g_stop.store(false, std::memory_order_release);
}

void stop(bool process_exit)
{
    // Always set, even if no worker ever ran: a post after this must not
    // start one inside the unload window.
    g_stop.store(true, std::memory_order_release);
    if (!g_started.load(std::memory_order_acquire)) return;
    HANDLE w = g_wake.load(std::memory_order_acquire);
    if (w != nullptr) SetEvent(w);
    if (process_exit) return;   // every other thread is already gone
    for (int i = 0; i < 30 && !g_loop_left.load(std::memory_order_acquire); ++i) Sleep(10);
}
}   // namespace mgpu::lq
