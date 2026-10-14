#!/usr/bin/env python3
"""Rasterize both actual D3D11 vertex epilogues using offscreen WARP."""
import json
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / "hw/xbox/nv2a/pgraph/d3d11/shaders.c").read_text()
draw = (root / "hw/xbox/nv2a/pgraph/d3d11/draw.c").read_text()
front_face = re.search(r'\.FrontCounterClockwise\s*=\s*([^,]+),', draw).group(1)
front_face = front_face.replace('NV_PGRAPH_SETUPRASTER_FRONTFACE', '1')
epilogues = []
for start in ('"  oPos.xy = round(', '"  oPos.w=nv_clamp_away('):
    # The latter identifies the fixed-function output block.
    section = source[source.index(start):]
    section = section[:section.index('"  VSOut output;')]
    lines = [json.loads(match) for match in re.findall(r'"(?:[^"\\]|\\.)*"', section)]
    body = ''.join(lines)
    body = re.sub(r'C\[%u\]', 'float4(0,0,0,0)', body)
    epilogues.append(body)
# Negative control: reproduce the old fixed-function projection.
epilogues.append(epilogues[1].replace('oPos.xy/=oPos.w;', '')
                 .replace('oPos.xy*=oPos.w;', 'oPos.xyz*=oPos.w;'))
with tempfile.TemporaryDirectory(prefix="xemu-vertex-") as directory:
    tmp = pathlib.Path(directory)
    (tmp / "vertex-output-generated.h").write_text(
        'static const char *epilogues[] = {' +
        ','.join(json.dumps(x) for x in epilogues) + '};\n' +
        '#define XEMU_FRONT_COUNTER_CLOCKWISE(raster) (' + front_face + ')\n')
    exe = tmp / "test.exe"
    subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', '-I' + str(tmp),
                    str(root / 'tests/unit/test-d3d11-vertex-output.c'),
                    '-o', str(exe), '-ld3d11', '-ld3dcompiler'], check=True)
    subprocess.run([str(exe)], check=True)
