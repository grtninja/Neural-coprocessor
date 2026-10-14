// Off-device test for mgpu::tdiag (policy + counters). Build: see CMakeLists.txt (mgpu_temporal_diag_test).
#include "../src/temporal_diag.hpp"
#include "../src/diag.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::vector<std::string> g_lines;
namespace mgpu::diag {
    void info(const char *l)  { g_lines.emplace_back(l); }
    void warn(const char *l)  { g_lines.emplace_back(l); }
    void error(const char *l) { g_lines.emplace_back(l); }
}
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

using namespace mgpu::tdiag;
static eval_in mk(unsigned long long f, unsigned long long gf, unsigned el)
{
    eval_in e; e.sealed_id = f; e.slot = (unsigned)((f - 1) % 6); e.game_frame = gf; e.cur_game_frame = gf + 1;
    e.prev_eval_gf = gf - el; e.produced = f + 1; e.elapsed = el; e.mvec_valid_live = e.mvec_valid_seal = true;
    e.depth_valid = true; e.scale_x = -1436.f; e.scale_y = 808.f; e.render_w = 2872; e.render_h = 1616;
    e.mvec_w = 2872; e.mvec_h = 1616; return e;
}
int main()
{
    // 1. defaults == T1.8: k = elapsed, no reset, always bind
    reset_all(); config c; configure(c);
    note_mvec_copy(2, 3); note_seal(2, 3, 100, true, true);
    decision d = decide(mk(3, 100, 4));
    CHECK(d.mv_k == 4.0f); CHECK(!d.reset); CHECK(d.bind_mvec); CHECK(d.coherent == 1);

    // 2. single policy
    c.mv_policy = MV_SINGLE; configure(c);
    d = decide(mk(3, 100, 4)); CHECK(d.mv_k == 1.0f);

    // 3. reset at K
    c.reset_k = 3; configure(c);
    note_mvec_copy(3, 4); d = decide(mk(4, 101, 2)); CHECK(!d.reset);
    note_mvec_copy(4, 5); d = decide(mk(5, 102, 3)); CHECK(d.reset);
    note_mvec_copy(5, 6); d = decide(mk(6, 103, 5)); CHECK(d.reset);

    // 4. slot recycled: stamp is a newer frame -> overwritten, not coherent
    note_mvec_copy(0, 7); /* frame 7 reuses slot 0 */
    d = decide(mk(1, 90, 2));               // consumer still evaluating sealed id 1 on slot 0
    CHECK(d.coherent == 0);
    c.require_tuple = true; configure(c);
    d = decide(mk(1, 90, 2)); CHECK(!d.bind_mvec);
    note_mvec_copy(1, 2); d = decide(mk(2, 91, 2)); CHECK(d.bind_mvec);

    // 5. a frame with no vector is never "unbound by policy"
    eval_in n = mk(9, 110, 2); n.mvec_valid_live = n.mvec_valid_seal = false;
    unsigned long long before = snapshot().unbound_by_policy;
    d = decide(n); CHECK(snapshot().unbound_by_policy == before); CHECK(d.bind_mvec);

    counters s = snapshot();
    CHECK(s.mvec_overwritten >= 2); CHECK(s.mvec_missing == 1); CHECK(s.scale_zero == 0);

    // 6. scale zero is flagged
    eval_in z = mk(10, 111, 1); z.scale_x = 0.f; decide(z); CHECK(snapshot().scale_zero == 1);

    // 6b. a frame with NO vector is 'missing', never a policy reject and never a tuple mismatch
    { reset_all(); c.require_tuple = true; configure(c); eval_in m = mk(20, 120, 2); m.mvec_valid_live = m.mvec_valid_seal = false;
      for (int k = 0; k < 5; ++k) decide(m);
      counters q = snapshot(); CHECK(q.mvec_missing == 5); CHECK(q.unbound_by_policy == 0); CHECK(q.present == 0);
      // a present vector from another frame IS rejected and logged as R300a6
      g_lines.clear(); note_mvec_copy(19 % 6, 99); eval_in o = mk(20, 120, 2); o.slot = 19 % 6; decide(o);
      bool r304 = false; for (auto &l : g_lines) if (l.find("[R300a6]") != std::string::npos) r304 = true;
      CHECK(r304); CHECK(snapshot().unbound_by_policy == 1); }

    // 6c. T1.9.2 jitter: slot delta uses the EVALUATED frames; R104 reconstruction uses the newest-at-call
    { reset_all(); config jc; jc.jitter_mode = 1; jc.jitter_slot = true; configure(jc);
      // game frames 1..9 jitter (0.1*k, 0); NR evaluates frame 2 (newest=5), then frame 5 (newest=9)
      for (unsigned long long k = 1; k <= 6; ++k) note_jitter((unsigned)((k - 1) % 6), k, 0.1f * (float)k, 0.f, 0u, 2872, 1616);
      jitter_in a; a.sealed_id = 2; a.slot = 1; a.game_frame = 2; a.queue_lag = 3; a.newest_have = true; a.newest_jx = 0.5f; a.scale_x = -1436.f; a.scale_y = 808.f;
      jitter_out oa = jitter_decide(a); CHECK(!oa.apply);                          // first evaluate: no baseline -> author's R104 keeps running
      CHECK(snapshot().jit_no_baseline == 1);
      jitter_in b = a; b.sealed_id = 5; b.slot = 4; b.game_frame = 5; b.queue_lag = 4; b.newest_jx = 0.9f;
      jitter_out ob = jitter_decide(b);
      CHECK(ob.apply);
      CHECK(std::fabs(ob.ox - (0.3f / -1436.f * -1.0f * -1.0f)) < 1e-6f || std::fabs(ob.ox - (0.3f / -1436.f)) < 1e-6f);  // slot delta 0.5-0.2=0.3 px / scale
      // a recycled slot is unknown -> falls back to the author's path
      note_jitter(4, 11, 0.f, 0.f, 0u, 2872, 1616);
      // a reset restarts NR history: no delta applied, baseline re-seeded from this frame
      note_jitter(3, 4, 0.4f, 0.f, 0u, 2872, 1616);
      { jitter_in rr = b; rr.sealed_id = 4; rr.slot = 3; rr.nr_reset = true; jitter_out orr = jitter_decide(rr); CHECK(!orr.apply); CHECK(snapshot().jit_no_baseline == 2); }
      jitter_in c2 = b; c2.sealed_id = 5; jitter_out oc = jitter_decide(c2); CHECK(!oc.apply); CHECK(snapshot().jit_slot_unknown == 1); CHECK(snapshot().jit_fallback == 1);
      // after an UNKNOWN slot the baseline is dropped: the next known evaluate must not apply a stale-span delta
      note_jitter(1, 2, 0.2f, 0.f, 0u, 2872, 1616); { jitter_in nx = a; jitter_out onx = jitter_decide(nx); CHECK(!onx.apply); }
      // MVJittered=1 slot: nothing to apply
      note_jitter(2, 3, 0.1f, 0.f, 1u, 2872, 1616); jitter_in d = b; d.sealed_id = 3; d.slot = 2; jitter_out od = jitter_decide(d); CHECK(!od.apply);
      // sign inversion
      jc.jitter_mode = -1; configure(jc); note_jitter(1, 2, 0.2f, 0.f, 0u, 2872, 1616); note_jitter(4, 5, 0.5f, 0.f, 0u, 2872, 1616);
      jitter_decide(a); jitter_out oi = jitter_decide(b); CHECK(oi.apply); CHECK(oi.ox * ob.ox < 0.f);
      // JitterSlot=0 never applies
      jc.jitter_slot = false; configure(jc); jitter_out on = jitter_decide(b); CHECK(!on.apply);
      note_mvec_copy(0, 21); note_seal(0, 21, 300, true, true); decide(mk(21, 300, 2)); }   // feeds the summary check

    // 6d. T1.11 audit: JitterFallback. Default author = T1.10a; sync keeps R104's state current after SLOT; zero replaces the fallback
    { reset_all(); config jc; jc.jitter_mode = 1; jc.jitter_slot = true; configure(jc);
      for (unsigned long long k = 1; k <= 6; ++k) note_jitter((unsigned)((k - 1) % 6), k, 0.1f * (float)k, 0.f, 0u, 2872, 1616);
      jitter_in a; a.sealed_id = 2; a.slot = 1; a.game_frame = 2; a.queue_lag = 1; a.newest_have = true; a.newest_jx = 0.5f; a.scale_x = -1436.f; a.scale_y = 808.f;
      jitter_decide(a);
      jitter_in b = a; b.sealed_id = 5; b.slot = 4; b.game_frame = 5;
      jitter_out ob = jitter_decide(b); CHECK(ob.apply); CHECK(!ob.sync);                       // author mode: no sync request
      jc.jitter_fallback = 1; configure(jc); jitter_decide(a); ob = jitter_decide(b); CHECK(ob.apply && ob.sync);
      // a fallback evaluate (slot recycled): sync mode still hands over to the author's path
      note_jitter(4, 11, 0.f, 0.f, 0u, 2872, 1616); jitter_decide(a); jitter_out oc = jitter_decide(b); CHECK(!oc.apply); CHECK(!oc.sync);
      // zero mode: the same fallback becomes an explicit zero offset + sync, counted
      jc.jitter_fallback = 2; configure(jc); jitter_out oz = jitter_decide(b);
      CHECK(oz.apply && oz.ox == 0.f && oz.oy == 0.f && oz.sync); CHECK(snapshot().jit_zero_fallback >= 1);
      // zero mode never fires when JitterSlot is off
      jc.jitter_slot = false; configure(jc); jitter_out of = jitter_decide(b); CHECK(!of.apply);
      note_mvec_copy(0, 31); note_seal(0, 31, 400, true, true); decide(mk(31, 400, 2)); }   // feeds the summary check

    // 7. summary + reading produce [R300a2]/[R300a5]
    g_lines.clear(); periodic(300); final_summary();
    bool r301 = false, r303 = false;
    for (auto &l : g_lines) { if (l.find("[R300a2]") != std::string::npos) r301 = true; if (l.find("[R300a5]") != std::string::npos) r303 = true; }
    CHECK(r301); CHECK(r303);

    // 8. windowed presence: a stretch with no vectors reads 0% in its window, not a slow cumulative decline
    { reset_all(); config wc; configure(wc);
      for (unsigned long long k = 1; k <= 10; ++k) { note_mvec_copy((unsigned)((k-1)%6), k); note_seal((unsigned)((k-1)%6), k, 100 + k, true, true); decide(mk(k, 100 + k, 2)); }
      g_lines.clear(); periodic(300); std::string w1; for (auto &l : g_lines) w1 += l + "\n";
      CHECK(w1.find("[R300a3]") != std::string::npos); CHECK(w1.find("vector_present=100.00%") != std::string::npos);
      for (unsigned long long k = 11; k <= 20; ++k) { eval_in e = mk(k, 100 + k, 2); e.mvec_valid_live = e.mvec_valid_seal = false; decide(e); }
      g_lines.clear(); periodic(700); std::string w2; for (auto &l : g_lines) w2 += l + "\n";
      CHECK(w2.find("evals=10 vector_present=0.00%") != std::string::npos); }
    std::printf(fails ? "temporal_diag_test: %d FAILED\n" : "temporal_diag_test: all passed\n", fails);
    return fails ? 1 : 0;
}
