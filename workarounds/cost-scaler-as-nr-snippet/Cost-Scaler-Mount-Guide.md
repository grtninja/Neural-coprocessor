# Mounting a third-party DLSS-NR proxy as the private NR snippet

Unsupported. Third-party. Nothing in this folder is built, packaged or tested by
CI, and nothing here is part of the add-on.

## What the arrangement is

The bridge loads `nvngx_dlssnr.dll` from a `mgpu\` folder beside the game's
executable as its private NR snippet - the log line that says so is
`[P1.0c] ... (used:snippet)`. DLSSNR-Cost-Scaler is a proxy: it intercepts four
NGX entry points and carries the rest of the snippet's export surface as linker
forwarders to `nvngx_dlssnr_real.dll` beside it. Placed at that path it takes
over the neural stage without renaming anything the game ships, and it runs the
NR model at a reduced internal resolution while the picture that reaches the
screen stays at the game's own resolution.

That is a different thing from `SRUpscale=1` with `SRScale=N`, which shrinks the
whole output and lets DLSS Super Resolution scale it back up - on a title that is
not already running DLSS SR there is nothing to scale it back with, and the
picture softens. The proxy leaves the frame the game produced at native
resolution and adds the model's output back as a residual, so no detail has to be
re-invented at display size. It is not free: its own composite passes cost 7-8 ms
of latency here, so it only pays when the neural stage is what is holding the
frame rate back.

## Files

```
<game>\bin\
    nvngx.dll_mgpu_bridge.addon64     the bridge
    mgpu.ini                          DcompOverlay=1 and the keys in MGPU-Keys.txt
    mgpu\
        nvngx_dlssnr.dll              the proxy, built from the project above
        nvngx_dlssnr_real.dll         the real snippet: same folder, exact name
        nvngx_dlssnr.ini              optional, see the lookup order below
```

The file name `nvngx_dlssnr_real.dll` is baked into the proxy's export table at
link time, and a forwarder is resolved by the loader against the modules already
in the process or on the standard search path - never against the proxy's own
folder by name alone. The real snippet must therefore keep that exact name and
sit beside the proxy, or the module answers four exports and nothing else.

## The 0.3.1 requirement

`[P4.1]` resolves `NVSDK_NGX_D3D12_PopulateParameters_Impl` **strictly** from the
private snippet, before anything has called `Init` on it. A forwarded name cannot
answer until the module it forwards to has been loaded, and that module is loaded
by the proxy's own `Init` - so on 0.3.1 the resolve fails first and the stream
comes up without a neural stage:

```
[MGPU][P4.1] an NGX entry point did not resolve - transport-only
```

Either side can remove this, and both are proposed:

- the proxy can load its real module at attach instead of on first use, or
- the bridge can load a forwarder target that is present beside the snippet
  before the strict resolve, and name the entry point it could not resolve.

With one of the two in place the mount works on 0.3.1: `[P4.1] Init_Ext:
result=0x00000001 (Success)` appears and the neural stage runs. This was the
entire difference between 0.3.0 and 0.3.1 on this machine - the older build does
not ask for that name.

## The proxy's own configuration

`LoadConfig` looks for `nvngx_dlssnr.ini` in this order: the executable's own
directory first, and only if a file exists there; otherwise the copy beside the
proxy; otherwise built-in defaults (`ResolutionScale=0.75`). Whichever file is
picked is also the file the panel and the hotkeys write back to. Here one title
has a file in the executable directory and the other does not, so the two titles
read different files - worth knowing before concluding that a change "did not
stick".

## What to look for in ReShade.log

```
[P1.0c] ... (used:snippet)                  the proxy is the snippet
[P4.1] Init_Ext: result=0x00000001          the neural stage started
[R78] MVEC ARM: mode=3 REAL                 vectors are flowing
```

and in the proxy's own log, `mgpu\nvngx_dlssnr_proxy.log`:

```
[Proxy] Loaded real module successfully
[Proxy] Allocated slot 0 textures: work=WxH, native=WxH (Scale=...)
```

The second line is the whole trade in one place: the model runs at `work`, the
screen gets `native`.

## What was measured here

Two RTX 5070 Ti, 3840x2160, Far Cry 6, render card headless and the neural card
driving the display, `DcompOverlay=1`. Lowering the model's internal resolution
moved the card that runs the neural stage, and the screen followed it:

| neural card (also the display card) | presentation |
| ----------------------------------- | ------------ |
| 96-98%                              | 40-50/s      |
| 77-80%                              | 54.8-66.4/s  |

Paired A/B at 0.65 against 1.00: **+20.1 / +14.1 / +16.6 fps** (three pairs, same
sign in all four comparisons). The same title with the GPU pin moved so that the
neural result has to cross the bus before it reaches the display: **no gain at
all** (lag ratio 0.97-1.11 over 11 probes, render card at 98-99%). In other
words the knob is worth what the arrangement allows it to be worth, not what the
dial says.

Two cautions for anyone repeating this:

- **The only frame rate is `[T5] present loop alive`.** Transport and processing
  rates can overstate the screen by 39% to 147%; 74/74/30 has been measured in the
  same instant here. Two of my own earlier conclusions had to be withdrawn for
  reading the wrong number.
- **The proxy's own governor uses a producer-side interval** (the gap between two
  `EvaluateFeature` calls), so it does not act in the case it exists for: it
  reported 64 while the screen was at 46.

## What is not verified

Two titles on one machine, one of them without a proxy at all. The claim that the
picture does not soften is a **user judgement, not a measurement** - anyone
enabling this should make their own `Ctrl+Alt+F7` split comparison rather than
take it from here. A title without native DLSS has not been tried at all. The
arrangement in which the neural card does not drive the display gains nothing, as
above.
