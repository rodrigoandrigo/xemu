# D3D11 renderer regression checks

The D3D11 corrections use the Cxbx D3D8 implementation as reference:

- `Rendering/Backend/HostResourceUpload.cpp`: row/slice pitch and mip uploads.
- `Rendering/Backend/HostResourceCreate.cpp`: native volume resources.
- `Rendering/Backend/HostSync.cpp`: normalization of linear texture coordinates.
- `Rendering/Shaders/CxbxRegisterCombinerInterpreter.hlsl`: initial combiner
  alpha comes from T0 **after** the texture stages execute.

The common NV2A state, conversion helpers and texture-stage definitions are
also used to verify guest semantics. This backend does not call another
renderer to execute native D3D11 draws.

## Local checks

From an MSYS2 UCRT64 shell with GCC, GLib and a configured xemu Meson build:

```sh
python tests/unit/test-d3d11-textures.py
python tests/unit/test-d3d11-bind-texture-alias.py
python tests/unit/test-d3d11-shaders.py build-d3d11-check
python tests/unit/test-d3d11-depth-selection.py
python tests/unit/test-d3d11-polygon-line.py
python tests/unit/test-d3d11-presentation-hlsl.py
python tests/unit/test-d3d11-state.py
python tests/unit/test-d3d11-logicop.py
python tests/unit/test-d3d11-surface-formats.py
python tests/unit/test-d3d11-callback-lifetime.py
python tests/unit/test-d3d11-blits.py
python tests/unit/test-d3d11-blit-integration.py
python tests/unit/test-glsl-shadow-project2d-3d.py
ninja -C build-d3d11-check qemu-system-i386.dll
```

Use your build directory instead of `build-d3d11-check` if different. The shader
test needs generated headers and `compile_commands.json` from that directory.

The texture harness checks padded rows, slices, channel ordering, swizzled
volumes, full-precision Y16, fixed/24-bit/half-float depth conversion, border-
expanded 2D mip offsets, and explicit rejection of unknown formats. It contains
native D3D11 WARP resource creation/readback cases, including depth16; compressed
format coverage remains incomplete. The WARP harness now also binds an indexed
texture whose palette aliases a dirty render target, flushes the target to VRAM,
rebinds, and checks the updated texture pixel by GPU readback. These WARP cases
were not run in the current Linux session. The surface-format harness compiles the real color/ZETA validators,
accepts the ten defined color codes plus Z16/Z24S8, rejects reserved values
before target allocation, and runs in the Windows workflow.
The D3D11 output-merger mask is intersected with the logical guest components:
`R5G6B5` keeps RGB, `B8` maps to R, and `G8B8` maps to RG; alpha is enabled
only for formats whose surface mapping carries writable alpha. This matters
because the canonical BGRA8 target SRV is sampled directly by presentation before
some VRAM writebacks, so writes to absent guest channels could otherwise leak
into the displayed image. The portable surface-format harness compiles and
checks the actual mask helper for these cases and the X/A format variants.
For `X1R5G5B5_Z1R5G5B5`, target selection queries the active device and uses
`B5G5R5A1_UNORM` only when Texture2D, render-target, blendable and shader-sample
and CPU-lockable support are all reported and NV2A logic-op is disabled. Packed
upload/readback and masked clear are covered by portable source-helper checks;
pending draws are flushed before a target-format transition. The clear harness
contains an optional WARP capability/RTV/SRV/readback probe. If unavailable, the
backend warns and retains the BGRA8 approximation; that fallback still delays
5:5:5 RGB and one-bit alpha quantization until VRAM readback. Neither WARP probe
nor Xbox runtime parity has been validated in this Linux session.

The shader test compiles 18 baseline HLSL pixel shaders (2D/3D, 0–8 combiners)
plus shadow-compare cases for both projected modes, all eight comparison
functions and the three format divisors. It creates them with WARP and verifies
constant-buffer size. These checks do not
install or launch UWP-Port, and are not Xbox/game compatibility tests.
An offscreen WARP draw also checks the resulting pixel for normalized linear
coordinates and the sampled T0 alpha passed into R0.
The shader harness also includes an LSB-MUX generation/compile regression; it
has not been executed in this Linux session. Shadow `PROJECT2D` with a 3D depth
texture now uses the GLSL-defined `z=0` volume slice and compiles as `Texture3D`
on the Windows/WARP harness; the corrected GLSL reference uses the corresponding
`sampler3D` projective overload. The portable test compiles/runs the actual
HLSL-appending C helper and, when `glslangValidator` is installed, compiles the
corresponding GLSL expression. Full PS compilation on WARP and pixel readback
remain configured in CI but did not run here.

The depth harness compiles/creates eight triangle fill GS variants, eight
triangle LineStream and eight triangle PointStream variants, four quad-adjacency
variants per fill/line/point mode, four line GS variants, and the point-to-quad
GS (`gs_4_0` and `gs_4_1`), plus PS
variants with `SV_Depth` for perspective/affine reconstruction, clipping and
D16/D24/F16/F24; it also checks `gs_5_0`/`ps_5_0` when WARP provides FL 11.0 or
higher. Quads and quad-strips with Z16/Z24S8 depth use native D3D11 adjacency
input in every polygon fill mode. In LINE mode the GS emits only the four outer
edges (with the depth plane of the triangle owning each edge), avoiding the
internal triangulation diagonal. In POINT mode it emits the four original
vertices and selects the split triangle's depth plane for each one. The
adjacency GS also runs without custom depth, preserving the flat provoking
vertex; `SV_Depth` is applied only when the format requires it. For triangle
list/strip/fan/polygon POINT mode, a PointStream GS emits each source vertex and
selects the triangle's flat provoking vertex and depth plane; quad-strip skips
alternate adjacency primitives as the reference does. FILL mirrors the GLSL
triangulation. Triangle list/strip/fan LINE emits the three source edges through
LineStream with an edge-local depth plane. `PRIM_TYPE_POLYGON` LINE is lowered
to a closed boundary line list and a two-vertex GS; it uses zero depth slope and
the first endpoint for flat attributes, matching the current GLSL line-loop
path. This source-level alignment is not pixel-validated. The GLSL reference
rejects `PRIM_TYPE_POLYGON` + POINT; D3D11 now rejects it explicitly too. Lines,
line-strips and line-loops with Z16/Z24S8 use a line-input GS that recreates the
GLSL perpendicular depth plane and flat provoking endpoint. The line-loop draw
is lowered to line-list indices. Direct `PRIM_TYPE_POINTS` now uses a dedicated
GS that expands each source point into four clip-space vertices, derives its
pixel footprint from the scaled viewport and transported point size, and emits
interpolated point coordinates in TEXCOORD11. Point size travels as generic
TEXCOORD12 rather than relying on D3D11's PSIZE raster semantics. The point
draw disables polygon culling and forces solid fill; custom depth carries the
source point depth across the quad with zero slope. The Windows harness also
includes a minimal in-memory `D3DCompile2`
`ps_4_0` compile/create smoke test. The harness compiles and creates the GS
variants but does not rasterize them or compare pixels. The portable GCC test
compiles the real GS source generator and checks triangle LINE edge order,
triangle POINT provoking modes, quad LINE/POINT output order, split-depth
selection and flat/smooth varyings;
it does not validate HLSL with D3DCompile or rasterize the geometry. No WARP
test was run in this Linux session.

Projected 1D textures in PROJECT2D/PROJECT3D now use the existing one-row
Texture2D resource representation, sampling U from `x/w` and the row center
`v=0.5`; border expansion adjusts U only. The harness compiles/creates these
variants on SM4 with and without border adjustment, but does not yet upload or
read back a 1D fixture. Other dependent/reflection modes do not accept dimension
1. The current GLSL reference does not cover dimension 1, so this is an
unvalidated D3D11 extension rather than a demonstrated GL-parity result.

Presentation now contains a partial PVIDEO overlay path: the guest YUY2 buffer
is bounds-checked and converted on the CPU, then composited with the NV2A
position, scale, input offset, and color-key state. The WARP harness now
includes presentation VS/PS compilation and shader creation at SM 4.0/4.1,
checks the scanout vertical texcoord factor derived from source/display pitch,
and reads back both a two-row pitch-scaled fixture and a four-row full-height
mapping fixture. It also tests the CPU scale calculation. These checks have not
been run on Windows in this session. Interlaced field mapping, partial PCRTC
origins, and visual parity remain open gaps.

Shader cache version 15 invalidates old bytecode after the latest D3D11 shader
translation changes. Both vertex paths use the top-left D3D11 viewport convention, with
the native front-face flag mapping used by Cxbx. Fixed-function CMAT output is
perspective-divided before applying VPOFF; depth is normalized without an extra
W multiplication, matching Cxbx's fixed-function projection. Run
`python tests/unit/test-d3d11-vertex-output.py` to rasterize both real vertex
epilogues with WARP and check pixel position, homogeneous W and back-face
culling. Occlusion-query waits are bounded to five seconds; a failed query
marks the device for recovery instead of trapping the renderer indefinitely.
Vertex ranges are checked before inline-buffer/DMA reads.
Fixed-function NORMAL_MAP and sphere/reflection texgen now use the transformed,
skinned `tNormal`, matching the GLSL generator; a source regression guards the
generated expression, but no texgen pixel readback has been run.

## Suggested hardware parity test

Presentation preserves the guest pipeline across BEGIN/END and shares the
PGRAPH method lock to protect resource lifetime and command sequences. PCRTC
matching probes both the framebuffer start and its first scanout row, like the
existing OpenGL surface lookup. Up to eight recently
replaced color targets are retained by COM reference, limited to 64 MiB total,
including buffers not yet selected by PCRTC. Cached targets are selected by
VRAM range and invalidated after CPU writes. No per-frame snapshot copy is
needed. Run `python tests/unit/test-d3d11-scanout-cache.py` for WARP pixel
readback across delayed flips, eviction and CPU invalidation. Each scanout
source kind is logged once per device lifetime.
Scanout source transitions are logged up to eight times per device lifetime.
The first draw also logs the color target address, size, pitch and PCRTC start.
These records do not prove successful
rasterization. Capture separate logs from a fresh process for each game;
visual correctness is not yet runtime-validated.
Run `python tests/unit/test-d3d11-presentation.py` in MSYS2 UCRT64 to verify
native WARP state restoration on success/error and first-frame timing without
launching the emulator. `--host-only` runs the host-target registration/cleanup
GCC regression and checks that the queued SwapChainPanel callback does not
capture `XemuHost*`; it does not exercise the Windows Dispatcher. A game still
needs runtime testing with this package.

A test XBE should render a padded linear ARGB texture, a swizzled volume with
distinct slice colors and a two-level mip chain. Include a combiner case that
uses R0 alpha without overwriting it, and compare screenshots against a physical
original Xbox. Repeat with minification and linear texel coordinates. This XBE
is a suggested follow-up, not an implemented or executed test in this change.

## UWP integration and xboxrecomp reference audit

`xboxrecomp-wip-gpu-interp/src/video/nv2a_d3d11.c` was consulted for
native device creation, texture-stage sequencing and GPU surface lifetime.
Its title-specific font, flare and sky workarounds were not imported.

UWP-Port exposes `Native Direct3D 11 (experimental)` alongside OpenGL; the UWP
default remains D3D12. The selected enum is persisted and written to the emulator
configuration.
Composition presentation uses the existing brokered SwapChainPanel attachment,
without a desktop HWND, D3D11On12, debug layers or DRED. VSync follows settings.
The queued swapchain attach retains shared state rather than `XemuHost*`; teardown
disables queued work, unregisters the Mesa callback and clears the host presentation
target. Its GCC/source-level regression does not replace a Windows Dispatcher
shutdown stress test.
NV2A shutdown disables D3D11 VRAM callbacks and drains CPU 0's FIFO before PFIFO
stops, then drains asynchronous callback removals after renderer finalization and
before destroying the PGRAPH mutex. The callback also checks the active renderer
under that mutex, so a delayed D3D11 callback cannot interpret another backend's
state union. `test-d3d11-callback-lifetime.py` compiles these production functions
with GCC `-Werror` and mocks; it does not stress the live TCG/PFIFO teardown path.
The internal HUD uses ImGui DX11; OpenGL image decorations/thumbnails are
omitted rather than interpreting GL texture names as D3D11 SRV pointers.
Device suspension/recreation requests a HUD rebuild, executed exclusively on
the UI presentation thread. Resource teardown does not mutate ImGui from the
emulation thread.
Font-size/DPI changes invalidate and rebuild DX11 device objects before the
next ImGui frame. Run `python tests/unit/test-d3d11-hud-fonts.py` to reproduce
the old unbuilt-atlas assertion and validate four rebuilds with native WARP.
Pixel constants use identity coordinate scales for disabled texture stages,
without decoding their uninitialized mip registers. Run
`python tests/unit/test-d3d11-disabled-textures.py` for this regression.

Clip-plane, dependent AR/GB, bump environment/luminance, dot-product, DOT_ST,
DOT_STR_3D/CUBE, diffuse/specular reflection, all eight final-combiner mappings,
quantized alpha test, and the three color-key actions have explicit HLSL
translations. Y16 luminance textures are converted to replicated
`R16G16B16A16_UNORM` samples, preserving the 16-bit luminance and alpha-one
semantics of the OpenGL `R16` texture swizzle.
The dot-map 5–7 fallback mirrors the current GLSL identity fallback and BRDF
still mirrors the current GLSL zero-output stub. `DOT_RFLCT_SPEC_CONST` now
reads the three `NV_PGRAPH_EYEVEC` registers and uses the Khronos constant-eye
reflection equation; this intentionally goes beyond the current GLSL zero
stub and remains unvalidated against Xbox output. Harness cases cover the
translated mappings, comparators,
color-key modes, Y16 CPU/GPU precision, border storage, and all eight shadow
comparators in both PROJECT2D/3D. Unsupported shadow modes fail explicitly.
These cases were not executed in this Linux session. The Windows workflow now
runs the D3D11/GLSL regressions through MSYS2 UCRT64 when triggered.

`PROJECT2D` with NV2A `CONVOLUTION_2D_LOD0` now emits the nine Gaussian 3×3 taps
from the GLSL reference, including real texture dimensions, border-coordinate
adjustment, and the homogeneous divide after each texel offset. Quincunx follows
the reference's current Gaussian `FIXME`; it is not a separate NV2A kernel.
Unsupported 3D/cubemap combinations fail explicitly. Samplers also clamp to mip
level zero for filters 1, 2, and 7, and for linear textures, so D3D11 does not
select mipmaps where GL/D3D12 disable them. The portable
`python tests/unit/test-d3d11-texture-filters.py` compiles the production helpers
with GCC `-Werror`, validates generated weights/coordinates and LOD policy, and
uses glslang when available. The Windows shader harness compiles the generated
variants in SM4 and now contains WARP impulse readbacks for center/edge/corner
weights; none of those WARP cases or GL/Xbox golden comparisons has run here.

NV2A `CLAMP_OGL` (address mode 5) follows the effective xemu OpenGL mapping to
`GL_CLAMP_TO_EDGE`: normalized sample coordinates are clamped per axis in the
generated pixel shader, `SampleGrad` preserves the original derivatives, and
the D3D11 sampler uses `D3D11_TEXTURE_ADDRESS_CLAMP`. This avoids a border-color
blend that diverged from the OpenGL backend. GCC/glslang checks the production
helpers; the Windows harness contains linear/nearest WARP edge readbacks at and
outside 0/1 with distinct edge texels, but neither WARP nor a pixel comparison
with OpenGL/Xbox ran in this Linux session. 3D and cube behavior remain to be
validated. Address mode 0 and invalid values retain the existing clamp fallback;
literal legacy `GL_CLAMP` semantics may differ from this backend mapping.

Fixed-function sphere/reflection texgen is emitted per component, so mixed modes
preserve disabled and independently generated channels; invalid mode/channel
pairs fail explicitly. Fixed-function and programmable fog now convert distance
to the NV2A per-vertex fog factor for LINEAR/EXP/EXP2 and ABS modes, including
NaN/Inf handling; the programmable VS cache key includes fog enable/mode. Run
`python tests/unit/test-d3d11-vertex-output.py --texgen-only` for portable GCC
`-Werror` regressions of texgen, generated fog strings and cache-key separation.
The full Windows/WARP mode additionally compiles the generated fog fragments via
D3DCompile2 and creates VS objects for `vs_4_0/4_1/5_0`; it has not been run in
this session and does not replace pixel-level parity testing.
`python tests/unit/test-d3d11-textures.py --overlap-only` likewise tests the real
VRAM-range helper with GCC and exits before its D3D11/WARP resource harness.
For swizzled cubemaps with a border and one mip, D3D11 now crops the four-texel
border from every face, matching the current GL upload path; the portable test
checks the production crop helper and the Windows harness contains a six-face
WARP readback. The WARP fixture has not run here. Border cubemap mip chains and
3D border sampling remain unresolved. 3D volumes retain the expanded dimensions
used by the GL upload path; the portable harness now tests the production extent
helper (including overflow rejection), and the Windows harness contains a
two-mip 3D border upload/readback fixture. That WARP fixture has not run here, so
3D sampling and mip-level pixel parity remain unvalidated.

Cubemap creation now explicitly rejects linear formats, dimensionalities other
than 2, and non-square faces before calling D3D11. This matches the GL cubemap
target constraints and avoids relying on a later API failure; the GCC regression
tests the production predicate. It does not define or resolve bordered cubemap
mip-chain behavior, which remains ambiguous in the GL upload path.

Bordered cubemaps with more than one mip are now rejected before DMA range
calculation/VRAM hashing and again before D3D11 resource creation. The GL upload
path does not define a safe mip-chain layout for this case, so D3D11 does not
guess at one; the portable GCC test covers the production predicate and guard
ordering. This is an explicit unsupported case, not a parity claim.

Device creation now includes FL 10.0 and FL 10.1 fallbacks; shader profiles are
selected from the created feature level (`vs/ps_4_0`, `_4_1`, or `_5_0`). This is
necessary because public Xbox One UWP references disagree on whether FL 10.0 or
10.1 is the effective minimum; confirm the actual device on each target console.
The harness is extended for WARP FL 10.0/10.1, but was not run here.

**UWP shader compilation is not yet validated on retail Xbox.** Runtime-generated
vertex, pixel, geometry and presentation HLSL now uses `D3DCompile2`. Microsoft’s
UWP-specific `D3DCompile2` remarks describe compiling loaded/generated shaders at
runtime, while recommending offline compilation where possible. Another Microsoft
page says `D3DCompileFromFile` cannot be used in Store-submitted apps; this backend
does not call that file API. The documentation is not a retail Xbox package test,
so confirm compiler-library availability, policy and startup cost on the actual
target before treating deployment as supported. The Windows/WARP harness is
developer validation only. Active surface/texture overlap still resolves to guest
VRAM before upload, using CPU readback rather than a full GPU-only render-to-texture
cache.

## Remaining limits

These focused corrections do not establish complete NV2A coverage. Triangle
list/strip/fan LINE, `PRIM_TYPE_POLYGON` LINE, triangle/quad/quad-strip
LINE/POINT, and direct `PRIM_TYPE_POINTS` use geometry-shader output streams;
none has raster readback or pixel comparison yet. Five depth formats
are packed into RGBA8_UNORM (filtered sampling is required at FL10.0, unlike
R32_FLOAT) and compared in PROJECT2D/PROJECT3D, while unknown formats and shadow
modes without a GLSL equivalent are rejected. Other special/compressed
formats still need dedicated tests, and custom `SV_Depth` has no pixel-readback
or OpenGL comparison coverage for triangles, quads, lines, or points. With
Z16/Z24S8, filled triangle
list/strip/fan/polygon, quads, quad-strips, lines, line-strips, line-loops, and
points select the custom-depth PS path; direct points now pass through the
point-to-quad GS before the custom-depth PS. Different
front/back polygon modes are now rejected before shader setup rather than
silently rendered as FILL; the OpenGL reference also marks two-sided polygon
modes unsupported. For `surface_scale_factor > 1`, D3D11 expands direct lines and LINE polygon
modes into screen-space rectangles through a geometry shader; 1× keeps the
native line path. Joins, caps, coverage, antialiasing, and the OpenGL device-
dependent width clamp remain unvalidated.
Mappings 5–7 and BRDF only mirror current GLSL fallbacks;
constant-eye reflection now uses `NV097_SET_EYE_VECTOR` and the Khronos equation,
but diverges from the current GLSL zero stub until that renderer is implemented
or a compatibility decision is made. Reflection, DOT_STR, MUX, final-combiner,
alpha-test and color-key results have no pixel comparison against OpenGL, and no
end-to-end game/Xbox validation was performed. The D3D11 blend-state guard now
does not reject simultaneous `CONSTANT_COLOR` and `CONSTANT_ALPHA` factors when
the NV2A equation is MIN or MAX: those equations use the unmodified source and
destination values, so the factors and blend-constant vector do not affect the
result. This is confirmed by the NV097 equation method values `0x8007`/`0x8008`
in `nv2a_regs.h`, their encoding as `NV_PGRAPH_BLEND_EQN` values 3/4 in
`pgraph.c`, and the matching MIN/MAX entries at indices 3/4 in the D3D11, GL,
and Vulkan maps. The API definitions likewise specify MIN/MAX without factors
([D3D11](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_blend_op),
[OpenGL](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glBlendEquation.xhtml),
[Vulkan](https://docs.vulkan.org/refpages/latest/refpages/source/VkBlendOp.html)).
The D3D11 rejection remains for ADD/SUBTRACT/REVERSE_SUBTRACT, where one
NV2A constant-alpha factor and one constant-color factor cannot both be
represented by D3D11's single blend-factor vector. No other blending behavior
or backend was changed. `python tests/unit/test-d3d11-state.py` compiles the
production D3D11 equation-map, conflict predicate and constant helpers with GCC
`-Werror`; it covers indices 3/4 and confirms arithmetic equations still reject
the mixed factors. This source-level test does not execute D3D11, WARP, Windows,
Xbox, or compare pixels.


Indexed `I8_A8R8G8B8` texture binding now checks both the texel VRAM span and,
when applicable, the palette span against dirty color/ZETA render targets before
reading VRAM. `python tests/unit/test-d3d11-textures.py --overlap-only` compiles
the production overlap decision with GCC and covers palette-only aliasing,
non-indexed textures, texel aliasing, adjacent boundaries and empty ranges. This
is a portable source-helper regression; it does not exercise D3D11/WARP binding,
render-to-texture feedback, or Xbox runtime behavior.


The texture bind translates texel and indexed-palette offsets through the selected DMA-A/DMA-B object and validates the full range against both that mapping's declared length and VRAM before reading. D3D11 texture, vertex, blit and render-target paths use checked DMA object loading/mapping, which rejects truncated RAMIN objects, classes other than `NV_DMA_IN_MEMORY_CLASS`, and bases outside VRAM without invoking the legacy mapper's assertions. The checked mapping length is capped to the bytes physically remaining in VRAM, so vertex/blit callers cannot trust an oversized DMA limit to extend the host pointer past the VRAM buffer. The mapper still does not interpret `dma_target`; PCI/AGP target behavior is not claimed. Other renderer paths still use the legacy API. `python tests/unit/test-d3d11-bind-texture-alias.py` compiles the production bind and checked mapper with GCC `-Werror`, covering DMA-window boundaries, invalid classes, truncated objects, invalid bases and VRAM-edge length clipping. The Windows/WARP fixture in `test-d3d11-textures.py` remains unrun on Linux. This does not replace an integrated build, pixel comparison, or Xbox/UWP validation.


## Integrated image-blit regression

Run `python tests/unit/test-d3d11-blit-integration.py` to compile and execute the production `pgraph_d3d11_image_blit`, checked DMA mapper, and GPU-tile clip helper together. Runtime/D3D, flush, dirty-memory, and surface-invalidation edges are mocked. The fixture covers partial-byte `SRCCOPY` clipping, complete-pixel `BLEND_AND` clipping, inclusive DMA-end acceptance, overlapping source/destination DMA mappings checked against the original source snapshot, and atomic rejection of a short DMA range, truncated RAMIN descriptor, or out-of-VRAM DMA base. It also checks 512 deterministic varied cases against a byte-wise snapshot reference model across all six supported surface formats (1-, 2-, and 4-byte pixels), operation, valid page-aligned DMA base, pitch, coordinates, offsets, overlap, alpha patching, beta, and tile clipping. This is portable source-level integration coverage; it does not execute D3D11/WARP or establish pixel/game/Xbox parity.
