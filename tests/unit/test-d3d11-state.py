#!/usr/bin/env python3
"""Compile and exercise pure D3D11 output-merger state helpers with GCC."""

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
    signatures = (
        "static bool d3d11_blend_factors_use_constant_alpha(",
        "static bool d3d11_blend_factors_use_constant_color(",
        "static bool d3d11_blend_factors_conflict(",
        "static void d3d11_adjust_blend_factor_constant_alpha(",
        "static D3D11_BLEND_OP d3d11_blend_op(",
    )
    functions = "\n\n".join(extract_function(source, signature)
                               for signature in signatures)
    update_start = source.index("bool pgraph_d3d11_update_output_merger(")
    update_end = source.index("void pgraph_d3d11_bind_render_targets(", update_start)
    update = source[update_start:update_end]
    assert "rt->BlendEnable = !rt->LogicOpEnable" in update
    assert "bool dual_source = pgraph_d3d11_dual_source_blend(blend);" in update
    assert "d3d11_dual_source_factor(source_factor)" in update
    assert "d3d11_dual_source_factor(destination_factor)" in update
    assert "D3D11_BLEND_OP blend_op =" in update
    assert update.index("rt->DestBlend = dual_source") < \
        update.index("rt->BlendOp = blend_op;")
    assert update.index("pgraph_d3d11_dual_source_blend(") < \
        update.index("d3d11_update_blend_state(")
    assert "d3d11_adjust_blend_factor_constant_alpha(" in update

    preamble = r"""
#include <assert.h>
#include <stdbool.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
typedef unsigned int D3D11_BLEND_OP;
#define D3D11_BLEND_OP_ADD 1
#define D3D11_BLEND_OP_SUBTRACT 2
#define D3D11_BLEND_OP_REV_SUBTRACT 3
#define D3D11_BLEND_OP_MIN 4
#define D3D11_BLEND_OP_MAX 5
#define NV_PGRAPH_BLEND_SFACTOR_CONSTANT_COLOR 12
#define NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_CONSTANT_COLOR 13
#define NV_PGRAPH_BLEND_SFACTOR_CONSTANT_ALPHA 14
#define NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_CONSTANT_ALPHA 15
#define NV_PGRAPH_BLEND_DFACTOR_CONSTANT_COLOR 12
#define NV_PGRAPH_BLEND_DFACTOR_ONE_MINUS_CONSTANT_COLOR 13
#define NV_PGRAPH_BLEND_DFACTOR_CONSTANT_ALPHA 14
#define NV_PGRAPH_BLEND_DFACTOR_ONE_MINUS_CONSTANT_ALPHA 15
"""
    tests = r"""
int main(void)
{
    assert(d3d11_blend_factors_use_constant_alpha(14, 1));
    assert(d3d11_blend_factors_use_constant_alpha(1, 15));
    assert(d3d11_blend_factors_use_constant_color(12, 1));
    assert(d3d11_blend_factors_use_constant_color(1, 13));
    assert(!d3d11_blend_factors_use_constant_alpha(12, 13));
    assert(!d3d11_blend_factors_use_constant_color(14, 15));

    assert(d3d11_blend_op(3) == D3D11_BLEND_OP_MIN);
    assert(d3d11_blend_op(4) == D3D11_BLEND_OP_MAX);
    assert(d3d11_blend_factors_conflict(14, 12,
                                        d3d11_blend_op(2)));
    assert(d3d11_blend_factors_conflict(14, 13,
                                        d3d11_blend_op(2)));
    assert(d3d11_blend_factors_conflict(15, 12,
                                        d3d11_blend_op(2)));
    assert(d3d11_blend_factors_conflict(15, 13,
                                        d3d11_blend_op(2)));
    assert(d3d11_blend_factors_conflict(12, 14,
                                        d3d11_blend_op(2)));
    assert(d3d11_blend_factors_conflict(12, 15,
                                        d3d11_blend_op(2)));
    assert(d3d11_blend_factors_conflict(13, 14,
                                        d3d11_blend_op(2)));
    assert(d3d11_blend_factors_conflict(13, 15,
                                        d3d11_blend_op(2)));
    assert(!d3d11_blend_factors_conflict(14, 12,
                                         d3d11_blend_op(3)));
    assert(!d3d11_blend_factors_conflict(14, 12,
                                         d3d11_blend_op(4)));
    assert(!d3d11_blend_factors_conflict(14, 15,
                                         d3d11_blend_op(2)));
    assert(!d3d11_blend_factors_conflict(12, 13,
                                         d3d11_blend_op(2)));
    assert(!d3d11_blend_factors_conflict(1, 2,
                                         d3d11_blend_op(2)));

    float factor[4] = {0.1f, 0.2f, 0.3f, 0.75f};
    d3d11_adjust_blend_factor_constant_alpha(factor, 14, 1);
    assert(factor[0] == 0.75f && factor[1] == 0.75f &&
           factor[2] == 0.75f && factor[3] == 0.75f);

    factor[0] = 0.1f; factor[1] = 0.2f; factor[2] = 0.3f; factor[3] = 0.75f;
    d3d11_adjust_blend_factor_constant_alpha(factor, 15, 0);
    assert(factor[0] == 0.75f && factor[1] == 0.75f &&
           factor[2] == 0.75f && factor[3] == 0.75f);

    factor[0] = 0.1f; factor[1] = 0.2f; factor[2] = 0.3f; factor[3] = 0.75f;
    d3d11_adjust_blend_factor_constant_alpha(factor, 12, 1);
    assert(factor[0] == 0.1f && factor[1] == 0.2f &&
           factor[2] == 0.3f && factor[3] == 0.75f);

    factor[0] = 0.1f; factor[1] = 0.2f; factor[2] = 0.3f; factor[3] = 0.75f;
    d3d11_adjust_blend_factor_constant_alpha(factor, 14, 12);
    assert(factor[0] == 0.1f && factor[1] == 0.2f &&
           factor[2] == 0.3f && factor[3] == 0.75f);

    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-state-") as tmp:
        test = pathlib.Path(tmp) / "test.c"
        exe = pathlib.Path(tmp) / "test"
        test.write_text(preamble + functions + tests, encoding="utf-8")
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: mixed-constant dual-source selection; MIN/MAX ignore factors")


if __name__ == "__main__":
    main()
