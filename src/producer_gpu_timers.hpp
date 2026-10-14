// MGPU Bridge 0.2.6-dev - T1.16 GPU 0 TIMESTAMPS FOR THE PRODUCER. Windows/D3D12 only. MEASUREMENT ONLY.
//
// Brackets, with GPU timestamps on the GAME's device, what the bridge's producer records into the game's own command lists.
// Six timestamps per frame (marks 0..5):
//   0  start of the end-of-frame block (before the seal copy)                            (stream_on_finish_effects)
//   1  AFTER the first barrier (render target -> copy source), just before the colour copy
//   2  after the colour copy and the barrier back, just before the depth copy
//   3  after the depth copy = end of the block
//   4  before the MVec copy recorded at the game's DLSS Evaluate                          (stream_mvec_copy)
//   5  after the MVec copy
// WHY 1 AND 2 EXIST: that first barrier waits for the game's own rendering into the render target to finish. A single bracket from
// mark 0 to mark 3 would put that drain - the GAME's work - into the bridge's cost. 0->1 isolates seal copy + barrier + drain;
// 1->2 is the colour copy alone; 2->3 the depth copy alone.
//
// WHAT IT ADDS TO THE GAME'S LIST, AND WHAT IT DOES NOT:
//   4 x EndQuery(TIMESTAMP) per frame, 1-2 x ResolveQueryData (16 bytes each) into a READBACK buffer. No barrier, no fence,
//   no queue, no wait. The readback is mapped once and read ONLY by the bridge thread, and only after the game's own fence
//   says the frame is complete (poll()), so the game's thread never waits for it and nothing else is ordered after it.
//   Gpu0Timers=0 inserts none of it.
//
// No D3D12 headers here: the pure bookkeeping is in producer_diag.{hpp,cpp}, which this file feeds.
#pragma once

namespace mgpu::p0t
{
    // gdev = ID3D12Device* of the GAME (borrowed), gq = ID3D12CommandQueue* the producer signals on. Returns false and logs why
    // if any step fails; every other function is then a no-op.
    bool init(void *gdev, void *gq);
    void shutdown();
    bool active();

    // which: 0..5 as listed above. `frame` is the seal index the frame travels with.
    void mark(void *cmd_list, unsigned long long frame, int which);

    // The MVec copy for `frame` was really recorded (its two marks are meaningful). Called by stream_mvec_copy.
    void note_mvec_recorded(unsigned long long frame);

    // Records the resolve of this frame's timestamps into the readback buffer, in the same list, after block 0's end mark.
    void resolve_frame(void *cmd_list, unsigned long long frame);

    // Bridge thread, under the lock stream_poll already holds: reads every frame whose fence value has completed.
    // Non-blocking by construction - it never calls anything that waits.
    void poll(unsigned long long game_fence_completed);
}
