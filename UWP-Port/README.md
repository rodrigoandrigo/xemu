# xemu UWP Port

This directory contains the Universal Windows Platform host used to run xemu as
an embedded DLL on Windows and Xbox in Developer Mode. It owns the application
lifecycle, brokered file access, controller input, settings, logs, and the XAML
render surface.

The two graphics paths are:

```text
xemu NV2A -> OpenGL -> Mesa Gallium D3D12 -> D3D12/DXGI -> SwapChainPanel
xemu NV2A -> Vulkan -> Mesa DZN -> D3D12/DXGI -> SwapChainPanel
```

SDL3 supplies the UWP platform and input integration. Mesa supplies OpenGL
through Gallium D3D12 and a native Vulkan ICD through DZN. The Vulkan path
creates a composition swapchain and attaches it directly to the XAML
`SwapChainPanel`; it does not pass Vulkan frames through OpenGL.

## Requirements

- Windows 10 or Windows 11
- Visual Studio with the Desktop C++ and Universal Windows Platform workloads
- Windows SDK 10.0.26100.0 or a compatible installed SDK
- MSYS2 UCRT64 with the runtime dependencies required by xemu
- A UWP-compatible SDL3 build
- The xemu UWP DLL and Mesa UWP OpenGL binaries
- Developer Mode enabled on the target PC or Xbox

Only the x64 package is currently configured and tested.
The GitHub Actions build uses
[`rodrigoandrigo/SDL3_UWP`](https://github.com/rodrigoandrigo/SDL3_UWP).

## Expected build outputs

Before building the host, prepare these artifacts:

```text
build-uwp-embed/qemu-system-i386.dll
build-uwp/mesa/src/gallium/targets/libgl-gdi/opengl32.dll
build-uwp/mesa/src/gallium/targets/wgl/gallium_wgl.dll
build-uwp/mesa/src/microsoft/vulkan/vulkan_dzn.dll
```

The SDL3 include/runtime directory and MSYS2 UCRT64 runtime directory are
configured through the `SDL3UwpRoot` and `Msys2Ucrt64Root` MSBuild properties.
The project provides sibling-directory defaults, and CI supplies both
properties explicitly.

The packaged application must contain at least:

```text
qemu-system-i386.dll
opengl32.dll
gallium_wgl.dll
vulkan_dzn.dll
glslang.dll
libSPIRV-Tools.dll
libSPIRV-Tools-opt.dll
SDL3.dll
dxil.dll
libslirp-0.dll
```

It must also contain the transitive runtime DLLs referenced by these binaries.

## Build xemu as an embeddable DLL

Run the following commands from the repository root in an MSYS2 UCRT64 shell:

```sh
mkdir -p build-uwp-embed
cd build-uwp-embed
../configure \
  --target-list=i386-softmmu \
  --enable-uwp \
  --enable-slirp \
  --enable-sdl \
  --enable-opengl \
  --disable-werror \
  --disable-docs \
  --audio-drv-list=sdl
ninja qemu-system-i386.dll
strip --strip-unneeded qemu-system-i386.dll
```

The result required by the host is `build-uwp-embed/qemu-system-i386.dll`.
Configuration may require additional dependency switches according to the
installed MSYS2 packages.

## Build SDL3 and Mesa

Build SDL3 for UWP x64 in Release mode. Configure the host project to use the
resulting SDL3 include directory and `SDL3.dll`.

Build Mesa for Windows/UWP x64 with:

- OpenGL enabled
- Gallium D3D12 graphics enabled
- Gallium D3D12 video enabled
- Vulkan DZN (`microsoft-experimental`) enabled
- LLVM disabled when the internal DXIL compiler is available

Place or configure the Mesa outputs so the host project can package
`opengl32.dll`, `gallium_wgl.dll`, and `vulkan_dzn.dll` from the expected build
tree. DZN is loaded as the packaged Vulkan ICD when Vulkan is selected in the
Settings page; it does not depend on the desktop Vulkan loader or registry.

## Build and package UWP-Port

Open `UWP-Port.vcxproj` in Visual Studio, select `Release` and `x64`, and build
the project. To create a sideload package from a Developer PowerShell for Visual
Studio, run:

```powershell
msbuild .\UWP-Port\UWP-Port.vcxproj `
  /t:Rebuild `
  /p:Configuration=Release `
  /p:Platform=x64 `
  /p:AppxBundle=Never `
  /p:UapAppxPackageBuildMode=SideloadOnly `
  /m
```

When the dependencies are not in the default sibling directories, append:

```powershell
/p:SDL3UwpRoot=<SDL3_UWP-directory> `
/p:Msys2Ucrt64Root=<MSYS2-UCRT64-directory>
```

The signed MSIX, certificate, symbols, and dependency packages are generated
under:

```text
UWP-Port/AppPackages/UWP-Port/<package-version>_x64_Test/
```

Increment the four-part `Identity Version` in `Package.appxmanifest` before
creating an update for an already installed PC or Xbox package.

The current source manifest version is `1.0.0.110`.

## GitHub Actions

The `Build UWP-Port` workflow performs the complete x64 Release build on a
Windows runner. It checks out `rodrigoandrigo/SDL3_UWP`, builds its WinRT
project, builds the xemu embedding DLL, strips unneeded symbols from that DLL,
builds Mesa Gallium D3D12 and Vulkan DZN, packages UWP-Port, and uploads the
MSIX, certificate, symbols, and framework dependencies as the
`UWP-Port-x64-Release` artifact.

The workflow runs when relevant sources change and can also be started manually
from the GitHub Actions page.

## Runtime setup

On the Files page, select:

- Xbox BIOS/flash ROM
- MCPX boot ROM
- Xbox hard disk image
- DVD/XISO image (optional)

The Start xemu button is enabled after BIOS, MCPX, and hard disk are available.
At startup, UWP-Port creates `LocalState/BIOS`, `LocalState/MCPX`, and
`LocalState/hard_disk`; when no Future Access List selection exists, the first
file found in each corresponding folder is selected automatically. Manually
selected files remain in the Future Access List and override these defaults.
Files and folders are retained through the UWP Future Access List, so xemu uses
brokered virtual paths instead of unrestricted desktop filesystem paths.

Optional screenshot, games, and Memory Unit locations are configured on the
Storage page. Emulator settings are saved automatically. Network settings are
committed with the Save settings button on the Network page.

At first launch, UWP-Port creates `LocalState/games` and uses it as the default
Games folder. Selecting another Games folder stores that brokered folder in the
Windows Future Access List and overrides the LocalState default on later runs.

## Performance and memory behavior

OpenGL shader compilation and cache-file writes do not hold the renderer cache
mutex. Mesa performs NIR-to-DXIL compilation and D3D12 pipeline creation on its
compiler workers. Vulkan/DZN also persists its native pipeline cache in
`LocalState`, reducing compilation work on later launches. Cache files may be
removed to force a clean shader and pipeline rebuild when diagnosing renderer
problems.

The MCPX APU produces audio on its own thread. On UWP, PCI interrupt updates are
forwarded to the QEMU main loop without blocking that real-time producer on the
global QEMU lock. The audio stream uses bounded low and high watermarks so a
long renderer frame does not cause either an underrun or a CPU-heavy catch-up
burst.

The small performance overlay displayed over the emulation surface reports:

- **FPS**: completed video frames per second.
- **MSPF**: average milliseconds spent per video frame.

A temporary MSPF increase is expected while a new shader or pipeline is first
compiled. Persistent increases should be investigated with `xemu.log`, a cold
and warm cache comparison, and the same scene in both OpenGL and Vulkan/DZN.

## VLan/VPN rooms

The VLan/VPN page connects Xbox system-link traffic between compatible
UWP-Port instances. One participant selects **Host** to bridge the virtual LAN
to xemu's SLiRP Internet gateway. All other participants select **Client**.
Every participant must enter the same coordinator address and 32-character
room code before starting xemu.

The transport first uses the coordinator for endpoint discovery and attempts a
direct peer-to-peer UDP path. If hole punching is not possible, frames are
automatically sent through the relay. Rooms with more than two participants use
the relay so Ethernet broadcasts reach every member. The room code is a bearer
credential: generate a random code, share it privately, and do not reuse it for
public rooms.

The standalone coordinator is in `tools/vlan-relay`. Run it on a public server
and allow inbound and outbound UDP on its configured port:

```console
python tools/vlan-relay/server.py --host 0.0.0.0 --port 9939
```

The service stores no persistent account or room database. A production
operator should add firewall rate limits and monitoring. The current `XVL1`
transport provides private room separation but does not encrypt Ethernet frame
payloads; do not treat it as a confidentiality VPN on an untrusted relay.

## Logs

Runtime diagnostics are written to:

```text
ApplicationData/LocalFolder/xemu.log
```

The Logs page displays and refreshes the latest 256 KB of this file. On a PC,
the file can also be retrieved from the installed package's `LocalState`
directory. On Xbox, use Device Portal to access application files and download
the log.

Renderer shader and pipeline caches are also stored under `LocalState`; they do
not require access to unrestricted desktop paths.

## UWP limitations

- The application must not terminate the process through `exit()` or `abort()`;
  failures are returned through the embedding API and log callback.
- Desktop-only Win32 APIs and libraries must not be introduced into packaged
  binaries.
- File access outside application storage must use brokered `StorageFile` or
  `StorageFolder` objects.
- PCAP is unavailable in this UWP build. NAT, UDP tunnel, and VLan/VPN are the
  supported network backends.
- UWP apps can be suspended by the operating system. VLan/VPN remains connected
  only while UWP-Port and xemu are active; the standalone coordinator must run
  on an always-on public host.
- Rendering must remain attached to the XAML `SwapChainPanel`; desktop window
  ownership and desktop DXGI debug interfaces are not available on Xbox retail
  environments.
- Video settings that operate on the guest framebuffer or xemu HUD are
  supported: internal resolution, filtering, display fit, aspect ratio, VSync,
  notifications, animations, and HUD scale. Desktop window size, exclusive
  fullscreen, the desktop menu bar, and cursor timeout are intentionally fixed
  to UWP-safe values because presentation and the system pointer are owned by
  the `SwapChainPanel` host.
- UWP package updates are handled by the package distribution channel, and
  background controller capture cannot bypass UWP suspension. The UWP audio
  build uses the portable DSP interpreter and one voice worker; DSP JIT and a
  configurable worker count are therefore not exposed. Controller binding,
  axis inversion, controller models, Memory Units, real-time DSP, HRTF, volume,
  Xbox memory size, and AV Pack selection remain configurable.

## Legal notice

No copyrighted Xbox firmware, MCPX ROM, hard disk image, game image, keys, or
other proprietary content is included. Users must provide files they are
legally entitled to use.
