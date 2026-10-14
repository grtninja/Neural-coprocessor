// MGPU Bridge 0.2.6-dev - T1.16 GPU 0 timestamps for the producer. See producer_gpu_timers.hpp.
#include "producer_gpu_timers.hpp"
#include "producer_diag.hpp"
#include "diag.hpp"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace mgpu::p0t
{
namespace
{
    // Eight frames in flight is more than the ring's six: a frame's four query slots are reused only after the bridge
    // thread has had two more frames to read them. A slot that is still pending when it would be reused is counted
    // invalid, never overwritten silently.
    const unsigned RINGT = 8;
    const unsigned PER_FRAME = 6;

    struct Slot
    {
        unsigned long long pending_frame = 0;      // written by the producer thread under the stream lock, read by poll() under the same lock
        bool               pending_mvec = false;
    };

    std::atomic<bool> g_on{false};
    ID3D12QueryHeap *g_heap = nullptr;
    ID3D12Resource *g_rb = nullptr;
    const unsigned long long *g_map = nullptr;
    double g_hz = 0.0;
    Slot g_slot[RINGT];
    std::atomic<unsigned long long> g_mvec_frame[RINGT];   // frame id whose MVec marks were really recorded (render thread, lock-free)

    inline unsigned idx(unsigned long long frame) { return (unsigned)(frame % RINGT); }
    void say(const char *s) { mgpu::diag::info(s); }
    void warn(const char *s) { mgpu::diag::warn(s); }
}

bool active() { return g_on.load(std::memory_order_acquire); }

bool init(void *gdev_v, void *gq_v)
{
    if (g_on.load(std::memory_order_acquire)) return true;
    ID3D12Device *dev = reinterpret_cast<ID3D12Device *>(gdev_v);
    ID3D12CommandQueue *gq = reinterpret_cast<ID3D12CommandQueue *>(gq_v);
    if (dev == nullptr || gq == nullptr)
    { warn("[MGPU][R300h] GPU0 TIMERS OFF: no game device or queue to create them on."); return false; }

    UINT64 hz = 0;
    if (FAILED(gq->GetTimestampFrequency(&hz)) || hz == 0)
    { warn("[MGPU][R300h] GPU0 TIMERS OFF: the game's queue reports no timestamp frequency."); return false; }

    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = RINGT * PER_FRAME;
    qd.NodeMask = 0;
    HRESULT hr = dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_heap));
    if (FAILED(hr) || g_heap == nullptr)
    {
        char b[200]; snprintf(b, sizeof b, "[MGPU][R300h] GPU0 TIMERS OFF: CreateQueryHeap failed hr=0x%08X.", (unsigned)hr);
        warn(b); g_heap = nullptr; return false;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (UINT64)RINGT * PER_FRAME * sizeof(UINT64);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_rb));
    if (FAILED(hr) || g_rb == nullptr)
    {
        char b[200]; snprintf(b, sizeof b, "[MGPU][R300h] GPU0 TIMERS OFF: readback buffer failed hr=0x%08X.", (unsigned)hr);
        warn(b); g_heap->Release(); g_heap = nullptr; g_rb = nullptr; return false;
    }
    void *p = nullptr;
    D3D12_RANGE none{0, 0};                               // we never write through this mapping
    if (FAILED(g_rb->Map(0, &none, &p)) || p == nullptr)
    {
        warn("[MGPU][R300h] GPU0 TIMERS OFF: could not map the readback buffer.");
        g_rb->Release(); g_rb = nullptr; g_heap->Release(); g_heap = nullptr; return false;
    }
    g_map = static_cast<const unsigned long long *>(p);
    g_hz = (double)hz;
    for (unsigned i = 0; i < RINGT; ++i) { g_slot[i] = Slot{}; g_mvec_frame[i].store(0ull, std::memory_order_relaxed); }
    g_on.store(true, std::memory_order_release);

    char b[480];
    snprintf(b, sizeof b,
             "[MGPU][R300h] GPU0 TIMERS ON: query heap of %u timestamps + %u-byte readback on the GAME's device, timestamp clock %.0f MHz. "
             "Per frame: 6 EndQuery + up to 2 ResolveQueryData in the game's own lists; no barrier, fence, queue or wait added; only the bridge thread reads.",
             RINGT * PER_FRAME, (unsigned)(RINGT * PER_FRAME * sizeof(UINT64)), g_hz / 1.0e6);
    say(b);
    return true;
}

void shutdown()
{
    g_on.store(false, std::memory_order_release);
    if (g_rb != nullptr) { D3D12_RANGE none{0, 0}; g_rb->Unmap(0, &none); g_rb->Release(); g_rb = nullptr; }
    g_map = nullptr;
    if (g_heap != nullptr) { g_heap->Release(); g_heap = nullptr; }
}

void mark(void *cl_v, unsigned long long frame, int which)
{
    if (!g_on.load(std::memory_order_acquire) || cl_v == nullptr || which < 0 || which >= (int)PER_FRAME) return;
    ID3D12GraphicsCommandList *cl = reinterpret_cast<ID3D12GraphicsCommandList *>(cl_v);
    cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, idx(frame) * PER_FRAME + (unsigned)which);
}

void note_mvec_recorded(unsigned long long frame)
{
    if (!g_on.load(std::memory_order_acquire)) return;
    g_mvec_frame[idx(frame)].store(frame, std::memory_order_release);
}

void resolve_frame(void *cl_v, unsigned long long frame)
{
    if (!g_on.load(std::memory_order_acquire) || cl_v == nullptr) return;
    ID3D12GraphicsCommandList *cl = reinterpret_cast<ID3D12GraphicsCommandList *>(cl_v);
    const unsigned i = idx(frame);
    Slot &sl = g_slot[i];
    if (sl.pending_frame != 0ull)                         // the bridge thread has not read the previous occupant: do not overwrite it
    { mgpu::pdiag::note_gpu_invalid(0); return; }
    const unsigned base = i * PER_FRAME;
    cl->ResolveQueryData(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, base, 4, g_rb, (UINT64)base * sizeof(UINT64));       // marks 0..3
    const bool mv = (g_mvec_frame[i].load(std::memory_order_acquire) == frame);
    if (mv)
        cl->ResolveQueryData(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, base + 4, 2, g_rb, (UINT64)(base + 4) * sizeof(UINT64));   // marks 4..5
    sl.pending_frame = frame;
    sl.pending_mvec = mv;
}

void poll(unsigned long long completed)
{
    if (!g_on.load(std::memory_order_acquire) || g_map == nullptr) return;
    for (unsigned i = 0; i < RINGT; ++i)
    {
        Slot &sl = g_slot[i];
        if (sl.pending_frame == 0ull || sl.pending_frame > completed) continue;   // the game's fence has not reached it: leave it
        const unsigned base = i * PER_FRAME;
        mgpu::pdiag::note_gpu_block(0, g_map[base + 0], g_map[base + 3], g_hz);   // whole block
        mgpu::pdiag::note_gpu_block(2, g_map[base + 0], g_map[base + 1], g_hz);   // seal copy + barrier + drain of the game's rendering
        mgpu::pdiag::note_gpu_block(3, g_map[base + 1], g_map[base + 2], g_hz);   // colour copy (+ barrier back)
        mgpu::pdiag::note_gpu_block(4, g_map[base + 2], g_map[base + 3], g_hz);   // depth copy
        if (sl.pending_mvec) mgpu::pdiag::note_gpu_block(1, g_map[base + 4], g_map[base + 5], g_hz);   // MVec copy
        sl.pending_frame = 0ull; sl.pending_mvec = false;
    }
}
} // namespace mgpu::p0t
