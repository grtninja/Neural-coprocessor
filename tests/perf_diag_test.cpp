// Off-device test for mgpu::perf. Fake clock, scripted jobs.
#include "../src/perf_diag.hpp"
#include "../src/diag.hpp"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
static std::vector<std::string> L;
namespace mgpu::diag { void info(const char *l){L.emplace_back(l);} void warn(const char *l){L.emplace_back(l);} void error(const char *l){L.emplace_back(l);} }
static double T = 0.0; static double clk() { return T; }
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
using namespace mgpu::perf;
// one job: begin, 1 ms record, 2 ms submit, wall ms, then GPU numbers
static void job(unsigned long long f, double gap, double wall, double pre, double ngx, double post, bool cq, bool newest = true, unsigned long long produced = 0)
{
    T += gap; note_seal((unsigned)((f-1)%6), f);
    T += 5.0;                                   // frame sits 5 ms before the consumer starts it
    job_begin(f, (unsigned)((f-1)%6), f, true, newest, 100 + f, 100 + f);
    T += 1.0; job_recorded(); T += 2.0; job_exec(cq); T += wall; job_done();
    note_gpu(pre, ngx, post, false); job_end(produced ? produced : f);
}
int main()
{
    set_clock_for_test(clk); reset_all(); config c; configure(c);
    for (unsigned long long f = 1; f <= 40; ++f) job(f, 4.0, 30.0, 0.5, 18.0 + (f % 2) * 0.1, 1.0, true);   // ngx stable ~18, total 19.5, outside 10.5
    counters s = snapshot();
    CHECK(s.jobs == 40); CHECK(s.used_cq == 40); CHECK(s.no_gpu_ts == 0);
    L.clear(); periodic(600); final_summary();
    std::string all; for (auto &l : L) all += l + "\n";
    CHECK(all.find("[R300e2]") != std::string::npos); CHECK(all.find("[R300e3]") != std::string::npos);
    CHECK(all.find("OUTSIDE-GPU-TIMELINE=10.450") != std::string::npos);                // 30 - 19.5
    CHECK(all.find("NGX=18.05") != std::string::npos);
    CHECK(all.find("incompressible") == std::string::npos);                          // ngx/wall = 0.60 < 0.80: not "the job"
    CHECK(all.find("OUTSIDE THE JOB'S GPU TIMELINE") != std::string::npos);          // 10.5/30 = 35% >= 25%
    CHECK(all.find("age_ms") != std::string::npos);

    // NGX dominant and stable
    reset_all(); configure(c);
    for (unsigned long long f = 1; f <= 40; ++f) job(f, 0.5, 20.0, 0.3, 19.0, 0.4, false);
    L.clear(); final_summary(); all.clear(); for (auto &l : L) all += l + "\n";
    CHECK(all.find("incompressible") != std::string::npos);
    CHECK(snapshot().used_cq == 0);

    // a tail is called a tail
    reset_all(); configure(c);
    for (unsigned long long f = 1; f <= 40; ++f) job(f, 0.5, 30.0, 0.3, (f % 5 == 0) ? 28.0 : 19.0, 0.4, false);
    L.clear(); final_summary(); all.clear(); for (auto &l : L) all += l + "\n";
    CHECK(all.find("HAS A TAIL") != std::string::npos);

    // idle gap verdict
    reset_all(); configure(c);
    for (unsigned long long f = 1; f <= 40; ++f) job(f, 12.0, 20.0, 0.3, 19.0, 0.4, false);
    L.clear(); final_summary(); all.clear(); for (auto &l : L) all += l + "\n";
    CHECK(all.find("SITS IDLE BETWEEN JOBS") != std::string::npos);                    // gap 17 ms vs wall 20 ms
    // skipped-older and shadow counters
    reset_all(); configure(c);
    job_begin(5, 4, 7, false, false, 200, 190);            // older frame: skipped, not evaluated
    CHECK(snapshot().skipped_older == 1); CHECK(snapshot().jobs == 0);
    job_begin(7, 0, 7, true, true, 205, 202); job_recorded(); job_exec(false); job_done(); note_gpu(1,2,3,false); job_end(9);
    counters q = snapshot(); CHECK(q.shadow_obs1 == 1 && q.shadow_obs2 == 1 && q.shadow_obs3 == 1); CHECK(q.done_lag[2] == 1);
    // calibration frames are out of the statistics; aborted jobs counted
    reset_all(); configure(c);
    job_begin(1, 0, 1, true, true, 1, 1); job_recorded(); job_exec(false); job_done(); note_gpu(1,2,3,true); job_end(1);
    CHECK(snapshot().calib == 1); CHECK(snapshot().jobs == 0);
    job_begin(2, 1, 2, true, true, 2, 2); job_abort(); CHECK(snapshot().aborted == 1);
    // disabled = nothing recorded
    reset_all(); config off; off.enabled = false; configure(off);
    job_begin(1, 0, 1, true, true, 1, 1); job_recorded(); job_exec(false); job_done(); note_gpu(1,2,3,false); job_end(1);
    CHECK(snapshot().jobs == 0);
    // ---- T1.12: policy ----
    reset_all(); { config pc; pc.prefetch = false; configure(pc); }
    CHECK(sched_target(20, 10, 11, true, false) == 20);                                   // off: newest, always
    CHECK(!prefetch_active());
    reset_all(); { config pc; pc.prefetch = true; pc.prefetch_max = 2; configure(pc); }
    CHECK(prefetch_active());
    CHECK(sched_target(12, 10, 11, true, false) == 11);                                   // armed, arrived, trails by 1: use it
    CHECK(sched_target(13, 10, 11, true, false) == 11);                                   // trails by 2 == max: still use it
    CHECK(sched_target(14, 10, 11, true, false) == 14);                                   // trails by 3: jump to the newest
    CHECK(sched_target(12, 10, 13, true, false) == 10);                            // armed frame has not arrived yet: WAIT (work up to consumed)
    CHECK(sched_target(12, 10, 13, true, true) == 12);                             // ...but never while draining at shutdown
    CHECK(sched_target(12, 11, 11, true, false) == 12);                                   // armed frame already consumed: newest
    CHECK(sched_target(12, 10, 11, false, false) == 12);                                  // copy queue not gated: policy inert
    counters pq = snapshot(); CHECK(pq.pol_hit == 2 && pq.pol_catchup == 1 && pq.pol_none == 1 && pq.pol_wait == 1);
    CHECK(arm_target(10, 10) == 11);                                               // f is newest: next frame, parity differs
    CHECK(arm_target(11, 10) == 13);                                               // 12 shares f's parity -> 13
    CHECK(arm_target(10, 12) == 13);                                               // never below f+1
    for (unsigned long long cm = 1; cm < 40; ++cm) for (unsigned long long ff = 1; ff <= cm; ++ff)
        CHECK(arm_target(cm, ff) > ff && (arm_target(cm, ff) & 1ull) != (ff & 1ull) && arm_target(cm, ff) > cm);
    // ---- T1.12: decomposition says "the transfer itself" when copy time ~= outside ----
    reset_all(); { config pc; configure(pc); }
    for (unsigned long long f = 1; f <= 30; ++f)
    {
        note_seal((unsigned)((f-1)%6), f); T += 2.0; note_cq_state(false);
        job_begin(f, (unsigned)((f-1)%6), f, true, true, 100 + f, 100 + f);
        note_cq_issue(); T += 1.0; job_recorded(); T += 1.0; job_exec(true); T += 35.6; job_done();
        note_cq_copy(15.0, 84972032.0); note_gpu(0.08, 20.15, 0.01, false); job_end(f);
    }
    L.clear(); final_summary(); all.clear(); for (auto &l : L) all += l + "\n";
    CHECK(all.find("OUTSIDE IS THE TRANSFER ITSELF") != std::string::npos);
    CHECK(all.find("5.66 GB/s") != std::string::npos);
    CHECK(all.find("no armed jobs to compare") != std::string::npos);
    CHECK(snapshot().cq_used == 30 && snapshot().armed_jobs == 0);
    // armed jobs that wait little are called out
    reset_all(); { config pc; configure(pc); }
    for (unsigned long long f = 1; f <= 20; ++f)
    {
        bool armed = (f % 2 == 0);
        note_seal((unsigned)((f-1)%6), f); T += 2.0; note_cq_state(armed);
        job_begin(f, (unsigned)((f-1)%6), f, true, true, 100 + f, 100 + f);
        note_cq_issue(); T += 1.0; job_recorded(); T += 1.0; job_exec(true); T += armed ? 21.0 : 35.6; job_done();
        note_cq_copy(15.0, 84972032.0); note_gpu(0.08, 20.15, 0.01, false); job_end(f);
    }
    L.clear(); final_summary(); all.clear(); for (auto &l : L) all += l + "\n";
    CHECK(all.find("OVERLAP WORKS WHEN THE FRAME IS ARMED") != std::string::npos);
    // ---- T1.13: windows ----
    reset_all(); { config pc; configure(pc); }
    for (unsigned long long f = 1; f <= 20; ++f)    // window 1: cold jobs, 35 ms
    {
        note_seal((unsigned)((f-1)%6), f); T += 2.0; note_cq_state(false);
        job_begin(f, (unsigned)((f-1)%6), f, true, true, 100 + f, 100 + f); note_cq_issue();
        T += 1.0; job_recorded(); T += 1.0; job_exec(true); T += 35.0; job_done(); note_cq_copy(15.0, 84972032.0); note_gpu(0.08, 20.0, 0.01, false); job_end(f);
    }
    L.clear(); periodic(600); all.clear(); for (auto &l : L) all += l + "\n";
    CHECK(all.find("[R300e6]") != std::string::npos); CHECK(all.find("jobs=20 armed=0.0%") != std::string::npos);
    CHECK(all.find("wall=35.000") != std::string::npos);
    for (unsigned long long f = 21; f <= 30; ++f)   // window 2: armed jobs, 24 ms, stride 2
    {
        note_seal((unsigned)((f-1)%6), f); T += 2.0; note_cq_state(true);
        job_begin(f, (unsigned)((f-1)%6), f, true, true, 100 + f, 100 + f); note_cq_issue();
        T += 1.0; job_recorded(); T += 1.0; job_exec(true); T += 24.0; job_done(); note_cq_copy(13.0, 84972032.0); note_gpu(0.08, 20.0, 0.01, false); job_end(f);
    }
    L.clear(); periodic(1200); all.clear(); for (auto &l : L) all += l + "\n";
    CHECK(all.find("jobs=10 armed=100.0%") != std::string::npos);   // the window is NOT the cumulative 33%
    CHECK(all.find("wall=24.000") != std::string::npos);
    std::printf(fails ? "perf_diag_test: %d FAILED\n" : "perf_diag_test: all passed\n", fails);
    return fails ? 1 : 0;
}
