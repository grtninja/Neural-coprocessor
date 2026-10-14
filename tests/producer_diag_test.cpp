// Off-device test for the producer (GPU 0) cost bookkeeping.
#include "../src/producer_diag.hpp"
#include "../src/diag.hpp"
#include <cstdio>
#include <string>
#include <vector>
static std::vector<std::string> L;
namespace mgpu::diag { void info(const char *l){L.emplace_back(l);} void warn(const char *l){L.emplace_back(l);} void error(const char *l){L.emplace_back(l);} }
static double T = 0.0; static double clk() { return T; }
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
using namespace mgpu::pdiag;
static std::string all() { std::string s; for (auto &l : L) s += l + "\n"; return s; }
int main()
{
    set_clock_for_test(clk); reset_all(); config c; configure(c);
    CHECK(all().find("[R300g6]") != std::string::npos);
    const double hz = 1.0e9;                           // 1 GHz timestamp clock: ticks == ns
    backlog b{}; b.hist[0] = 90; b.hist[1] = 10; b.sum = 10; b.n = 100; b.max = 1;
    // 600 sealed frames at 12.5 ms: every frame costs 2.0 ms colour+depth on GPU0, MVec 0.8 ms on half of them
    for (unsigned long long f = 1; f <= 600; ++f)
    {
        note_sealed(f, 63ull << 20, (f % 2) ? (17ull << 20) : 0ull);
        note_gpu_block(0, 1000, 1000 + 3000000, hz);               // whole block 3.000 ms = pre 1.0 (the game's drain) + colour 1.2 + depth 0.8
        note_gpu_block(2, 1000, 1000 + 1000000, hz);               // pre/drain 1.000 ms
        note_gpu_block(3, 1000, 1000 + 1200000, hz);               // colour copy 1.200 ms
        note_gpu_block(4, 1000, 1000 + 800000, hz);                // depth copy 0.800 ms
        if (f % 2) note_gpu_block(1, 5000, 5000 + 800000, hz);      // MVec 0.800 ms
        note_cpu_record(0, 0.020); if (f % 2) note_cpu_record(1, 0.010);
        if (f % 2 == 1 && f % 4 == 1) note_evaluated(f);           // GPU1 evaluates every 4th frame
        T += 0.0125;
    }
    L.clear(); periodic(600, b);
    std::string s = all();
    CHECK(s.find("[R300g1]") != std::string::npos); CHECK(s.find("window mean=3.000") != std::string::npos);        // the whole block
    CHECK(s.find("PRODUCER BLOCK SPLIT") != std::string::npos);
    CHECK(s.find("DRAIN of the game's rendering (game's own work, not bridge work): window mean=1.000") != std::string::npos);
    CHECK(s.find("COLOUR copy: window mean=1.200") != std::string::npos); CHECK(s.find("DEPTH copy: window mean=0.800") != std::string::npos);
    CHECK(s.find("(colour + depth copies = 2.000 ms on average") != std::string::npos);
    CHECK(s.find("window mean=0.800") != std::string::npos);
    CHECK(s.find("block0 mean/max=0.0200") != std::string::npos);
    CHECK(s.find("[R300g3]") != std::string::npos);
    // sealed 600 frames; evaluated 150 of them: 300 odd frames carry 80 MB, 300 even carry 63 MB; every 4th odd... bytes: evaluated frames are odd (80 MB each)
    const counters k = snapshot();
    CHECK(k.sealed_frames == 600 && k.evaluated_frames == 150);
    CHECK(s.find("NEVER EVALUATED") != std::string::npos);
    CHECK(s.find("[R300g4]") != std::string::npos); CHECK(s.find("window histogram 0/1/2/3/4/5/6/7+: 90 10 0 0 0 0 0 0") != std::string::npos);
    CHECK(s.find("[R300g5]") != std::string::npos); CHECK(s.find("= 3.400 ms on average") == std::string::npos);   // the drain (1.0) must NOT be counted as the bridge's: 2.0 + 0.8 x 50% = 2.4
    CHECK(s.find("NOT counting the barrier/drain wait") != std::string::npos);
    CHECK(s.find("= 2.400 ms on average") != std::string::npos);
    // invalid timestamp pairs are counted, never averaged
    note_gpu_block(0, 5000, 4000, hz); note_gpu_block(0, 0, 0, hz); note_gpu_block(0, 1, 1 + 500000000ull, hz); note_gpu_invalid(1);
    const counters k2 = snapshot(); CHECK(k2.gpu_invalid[0] == 3 && k2.gpu_n[0] == 600 && k2.gpu_invalid[1] == 1 && k2.gpu_n[3] == 600 && k2.gpu_n[4] == 600);
    // a window reports only its own frames
    for (unsigned long long f = 601; f <= 1200; ++f) { note_sealed(f, 63ull << 20, 0); note_gpu_block(0, 1, 1 + 4000000, hz); note_gpu_block(3, 1, 1 + 2500000, hz); note_gpu_block(4, 1, 1 + 1500000, hz); T += 0.0125; }
    b.hist[0] += 580; b.hist[2] += 20; b.sum += 40; b.n += 600; b.max = 2;
    L.clear(); periodic(1200, b); s = all();
    CHECK(s.find("window mean=4.000") != std::string::npos);
    CHECK(s.find("window histogram 0/1/2/3/4/5/6/7+: 580 0 20 0 0 0 0 0") != std::string::npos);
    CHECK(s.find("sealed 600 frames") != std::string::npos);
    CHECK(s.find("COLOUR copy: window mean=2.500") != std::string::npos);
    // a recycled ring entry is not counted as evaluated
    const unsigned long long before = snapshot().evaluated_frames; note_evaluated(5); CHECK(snapshot().evaluated_frames == before);
    // without any GPU reading the reading says so, and volume is still reported
    reset_all(); configure(c); note_sealed(1, 63ull << 20, 0); L.clear(); final_summary(); s = all();
    CHECK(s.find("PRODUCER BLOCK SPLIT") != std::string::npos);
    CHECK(s.find("no GPU0 timestamp readings yet") != std::string::npos); CHECK(s.find("[R300g3]") != std::string::npos);
    std::printf(fails ? "producer_diag_test: %d FAILED\n" : "producer_diag_test: all passed\n", fails);
    return fails ? 1 : 0;
}
