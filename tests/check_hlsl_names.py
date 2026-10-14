#!/usr/bin/env python3
"""Static guard for the embedded HLSL in gpu1_context.cpp. No compiler needed: it extracts every embedded HLSL string
(kReduceHLSL, ...) and fails on a name declared twice at global scope (texture, sampler, cbuffer member, function-scope
globals). This would have caught T1.14's `gDepth` collision, which the real compiler rejected only at run time."""
import re, sys, collections
g = open(sys.argv[1] if len(sys.argv) > 1 else 'src/gpu1_context.cpp', encoding='latin-1').read()
bad = 0
for m in re.finditer(r'static const char \*(k\w*HLSL)\s*=', g):
    start = m.end(); end = g.index(';', g.index('"', start))
    # a C string-literal block ends at the first ';' that follows the closing quote of the last literal
    while True:
        tail = g[start:end]
        if tail.rstrip().endswith('"') or tail.count('"') % 2 == 0: break
        end = g.index(';', end + 1)
    src = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', g[start:end])).replace('\\n', '\n')
    names = []
    for decl in re.finditer(r'\b(?:Texture2D(?:<[^>]+>)?|RWTexture2D(?:<[^>]+>)?|RWStructuredBuffer<[^>]+>|StructuredBuffer<[^>]+>|SamplerState|ByteAddressBuffer)\s+(\w+)\s*:', src):
        names.append(decl.group(1))
    cb = re.search(r'cbuffer\s+\w+\s*:\s*register\([^)]*\)\s*\{([^}]*)\}', src)
    if cb:
        names += re.findall(r'\b(?:uint|int|float)\d?(?:x\d)?\s+(\w+)\s*;', cb.group(1))
    dup = [k for k, v in collections.Counter(names).items() if v > 1]
    print('%-18s %2d global names, duplicates: %s' % (m.group(1), len(names), dup if dup else 'none'))
    bad += len(dup)
sys.exit(1 if bad else 0)
