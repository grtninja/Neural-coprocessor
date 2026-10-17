// tex_census.hpp - R279: what screen-sized textures a D3D12 title keeps alive,
// in every format. An INSTRUMENT, removable: one file pair, one key.
//
// MVEC_D3D12_LEDGER.md, RE4-L2. The velocity census (probe.cpp) only admits
// two-channel formats (R16G16_*, R32G32_*). On RE4 at native 2560x1440 the
// only two-channel buffer alive in gameplay is a 1280x720 one that is not
// velocity. This module answers the next question by watching, not touching:
// which textures between half and full scene size are alive in gameplay, in
// what format, with what usage flags, and how the engine transitions them
// (barrier counts, UAV writes, UAV -> SRV reads). A velocity buffer packed in
// another format shows up here; if nothing velocity-shaped does, the census
// is not the route for this engine.
//
// OFF unless TexCensus=1 in mgpu.ini (one ini read at the D3D12 game chain's
// init). Off: nothing registered, no per-frame work. On: observe-only - it
// never creates, binds or copies anything on the game's device. Reports go
// to the log through mgpu::diag (the log queue, never written on the game
// thread): one header and up to 24 lines, at game frames 1200, 3000, 6000.
//
// R303: three report modes, each behind its own key - fixed (TexCensus=1
// alone: frames 1200/3000/6000), periodic (TexCensusEvery=N: also every N
// frames after 6000, 12 at most) and hotkey (TexCensusHotkey=1: CTRL+ALT+END
// - R308; CTRL+ALT+F5 before, which opens Far Cry 6's photo mode - reports
// now, and arms the peek when TexPeek=1). A second D3D12 chain no
// longer overwrites the scene size, and dllmain calls init for the game's
// chain only. The report header names the mode and counts table overflow.
//
// R304: the table holds 1024 textures (was 256), and every report adds one
// line of live formats by count and a list of every live two-channel texture
// in the band (48 at most), whatever its activity rank. R304b: still a fixed
// table with the original scan and sort - nothing allocated on the game's threads.
//
// R279b: TexPeek=1 (with TexCensus=1) adds the PEEK: the texture with the
// velocity USAGE signature (two channels, scene size, into RT and RT->SRV about
// once per frame) is copied three times from its known state into one
// readback buffer and its per-channel distribution logged. Unlike the census
// this creates one resource and three copies on the game's device.
//
// TO REMOVE: delete these two files, the line in CMakeLists.txt, and the
// lines in dllmain.cpp marked R279 (include, init, two shutdowns).
#pragma once

namespace mgpu::tex_census
{
    // D3D12 game chain at init (dllmain). Reads TexCensus once; off = nothing.
    void init(void *game_device, unsigned screen_w, unsigned screen_h);
    // Game device destroyed (dllmain). Unregisters what init registered.
    void shutdown();
}
