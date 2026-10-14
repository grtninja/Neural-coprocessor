// Off-device test for the DLSS-silence detector.
#include "../src/feature_diag.hpp"
#include "../src/diag.hpp"
#include <cstdio>
#include <string>
#include <vector>
static std::vector<std::string> L;
namespace mgpu::diag { void info(const char *l){L.emplace_back(l);} void warn(const char *l){L.emplace_back(l);} void error(const char *l){L.emplace_back(l);} }
static double T = 0.0; static double clk() { return T; }
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
using namespace mgpu::feature_diag;
static std::string all() { std::string s; for (auto &l : L) s += l + "\n"; return s; }
int main()
{
    set_clock_for_test(clk); reset_all(); set_step_for_test(30);
    unsigned long long gf = 1000, ev = 5000;
    // normal: evaluates keep arriving, 75 fps
    for (int i = 0; i < 20; ++i) { gf += 30; ev += 25; T += 0.4; observe(gf, ev, ev, 4, 2); }
    CHECK(snapshot().silences == 0);
    // the game stops calling DLSS but keeps presenting at 163 fps
    L.clear();
    for (int i = 0; i < 3; ++i) { gf += 30; T += 30.0 / 163.0; observe(gf, ev, ev, 5, 4); }
    CHECK(snapshot().silences == 1 && snapshot().silent);
    CHECK(all().find("NO SCENE-FEATURE EVALUATE") != std::string::npos);
    CHECK(all().find("THE GAME STOPPED CALLING DLSS") == std::string::npos);                 // the overstated T1.15 wording is gone
    CHECK(all().find("NOTHING evaluated in the last observation either") != std::string::npos);
    CHECK(all().find("creates=5 releases=4 live=1") != std::string::npos);
    // one line per silence, not one per look
    size_t n1 = L.size(); for (int i = 0; i < 50; ++i) { gf += 30; T += 0.18; observe(gf, ev, ev, 5, 4); } CHECK(L.size() == n1);
    // it resumes (menu re-creates the upscaler)
    L.clear(); gf += 30; ev += 75; T += 0.2; observe(gf, ev, ev, 6, 4);
    CHECK(!snapshot().silent && snapshot().resumed == 1); CHECK(all().find("SCENE-FEATURE EVALUATES RESUMED") != std::string::npos);
    CHECK(all().find("creates=6 releases=4 live=2") != std::string::npos);
    // before the first evaluate of a run there is no silence to report
    reset_all(); L.clear(); gf = 10; ev = 0;
    for (int i = 0; i < 10; ++i) { gf += 30; T += 0.4; observe(gf, ev, ev, 0, 0); }
    CHECK(snapshot().silences == 0);
    // a single quiet look is not a silence
    reset_all(); L.clear(); gf = 1000; ev = 100; observe(gf, ev, ev, 1, 0);
    gf += 30; ev += 10; T += .4; observe(gf, ev, ev, 1, 0); gf += 30; T += .4; observe(gf, ev, ev, 1, 0); gf += 30; ev += 10; T += .4; observe(gf, ev, ev, 1, 0);
    CHECK(snapshot().silences == 0);
    // the run ends silent: the summary says so
    reset_all(); L.clear(); gf = 1000; ev = 100; observe(gf, ev, ev, 2, 1);
    gf += 30; ev += 5; T += .4; observe(gf, ev, ev, 2, 1);
    for (int i = 0; i < 4; ++i) { gf += 30; T += .2; observe(gf, ev, ev, 2, 1); }
    L.clear(); final_summary(); CHECK(all().find("NO SCENE-FEATURE EVALUATE WAS ARRIVING") != std::string::npos);
    // scene feature silent while ANOTHER feature keeps evaluating: the line must say so, and must not claim DLSS stopped
    reset_all(); L.clear(); gf = 1000; ev = 100; unsigned long long allc = 500; observe(gf, ev, allc, 2, 1);
    gf += 30; ev += 5; allc += 5; T += .4; observe(gf, ev, allc, 2, 1);
    for (int i = 0; i < 4; ++i) { gf += 30; allc += 25; T += .3; observe(gf, ev, allc, 2, 1); }      // scene flat, all-handle counter climbing
    CHECK(snapshot().silent); CHECK(all().find("STILL EVALUATING in the last observation") != std::string::npos);
    L.clear(); gf += 30; ev += 10; allc += 40; T += .3; observe(gf, ev, allc, 3, 1);
    CHECK(all().find("another feature kept evaluating") != std::string::npos);
    std::printf(fails ? "feature_diag_test: %d FAILED\n" : "feature_diag_test: all passed\n", fails);
    return fails ? 1 : 0;
}
