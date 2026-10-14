// MGPU Bridge 0.2.6-dev - T1.16 PRODUCER (GPU 0) COST MEASUREMENT. Log ids [R300g1]..[R300g6]. MEASUREMENT ONLY.
//
// Question this module exists to answer (measured, not guessed): what does the bridge's PRODUCER cost the GAME's GPU?
//   - GPU 0 time of the colour + depth + seal copies (and their barriers) the producer records into the game's own
//     command list every frame, from GPU TIMESTAMPS on GPU 0, not a CPU clock;
//   - GPU 0 time of the MVec copy recorded at the game's DLSS Evaluate (separately);
//   - the CPU time spent RECORDING those same blocks (QPC) - recording cost, never waiting: the producer path has no blocking wait;
//   - the author's existing producer backlog (q_hist / q_max), made visible periodically instead of once at a summary
//     that no unbounded run ever reaches;
//   - how many bytes are sealed (copied by GPU 0) versus how many belong to frames a NR job actually evaluated.
//
// Pure standard C++ apart from diag.hpp so the bookkeeping is unit-tested off-device (tests/producer_diag_test.cpp).
// The D3D12 half (query heap, readback) lives in producer_gpu_timers.{hpp,cpp} and calls note_gpu_block() from here.
#pragma once

namespace mgpu::pdiag
{
    struct config
    {
        bool     gpu0_timers  = true;   // Gpu0Timers: insert the GPU 0 timestamps. 0 = none (kill-switch, also for A/B of the instrument itself)
        unsigned report_every = 600;    // ProducerReport: sealed frames between report windows
    };

    void   configure(const config &c);  // logs [R300g6]
    config current();

    // CPU time to RECORD a block, ms. block 0 = colour+depth+seal+barriers (+ the timestamps and the resolve), block 1 = MVec copy.
    void note_cpu_record(int block, double ms);

    // GPU 0 time of a block, from two raw GPU timestamps. block: 0 whole producer block, 1 MVec copy, 2 seal copy + barrier + drain of the
    // game's own rendering (NOT bridge work), 3 colour copy, 4 depth copy. Invalid readings (end < begin, > 100 ms, zero) are counted, never averaged.
    void note_gpu_block(int block, unsigned long long t_begin, unsigned long long t_end, double ticks_per_s);
    void note_gpu_invalid(int block);

    // Volume. Called once per sealed frame by the producer, and once per frame a NR job evaluates by the consumer.
    void note_sealed(unsigned long long frame, unsigned long long colour_depth_bytes, unsigned long long mvec_bytes);
    void note_evaluated(unsigned long long frame);

    // The author's own producer backlog counters (stream_state::q_hist/q_sum/q_n/q_max), cumulative, passed through unchanged.
    struct backlog { unsigned long long hist[8]; unsigned long long sum, n, max; };

    // Emits one report window each time `sealed_frames` crosses the next mark.
    void periodic(unsigned long long sealed_frames, const backlog &b);
    void final_summary();

    // Tests.
    void set_clock_for_test(double (*fn)());   // seconds
    struct counters
    {
        unsigned long long gpu_n[5], gpu_invalid[5], cpu_n[2];
        unsigned long long sealed_frames, sealed_bytes, evaluated_frames, evaluated_bytes;
    };
    counters snapshot();
    void     reset_all();
}
