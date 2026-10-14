// MGPU Bridge 0.2.6-dev - T1.15/T1.16 SCENE-FEATURE SILENCE DETECTOR. Log id [R300f1]. OBSERVATIONAL ONLY.
//
// T1.16 CORRECTION: this detector tracks the SCENE feature's Evaluates (the table snapshot), not every DLSS Evaluate. Its T1.15 text said
// "THE GAME STOPPED CALLING DLSS", which overstated it: measured on the same logs, the all-handle Evaluate counter was silent only ~5% of the
// time where the scene feature was silent ~8-20%. The messages now say SCENE FEATURE and report the all-handle count beside it.
//
// Measured on Starfield (D0 and D1 runs): after a burst of DLSS feature creations/releases the game kept PRESENTING
// (~163 fps, constant) and called DLSS Evaluate ZERO times for minutes (D0: 288 s, until a menu re-created the upscaler).
// Nothing in the log said when it started or what the features looked like at that moment. This module says both.
//
// Pure standard C++ apart from diag.hpp: the state machine is unit-tested off-device (tests/feature_diag_test.cpp).
#pragma once

namespace mgpu::feature_diag
{
    // Called by the producer once per game frame (cheap: it only acts every `step` frames).
    //   gf          game frame counter
    //   scene_evals the calibrator table's snapshot, which advances ONLY when an Evaluate of the latched SCENE feature is captured.
    //               It is what detects the stretch. It is NOT "DLSS is silent": other features may keep evaluating.
    //   all_evals   every Evaluate the hook saw, any feature (T1.16): tells the two apart in the log line itself
    //   creates/releases   CreateFeature/ReleaseFeature the calibrator has seen, cumulative
    void observe(unsigned long long gf, unsigned long long scene_evals, unsigned long long all_evals,
                 unsigned long long creates, unsigned long long releases);
    void final_summary();                       // run end: says so if the run ended silent

    // Test hooks.
    void set_clock_for_test(double (*fn)());
    void set_step_for_test(unsigned long long frames);
    struct counters { unsigned long long silences, resumed; bool silent; unsigned long long last_silence_frames; };
    counters snapshot();
    void reset_all();
}
