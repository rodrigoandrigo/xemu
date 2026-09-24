/*
 * xemu/QEMU host embedding API
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_HOST_H
#define QEMU_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
# ifdef QEMU_HOST_BUILD
#  define QEMU_HOST_EXPORT __declspec(dllexport)
# elif defined(QEMU_HOST_INTERNAL) || defined(CONFIG_UWP)
#  define QEMU_HOST_EXPORT
# else
#  define QEMU_HOST_EXPORT __declspec(dllimport)
# endif
#else
# define QEMU_HOST_EXPORT __attribute__((visibility("default")))
#endif

#define QEMU_HOST_API_VERSION_MAJOR 1U
#define QEMU_HOST_API_VERSION_MINOR 6U
#define QEMU_HOST_API_VERSION \
    ((QEMU_HOST_API_VERSION_MAJOR << 16) | QEMU_HOST_API_VERSION_MINOR)

typedef enum QemuHostLogLevel {
    QEMU_HOST_LOG_ERROR = 0,
    QEMU_HOST_LOG_WARNING = 1,
    QEMU_HOST_LOG_INFO = 2,
    QEMU_HOST_LOG_DEBUG = 3,
} QemuHostLogLevel;

typedef enum QemuHostPointerButton {
    QEMU_HOST_POINTER_BUTTON_LEFT = 0,
    QEMU_HOST_POINTER_BUTTON_MIDDLE = 1,
    QEMU_HOST_POINTER_BUTTON_RIGHT = 2,
    QEMU_HOST_POINTER_BUTTON_WHEEL_UP = 3,
    QEMU_HOST_POINTER_BUTTON_WHEEL_DOWN = 4,
    QEMU_HOST_POINTER_BUTTON_SIDE = 5,
    QEMU_HOST_POINTER_BUTTON_EXTRA = 6,
} QemuHostPointerButton;

typedef enum QemuHostGamepadButton {
    QEMU_HOST_GAMEPAD_A = 1U << 0,
    QEMU_HOST_GAMEPAD_B = 1U << 1,
    QEMU_HOST_GAMEPAD_X = 1U << 2,
    QEMU_HOST_GAMEPAD_Y = 1U << 3,
    QEMU_HOST_GAMEPAD_DPAD_LEFT = 1U << 4,
    QEMU_HOST_GAMEPAD_DPAD_UP = 1U << 5,
    QEMU_HOST_GAMEPAD_DPAD_RIGHT = 1U << 6,
    QEMU_HOST_GAMEPAD_DPAD_DOWN = 1U << 7,
    QEMU_HOST_GAMEPAD_BACK = 1U << 8,
    QEMU_HOST_GAMEPAD_START = 1U << 9,
    QEMU_HOST_GAMEPAD_LEFT_SHOULDER = 1U << 10,
    QEMU_HOST_GAMEPAD_RIGHT_SHOULDER = 1U << 11,
    QEMU_HOST_GAMEPAD_LEFT_STICK = 1U << 12,
    QEMU_HOST_GAMEPAD_RIGHT_STICK = 1U << 13,
    QEMU_HOST_GAMEPAD_GUIDE = 1U << 14,
} QemuHostGamepadButton;

typedef struct QemuHostGamepadState {
    uint32_t size;
    uint32_t buttons;
    int16_t left_trigger;
    int16_t right_trigger;
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
    bool connected;
} QemuHostGamepadState;

#define QEMU_HOST_HAS_VIDEO_METRICS_API 1
typedef struct QemuHostVideoMetrics {
    uint32_t size;
    uint32_t fps;
    uint32_t mspf;
} QemuHostVideoMetrics;

typedef void (*QemuHostLogCallback)(void *opaque, QemuHostLogLevel level,
                                    const char *message);

typedef long (*QemuHostSwapChainAttachCallback)(void *opaque,
                                                void *swapchain);

typedef struct QemuHostD3D12PresentTarget {
    uint32_t width;
    uint32_t height;
    QemuHostSwapChainAttachCallback attach;
    void *opaque;
} QemuHostD3D12PresentTarget;

typedef struct QemuHostStorageStat {
    uint64_t size;
    uint64_t allocated_size;
    uint64_t modified_time_ns;
    uint32_t mode;
    uint32_t type;
} QemuHostStorageStat;

typedef int (*QemuHostStorageOpenCallback)(void *opaque, const char *path,
                                           int flags, int mode,
                                           int64_t *handle);
typedef int64_t (*QemuHostStorageReadCallback)(void *opaque, int64_t handle,
                                               void *buffer, size_t size);
typedef int64_t (*QemuHostStorageWriteCallback)(void *opaque, int64_t handle,
                                                const void *buffer,
                                                size_t size);
typedef int (*QemuHostStorageCloseCallback)(void *opaque, int64_t handle);
typedef int64_t (*QemuHostStorageSeekCallback)(void *opaque, int64_t handle,
                                               int64_t offset, int whence);
typedef int (*QemuHostStorageStatCallback)(void *opaque, const char *path,
                                           QemuHostStorageStat *stat);
typedef int (*QemuHostStorageMkdirCallback)(void *opaque, const char *path,
                                            int mode);
typedef int (*QemuHostStoragePathCallback)(void *opaque, const char *path);
typedef int (*QemuHostStorageRenameCallback)(void *opaque,
                                             const char *old_path,
                                             const char *new_path);
typedef int (*QemuHostStorageFlushCallback)(void *opaque, int64_t handle);
typedef int (*QemuHostStorageReadDirCallback)(
    void *opaque, int64_t handle, char *name, size_t name_size,
    QemuHostStorageStat *stat);
typedef int (*QemuHostStorageTruncateCallback)(void *opaque, int64_t handle,
                                               uint64_t size);

typedef struct QemuHostStorageCallbacks {
    uint32_t size;
    uint32_t version;
    QemuHostStorageOpenCallback open;
    QemuHostStorageReadCallback read;
    QemuHostStorageWriteCallback write;
    QemuHostStorageCloseCallback close;
    QemuHostStorageSeekCallback seek;
    QemuHostStorageStatCallback stat;
    QemuHostStorageMkdirCallback mkdir;
    QemuHostStoragePathCallback unlink;
    QemuHostStorageRenameCallback rename;
    QemuHostStorageFlushCallback flush;
    QemuHostStorageReadDirCallback readdir;
    QemuHostStorageTruncateCallback truncate;
} QemuHostStorageCallbacks;

/*
 * Objects are WinRT ABI pointers supplied by the UWP host, for example
 * get_abi(StorageFile), get_abi(StorageFolder), and
 * get_abi(IRandomAccessStream). xemu stores but never interprets them.
 */
typedef void (*QemuHostBrokeredObjectCallback)(void *opaque, void *object);
typedef int (*QemuHostBrokeredOpenFileCallback)(
    void *opaque, void *storage_file, void *random_access_stream, int flags,
    int64_t *handle);
typedef int (*QemuHostBrokeredOpenAtCallback)(
    void *opaque, void *storage_folder, const char *relative_path, int flags,
    int mode, int64_t *handle);
typedef int (*QemuHostBrokeredStatFileCallback)(
    void *opaque, void *storage_file, void *random_access_stream,
    QemuHostStorageStat *stat);
typedef int (*QemuHostBrokeredStatAtCallback)(
    void *opaque, void *storage_folder, const char *relative_path,
    QemuHostStorageStat *stat);
typedef int (*QemuHostBrokeredPathAtCallback)(
    void *opaque, void *storage_folder, const char *relative_path);
typedef int (*QemuHostBrokeredMkdirAtCallback)(
    void *opaque, void *storage_folder, const char *relative_path, int mode);
typedef int (*QemuHostBrokeredRenameAtCallback)(
    void *opaque, void *storage_folder, const char *old_relative_path,
    const char *new_relative_path);

typedef struct QemuHostBrokeredStorageCallbacks {
    uint32_t size;
    uint32_t version;
    QemuHostBrokeredObjectCallback retain;
    QemuHostBrokeredObjectCallback release;
    QemuHostBrokeredOpenFileCallback open_file;
    QemuHostBrokeredOpenAtCallback open_at;
    QemuHostStorageReadCallback read;
    QemuHostStorageWriteCallback write;
    QemuHostStorageSeekCallback seek;
    QemuHostStorageCloseCallback close;
    QemuHostBrokeredStatFileCallback stat_file;
    QemuHostBrokeredStatAtCallback stat_at;
    QemuHostBrokeredMkdirAtCallback mkdir_at;
    QemuHostBrokeredPathAtCallback unlink_at;
    QemuHostBrokeredRenameAtCallback rename_at;
    QemuHostStorageFlushCallback flush;
    QemuHostStorageReadDirCallback readdir;
    QemuHostStorageTruncateCallback truncate;
} QemuHostBrokeredStorageCallbacks;

#define QEMU_HOST_STORAGE_CALLBACKS_VERSION 1U
#define QEMU_HOST_BROKERED_STORAGE_CALLBACKS_VERSION 1U
#define QEMU_HOST_STORAGE_OPEN_DIRECTORY 0x40000000

QEMU_HOST_EXPORT uint32_t qemu_host_get_api_version(void);
QEMU_HOST_EXPORT int qemu_host_init(int argc, char **argv);
QEMU_HOST_EXPORT int qemu_host_start(void);
QEMU_HOST_EXPORT int qemu_host_render_frame(void);
QEMU_HOST_EXPORT int qemu_host_main_loop_step(bool nonblocking,
                                              int *exit_status);
QEMU_HOST_EXPORT void qemu_host_wake_main_loop(void);
QEMU_HOST_EXPORT int qemu_host_pause(void);
QEMU_HOST_EXPORT int qemu_host_resume(void);
QEMU_HOST_EXPORT int qemu_host_request_shutdown(void);
QEMU_HOST_EXPORT int qemu_host_request_stop(void);
QEMU_HOST_EXPORT int qemu_host_reset(void);
QEMU_HOST_EXPORT int qemu_host_join(int *exit_status);
QEMU_HOST_EXPORT int qemu_host_cleanup(void);
QEMU_HOST_EXPORT bool qemu_host_is_initialized(void);
QEMU_HOST_EXPORT bool qemu_host_is_running(void);
QEMU_HOST_EXPORT int qemu_host_get_exit_status(void);
QEMU_HOST_EXPORT int qemu_host_get_video_metrics(
    QemuHostVideoMetrics *metrics);
QEMU_HOST_EXPORT int qemu_host_set_d3d12_present_target(
    uint32_t width, uint32_t height,
    QemuHostSwapChainAttachCallback attach, void *opaque);
int qemu_host_get_d3d12_present_target(QemuHostD3D12PresentTarget *target);

QEMU_HOST_EXPORT void qemu_host_register_log_callback(QemuHostLogCallback cb,
                                                      void *opaque);
QEMU_HOST_EXPORT bool qemu_host_log_sink_enabled(void);
QEMU_HOST_EXPORT void qemu_host_emit_log(QemuHostLogLevel level,
                                         const char *message);
/* The UWP host passes a UTF-8 path below ApplicationData LocalFolder. */
QEMU_HOST_EXPORT int qemu_host_set_log_file(const char *path);
/* Persistent Vulkan cache stored below ApplicationData LocalFolder. */
#define QEMU_HOST_HAS_PIPELINE_CACHE_FILE_API 1
QEMU_HOST_EXPORT int qemu_host_set_pipeline_cache_file(const char *path);
char *qemu_host_dup_pipeline_cache_file(void);

QEMU_HOST_EXPORT int qemu_host_register_storage_callbacks(
    const QemuHostStorageCallbacks *callbacks, void *opaque);
QEMU_HOST_EXPORT int qemu_host_register_brokered_storage_callbacks(
    const QemuHostBrokeredStorageCallbacks *callbacks, void *opaque);
QEMU_HOST_EXPORT int qemu_host_mount_brokered_file(
    const char *virtual_path, void *storage_file, void *random_access_stream);
QEMU_HOST_EXPORT int qemu_host_mount_brokered_folder(
    const char *virtual_path, void *storage_folder);
QEMU_HOST_EXPORT int qemu_host_unmount_brokered_storage(
    const char *virtual_path);

QEMU_HOST_EXPORT int qemu_host_storage_open(const char *path, int flags,
                                             int mode, int64_t *handle);
QEMU_HOST_EXPORT int64_t qemu_host_storage_read(int64_t handle, void *buffer,
                                                size_t size);
QEMU_HOST_EXPORT int64_t qemu_host_storage_write(int64_t handle,
                                                 const void *buffer,
                                                 size_t size);
QEMU_HOST_EXPORT int qemu_host_storage_close(int64_t handle);
QEMU_HOST_EXPORT int64_t qemu_host_storage_seek(int64_t handle,
                                                int64_t offset, int whence);
QEMU_HOST_EXPORT int qemu_host_storage_stat(const char *path,
                                             QemuHostStorageStat *stat);
QEMU_HOST_EXPORT int qemu_host_storage_mkdir(const char *path, int mode);
QEMU_HOST_EXPORT int qemu_host_storage_unlink(const char *path);
QEMU_HOST_EXPORT int qemu_host_storage_rename(const char *old_path,
                                               const char *new_path);
QEMU_HOST_EXPORT int qemu_host_storage_flush(int64_t handle);
QEMU_HOST_EXPORT int qemu_host_storage_readdir(int64_t handle, char *name,
                                                size_t name_size,
                                                QemuHostStorageStat *stat);
QEMU_HOST_EXPORT int qemu_host_storage_truncate(int64_t handle, uint64_t size);
QEMU_HOST_EXPORT bool qemu_host_storage_path_is_brokered(const char *path);

QEMU_HOST_EXPORT int qemu_host_send_key_number(int key_number, bool down);
QEMU_HOST_EXPORT int qemu_host_send_key_qcode(int qcode, bool down);
QEMU_HOST_EXPORT int qemu_host_send_pointer_rel(int dx, int dy);
QEMU_HOST_EXPORT int qemu_host_send_pointer_abs(int x, int y,
                                                int width, int height);
QEMU_HOST_EXPORT int qemu_host_send_pointer_button(
    QemuHostPointerButton button, bool down);
QEMU_HOST_EXPORT bool qemu_host_pointer_is_absolute(void);
QEMU_HOST_EXPORT int qemu_host_set_gamepad_state(
    unsigned int port, const QemuHostGamepadState *state);

#ifdef __cplusplus
}
#endif

#endif
