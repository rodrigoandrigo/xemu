#!/usr/bin/env python3
"""Test the existing D3D11 NV2A logic-op mapping without graphics runtime."""
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SURFACES = ROOT / "hw/xbox/nv2a/pgraph/d3d11/surfaces.c"
REGS = ROOT / "hw/xbox/nv2a/nv2a_regs.h"
PGRAPH = ROOT / "hw/xbox/nv2a/pgraph/pgraph.c"


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
    raise AssertionError(f"unterminated function: {signature}")


def main():
    source = SURFACES.read_text(encoding="utf-8")
    registers = REGS.read_text(encoding="utf-8")
    pgraph = PGRAPH.read_text(encoding="utf-8")
    logic_op = extract_function(source, "static D3D11_LOGIC_OP d3d11_logic_op(")
    update = extract_function(source, "bool pgraph_d3d11_update_output_merger(")
    create_blend_state = extract_function(
        source, "static bool d3d11_update_blend_state("
    )
    set_logic_enable = extract_function(pgraph, "DEF_METHOD(NV097, SET_LOGIC_OP_ENABLE)")
    set_logic_op = extract_function(pgraph, "DEF_METHOD(NV097, SET_LOGIC_OP)")

    assert re.search(r"#\s*define NV_PGRAPH_BLEND_LOGICOP\s+0x0000F000", registers)
    assert "NV_PGRAPH_BLEND_LOGICOP_ENABLE" in registers
    assert "parameter & 0xF" in set_logic_op
    assert "NV_PGRAPH_BLEND_LOGICOP_ENABLE" in set_logic_enable
    assert "rt->LogicOpEnable = !!(blend & NV_PGRAPH_BLEND_LOGICOP_ENABLE)" in update
    assert "rt->LogicOp = d3d11_logic_op(GET_MASK(blend, NV_PGRAPH_BLEND_LOGICOP))" \
        in update
    assert "rt->BlendEnable = !rt->LogicOpEnable && !!(blend & NV_PGRAPH_BLEND_EN)" \
        in update
    assert "if (desc->RenderTarget[0].LogicOpEnable)" in create_blend_state
    assert "if (!r->device1 || !r->output_merger_logic_op)" in create_blend_state
    assert "ID3D11Device1_CreateBlendState1" in create_blend_state

    # Numeric values follow Microsoft's D3D11_LOGIC_OP enum; the expected sequence
    # is the NV2A 4-bit operation order, not a direct enum cast.
    preamble = r"""
#include <assert.h>
#include <stddef.h>
typedef unsigned int D3D11_LOGIC_OP;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define D3D11_LOGIC_OP_CLEAR 0
#define D3D11_LOGIC_OP_SET 1
#define D3D11_LOGIC_OP_COPY 2
#define D3D11_LOGIC_OP_COPY_INVERTED 3
#define D3D11_LOGIC_OP_NOOP 4
#define D3D11_LOGIC_OP_INVERT 5
#define D3D11_LOGIC_OP_AND 6
#define D3D11_LOGIC_OP_NAND 7
#define D3D11_LOGIC_OP_OR 8
#define D3D11_LOGIC_OP_NOR 9
#define D3D11_LOGIC_OP_XOR 10
#define D3D11_LOGIC_OP_EQUIV 11
#define D3D11_LOGIC_OP_AND_REVERSE 12
#define D3D11_LOGIC_OP_AND_INVERTED 13
#define D3D11_LOGIC_OP_OR_REVERSE 14
#define D3D11_LOGIC_OP_OR_INVERTED 15
"""
    tests = r"""
int main(void)
{
    static const D3D11_LOGIC_OP expected[16] = {
        D3D11_LOGIC_OP_CLEAR,
        D3D11_LOGIC_OP_AND,
        D3D11_LOGIC_OP_AND_REVERSE,
        D3D11_LOGIC_OP_COPY,
        D3D11_LOGIC_OP_AND_INVERTED,
        D3D11_LOGIC_OP_NOOP,
        D3D11_LOGIC_OP_XOR,
        D3D11_LOGIC_OP_OR,
        D3D11_LOGIC_OP_NOR,
        D3D11_LOGIC_OP_EQUIV,
        D3D11_LOGIC_OP_INVERT,
        D3D11_LOGIC_OP_OR_REVERSE,
        D3D11_LOGIC_OP_COPY_INVERTED,
        D3D11_LOGIC_OP_OR_INVERTED,
        D3D11_LOGIC_OP_NAND,
        D3D11_LOGIC_OP_SET,
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(expected); i++) {
        assert(d3d11_logic_op(i) == expected[i]);
    }
    assert(d3d11_logic_op(16) == D3D11_LOGIC_OP_COPY);
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-logicop-") as tmp:
        test_c = pathlib.Path(tmp) / "logicop.c"
        executable = pathlib.Path(tmp) / "logicop"
        test_c.write_text(
            preamble + "\n" + logic_op + "\n" + tests,
            encoding="utf-8",
        )
        subprocess.run(
            ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             str(test_c), "-o", str(executable)],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
    print("PASS: all 16 NV2A values map to the official D3D11 logic-op enums")


if __name__ == "__main__":
    main()
