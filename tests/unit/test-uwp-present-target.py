#!/usr/bin/env python3
"""Test host registration with a fake embedding API, without launching UWP.

MSYS2 UCRT64: python tests/unit/test-uwp-present-target.py
"""

import pathlib
import subprocess
import tempfile


root = pathlib.Path(__file__).resolve().parents[2]
source = (root / "UWP-Port/XemuHost.cpp").read_text(encoding="utf-8")
registration = source[source.index("bool XemuHost::RegisterNativePresentTarget()"):
                      source.index("void __cdecl XemuHost::MesaLog")]
start = source[source.index("bool XemuHost::Start("):
               source.index("void XemuHost::Run(")]
assert start.index("RegisterNativePresentTarget()") < start.index("std::thread(")
attach_start = source.index("long __cdecl XemuHost::AttachMesaSwapChain(")
attach_end = source.index("bool XemuHost::UpdateRenderPanelSize(", attach_start)
attach = source[attach_start:attach_end]
assert "const bool detach = nativeSwapChain == nullptr;" in attach
assert "panelNative->SetSwapChain(nullptr)" in attach
assert "state->swapChain.Reset()" in attach
assert "if (!detach && !state->active.load())" in attach
assert "state->requestedSwapChain = retainedSwapChain" in attach
assert "state->requestGeneration" in attach
assert "state->pending.store(true)" in attach
assert "recordResult(result)" in attach
assert "state->pending.store(false)" in attach
assert "state->requestGeneration != generation" in attach

host_error = source[source.index("std::string XemuHost::LastError() const"):
                    source.index("void XemuHost::WriteDiagnostic(")]
assert "bool XemuHost::RetrySwapChainAttach()" in host_error
assert "bool XemuHost::IsSwapChainAttachPending() const" in host_error
page = (root / "UWP-Port/DirectXPage.xaml.cpp").read_text(encoding="utf-8")
rendering_start = page.index("void DirectXPage::OnRendering(")
rendering_end = page.index("void DirectXPage::UpdateFpsOverlay(", rendering_start)
rendering = page[rendering_start:rendering_end]
assert "m_xemu->LastError()" in rendering
assert "m_xemu->RetrySwapChainAttach()" in rendering
assert "m_xemu->IsSwapChainAttachPending()" in rendering

renderer = (root / "hw/xbox/nv2a/pgraph/d3d11/renderer.c").read_text(
    encoding="utf-8"
)
release_start = renderer.index("static void d3d11_release_device_resources(")
release_end = renderer.index("static void d3d11_release_state(", release_start)
release = renderer[release_start:release_end]
assert "target.attach(target.opaque, NULL)" in release
assert release.index("d3d11_release_backbuffer(r);") < release.index("target.attach")
assert release.index("target.attach") < release.index("IDXGISwapChain3_Release")

for begin, end in [("bool XemuHost::AttachRenderPanel(",
                    "bool XemuHost::ResolveSDLInput()"),
                   ("bool XemuHost::UpdateRenderPanelSize(",
                    "bool XemuHost::RegisterNativePresentTarget()")]:
    assert "RegisterNativePresentTarget()" in source[source.index(begin):
                                                    source.index(end)]

fixture = r"""
#include <cassert>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
using Attach = long (*)(void *, void *);
struct AttachState { std::atomic<long> lastResult{0}; };
struct XemuHost {
    int (*m_setD3D12PresentTarget)(uint32_t, uint32_t, Attach, void *) = nullptr;
    void *m_renderPanel = nullptr;
    uint32_t m_renderWidth = 0, m_renderHeight = 0;
    std::shared_ptr<AttachState> m_swapChainAttachState =
        std::make_shared<AttachState>();
    std::string error, diagnostic;
    bool RegisterNativePresentTarget();
    static long AttachMesaSwapChain(void *, void *) { return 0; }
    void SetError(const std::string &text) { error = text; }
    void WriteDiagnostic(const std::string &text) { diagnostic = text; }
};
static unsigned calls;
static uint32_t registeredWidth, registeredHeight;
static int apiResult;
static void *expectedHost;
static int registerTarget(uint32_t w, uint32_t h, Attach callback, void *opaque)
{
    assert(callback == &XemuHost::AttachMesaSwapChain);
    assert(opaque == expectedHost);
    calls++;
    registeredWidth = w;
    registeredHeight = h;
    return apiResult;
}
"""
tests = r"""
int main()
{
    XemuHost host;
    expectedHost = &host;
    assert(!host.RegisterNativePresentTarget());
    host.m_setD3D12PresentTarget = &registerTarget;
    assert(!host.RegisterNativePresentTarget());
    host.m_renderPanel = &host;
    assert(!host.RegisterNativePresentTarget());
    assert(calls == 0);
    host.m_renderWidth = 1200;
    host.m_renderHeight = 900;
    assert(host.RegisterNativePresentTarget());
    assert(registeredWidth == 1200 && registeredHeight == 900);
    assert(host.diagnostic.find("1200x900") != std::string::npos);
    host.m_renderWidth = 1920;
    host.m_renderHeight = 1080;
    assert(host.RegisterNativePresentTarget());
    assert(registeredWidth == 1920 && registeredHeight == 1080);
    apiResult = -22;
    assert(!host.RegisterNativePresentTarget());
    assert(host.error.find("-22") != std::string::npos);
    std::cout << "PASS: readiness, registration, resize, callback, API failure\n";
}
"""
with tempfile.TemporaryDirectory(prefix="xemu-present-target-") as tmp:
    tmp = pathlib.Path(tmp)
    test = tmp / "test.cpp"
    exe = tmp / "test.exe"
    test.write_text(fixture + registration + tests, encoding="utf-8")
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    str(test), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: registration precedes startup; attach and resize refresh target")
