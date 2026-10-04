# Dozen host extension smoke test

`host_extensions_smoke.cpp` is a standalone Windows diagnostic, not a Vulkan
conformance test. It loads a specified ICD directly without installing it.

From an x64 MSVC developer shell, at the repository root:

```powershell
cl /EHsc /std:c++17 /Ithirdparty/mesa/include `
  thirdparty/mesa/src/microsoft/vulkan/tests/host_extensions_smoke.cpp `
  /Fe:host_extensions_smoke.exe
./host_extensions_smoke.exe <desktop-vulkan_dzn.dll> <x64-dxil.dll>
```

Use a desktop build for this executable. An AppContainer build may require the
package graph for DXIL loading and fail instance creation outside its package.
This diagnostic does not install or launch UWP-Port.

The test checks advertised extensions, float/integer custom sampler creation,
host-pointer type compatibility, buffer binding, mapping identity, caller-owned
memory lifetime, and rejection of misaligned and unsupported foreign pointers.
Unavailable extensions are reported and skipped, not validated by a skip.

The diagnostic also enables `shaderTessellationAndGeometryPointSize` and
`VK_EXT_memory_budget`, validates the fixed `[1, 1]` point-size range, and runs
1024 budget queries per adapter, checking heap bounds and unused entries.
This does not verify GPU rasterization or Vulkan CTS point-size conformance.

Dozen supports PointSize reads/writes at size 1.0; `largePoints` remains false.
The fixed-size lowering removes PointSize from D3D12 stage signatures, including
input-only geometry shaders, without multiplying geometry output vertices.
Memory budgets prefer checked DXCore results, then DXGI Adapter3 per segment.
If both queries are unavailable, the heap size and live Vulkan allocation
counter supply a best-effort estimate, not the Xbox application's remaining
memory allowance. Heap usage is a process estimate, not total system usage.

## Runtime requirements

- Custom border colors require `ID3D12Device11::CreateSampler2`, Shader Model 6.7,
  and `AdvancedTextureOpsSupported`. Both float and integer colors are supported;
  custom samplers use dynamic descriptors, including in immutable layouts.
- Host allocation import requires `D3D12_FEATURE_EXISTING_HEAPS`. Address and size
  must be 64 KiB aligned. The sized Device13 API is preferred; the Device3 route
  accepts committed, accessible VirtualAlloc allocation bases, not interior
  pointers. Native heap and memory-type restrictions still apply.
- Host pointers are imported, not exported or copied, and remain caller-owned.
  Foreign mapped pointers are explicitly unsupported. Images are restricted to
  dedicated linear transfer resources; optimal image imports are unsupported.

Before claiming conformance, run the Vulkan CTS custom-border-color and external
host-memory suites on each supported runtime. In particular, sampler creation
alone does not validate sampled colors, image swizzles, or GPU/CPU visibility.
Desktop smoke results do not establish UWP/Xbox runtime support.

References:

- https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_custom_border_color.html
- https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_external_memory_host.html
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device3-openexistingheapfromaddress
