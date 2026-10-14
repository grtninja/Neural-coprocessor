// MGPU Bridge 0.2.6-dev - T1.9 temporal diagnostic + gated T1.10 experiment.
//
// Dedicated module (the author's gpu1_context.cpp is already ~21k lines).
// Log ids: [R300a1] per-evaluate line, [R300a2] periodic summary, [R300a4] config,
// [R300a5] findings. Pure C++ standard library apart from diag.hpp, so the
// policy and the counters are unit-testable off-device (tests/temporal_diag_test.cpp).
//
// DEFAULTS REPRODUCE T1.8 EXACTLY: TemporalMV=multiply, TemporalResetK=0,
// TemporalRequireTuple=0. Diagnostics (TemporalDiag=1) only count and log.
//
// Nothing here touches GPU0/GPU1 synchronisation, NGX, ring, queue or fences.
#pragma once

namespace mgpu::tdiag
{
    static const unsigned MAXSLOT = 16;   // >= stream_state::RING (6)

    enum mv_policy : int
    {
        MV_MULTIPLY = 0,   // T1.8 / R87: MVecScale *= game frames since last NR evaluate
        MV_SINGLE   = 1    // one-step vector as transported: MVecScale *= 1
    };

    struct config
    {
        bool     diag           = true;            // TemporalDiag
        int      mv_policy      = MV_MULTIPLY;     // TemporalMV=multiply|single
        unsigned reset_k        = 0;               // TemporalResetK (0 = off): reset NR history when elapsed >= K
        bool     require_tuple  = false;           // TemporalRequireTuple: do not bind an MVec whose frame != colour frame
        unsigned line_budget    = 300;             // TemporalDiagLines: full [R300a1] lines for the first N evaluates
        unsigned line_every     = 60;              // afterwards one [R300a1] line per this many evaluates
        // ---- T1.9.2: slot-aware jitter. All defaults leave the picture exactly as T1.9.1. ----
        bool     jitter_stamp   = true;            // JitterSlotStamp: record the game's jitter with each slot (no visual effect)
        bool     jitter_diag    = true;            // JitterDiag: [R300a7]/[R300a8], compares the author's R104 delta with the slot delta
        bool     jitter_slot    = false;           // JitterSlot=1: apply the SLOT delta instead of R104's (needs JitterComp=1)
        int      jitter_fallback = 0;               // JitterFallback: 0 author (T1.10a), 1 sync R104's state after SLOT, 2 zero offset instead of R104
        int      jitter_mode    = 0;               // 0 off, 1 apply, -1 negated: same meaning as the author's JitterComp/InvertJitter
    };

    void   configure(const config &c);             // logs [R300a4]
    config current();

    // ---- producer side (game thread). sealed_id is the seal index the frame will carry. ----
    // Called from stream_mvec_copy right after the slot is chosen, BEFORE the copy is recorded.
    void note_mvec_copy(unsigned slot, unsigned long long sealed_id);
    // Called where slot_game_frame[slot] is written at seal.
    void note_seal(unsigned slot, unsigned long long sealed_id, unsigned long long game_frame,
                   bool mvec_valid, bool depth_valid);

    // The seal's own mvec_valid for sealed id `f`, as the producer wrote it. known=false when the
    // slot has since been recycled for a newer frame (itself a finding: R300a1 counts it).
    bool seal_mvec_valid(unsigned slot, unsigned long long f, bool &known);

    // ---- T1.9.2 jitter ----
    // Producer, game thread, at the moment the vector copy is recorded (the calibrator has just
    // published the table for THIS evaluate - capture() runs before the R106 copy).
    void note_jitter(unsigned slot, unsigned long long sealed_id, float jx, float jy,
                     unsigned mv_jittered, unsigned mvec_w, unsigned mvec_h);

    struct jitter_in
    {
        unsigned long long sealed_id = 0, game_frame = 0, queue_lag = 0;
        unsigned slot = 0;
        bool  newest_have = false;               // calibrator table has a jitter
        float newest_jx = 0.f, newest_jy = 0.f;  // calibrator's NEWEST game evaluate (what R104 reads)
        unsigned newest_mvj = 0;
        float scale_x = 0.f, scale_y = 0.f;      // the un-multiplied NGX scale R104 divides by
        bool  nr_reset = false;                  // NR history is restarted on this evaluate: no delta to apply
    };
    struct jitter_out
    {
        bool  apply = false;      // true only with a known slot AND a previous evaluated slot: set DLSSNR.MVecOffset* to ox,oy and SKIP the author's apply_jitter_offset
        float ox = 0.f, oy = 0.f;
        bool  sync = false;       // JitterFallback=sync|zero: also bring the author's R104 state up to date
    };
    jitter_out jitter_decide(const jitter_in &in);   // once per NR evaluate; logs [R300a7]

    // ---- consumer side (GPU1 thread), once per NR evaluate, before the params are set ----
    struct eval_in
    {
        unsigned long long sealed_id      = 0;   // f
        unsigned           slot           = 0;
        unsigned long long game_frame     = 0;   // game frame this slot was sealed on (gnow)
        unsigned long long cur_game_frame = 0;   // newest game frame the producer has reached
        unsigned long long prev_eval_gf   = 0;   // game frame of the previous NR evaluate (0 = none)
        unsigned long long produced       = 0;   // newest sealed id
        unsigned           elapsed        = 1;   // game frames since the previous NR evaluate (R87)
        bool  mvec_valid_live = false;           // s.mvec_slot_valid[slot] now
        bool  mvec_valid_seal = false;           // what the seal said
        bool  depth_valid     = false;
        bool  seal_known      = true;     // false: the seal's slot was recycled before we read it
        float scale_x = 0.f, scale_y = 0.f;      // NGX scale actually in use (R105), BEFORE the policy factor
        float jitter_x = 0.f, jitter_y = 0.f;    // calibrator's NEWEST game evaluate (not per frame - see R300a5)
        unsigned mv_jittered = 0;
        unsigned render_w = 0, render_h = 0;     // evaluated extent
        unsigned mvec_w = 0, mvec_h = 0;
        unsigned ring = 6;
    };

    struct decision
    {
        float mv_k      = 1.0f;   // factor applied to MVecScaleX/Y
        bool  reset     = false;  // force DLSSNR.Reset=1 for this evaluate
        bool  bind_mvec = true;   // false: take the existing "unbind" branch
        unsigned coherent = 1;    // diagnostic: mvec stamp == colour frame
    };

    decision decide(const eval_in &in);            // records + applies policy

    // Emits [R300a2]/[R300a5] when `game_frame` crosses the next 300-frame mark.
    void periodic(unsigned long long game_frame);
    void final_summary();                          // run end

    // For tests.
    struct counters
    {
        unsigned long long evals, coherent, incoherent, mvec_missing, mvec_overwritten,
                           resets_forced, unbound_by_policy, flag_disagree,
                           age_color[8], age_mvec[8], delta_hist[8],
                           scale_changes, scale_zero, extent_changes, seal_lost, present, present_coherent, rejects_logged,
                           jit_evals, jit_slot_unknown, jit_slot_applied, jit_fallback, jit_no_baseline, jit_zero_fallback;
    };
    counters snapshot();
    void     reset_all();
}
