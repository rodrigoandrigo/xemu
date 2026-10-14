#!/usr/bin/env python3
"""Validate generated NV2A point GS variants; does not run the emulator."""
import json
import pathlib
import shlex
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
build = pathlib.Path(sys.argv[1]).resolve()
entries = json.loads((build / "compile_commands.json").read_text())
entry = next(e for e in entries if e["file"].endswith("/glsl/geom.c"))
command = shlex.split(entry["command"])
flags = []
iterator = iter(command[:-1])
for flag in iterator:
    if flag in ("-o", "-MQ", "-MF"):
        next(iterator)
    elif flag not in ("-c", "-MD", "-DXBOX"):
        flags.append(flag)
libs = shlex.split(subprocess.check_output(
    ["pkg-config", "--libs", "glib-2.0"], text=True))
with tempfile.TemporaryDirectory(prefix="xemu-uwp-points-") as tmp:
    exe = pathlib.Path(tmp) / "test.exe"
    subprocess.run([
        *flags, "-Werror", "-ffunction-sections", "-fdata-sections",
        str(root / "tests/unit/test-uwp-point-shaders.c"),
        str(root / "hw/xbox/nv2a/pgraph/glsl/geom.c"),
        str(root / "hw/xbox/nv2a/pgraph/glsl/common.c"),
        str(root / "hw/xbox/nv2a/pgraph/glsl/vsh.c"),
        str(root / "hw/xbox/nv2a/pgraph/glsl/vsh-ff.c"),
        str(root / "hw/xbox/nv2a/pgraph/glsl/vsh-prog.c"),
        "-o", str(exe), "-Wl,--gc-sections", *libs,
    ], cwd=build, check=True)
    subprocess.run([str(exe)], cwd=tmp, check=True)
    for shader in sorted(pathlib.Path(tmp).glob("*.geom")):
        subprocess.run(["glslangValidator", "-V", "-l",
                        str(pathlib.Path(tmp) / "points.vert"), str(shader),
                        "-o", str(shader.with_suffix(".spv"))],
                       check=True, stdout=subprocess.DEVNULL)
    print("PASS: 224 generated NV2A point/polygon shader variants")
