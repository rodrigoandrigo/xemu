# XeSS-SR runtime preparation

The unmodified x64 `libxess.dll` is from XeSS SDK 3.0.2 and reports file
version 2.0.2.68. Its Intel license and third-party notices are in `Licenses`.
The Microsoft runtime license is archived from
https://visualstudio.microsoft.com/license-terms/vs2022-cruntime/.

The three CRT DLLs in `x64` were copied from Teste-HW's VC143 OneCore runtime
directory (14.44.35211.0). UWP-Port continues packaging its existing matching
OneCore CRT for Mesa; do not overwrite a newer shared CRT with these older
copies. There must be only one runtime of each name at the package root.

## Integration status

The runtime and license notices are included by the host project. XeSS frame
execution and the Settings toggle are not implemented yet. Copying the runtime
does not enable XeSS.

The intended quality preset is `XESS_QUALITY_SETTING_AA`: input and output
have the same resolution. The Teste-HW SR sample supplies jittered color and
high-resolution motion vectors, then runs `xessD3D12Execute` before presenting
the output. In high-resolution motion-vector mode it omits the depth input.

The xemu OpenGL framebuffer currently does not provide those temporal inputs.
Its Mesa presenter owns the D3D12 device, queue, resources and resource states;
the host receives a swapchain attachment, not a complete XeSS input contract.
Integration needs a renderer-to-presenter contract with actual frame identity,
color, motion vectors, jitter, resource states and reset-history events.
Initialization must use the device which owns those resources. Execution must
precede presentation and preserve command-queue ordering and GPU lifetimes.

Generating temporal inputs in NV2A versus estimating motion between output
frames is an outstanding implementation choice. Zero motion for moving scenes
or resetting history on every frame is not equivalent to temporal XeSS AA.
