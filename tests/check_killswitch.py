#!/usr/bin/env python3
"""Guard for the T1.16 defect (Dom): a switch that decides whether something is CREATED at arm time must be
known at that point, not taken from a module whose configure() runs later. In R300 the keys are
read once by t119_keys (from probe's ini read at swapchain init, before any arm), so the guard checks that
p0t::init is only reachable behind mgpu::t119::gpu0_timers().
Usage: python tests/check_killswitch.py [src/gpu1_context.cpp]"""
import re, sys
g = open(sys.argv[1] if len(sys.argv) > 1 else 'src/gpu1_context.cpp', encoding='latin-1').read()
calls = [m.start() for m in re.finditer(r'p0t::init\(', g)]
bad = 0 if calls else 1
for c in calls:
    ctx = g[max(0, c - 200):c]
    ok = 'mgpu::t119::gpu0_timers()' in ctx
    print('p0t::init call at char %d: guarded by Gpu0Timers: %s' % (c, 'yes' if ok else 'NO'))
    bad += 0 if ok else 1
if 'pdiag::current().gpu0_timers) (void)mgpu::p0t::init' in g: print('the T1.16 pattern is still present'); bad += 1
sys.exit(1 if bad else 0)
