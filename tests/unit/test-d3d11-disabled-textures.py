#!/usr/bin/env python3
"""Disabled texture registers must never be decoded during uniform updates."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / "hw/xbox/nv2a/pgraph/d3d11/shaders.c").read_text()


def extract_function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0
    for end in range(opening, len(text)):
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
            if depth == 0:
                return text[start:end + 1]
    raise AssertionError(f"unterminated function: {signature}")


helper = extract_function(
    source, "static void d3d11_texture_coordinate_scale(")
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define MAX(a,b) ((a)>(b)?(a):(b))
typedef struct { bool enabled[4]; } PGRAPHState;
typedef struct { unsigned int width,height,color_format; } TextureShape;
static struct { bool linear; } kelvin_color_format_info_map[2] = {{false},{true}};
static unsigned int reads;
static bool pgraph_is_texture_enabled(PGRAPHState *pg,unsigned int stage)
{ return pg->enabled[stage]; }
static TextureShape pgraph_get_texture_shape(PGRAPHState *pg,unsigned int stage)
{
    assert(pg->enabled[stage]);
    reads++;
    return (TextureShape){640,480,stage & 1};
}
'''
main = r'''
int main(void)
{
    PGRAPHState pg = {0};
    float scale[4];
    for (unsigned int stage=0;stage<4;stage++) {
        d3d11_texture_coordinate_scale(&pg,stage,scale);
        assert(reads==0);
        for(unsigned int c=0;c<4;c++) assert(scale[c]==1);
    }
    pg.enabled[1]=true;
    d3d11_texture_coordinate_scale(&pg,1,scale);
    assert(reads==1 && scale[0]==1.0f/640 && scale[1]==1.0f/480);
    pg.enabled[2]=true;
    d3d11_texture_coordinate_scale(&pg,2,scale);
    assert(reads==2 && scale[0]==1 && scale[1]==1);
    puts("PASS: disabled stages never decode shape; enabled linear/swizzled coordinates");
}
'''
with tempfile.TemporaryDirectory(prefix="xemu-disabled-textures-") as directory:
    temporary = pathlib.Path(directory)
    test = temporary / "test.c"
    test.write_text(fixture + helper + main)
    exe = temporary / "test.exe"
    subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", str(test),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
