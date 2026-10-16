#!/usr/bin/env python3
"""Run the native shader test using an existing Meson build's include flags.

MSYS2 UCRT64: python tests/unit/test-d3d11-shaders.py build-d3d11-check
"""

import json
import pathlib
import shlex
import subprocess
import sys
import tempfile


root = pathlib.Path(__file__).resolve().parents[2]
build = pathlib.Path(sys.argv[1]).resolve()
entries = json.loads((build / "compile_commands.json").read_text())
entry = next(e for e in entries if e["file"].endswith("/d3d11/shaders.c"))
command = shlex.split(entry["command"])
# Remove compilation output/dependency arguments and original source.
flags = []
iterator = iter(command[:-1])
for flag in iterator:
    if flag in ("-o", "-MQ", "-MF"):
        next(iterator)
    elif flag not in ("-c", "-MD"):
        flags.append(flag)
libs = shlex.split(subprocess.check_output(
    ["pkg-config", "--libs", "glib-2.0"], text=True))
with tempfile.TemporaryDirectory(prefix="xemu-d3d11-shaders-") as tmp:
    tmp = pathlib.Path(tmp)
    source = (root / "hw/xbox/nv2a/pgraph/d3d11/shaders.c").read_text()
    includes = source[source.index('#include "qemu/osdep.h"'):
                      source.index("#define D3D11_SHADER_CACHE_VERSION")]
    constant_buffer_sizes = source[source.index(
        "#define D3D11_PIXEL_CONSTANT_BUFFER_SIZE"):source.index(
        "typedef enum D3D11ShaderStage")]
    translator = source[source.index("static const char d3d11_psh_preamble"):
                        source.index("static bool d3d11_triangle_primitive(")]
    constants = source[source.index("typedef struct PGRAPHD3D11PixelConstants"):
                       source.index("QEMU_BUILD_BUG_ON(sizeof(PGRAPHD3D11PixelConstants)")]
    surfaces = (root / "hw/xbox/nv2a/pgraph/d3d11/surfaces.c").read_text()
    blend_helpers = surfaces[surfaces.index("static D3D11_BLEND d3d11_blend_factor("):
                             surfaces.index("static D3D11_LOGIC_OP d3d11_logic_op(")]
    presentation_source = (root / "hw/xbox/nv2a/pgraph/d3d11/presentation.c").read_text()
    presentation_shader = presentation_source[
        presentation_source.index("static const char d3d11_presentation_shader[]"):
        presentation_source.index("static bool d3d11_compile_presentation_shader")]
    (tmp / "test-d3d11-pixel-generated.h").write_text(
        includes + constant_buffer_sizes + translator + constants + blend_helpers +
        presentation_shader)
    exe = pathlib.Path(tmp) / "test.exe"
    subprocess.run([*flags, "-Werror", "-ffunction-sections", "-fdata-sections",
                    f"-I{tmp}",
                    f"-I{root / 'hw/xbox/nv2a/pgraph/d3d11'}",
                    str(root / "tests/unit/test-d3d11-shaders.c"), "-o", str(exe),
                    "-Wl,--gc-sections", *libs, "-ld3dcompiler", "-ld3d11",
                    "-ldxguid"], cwd=build, check=True)
    subprocess.run([str(exe)], check=True)
