# NV2A UWP point expansion

This path handles `PRIM_TYPE_POINTS` and polygon-mode points (triangles, strips,
fans, quads, quad strips and polygons) in the xemu Vulkan renderer when UWP is
built and the enabled Vulkan device does not support `largePoints`. It is not
general Vulkan large-point support, and does not change Dozen feature flags.

The vertex shader transports a clip-space half extent at location 13. It uses
the physical viewport dimensions (including AA/render scaling), not the
logical surface dimensions. The geometry shader emits four vertices per point
and reassigns every output for each corner. Point sprites use interpolated
texture coordinate 3 instead of the native fragment `PointCoord` builtin.
Original texture coordinate 3 is preserved when point sprites are disabled.
Polygon culling runs on the original homogeneous positions before expansion.
The generated billboards always rasterize filled, without hardware face culling.
`POLYGON` fans emit shared vertices only in the first input triangle; quads
produce at most 16 output vertices (800 32-bit output components), below D3D12's
1024-component limit for these generated NV2A shaders.
The point center is rejected outside the X/Y clip volume before expansion;
the remaining square is clipped by normal rasterization and scissoring.
Billboards do not inherit guest polygon culling or polygon raster mode.

Run from an MSYS2 UCRT64 shell with an existing configured xemu build:

```sh
python tests/unit/test-uwp-point-shaders.py build-uwp-embed
```

The diagnostic compiles the actual generators with `-Werror`, generates four
224 geometry variants covering seven primitive modes, four cull modes, both
front-face settings, flat/smooth shading and sprites on/off. It uses glslang
to compile/link vertex and geometry stages.
It does not run a UWP application or validate GPU pixels.

Remaining validation: use a test XBE with sizes 1, 2, 8, 32 and 64, textured and
untextured points, attenuation, programmable vertex output, clip-edge centers,
depth/scissor tests, AA and render scales. Compare original Xbox, desktop xemu
and UWP/Xbox output. Also test winding, flat colors, clipped polygons and fan
decomposition with blending to catch duplicated points. Existing fan-based
flat shading limitations are not resolved by point expansion itself.
