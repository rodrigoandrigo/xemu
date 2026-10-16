#!/usr/bin/env python3
"""Check the real HUD atlas rebuild helper with ImGui and offscreen WARP."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
imgui = root / "subprojects/imgui"
source = (root / "ui/xui/main.cc").read_text()
helper = source[source.index("bool xemu_hud_d3d11_rebuild_fonts("):
                source.index("void xemu_hud_d3d11_shutdown(")]
font_manager = (root / "ui/xui/font-manager.cc").read_text()
assert "xemu_hud_d3d11_rebuild_fonts();" in font_manager
if sys.platform != "win32" or not imgui.is_dir():
    print("SKIP: HUD D3D11/WARP fixture requires Windows/MSYS2 and the ImGui submodule")
    raise SystemExit(0)

fixture = r'''
#include <cassert>
#include <cstdio>
#include <d3d11.h>
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
static bool g_hud_d3d11_initialized;
'''
main = r'''
int main(int argc, char **) {
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,
        0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640,480);
    assert(xemu_hud_d3d11_rebuild_fonts()); // Deferred until backend init.
    g_hud_d3d11_initialized = ImGui_ImplDX11_Init(device,context);
    assert(g_hud_d3d11_initialized);
    ImGui_ImplDX11_NewFrame();
    assert(io.Fonts->IsBuilt() && io.Fonts->TexID != 0);
    for (int i=0; i<4; i++) {
        io.Fonts->Clear();
        ImFontConfig config;
        config.SizePixels = 16.0f + i * 4.0f;
        io.Fonts->AddFontDefault(&config);
        if (argc == 1) assert(xemu_hud_d3d11_rebuild_fonts());
        ImGui_ImplDX11_NewFrame();
        // Old path leaves the sampler alive but the rebuilt atlas unbuilt.
        assert(io.Fonts->IsBuilt() && io.Fonts->TexID != 0);
        ImGui::NewFrame();
        ImGui::Begin("HUD"); ImGui::TextUnformatted("font rebuild"); ImGui::End();
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    context->Release(); device->Release();
    puts("PASS: native HUD atlas rebuild, four font sizes, ImGui frames and WARP draws");
}
'''
with tempfile.TemporaryDirectory(prefix="xemu-hud-fonts-") as directory:
    temporary = pathlib.Path(directory)
    test = temporary / "test.cpp"
    test.write_text(fixture + helper + main)
    exe = temporary / "test.exe"
    subprocess.run([
        "g++", "-std=c++17", "-DCONFIG_UWP", "-I" + str(imgui),
        str(test), *[str(imgui / name) for name in
                     ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp",
                      "imgui_widgets.cpp", "backends/imgui_impl_dx11.cpp")],
        "-o", str(exe), "-ld3d11", "-ld3dcompiler", "-ldxgi"], check=True)
    negative = subprocess.run([str(exe), "old-path"], capture_output=True)
    assert negative.returncode != 0
    assert b"io.Fonts->IsBuilt()" in negative.stderr
    print("PASS: old missing-upload path reproduced the font assertion")
    subprocess.run([str(exe)], check=True)
