# D3D11 renderer regression checks

The D3D11 corrections use the Cxbx D3D8 implementation as reference:

- `Rendering/Backend/HostResourceUpload.cpp`: row/slice pitch and mip uploads.
- `Rendering/Backend/HostResourceCreate.cpp`: native volume resources.
- `Rendering/Backend/HostSync.cpp`: normalization of linear texture coordinates.
- `Rendering/Shaders/CxbxRegisterCombinerInterpreter.hlsl`: initial combiner
  alpha comes from T0 **after** the texture stages execute.

No other xemu renderer is used as an implementation reference for these fixes.
The existing common guest-state types and conversion helpers remain integrated.

## Local checks

From an MSYS2 UCRT64 shell with GCC, GLib and a configured xemu Meson build:

```sh
python tests/unit/test-d3d11-textures.py
python tests/unit/test-d3d11-shaders.py build-d3d11-check
ninja -C build-d3d11-check qemu-system-i386.dll
```

Use your build directory instead of `build-d3d11-check` if different. The shader
test needs generated headers and `compile_commands.json` from that directory.

The texture test checks padded rows, slices, channel ordering and swizzled
volumes. It also creates a native D3D11 WARP volume/SRV and reads both mip levels
back through a staging resource. Special-format conversion and compressed
textures are not covered by this isolated test.

The shader test compiles 18 generated HLSL pixel shaders (2D/3D, 0–8 combiners),
creates them with WARP and verifies constant-buffer size. These checks do not
install or launch UWP-Port, and are not Xbox/game compatibility tests.
An offscreen WARP draw also checks the resulting pixel for normalized linear
coordinates and the sampled T0 alpha passed into R0.

Shader cache version 8 invalidates old bytecode after the D3D11 screen-space Y
correction. Both vertex paths use the top-left D3D11 viewport convention, with
the native front-face flag mapping used by Cxbx. Fixed-function CMAT output is
perspective-divided before applying VPOFF; depth is normalized without an extra
W multiplication, matching Cxbx's fixed-function projection. Run
`python tests/unit/test-d3d11-vertex-output.py` to rasterize both real vertex
epilogues with WARP and check pixel position, homogeneous W and back-face
culling. Occlusion-query waits are bounded to five seconds; a failed query
marks the device for recovery instead of trapping the renderer indefinitely.
Vertex ranges are checked before inline-buffer/DMA reads.

## Suggested hardware parity test

Presentation preserves the guest pipeline across BEGIN/END and shares the
PGRAPH method lock to protect resource lifetime and command sequences. PCRTC
matching uses the framebuffer start, not the row pitch. Up to eight recently
replaced color targets are retained by COM reference, limited to 64 MiB total,
including buffers not yet selected by PCRTC. Cached targets are selected by
VRAM range and invalidated after CPU writes. No per-frame snapshot copy is
needed. Run `python tests/unit/test-d3d11-scanout-cache.py` for WARP pixel
readback across delayed flips, eviction and CPU invalidation. Each scanout
source kind is logged once per device lifetime.
For Counter-Strike/Metal Slug 3 corruption investigation, `D3D11 diagnostic`
records are emitted at most once per second, for at most 60 samples. They
include the selected source, current/cached VRAM ranges, CPU invalidations,
submitted batches, surface/scissor state and enabled texture formats. These
records do not prove successful rasterization. Capture separate logs from a
fresh process for each game; the visual fault is not yet runtime-validated.
Run `python tests/unit/test-d3d11-presentation.py` in MSYS2 UCRT64 to verify
native WARP state restoration on success/error and first-frame timing without
launching the emulator. A game still needs runtime testing with this package.

A test XBE should render a padded linear ARGB texture, a swizzled volume with
distinct slice colors and a two-level mip chain. Include a combiner case that
uses R0 alpha without overwriting it, and compare screenshots against a physical
original Xbox. Repeat with minification and linear texel coordinates. This XBE
is a suggested follow-up, not an implemented or executed test in this change.

## Remaining limits

These focused corrections do not establish complete NV2A coverage. The current
renderer still approximates point polygon mode with wireframe, returns black
for several unsupported/depth texture formats, and does not implement all NV2A
dependent texture operations. WARP success must not be reported as UWP/Xbox
runtime validation.
