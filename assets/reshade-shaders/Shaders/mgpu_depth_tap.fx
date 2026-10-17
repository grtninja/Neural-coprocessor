// MGPU Bridge - DEPTH TAP. See history/P0_DENOISE_RECORD.md R53 and R59.
//
// P0.0 sampled depth and discarded every pixel, purely to keep ReShade's DEPTH
// semantic bound. P0.1 keeps that job and adds a second one that may remove an
// entire subsystem from the transport.
//
// WHY THERE IS NOW A RENDER TARGET
//
// The game renders depth at 65-67 percent of output (R54), and DLSS-NR runs at
// OUTPUT resolution. Something has to resample. Doing it on GPU 1 means
// building a root signature, a shader, a PSO and a descriptor heap in a
// project that today has NONE of those - grep the whole of gpu1_context.cpp
// and there is not one CreateRootSignature, CreateComputePipelineState or
// D3DCompile in it.
//
// This pass is already a shader, already runs on GPU 0, and ReShade already
// renders it at BUFFER_WIDTH x BUFFER_HEIGHT - which IS output resolution. So
// sampling the render-resolution depth here and writing it to a full-size
// target does the resample for free, in code that already exists, with ReShade
// owning every piece of state.
//
// It also removes two other named risks: the target is single-plane R32F, so
// none of the R32G8_TYPELESS plane-0 footprint care applies to what crosses
// the bus; and reversed-Z becomes a one-line decision in a file we own rather
// than a guess about what DLSSNR.DepthInverted means.
//
// IT COSTS BUS BYTES. Output-resolution R32F is the same size as the colour
// payload, where render-resolution depth would have been about 42 percent of
// it. That trade is the open question, not this file.
//
// AND IT MAY NOT BE READABLE AT ALL - WHICH IS WHAT THIS BUILD TESTS
//
// Every run so far shows get_texture_binding returning res=0x0 for ordinary
// effect textures - texLUT, BackupTex, MaskTex - and a real resource ONLY for
// DepthBufferTex, which is bound by SEMANTIC. Every one of those was in a
// DISABLED effect, so it proves nothing either way. This technique is enabled,
// so MGPU_DepthOutTex answers it: the semantic lane already walks every texture
// variable and will print this one with no C++ change at all.
//
//   a real resource on the MGPU_DepthOutTex row  -> the whole GPU 1 shader
//                                                   subsystem is unnecessary
//   res=0x0                                       -> it is necessary, and we
//                                                   learned that for free

// ---- 0.2.1: NO INCLUDE. THIS FILE STANDS ALONE. ----
//
// 0.2.0 opened with #include "ReShade.fxh", and a user reported the add-on
// failing after skipping the standard effects pack in the ReShade installer.
// They were right and the cause was this line: that header ships WITH the
// effects pack. Without it the tap does not compile, the log says TAP =
// ABSENT, and with Depth=1 the bridge never arms. 0.1.0 needed no effects at
// all, so 0.2.0 quietly added an install step and only found out from a user.
//
// The header supplied three things to this file and only two came from it:
//
//   ReShade::DepthBuffer   a texture with the DEPTH semantic, and a sampler
//   PostProcessVS          the fullscreen-triangle vertex shader
//   BUFFER_WIDTH/HEIGHT    NOT from the header. ReShade's compiler injects
//                          these; the header consumes them like anyone else.
//
// Both are declared below, which is eight lines. Nothing else in that header
// was used.
//
// AND IT IS MORE CORRECT THIS WAY. ReShade.fxh exists mostly to apply depth
// transforms - reversed-Z, upside-down, mirrored, scale and offset, the
// linearizer. This file samples RAW depth on purpose (see PS_MGPUDepthTap
// below) because the rig carries RESHADE_DEPTH_INPUT_IS_REVERSED=0 on a
// reversed-Z title, so the linearizer is wrong here and the reversed-Z
// decision belongs next to the model instead. Including a header whose whole
// job is transforms this file then refuses to use was backwards.
//
// The semantic is what matters, and the semantic is declared here directly.
// ReShade's semantic system is a PUSH: it writes the detected buffer into
// EVERY texture variable declared with that semantic, so ours is filled the
// same way the header's would have been.

texture MGPU_SceneDepthTex : DEPTH;
sampler MGPU_SceneDepth { Texture = MGPU_SceneDepthTex; };

// The fullscreen triangle, bufferless. Same three vertices the standard
// header uses, under our own name so the two cannot collide if some other
// effect in the path does include it.
void MGPU_FullscreenVS(in uint id : SV_VertexID,
                       out float4 position : SV_Position,
                       out float2 texcoord : TEXCOORD)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// R312 (DepthTransport): the add-on pushes these as preprocessor definitions for
// this effect only. Absent, the defaults are exactly the texture that shipped.
#ifndef MGPU_DEPTH_WIDTH
#define MGPU_DEPTH_WIDTH BUFFER_WIDTH
#endif
#ifndef MGPU_DEPTH_HEIGHT
#define MGPU_DEPTH_HEIGHT BUFFER_HEIGHT
#endif
#ifndef MGPU_DEPTH_FORMAT
#define MGPU_DEPTH_FORMAT R32F
#endif
texture MGPU_DepthOutTex
{
    Width  = MGPU_DEPTH_WIDTH;
    Height = MGPU_DEPTH_HEIGHT;
    Format = MGPU_DEPTH_FORMAT;
};

// ---- R69: THE MOTION PROBE, AND IT COSTS NOTHING ----
//
// ReShade's semantic system is a PUSH: whoever detects a buffer writes it into
// every variable declared with that semantic (R48). DEPTH is the one this rig
// has. Whether anything on this machine publishes MOTION - an optical-flow
// add-on, a future ReShade, the game's own vectors through some path - has
// never been asked, and asking costs one texture declaration.
//
// The semantic lane already walks EVERY texture variable and prints what each
// is bound to. So this needs no C++ at all: a real resource on the
// MGPU_MotionProbeTex row means motion vectors are available the same way
// depth turned out to be, and res=0x0 means they are not.
//
// R66 bound depth and the picture did not change. The leading explanation is
// that depth's job is reprojection and MVec is null, so depth has nothing to
// work with. If that is right, THIS ROW is the next thing that matters.
texture MGPU_MotionProbeTex : MOTION;
sampler MGPU_MotionProbe { Texture = MGPU_MotionProbeTex; };

float PS_MGPUDepthTap(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    // tex2Dlod, which is what the standard header used on this sampler, so
    // 0.2.1 samples exactly what 0.2.0 sampled.
    //
    // Raw, NOT GetLinearizedDepth: this rig carries
    // RESHADE_DEPTH_INPUT_IS_REVERSED=0 on a UE5 reversed-Z title (R51), so
    // the linearizer is wrong here. Raw means this file has no opinion, and
    // the reversed-Z decision stays where it belongs - next to the model.
    //
    // The DEPTH sampler filters LINEAR by default, so sampling a render-resolution
    // depth at output-resolution coordinates IS the upsample. Bilinear across
    // a silhouette invents a depth that exists nowhere, which is the known
    // cost of this and is a one-line change to POINT if it shows.
    const float d = tex2Dlod(MGPU_SceneDepth, float4(uv, 0, 0)).x;

    // R69: the motion probe is SAMPLED so the compiler cannot strip the
    // texture and take the semantic with it. The test is only true for NaN,
    // which no real sample and no unbound black texture produces - so the
    // returned value is ALWAYS d and this line changes nothing but the
    // compiler's dead-code analysis.
    const float2 mv = tex2Dlod(MGPU_MotionProbe, float4(uv, 0, 0)).xy;
    return (mv.x != mv.x) ? 0.0 : d;
}

technique MGPU_DepthTap
<
    ui_tooltip = "MGPU Bridge: keeps ReShade's DEPTH semantic bound and mirrors "
                 "depth at output resolution. Draws nothing on screen. The "
                 "add-on enables this automatically; you do not need to.";
>
{
    pass
    {
        VertexShader = MGPU_FullscreenVS;
        PixelShader  = PS_MGPUDepthTap;
        // The back buffer is never touched now - P0.0 needed a discard to stay
        // invisible, this writes somewhere else entirely.
        RenderTarget = MGPU_DepthOutTex;
    }
}
