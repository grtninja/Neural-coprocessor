**Status: worked here, unverified elsewhere.** A third-party DLSS-NR proxy
placed where the bridge looks for a private NR snippet, so the NR model can run
at a reduced internal resolution while the frame that reaches the screen stays
point-to-point. This is the case the bridge's own `SRUpscale`/`SRScale` cannot
cover outside a title that is already running DLSS Super Resolution.

One change is required before the snippet initialises at all on 0.3.1 - see
"The 0.3.1 requirement" in the guide. Either side can remove it, and both are
proposed.

DLSSNR-Cost-Scaler is a separate project:
https://github.com/xenmods/DLSSNR-Cost-Scaler
These files are not part of MGPU Bridge and this arrangement is not supported.

Instructions: Cost-Scaler-Mount-Guide.md
Keys used here: MGPU-Keys.txt
Measurements and reasoning: issue #54
