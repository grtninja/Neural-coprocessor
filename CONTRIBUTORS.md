# Contributors

Code contributed by people other than the primary author, and what it covers.

## [NO1950](https://github.com/NO1950)

Two changes to the add-on, from a two-GPU machine where the composition mode
and the overlay could not be used at the same time:

- **The overlay relationship.** The overlay request is refused on the game's
  runtime and opened on the GPU 1 runtime instead, whose swapchain is the
  composition swapchain - the neural picture - so the panel is drawn over the
  live neural frame rather than in place of it. The host runtime is identified
  by having no window and is recorded when the panel opens; input is fed from
  `reshade_overlay` (the only per-frame callback that is not behind a tab
  selection) out of the game runtime's own `ImGuiIO.MousePos`, and the game's
  input is blocked while the panel is up. Opt-in via `PanelOverPicture`.
  Verified on Far Cry 6 and Kingdom Come: Deliverance II, in opposite
  arrangements of the two cards.

- **The P4.1 strict lookup against a proxy-style snippet.** `[P4.1]` resolves
  `NVSDK_NGX_D3D12_PopulateParameters_Impl` strictly from the private snippet,
  which cannot answer while that snippet's surface is made of linker
  forwarders to a module it loads later. The forwarder target is now loaded
  before the strict resolve, and the failure line names the entry point.

## [grtninja](https://github.com/grtninja)

Two fixes to the add-on's own code, reviewed and merged from external pull
requests:

- **Adapter selection.** When the game's swapchain-derived adapter LUID is
  absent from DXGI enumeration, the bridge no longer infers the target from
  output counts - a missing game LUID is now a hard identity gate ahead of
  candidate and tiebreak selection. The selection policy was factored into a
  small pure helper so the boundary is directly regression-tested, with
  table-driven coverage for missing-LUID cases, software filtering, no
  hardware candidates, output tiebreaks, LUID high-half mismatch, and
  enumeration reorder.
- **`mgpu.ini` parsing.** The previous reader accepted a silently clipped
  document past its 8192-byte buffer, so settings after the cutoff read as
  absent with no warning - a real problem once the shipped config itself grew
  past that size. The bounded read is now 64 KiB, a document that doesn't fit
  is rejected outright rather than partially applied, and the line-key parser
  is shared with a portable, CPU-only regression suite covering encoding
  (UTF-8 BOM, UTF-16 rejection, malformed UTF-8, embedded NULs, control
  bytes) and boundary conditions.

Thank you for both - especially for the regression coverage that makes these
correctness properties checkable going forward rather than just asserted.

## Mohammed Hasan - [MoHasan9505](https://github.com/MoHasan9505)

His test system: NVIDIA 617.14 (32.0.16.1714) on the RTX 5070, AMD Adrenalin
32.0.31041.1004 on the Radeon AI PRO R9700, and one display connected to the
RTX 5070 over HDMI.

Two findings against the add-on's NGX and teardown code, from a fork where
an AMD card renders the game and an RTX card runs DLSS-NR
([nr-redgreen](https://github.com/MoHasan9505/nr-redgreen)). Both merged in
a guarded shape, under his name:

- **The core `Init` takes four arguments.** The driver's `_nvngx.dll` export
  of `NVSDK_NGX_D3D12_Init` is the four-argument form; called through the
  SDK's five-argument typedef, the driver read a stack address as the
  version and the check passed or failed by ASLR - the intermittent
  `FAIL_OutOfDate` on roughly every other launch (#44). His disassembly on
  driver 617.14 is the first explanation of that failure that holds, and it
  also explains a chase on the primary author's side. Merged as the last
  rung of a ladder: the four-argument call is tried only after the
  five-argument one has returned `FAIL_OutOfDate`, so a driver whose export
  really takes five arguments is never handed a junk parameter.
- **The GPU 0 teardown drain never ran.** The drain checked a fence one line
  after that fence had been released, so it was dead code and the shared
  transfer buffer was freed with no wait. His fix waits on the game-side
  fence, for the last value actually signalled, so a normal exit waits
  nothing. Merged as written, plus a timing line on every teardown and a
  switch the add-on throws itself if the wait ever stalls, because it turns
  on a wait no shipped build had executed.

Thank you for both, and for the disassembly in particular.
