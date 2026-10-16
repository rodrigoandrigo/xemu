#!/usr/bin/env python3
"""Validate D3D11 omits an inactive color RTV from output-merger binding."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "hw/xbox/nv2a/pgraph/d3d11/surfaces.c"


def extract_function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for end in range(opening, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start:end + 1]
    raise ValueError(f"unterminated function: {signature}")


def main():
    source = SOURCE.read_text(encoding="utf-8")
    active_helper = extract_function(
        source, "static bool d3d11_color_target_active(")
    output_merger = extract_function(
        source, "bool pgraph_d3d11_update_output_merger(")
    bind_targets = extract_function(
        source, "void pgraph_d3d11_bind_render_targets(")

    assert "d3d11_color_target_active(" in output_merger
    assert "d3d11_color_target_active(" in bind_targets
    assert "color_active ? targets->color.rtv : NULL" in bind_targets
    assert "targets->depth_stencil.dsv" in bind_targets
    assert "rtv ? &targets->color : &targets->depth_stencil" in bind_targets

    fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef enum DXGI_FORMAT {
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_B5G5R5A1_UNORM,
} DXGI_FORMAT;
""" + active_helper + r"""
int main(void)
{
    assert(!d3d11_color_target_active(
        0, false, DXGI_FORMAT_B8G8R8A8_UNORM));
    assert(d3d11_color_target_active(
        1, false, DXGI_FORMAT_B8G8R8A8_UNORM));
    assert(d3d11_color_target_active(
        0xf, false, DXGI_FORMAT_B8G8R8A8_UNORM));
    assert(!d3d11_color_target_active(
        0, true, DXGI_FORMAT_B8G8R8A8_UNORM));
    assert(!d3d11_color_target_active(
        0, false, DXGI_FORMAT_B5G5R5A1_UNORM));
    assert(d3d11_color_target_active(
        0, true, DXGI_FORMAT_B5G5R5A1_UNORM));
    return 0;
}
"""

    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-color-rtv-") as tmp:
        c_file = pathlib.Path(tmp) / "color-rtv-binding.c"
        c_file.write_text(fixture, encoding="utf-8")
        executable = pathlib.Path(tmp) / "color-rtv-binding"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(c_file), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

    print("D3D11 color RTV binding: PASS (GCC -Werror)")


if __name__ == "__main__":
    main()
