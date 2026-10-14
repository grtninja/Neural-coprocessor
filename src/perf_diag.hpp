// MGPU Bridge 0.2.6-dev - T1.11 PERFORMANCE MEASUREMENT. Log ids [R300e1]..[R300e4].
//
// MEASUREMENT ONLY. It reads clocks and counters around the consumer's existing job; it changes no scheduling,
// no queue, fence, barrier or parameter. Perf11=0 stops all of it (the code path is then T1.10a's).
//
// What it answers (T1.11 mandate):
//   - where the ~20 ms "EVALUATE" goes: GPU1 time before NGX / in NGX / after NGX, measured by the author's GPU
//     timestamps on GPU1's own queue, versus the time the CPU actually waited for the job (submit -> fence done).
//     wall - (mark4 - mark0) is time the job spent OUTSIDE its own GPU timeline: the queue-level wait on the copy
//     queue fence, GPU idle before mark 0, and completion latency. A GPU1 timestamp bracket cannot contain a wait
//     for GPU0; if "outside" is large, that is where a dependency wait would show.
//   - how busy GPU1 really is: the idle gap between one job's completion and the next job's start.
//   - how old the work is: ms since the producer sealed the frame, at job start and at job completion, and how
//     many newer frames existed when the result was finished.
//   - how many jobs were evaluated / skipped because a newer frame existed, and how many WOULD have been
//     obsolete at start for K = 1, 2, 3 game frames (shadow counters: nothing is skipped).
#pragma once

namespace mgpu::perf
{
    static const unsigned MAXSLOT = 16;

    struct config
    {
        bool     enabled     = true;    // Perf11
        unsigned line_budget = 200;     // PerfLines: full [R300e1] lines for the first N evaluated jobs
        unsigned line_every  = 60;      // then one per this many jobs
        // ---- T1.12 experiment, OFF by default (off = T1.11 / T1.10a scheduling exactly) ----
        bool     prefetch     = false;  // PerfPrefetch: evaluate the frame whose copy the copy queue already armed, not the newest
        unsigned prefetch_max = 2;      // PerfPrefetchMax: use the armed frame only while it trails the newest sealed frame by <= this
    };

    void   configure(const config &c);  // logs [R300e4]
    config current();

    // Producer side (game thread): the moment a frame is sealed.
    void note_seal(unsigned slot, unsigned long long sealed_id);

    // ---- T1.12: copy-queue decomposition + the gated scheduling experiment ----
    // The author's CopyQueue=2 arms the copy of frame f+1 (GPU-gated on its arrival) while frame f's NGX runs, but the
    // consumer then evaluates the NEWEST sealed frame, which is usually not f+1: the armed copy is thrown away and the
    // newest frame is copied cold, in series in front of NGX. With PerfPrefetch=1 the consumer evaluates the armed frame
    // instead (bounded by PerfPrefetchMax, else it jumps to the newest as before), and arms the first frame that will
    // ARRIVE after now, on the parity that the running evaluate is not reading.
    bool prefetch_active();                                     // enabled && Perf11
    // Which sealed frame the consume loop works up to. == completed unless the policy picks the armed frame.
    // draining = the producer has stopped: then it is always `completed`, so the run can finish.
    unsigned long long sched_target(unsigned long long completed, unsigned long long consumed,
                                    unsigned long long cq_issued, bool copy_queue_gated, bool draining);
    // Which frame's copy to arm next (only meaningful when prefetch_active()). Never the parity of f.
    unsigned long long arm_target(unsigned long long completed, unsigned long long f);
    void note_cq_state(bool armed_before_start);   // was f's copy already issued when this job began
    void note_cq_issue();                          // one copy-queue submission (any frame)
    void note_cq_copy(double ms, double bytes);    // copy queue's own GPU time for this job's frame

    // Consumer side, once per loop iteration of the consume loop.
    void job_begin(unsigned long long f, unsigned slot, unsigned long long completed,
                   bool run_nr, bool newest, unsigned long long game_frame_now,
                   unsigned long long game_frame_slot);
    void job_recorded();                // command list closed
    void job_exec(bool used_copy_queue);// ExecuteCommandLists + Signal issued
    void job_done();                    // fence wait returned
    void job_abort();                   // submission failed
    // GPU1's own timestamps for this job, ms: mark0->2 (seal copy + unpack), 2->3 (NGX evaluate), 3->4 (output).
    void note_gpu(double pre_ms, double ngx_ms, double post_ms, bool calibration_frame);
    void job_end(unsigned long long produced_now);

    void periodic(unsigned long long consumed);
    void final_summary();

    struct counters
    {
        unsigned long long jobs, skipped_older, aborted, calib, no_gpu_ts, used_cq,
                           shadow_obs1, shadow_obs2, shadow_obs3,
                           done_lag[5],                 // newer sealed frames at completion: 0,1,2,3,4+
                           cq_issued_total, cq_used, armed_jobs, pol_hit, pol_catchup, pol_none, pol_wait;
    };
    counters snapshot();
    void     reset_all();
    // Test hook: replace the clock (ms since an arbitrary epoch). nullptr restores steady_clock.
    void set_clock_for_test(double (*fn)());
}
