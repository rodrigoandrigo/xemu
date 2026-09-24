#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include "../include/qemu/qemu-host.h"

#ifndef QEMU_HOST_HAS_VIDEO_METRICS_API
typedef struct QemuHostVideoMetrics {
    uint32_t size;
    uint32_t fps;
    uint32_t mspf;
} QemuHostVideoMetrics;
QEMU_HOST_EXPORT int qemu_host_get_video_metrics(
    QemuHostVideoMetrics* metrics);
#endif

namespace UWP_Port
{
    class XemuHost final
    {
    public:
        XemuHost();
        ~XemuHost();

        bool Start(const std::vector<std::string>& arguments = {});
        void Stop();
        void Pause();
        void Resume();
        void Reset();
        void Shutdown();
        bool RenderFrame();
        bool GetVideoMetrics(uint32_t& fps, uint32_t& mspf) const;
        bool IsRunning() const { return m_running.load(); }
        std::string LastError() const;
        bool AttachRenderPanel(Windows::UI::Xaml::Controls::SwapChainPanel^ panel);
        bool UpdateRenderPanelSize(Windows::UI::Xaml::Controls::SwapChainPanel^ panel);

        bool MountFile(const std::string& virtualPath,
                       Windows::Storage::StorageFile^ file,
                       Windows::Storage::Streams::IRandomAccessStream^ stream);
        bool MountFolder(const std::string& virtualPath,
                         Windows::Storage::StorageFolder^ folder);

    private:
        template<typename T> bool Resolve(T& target, const char* name);
        bool Load();
        void Run(std::vector<std::string> arguments);
        void WriteDiagnostic(const std::string& message);
        void SetError(const std::string& error);
        bool ResolveSDLInput();
        void UpdateUWPGamepad();
        void DetachUWPGamepad();
        void TrackBrokeredHandle(void* handle);
        bool UntrackBrokeredHandle(void* handle);
        void ReleaseBrokeredHandles();
        static void __cdecl Log(void* opaque, QemuHostLogLevel level,
                                const char* message);
        static void __cdecl SDLLog(void* opaque, int category, int priority,
                                   const char* message);
        static void __cdecl MesaLog(void* opaque, const char* message);
        static long __cdecl AttachMesaSwapChain(void* opaque, void* swapchain);
        static void RetainBrokeredObject(void* opaque, void* object);
        static void ReleaseBrokeredObject(void* opaque, void* object);
        static int OpenBrokeredFile(void* opaque, void* storageFile,
                                    void* randomAccessStream, int flags,
                                    int64_t* handle);
        static int OpenBrokeredPath(void* opaque, void* storageFolder,
                                    const char* relativePath, int flags,
                                    int mode, int64_t* handle);
        static int64_t ReadBrokeredFile(void* opaque, int64_t handle,
                                        void* buffer, size_t size);
        static int64_t WriteBrokeredFile(void* opaque, int64_t handle,
                                         const void* buffer, size_t size);
        static int64_t SeekBrokeredFile(void* opaque, int64_t handle,
                                        int64_t offset, int whence);
        static int CloseBrokeredFile(void* opaque, int64_t handle);
        static int StatBrokeredFile(void* opaque, void* storageFile,
                                    void* randomAccessStream,
                                    QemuHostStorageStat* stat);
        static int StatBrokeredPath(void* opaque, void* storageFolder,
                                    const char* relativePath,
                                    QemuHostStorageStat* stat);
        static int FlushBrokeredFile(void* opaque, int64_t handle);
        static int ReadBrokeredDirectory(void* opaque, int64_t handle,
                                         char* name, size_t nameSize,
                                         QemuHostStorageStat* stat);
        static int TruncateBrokeredFile(void* opaque, int64_t handle,
                                        uint64_t size);

        HMODULE m_module;
        HMODULE m_sdlModule;
        HMODULE m_openGLModule;
        HMODULE m_mesaModule;
        HMODULE m_vulkanModule;
        std::thread m_thread;
        std::atomic<bool> m_running;
        std::atomic<bool> m_stop;
        std::atomic<bool> m_firstFrameLogged;
        mutable std::mutex m_mutex;
        std::mutex m_logMutex;
		std::mutex m_storageMutex;
		std::unordered_set<void*> m_openBrokeredHandles;
        std::string m_error;
        std::wstring m_logPath;

        using AttachMesa = void (__cdecl *)(void*, int, int);
        using SetMesaSwapChainAttach = void (__cdecl *)(
            long (__cdecl *)(void*, void*), void*);
        using UpdateSDLPanelSize = bool (__cdecl *)(int, int, int, int);
        using SetD3D12PresentTarget = int (__cdecl *)(
            uint32_t, uint32_t, QemuHostSwapChainAttachCallback, void*);
        AttachMesa m_attachMesa;
        SetMesaSwapChainAttach m_setMesaSwapChainAttach;
        AttachMesa m_attachDzn;
        SetMesaSwapChainAttach m_setDznSwapChainAttach;
        UpdateSDLPanelSize m_updateSDLPanelSize;
        SetD3D12PresentTarget m_setD3D12PresentTarget;
        using AttachVirtualJoystick = SDL_JoystickID (__cdecl *)(
            const SDL_VirtualJoystickDesc*);
        using DetachVirtualJoystick = bool (__cdecl *)(SDL_JoystickID);
        using OpenJoystick = SDL_Joystick* (__cdecl *)(SDL_JoystickID);
        using CloseJoystick = void (__cdecl *)(SDL_Joystick*);
        using SetVirtualAxis = bool (__cdecl *)(SDL_Joystick*, int, int16_t);
        using SetVirtualButton = bool (__cdecl *)(SDL_Joystick*, int, bool);
        using SetEmbeddedCursorHidden = void (__cdecl *)(bool);
        AttachVirtualJoystick m_attachVirtualJoystick;
        DetachVirtualJoystick m_detachVirtualJoystick;
        OpenJoystick m_openJoystick;
        CloseJoystick m_closeJoystick;
        SetVirtualAxis m_setVirtualAxis;
        SetVirtualButton m_setVirtualButton;
        SetEmbeddedCursorHidden m_setEmbeddedCursorHidden;
        decltype(&qemu_host_set_gamepad_state) m_setGamepadState;
        SDL_JoystickID m_virtualJoystickId;
        SDL_Joystick* m_virtualJoystick;
        Windows::Gaming::Input::Gamepad^ m_uwpGamepad;
        bool m_gamepadErrorLogged;
        uint64_t m_lastGamepadTimestamp;
        unsigned int m_gamepadChangeLogs;
        Windows::UI::Xaml::Controls::SwapChainPanel^ m_renderPanel;
        Microsoft::WRL::ComPtr<IDXGISwapChain2> m_swapChain;
        uint32_t m_renderWidth;
        uint32_t m_renderHeight;

        decltype(&qemu_host_get_api_version) m_getApiVersion;
        decltype(&qemu_host_get_video_metrics) m_getVideoMetrics;
        decltype(&qemu_host_init) m_init;
        decltype(&qemu_host_start) m_start;
        decltype(&qemu_host_render_frame) m_renderFrame;
        decltype(&qemu_host_main_loop_step) m_step;
        decltype(&qemu_host_is_running) m_isHostRunning;
        decltype(&qemu_host_request_stop) m_requestStop;
        decltype(&qemu_host_pause) m_pause;
        decltype(&qemu_host_resume) m_resume;
        decltype(&qemu_host_reset) m_reset;
        decltype(&qemu_host_request_shutdown) m_shutdown;
        decltype(&qemu_host_join) m_join;
        decltype(&qemu_host_cleanup) m_cleanup;
        decltype(&qemu_host_register_log_callback) m_registerLog;
        decltype(&qemu_host_set_log_file) m_setLogFile;
        decltype(&qemu_host_set_pipeline_cache_file) m_setPipelineCacheFile;
        decltype(&qemu_host_register_brokered_storage_callbacks) m_registerBrokeredStorage;
        decltype(&qemu_host_mount_brokered_file) m_mountFile;
        decltype(&qemu_host_mount_brokered_folder) m_mountFolder;
    };
}
