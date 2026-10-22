# Native D3D12 integration

## Reference adaptation, 2026-10-20

This backend uses the tested `xemu-uwp-uwp-d3d12` implementation as a reference,
not as a drop-in replacement. That checkout uses a different HLSL/vertex-pulling
architecture, additional frontend APIs, fastmem and custom-texture services.
This checkout retains its common NV2A GLSL-to-SPIR-V-to-DXIL translation and its
UWP SwapChainPanel presentation contract. D3D11 and OpenGL remain available.

### Corrected paths

- Vertex packing converts the used min/max interval, rebases indices and sizes
  the upload/view to the packed interval rather than the absolute guest index.
  Attribute state follows the last guest input, not the largest index.
- Stride-zero attributes read their DMA element; inline buffers are already
  float4 and bypass packed-array decoding. UB_D3D with three components now
  applies the same channel swap as the four-component form without reading a
  fourth input byte. Invalid/overflowing vertex ranges use checked DMA access.
- Surface bindings capture their storage type. Upload, clear mirroring and
  readback no longer interpret an old binding using the next surface's layout.
  Dirty GPU data is preserved before binding replacement and frontend surface
  transitions. Shader binding dimensions are updated before use.
- Surface DMA objects are checked; the physical VRAM range is validated with
  overflow checks. As in the established renderers, temporarily incomplete
  render-target DMA limits are advisory rather than rejecting valid VRAM.
- Depth/stencil copy paths handle both subresources of D24S8/D32S8. Upload packs
  depth/stencil into separate placed footprints and readback copies both planes.
  Fixed Z16 uses D16_UNORM, while floating Z16 retains D32_FLOAT. Stencil-only
  clears are not submitted against depth-only formats. UNORM24 conversion
  preserves endpoint/upper-range values instead of overflowing at 1.0.
- GET_REPORT captures its DMA destination and validates all 16 output bytes.
  Scaled Z-pass values saturate at UINT32_MAX, matching the D3D12 reference.
  Pending report consumption no longer requires an empty PFIFO, and public
  consumption is protected by the PGRAPH lock.
- VRAM callback teardown uses asynchronous removal and checks shutdown/backend
  identity before accessing the renderer-state union. This avoids waiting for
  a CPU callback while holding PGRAPH and prevents stale backend casts.
- Native D3D12 is selectable and persisted in UWP-Port Settings, without
  changing the default. A suspended native frame cannot fall into OpenGL.
  Suspension and memory-pressure callbacks preserve GPU-authoritative state
  before trimming resources; critical pressure gates rendering until normal.
  Desktop debug layers are opt-in; UWP never enables the SDK layer.
- Completed draw uploads are recycled through a best-fit cache bounded to
  32 MiB and 64 resources. Pending GPU buffers never enter the cache; device
  teardown and pressure trimming release it. Failed queue signals quarantine
  submitted resources until teardown instead of freeing GPU-owned buffers.
- Compiler loading requires only Mesa's compile/free exports. The standalone
  `spirv_to_dxil_validate` extension is optional; when absent, startup logs that
  shader validation is deferred to D3D12 pipeline creation. When available,
  standalone validation failures still reject the shader.

The plane-copy contract is documented in Microsoft's
[planar depth/stencil specification](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/PlanarDepthStencilDDISpec.md).
Depth/stencil texture copies use entire subresource rectangles, as required by
[`CopyTextureRegion`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion).

### Verification

From the configured project root, build the embedded DLL:

```sh
ninja -C build-uwp-embed -j4 qemu-system-i386.dll
```

Run these scripts with MSYS2 UCRT64 Python/GCC on Windows:

```sh
python tests/unit/test-d3d12-vertices.py
python tests/unit/test-d3d12-state.py
python tests/unit/test-d3d12-surface-planes.py
python tests/unit/test-d3d12-lifecycle.py
python tests/unit/test-d3d12-upload-cache.py
python tests/unit/test-d3d12-compiler-loader.py
```

The WARP test uploads BGRA8, D16, D32, D24S8 and D32S8 and invokes the production
readback helper. Every pixel and stencil byte is checked, including high/odd
24-bit depths and endpoints. The other tests compile production CPU helpers
with deterministic fixtures for vertex packing, report DMA, range validation
and lifecycle failures. Static guards check UI persistence and backend routing.
The upload-cache WARP fixture checks reuse identity, data replacement, best-fit
selection and byte/entry limits; fence ownership also has static guards.
The compiler loader fixture invokes production initialization against the built
Mesa DLL on WARP and checks missing mandatory exports. This checks loading, not
complete shader translation or gameplay.
The existing D3D11 regression suite is retained to detect shared-path regressions.

### Validation boundaries and remaining differences

- Native WARP copies and CPU fixtures do not establish gameplay, framebuffer
  orientation, full shader/PSO parity, Xbox timing or a measured FPS gain.
- The reference's persistent upload rings, vertex pulling, deferred descriptor
  allocator, custom-texture subsystem and fastmem frontend were not transplanted.
  Current scanout still resolves VRAM and scales/uploads on the CPU, and draws
  still own individual upload resources (now recycled after fence completion).
  These remain performance differences.
- Packed vertex intervals are bounded by the current frontend's batch limit;
  widely separated indices can still require a future sparse/pulling path.
- Lifecycle tests do not exercise an installed AppContainer, real memory
  notifications, TDR or suspend deadlines. Synchronous Map/fence waits remain;
  successful preservation cannot be guaranteed after device removal. Critical
  trimming retains the device/swapchain, not a complete device teardown.
- A cross-hardware test XBE should cover high-index vertex streams, stride-zero
  attributes, inline data after format changes, surface layout transitions,
  Z16 quantization, nonzero stencil round trips and distinct report DMA objects.
  Compare bytes/images on original Xbox, the reference and this backend.

No package installation or gameplay run is implied by a successful DLL/host
build. Packaging/strip and device testing are separate steps.
