#!/usr/bin/env python3
"""Syntax-check the production D3D11 presentation HLSL with glslang.

This validates HLSL parsing for the actual presentation vertex/pixel entry
points, including PVIDEO composition. It does not replace D3DCompile, WARP,
pixel comparisons, or Xbox/UWP runtime validation.
"""

import ast
import pathlib
import re
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / "hw/xbox/nv2a/pgraph/d3d11/presentation.c").read_text(
    encoding="utf-8"
)
start = source.index("static const char d3d11_presentation_shader[]")
end = source.index("static bool d3d11_compile_presentation_shader", start)
declaration = source[start:end]
chunks = re.findall(r'"(?:\\.|[^"\\])*"', declaration)
assert chunks, "could not extract production presentation HLSL"
hlsl = "".join(ast.literal_eval(chunk) for chunk in chunks)

compiler = shutil.which("glslangValidator")
if not compiler:
    raise SystemExit("glslangValidator is required for this regression")

with tempfile.TemporaryDirectory(prefix="xemu-d3d11-presentation-") as directory:
    directory = pathlib.Path(directory)
    shader = directory / "presentation.hlsl"
    shader.write_text(hlsl, encoding="utf-8")
    for stage, entry in (("vert", "vs_main"), ("frag", "ps_main")):
        output = directory / f"presentation-{stage}.spv"
        subprocess.run(
            [compiler, "-D", "-V", "-S", stage, "-e", entry,
             "--hlsl-iomap", "-o", str(output), str(shader)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        assert output.is_file() and output.stat().st_size > 0
        print(f"PASS: production presentation {stage}/{entry} HLSL parsed")
