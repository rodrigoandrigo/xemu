/*
 * xemu/QEMU host embedding lifecycle API.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define QEMU_HOST_BUILD
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/qemu-host.h"
#include "qemu/thread.h"
#include "hw/xbox/nv2a/debug.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "system/system.h"
#include "ui/input.h"
#include "ui/xemu-settings.h"

#ifndef QEMU_HOST_HAS_VIDEO_METRICS_API
typedef struct QemuHostVideoMetrics {
    uint32_t size;
    uint32_t fps;
    uint32_t mspf;
} QemuHostVideoMetrics;
#endif

static GMutex host_state_lock;
static GMutex host_log_lock;
static bool host_initializing;
static bool host_initialized;
static bool host_running;
static bool host_loop_exited;
static bool host_thread_started;
static bool host_step_active;
static bool host_cleaned;
static int host_exit_status;
static QemuThread host_loop_thread;
static QemuHostLogCallback host_log_callback;
static void *host_log_opaque;
static FILE *host_log_file;
static char *host_pipeline_cache_file;
static QemuHostD3D12PresentTarget host_d3d12_present_target;
static GMutex host_storage_lock;
static QemuHostStorageCallbacks host_storage_callbacks;
static void *host_storage_opaque;
static QemuHostBrokeredStorageCallbacks host_brokered_callbacks;
static void *host_brokered_opaque;

typedef enum QemuHostBrokeredMountType {
    QEMU_HOST_BROKERED_FILE,
    QEMU_HOST_BROKERED_FOLDER,
} QemuHostBrokeredMountType;

typedef struct QemuHostBrokeredMount {
    char *path;
    QemuHostBrokeredMountType type;
    void *object;
    void *stream;
} QemuHostBrokeredMount;

typedef struct QemuHostBrokeredHandle {
    int64_t public_handle;
    int64_t backend_handle;
} QemuHostBrokeredHandle;

static GPtrArray *host_brokered_mounts;
static GHashTable *host_brokered_handles;
static uint64_t host_next_brokered_handle = UINT64_C(0x4000000000000000);

bool xemu_prepare_embedded_display(void);
void xemu_start_embedded_display(void);
void xemu_render_embedded_frame(void);
void xemu_stop_embedded_display(void);
void xemu_shutdown_embedded_display(void);

static bool host_brokered_mount_exists(const char *path)
{
    guint i;
    bool found = false;

    g_mutex_lock(&host_storage_lock);
    for (i = 0; host_brokered_mounts && i < host_brokered_mounts->len; i++) {
        QemuHostBrokeredMount *mount =
            g_ptr_array_index(host_brokered_mounts, i);

        if (!strcmp(mount->path, path)) {
            found = true;
            break;
        }
    }
    g_mutex_unlock(&host_storage_lock);
    return found;
}

static void host_apply_brokered_machine_files(void)
{
    bool has_flash = host_brokered_mount_exists("/broker/flash");

    if (has_flash) {
        xemu_settings_set_string(&g_config.sys.files.flashrom_path,
                                 "/broker/flash");
        g_config.general.show_welcome = false;
    }
    if (host_brokered_mount_exists("/broker/bootrom")) {
        xemu_settings_set_string(&g_config.sys.files.bootrom_path,
                                 "/broker/bootrom");
    }
    if (host_brokered_mount_exists("/broker/hdd")) {
        xemu_settings_set_string(&g_config.sys.files.hdd_path,
                                 "/broker/hdd");
    }
    if (host_brokered_mount_exists("/broker/eeprom")) {
        xemu_settings_set_string(&g_config.sys.files.eeprom_path,
                                 "/broker/eeprom");
    }
    if (host_brokered_mount_exists("/broker/dvd")) {
        xemu_settings_set_string(&g_config.sys.files.dvd_path,
                                 "/broker/dvd");
    }
    if (host_brokered_mount_exists("/broker/screenshots")) {
        xemu_settings_set_string(&g_config.general.screenshot_dir,
                                 "/broker/screenshots");
    }
    if (host_brokered_mount_exists("/broker/games")) {
        xemu_settings_set_string(&g_config.general.games_dir,
                                 "/broker/games");
    }
    if (has_flash) {
        qemu_host_emit_log(QEMU_HOST_LOG_DEBUG,
                           "embedding: brokered machine files applied");
    }
}

static bool host_brokered_path_valid(const char *path)
{
    const char *component;

    if (!path || path[0] != '/' || path[1] == '\0' || strchr(path, '\\')) {
        return false;
    }
    for (component = path + 1; *component;) {
        const char *end = strchr(component, '/');
        size_t length = end ? end - component : strlen(component);

        if (!length || (length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) {
            return false;
        }
        if (!end) {
            break;
        }
        component = end + 1;
        if (!*component) {
            return false;
        }
    }
    return true;
}

static void host_brokered_mount_free(QemuHostBrokeredMount *mount)
{
    if (host_brokered_callbacks.release) {
        if (mount->stream) {
            host_brokered_callbacks.release(host_brokered_opaque,
                                             mount->stream);
        }
        host_brokered_callbacks.release(host_brokered_opaque, mount->object);
    }
    g_free(mount->path);
    g_free(mount);
}

static void host_brokered_ensure_tables(void)
{
    if (!host_brokered_mounts) {
        host_brokered_mounts = g_ptr_array_new_with_free_func(
            (GDestroyNotify)host_brokered_mount_free);
    }
    if (!host_brokered_handles) {
        host_brokered_handles = g_hash_table_new_full(
            g_int64_hash, g_int64_equal, g_free, g_free);
    }
}

static QemuHostBrokeredMount *host_brokered_find_mount_locked(
    const char *path, const char **relative_path)
{
    QemuHostBrokeredMount *best = NULL;
    size_t best_length = 0;
    guint i;

    if (!host_brokered_mounts) {
        return NULL;
    }
    for (i = 0; i < host_brokered_mounts->len; i++) {
        QemuHostBrokeredMount *mount =
            g_ptr_array_index(host_brokered_mounts, i);
        size_t length = strlen(mount->path);

        if (strncmp(path, mount->path, length) ||
            (path[length] && path[length] != '/') ||
            (mount->type == QEMU_HOST_BROKERED_FILE && path[length])) {
            continue;
        }
        if (length > best_length) {
            best = mount;
            best_length = length;
        }
    }
    if (best && relative_path) {
        *relative_path = path + best_length;
        if (**relative_path == '/') {
            (*relative_path)++;
        }
    }
    return best;
}

static int64_t host_brokered_publish_handle(int64_t backend_handle)
{
    QemuHostBrokeredHandle *handle = g_new(QemuHostBrokeredHandle, 1);
    int64_t *key = g_new(int64_t, 1);

    g_mutex_lock(&host_storage_lock);
    host_brokered_ensure_tables();
    do {
        handle->public_handle = host_next_brokered_handle++;
        if (host_next_brokered_handle >= INT64_MAX) {
            host_next_brokered_handle = UINT64_C(0x4000000000000000);
        }
    } while (g_hash_table_contains(host_brokered_handles,
                                   &handle->public_handle));
    handle->backend_handle = backend_handle;
    *key = handle->public_handle;
    g_hash_table_insert(host_brokered_handles, key, handle);
    g_mutex_unlock(&host_storage_lock);
    return handle->public_handle;
}

static bool host_brokered_handle_snapshot(
    int64_t handle, int64_t *backend_handle,
    QemuHostBrokeredStorageCallbacks *callbacks, void **opaque)
{
    QemuHostBrokeredHandle *entry;

    g_mutex_lock(&host_storage_lock);
    entry = host_brokered_handles ?
            g_hash_table_lookup(host_brokered_handles, &handle) : NULL;
    if (entry) {
        *backend_handle = entry->backend_handle;
        *callbacks = host_brokered_callbacks;
        *opaque = host_brokered_opaque;
    }
    g_mutex_unlock(&host_storage_lock);
    return entry != NULL;
}

static bool host_is_ready(void)
{
    bool ready;

    g_mutex_lock(&host_state_lock);
    ready = host_initialized && !host_cleaned;
    g_mutex_unlock(&host_state_lock);
    return ready;
}

static void host_set_running(bool running)
{
    g_mutex_lock(&host_state_lock);
    host_running = running;
    if (!running) {
        host_loop_exited = true;
    }
    g_mutex_unlock(&host_state_lock);
}

void qemu_host_register_log_callback(QemuHostLogCallback cb, void *opaque)
{
    g_mutex_lock(&host_log_lock);
    host_log_callback = cb;
    host_log_opaque = opaque;
    g_mutex_unlock(&host_log_lock);
}

bool qemu_host_log_sink_enabled(void)
{
    bool enabled;

    g_mutex_lock(&host_log_lock);
    enabled = host_log_callback || host_log_file;
    g_mutex_unlock(&host_log_lock);
    return enabled;
}

void qemu_host_emit_log(QemuHostLogLevel level, const char *message)
{
    QemuHostLogCallback cb;
    void *opaque;

    if (!message) {
        return;
    }
    g_mutex_lock(&host_log_lock);
    cb = host_log_callback;
    opaque = host_log_opaque;
    if (host_log_file) {
        fprintf(host_log_file, "%s\n", message);
        fflush(host_log_file);
    }
    g_mutex_unlock(&host_log_lock);
    if (cb) {
        cb(opaque, level, message);
    }
}

int qemu_host_set_log_file(const char *path)
{
    FILE *file = NULL;

    if (path && path[0]) {
        file = fopen(path, "ab");
        if (!file) {
            return -errno;
        }
    }
    g_mutex_lock(&host_log_lock);
    if (host_log_file) {
        fclose(host_log_file);
    }
    host_log_file = file;
    g_mutex_unlock(&host_log_lock);
    return 0;
}

int qemu_host_set_pipeline_cache_file(const char *path)
{
    char *copy = path && *path ? g_strdup(path) : NULL;

    g_mutex_lock(&host_state_lock);
    g_free(host_pipeline_cache_file);
    host_pipeline_cache_file = copy;
    g_mutex_unlock(&host_state_lock);
    return 0;
}

int qemu_host_set_d3d12_present_target(
    uint32_t width, uint32_t height,
    QemuHostSwapChainAttachCallback attach, void *opaque)
{
    if (!width || !height || !attach) {
        return -EINVAL;
    }

    g_mutex_lock(&host_state_lock);
    host_d3d12_present_target.width = width;
    host_d3d12_present_target.height = height;
    host_d3d12_present_target.attach = attach;
    host_d3d12_present_target.opaque = opaque;
    g_mutex_unlock(&host_state_lock);
    return 0;
}

int qemu_host_get_d3d12_present_target(QemuHostD3D12PresentTarget *target)
{
    if (!target) {
        return -EINVAL;
    }

    g_mutex_lock(&host_state_lock);
    *target = host_d3d12_present_target;
    g_mutex_unlock(&host_state_lock);
    return target->attach ? 0 : -ENODEV;
}

int qemu_host_get_video_metrics(QemuHostVideoMetrics *metrics)
{
    if (!metrics || metrics->size < sizeof(*metrics)) {
        return -EINVAL;
    }

    nv2a_profile_get_video_metrics(&metrics->fps, &metrics->mspf);
    return 0;
}

char *qemu_host_dup_pipeline_cache_file(void)
{
    char *copy;

    g_mutex_lock(&host_state_lock);
    copy = g_strdup(host_pipeline_cache_file);
    g_mutex_unlock(&host_state_lock);
    return copy;
}

int qemu_host_register_storage_callbacks(
    const QemuHostStorageCallbacks *callbacks, void *opaque)
{
    g_mutex_lock(&host_state_lock);
    if (host_initializing || host_initialized) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    g_mutex_lock(&host_storage_lock);
    memset(&host_storage_callbacks, 0, sizeof(host_storage_callbacks));
    host_storage_opaque = NULL;
    if (callbacks) {
        if (callbacks->version != QEMU_HOST_STORAGE_CALLBACKS_VERSION ||
            callbacks->size < offsetof(QemuHostStorageCallbacks, seek) +
                              sizeof(callbacks->seek) ||
            !callbacks->open || !callbacks->read || !callbacks->close) {
            g_mutex_unlock(&host_storage_lock);
            g_mutex_unlock(&host_state_lock);
            return -EINVAL;
        }
        memcpy(&host_storage_callbacks, callbacks,
               MIN((size_t)callbacks->size, sizeof(host_storage_callbacks)));
        host_storage_opaque = opaque;
    }
    g_mutex_unlock(&host_storage_lock);
    g_mutex_unlock(&host_state_lock);
    return 0;
}

int qemu_host_register_brokered_storage_callbacks(
    const QemuHostBrokeredStorageCallbacks *callbacks, void *opaque)
{
    if (!callbacks ||
        callbacks->version != QEMU_HOST_BROKERED_STORAGE_CALLBACKS_VERSION ||
        callbacks->size < sizeof(*callbacks) || !callbacks->retain ||
        !callbacks->release || !callbacks->open_file || !callbacks->open_at ||
        !callbacks->read || !callbacks->seek || !callbacks->close ||
        !callbacks->readdir) {
        return -EINVAL;
    }
    g_mutex_lock(&host_state_lock);
    if (host_initializing || host_initialized) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    g_mutex_lock(&host_storage_lock);
    if (host_brokered_mounts && host_brokered_mounts->len) {
        g_mutex_unlock(&host_storage_lock);
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    host_brokered_callbacks = *callbacks;
    host_brokered_opaque = opaque;
    host_brokered_ensure_tables();
    g_mutex_unlock(&host_storage_lock);
    g_mutex_unlock(&host_state_lock);
    return 0;
}

static int host_mount_brokered(const char *virtual_path, void *object,
                               void *stream, QemuHostBrokeredMountType type)
{
    QemuHostBrokeredMount *mount;
    guint i;

    if (!host_brokered_path_valid(virtual_path) || !object ||
        (type == QEMU_HOST_BROKERED_FILE && !stream)) {
        return -EINVAL;
    }
    g_mutex_lock(&host_state_lock);
    if (host_initializing || host_initialized) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    g_mutex_lock(&host_storage_lock);
    if (!host_brokered_callbacks.retain) {
        g_mutex_unlock(&host_storage_lock);
        g_mutex_unlock(&host_state_lock);
        return -ENOSYS;
    }
    host_brokered_ensure_tables();
    host_brokered_callbacks.retain(host_brokered_opaque, object);
    if (stream) {
        host_brokered_callbacks.retain(host_brokered_opaque, stream);
    }
    mount = g_new0(QemuHostBrokeredMount, 1);
    mount->path = g_strdup(virtual_path);
    mount->type = type;
    mount->object = object;
    mount->stream = stream;
    for (i = 0; i < host_brokered_mounts->len; i++) {
        QemuHostBrokeredMount *existing =
            g_ptr_array_index(host_brokered_mounts, i);

        if (!strcmp(existing->path, virtual_path)) {
            g_ptr_array_index(host_brokered_mounts, i) = mount;
            host_brokered_mount_free(existing);
            g_mutex_unlock(&host_storage_lock);
            g_mutex_unlock(&host_state_lock);
            return 0;
        }
    }
    g_ptr_array_add(host_brokered_mounts, mount);
    g_mutex_unlock(&host_storage_lock);
    g_mutex_unlock(&host_state_lock);
    return 0;
}

int qemu_host_mount_brokered_file(const char *virtual_path,
                                  void *storage_file,
                                  void *random_access_stream)
{
    return host_mount_brokered(virtual_path, storage_file,
                               random_access_stream, QEMU_HOST_BROKERED_FILE);
}

int qemu_host_mount_brokered_folder(const char *virtual_path,
                                    void *storage_folder)
{
    return host_mount_brokered(virtual_path, storage_folder, NULL,
                               QEMU_HOST_BROKERED_FOLDER);
}

int qemu_host_unmount_brokered_storage(const char *virtual_path)
{
    guint i;

    if (!host_brokered_path_valid(virtual_path)) {
        return -EINVAL;
    }
    g_mutex_lock(&host_state_lock);
    if (host_initializing || host_initialized) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    g_mutex_lock(&host_storage_lock);
    for (i = 0; host_brokered_mounts && i < host_brokered_mounts->len; i++) {
        QemuHostBrokeredMount *mount =
            g_ptr_array_index(host_brokered_mounts, i);

        if (!strcmp(mount->path, virtual_path)) {
            g_ptr_array_remove_index(host_brokered_mounts, i);
            g_mutex_unlock(&host_storage_lock);
            g_mutex_unlock(&host_state_lock);
            return 0;
        }
    }
    g_mutex_unlock(&host_storage_lock);
    g_mutex_unlock(&host_state_lock);
    return -ENOENT;
}

int qemu_host_storage_open(const char *path, int flags, int mode,
                           int64_t *handle)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostBrokeredMount *mount;
    QemuHostStorageOpenCallback cb;
    g_autofree char *relative = NULL;
    void *object = NULL;
    void *stream = NULL;
    void *opaque;
    int64_t backend_handle;
    int ret;

    if (!path || !handle) {
        return -EINVAL;
    }
    g_mutex_lock(&host_storage_lock);
    mount = host_brokered_find_mount_locked(path, NULL);
    if (mount) {
        const char *relative_path;

        host_brokered_find_mount_locked(path, &relative_path);
        relative = g_strdup(relative_path);
        object = mount->object;
        stream = mount->stream;
        brokered = host_brokered_callbacks;
        opaque = host_brokered_opaque;
        ret = mount->type;
        g_mutex_unlock(&host_storage_lock);
        ret = ret == QEMU_HOST_BROKERED_FILE ?
              brokered.open_file(opaque, object, stream, flags,
                                 &backend_handle) :
              brokered.open_at(opaque, object, relative, flags, mode,
                               &backend_handle);
        if (ret < 0) {
            return ret;
        }
        *handle = host_brokered_publish_handle(backend_handle);
        return 0;
    }
    cb = host_storage_callbacks.open;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_storage_lock);
    return cb ? cb(opaque, path, flags, mode, handle) : -ENOSYS;
}

int64_t qemu_host_storage_read(int64_t handle, void *buffer, size_t size)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageReadCallback cb;
    int64_t backend_handle;
    void *opaque;

    if (!buffer && size) {
        return -EINVAL;
    }
    if (!host_brokered_handle_snapshot(handle, &backend_handle,
                                       &brokered, &opaque)) {
        g_mutex_lock(&host_storage_lock);
        cb = host_storage_callbacks.read;
        opaque = host_storage_opaque;
        g_mutex_unlock(&host_storage_lock);
        return cb ? cb(opaque, handle, buffer, size) : -ENOSYS;
    }
    return brokered.read(opaque, backend_handle, buffer, size);
}

int64_t qemu_host_storage_write(int64_t handle, const void *buffer, size_t size)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageWriteCallback cb;
    int64_t backend_handle;
    void *opaque;

    if (!buffer && size) {
        return -EINVAL;
    }
    if (!host_brokered_handle_snapshot(handle, &backend_handle,
                                       &brokered, &opaque)) {
        g_mutex_lock(&host_storage_lock);
        cb = host_storage_callbacks.write;
        opaque = host_storage_opaque;
        g_mutex_unlock(&host_storage_lock);
        return cb ? cb(opaque, handle, buffer, size) : -ENOSYS;
    }
    return brokered.write ?
           brokered.write(opaque, backend_handle, buffer, size) : -EROFS;
}

int qemu_host_storage_close(int64_t handle)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageCloseCallback cb;
    int64_t backend_handle;
    void *opaque;
    int ret;

    if (host_brokered_handle_snapshot(handle, &backend_handle,
                                      &brokered, &opaque)) {
        ret = brokered.close(opaque, backend_handle);
        if (ret >= 0) {
            g_mutex_lock(&host_storage_lock);
            g_hash_table_remove(host_brokered_handles, &handle);
            g_mutex_unlock(&host_storage_lock);
        }
        return ret;
    }
    g_mutex_lock(&host_storage_lock);
    cb = host_storage_callbacks.close;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_storage_lock);
    return cb ? cb(opaque, handle) : -ENOSYS;
}

int64_t qemu_host_storage_seek(int64_t handle, int64_t offset, int whence)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageSeekCallback cb;
    int64_t backend_handle;
    void *opaque;

    if (host_brokered_handle_snapshot(handle, &backend_handle,
                                      &brokered, &opaque)) {
        return brokered.seek(opaque, backend_handle, offset, whence);
    }
    g_mutex_lock(&host_storage_lock);
    cb = host_storage_callbacks.seek;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_storage_lock);
    return cb ? cb(opaque, handle, offset, whence) : -ENOSYS;
}

int qemu_host_storage_stat(const char *path, QemuHostStorageStat *stat)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostBrokeredMount *mount;
    QemuHostStorageStatCallback cb;
    g_autofree char *relative = NULL;
    void *object;
    void *stream;
    void *opaque;
    QemuHostBrokeredMountType type;

    if (!path || !stat) {
        return -EINVAL;
    }
    g_mutex_lock(&host_storage_lock);
    mount = host_brokered_find_mount_locked(path, NULL);
    if (mount) {
        const char *relative_path;

        host_brokered_find_mount_locked(path, &relative_path);
        relative = g_strdup(relative_path);
        object = mount->object;
        stream = mount->stream;
        type = mount->type;
        brokered = host_brokered_callbacks;
        opaque = host_brokered_opaque;
        g_mutex_unlock(&host_storage_lock);
        return type == QEMU_HOST_BROKERED_FILE ?
               (brokered.stat_file ? brokered.stat_file(
                    opaque, object, stream, stat) : -ENOSYS) :
               (brokered.stat_at ? brokered.stat_at(
                    opaque, object, relative, stat) : -ENOSYS);
    }
    cb = host_storage_callbacks.stat;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_storage_lock);
    return cb ? cb(opaque, path, stat) : -ENOSYS;
}

static int host_brokered_folder_snapshot(
    const char *path, void **folder, char **relative,
    QemuHostBrokeredStorageCallbacks *callbacks, void **opaque)
{
    QemuHostBrokeredMount *mount;
    const char *relative_path;

    g_mutex_lock(&host_storage_lock);
    mount = path ? host_brokered_find_mount_locked(path, &relative_path) : NULL;
    if (!mount) {
        g_mutex_unlock(&host_storage_lock);
        return -ENOENT;
    }
    if (mount->type != QEMU_HOST_BROKERED_FOLDER) {
        g_mutex_unlock(&host_storage_lock);
        return -ENOTDIR;
    }
    *folder = mount->object;
    *relative = g_strdup(relative_path);
    *callbacks = host_brokered_callbacks;
    *opaque = host_brokered_opaque;
    g_mutex_unlock(&host_storage_lock);
    return 0;
}

int qemu_host_storage_mkdir(const char *path, int mode)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageMkdirCallback cb;
    g_autofree char *relative = NULL;
    void *folder;
    void *opaque;
    int ret = host_brokered_folder_snapshot(path, &folder, &relative,
                                            &brokered, &opaque);

    if (ret != -ENOENT) {
        if (!ret && !relative[0]) {
            return -EEXIST;
        }
        return ret < 0 ? ret : brokered.mkdir_at ?
               brokered.mkdir_at(opaque, folder, relative, mode) : -ENOSYS;
    }
    g_mutex_lock(&host_storage_lock);
    cb = host_storage_callbacks.mkdir;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_storage_lock);
    return cb ? cb(opaque, path, mode) : -ENOSYS;
}

int qemu_host_storage_unlink(const char *path)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStoragePathCallback cb;
    g_autofree char *relative = NULL;
    void *folder;
    void *opaque;
    int ret = host_brokered_folder_snapshot(path, &folder, &relative,
                                            &brokered, &opaque);

    if (ret != -ENOENT) {
        return ret < 0 ? ret : brokered.unlink_at ?
               brokered.unlink_at(opaque, folder, relative) : -ENOSYS;
    }
    g_mutex_lock(&host_storage_lock);
    cb = host_storage_callbacks.unlink;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_storage_lock);
    return cb ? cb(opaque, path) : -ENOSYS;
}

int qemu_host_storage_rename(const char *old_path, const char *new_path)
{
    QemuHostBrokeredStorageCallbacks old_brokered;
    QemuHostBrokeredStorageCallbacks new_brokered;
    QemuHostStorageRenameCallback cb;
    g_autofree char *old_relative = NULL;
    g_autofree char *new_relative = NULL;
    void *old_folder;
    void *new_folder;
    void *old_opaque;
    void *new_opaque;
    int old_ret = host_brokered_folder_snapshot(
        old_path, &old_folder, &old_relative, &old_brokered, &old_opaque);
    int new_ret = host_brokered_folder_snapshot(
        new_path, &new_folder, &new_relative, &new_brokered, &new_opaque);

    if (old_ret != -ENOENT || new_ret != -ENOENT) {
        if (old_ret < 0 || new_ret < 0 || old_folder != new_folder ||
            old_opaque != new_opaque) {
            return -EXDEV;
        }
        return old_brokered.rename_at ? old_brokered.rename_at(
                   old_opaque, old_folder, old_relative, new_relative) :
               -ENOSYS;
    }
    g_mutex_lock(&host_storage_lock);
    cb = host_storage_callbacks.rename;
    old_opaque = host_storage_opaque;
    g_mutex_unlock(&host_storage_lock);
    return cb ? cb(old_opaque, old_path, new_path) : -ENOSYS;
}

int qemu_host_storage_flush(int64_t handle)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageFlushCallback cb;
    int64_t backend_handle;
    void *opaque;

    if (host_brokered_handle_snapshot(handle, &backend_handle,
                                      &brokered, &opaque)) {
        return brokered.flush ? brokered.flush(opaque, backend_handle) : 0;
    }
    g_mutex_lock(&host_storage_lock);
    cb = host_storage_callbacks.flush;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_storage_lock);
    return cb ? cb(opaque, handle) : -ENOSYS;
}

int qemu_host_storage_readdir(int64_t handle, char *name, size_t name_size,
                              QemuHostStorageStat *stat)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageReadDirCallback cb;
    int64_t backend_handle;
    void *opaque;

    if (!name || !name_size || !stat) {
        return -EINVAL;
    }
    if (host_brokered_handle_snapshot(handle, &backend_handle,
                                      &brokered, &opaque)) {
        return brokered.readdir(opaque, backend_handle, name, name_size, stat);
    }
    g_mutex_lock(&host_storage_lock);
    cb = host_storage_callbacks.readdir;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_storage_lock);
    return cb ? cb(opaque, handle, name, name_size, stat) : -ENOSYS;
}

int qemu_host_storage_truncate(int64_t handle, uint64_t size)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageTruncateCallback cb;
    int64_t backend_handle;
    void *opaque;

    if (host_brokered_handle_snapshot(handle, &backend_handle,
                                      &brokered, &opaque)) {
        return brokered.truncate ?
               brokered.truncate(opaque, backend_handle, size) : -ENOSYS;
    }
    g_mutex_lock(&host_storage_lock);
    cb = host_storage_callbacks.truncate;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_storage_lock);
    return cb ? cb(opaque, handle, size) : -ENOSYS;
}

bool qemu_host_storage_path_is_brokered(const char *path)
{
    bool brokered;

    g_mutex_lock(&host_storage_lock);
    brokered = path && host_brokered_find_mount_locked(path, NULL);
    g_mutex_unlock(&host_storage_lock);
    return brokered;
}

static void *qemu_host_loop_thread(void *opaque)
{
    int status;

#if defined(XBOX) || defined(CONFIG_UWP)
    qemu_mutex_lock_main_loop();
#endif
    replay_mutex_lock();
    bql_lock();
    status = qemu_main_loop();
    bql_unlock();
    replay_mutex_unlock();
#if defined(XBOX) || defined(CONFIG_UWP)
    qemu_mutex_unlock_main_loop();
#endif

    g_mutex_lock(&host_state_lock);
    host_exit_status = status;
    g_mutex_unlock(&host_state_lock);
    host_set_running(false);
    return NULL;
}

int qemu_host_init(int argc, char **argv)
{
    int i;

    if (argc <= 0 || !argv) {
        return -EINVAL;
    }
    for (i = 0; i < argc; i++) {
        if (!argv[i]) {
            return -EINVAL;
        }
    }
    g_mutex_lock(&host_state_lock);
    if (host_initializing || host_initialized) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    host_initializing = true;
    g_mutex_unlock(&host_state_lock);

    qemu_host_emit_log(QEMU_HOST_LOG_DEBUG,
                       "embedding: loading xemu configuration");
    for (i = 1; i + 1 < argc; i++) {
        if (argv[i] && !strcmp(argv[i], "-config_path")) {
            if (argv[i + 1]) {
                xemu_settings_set_path(argv[i + 1]);
            }
            argv[i] = NULL;
            argv[i + 1] = NULL;
            break;
        }
    }
    if (!xemu_settings_load()) {
        const char *message = xemu_settings_get_error_message();

        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           message ? message : "Failed to load xemu settings");
        g_mutex_lock(&host_state_lock);
        host_initializing = false;
        g_mutex_unlock(&host_state_lock);
        return -EINVAL;
    }
    qemu_host_emit_log(QEMU_HOST_LOG_DEBUG,
                       "embedding: xemu configuration loaded");
    host_apply_brokered_machine_files();
    qemu_host_emit_log(QEMU_HOST_LOG_DEBUG,
                       "embedding: preparing UWP OpenGL display");
    if (!xemu_prepare_embedded_display()) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "Failed to prepare UWP OpenGL display");
        g_mutex_lock(&host_state_lock);
        host_initializing = false;
        g_mutex_unlock(&host_state_lock);
        return -EIO;
    }
    qemu_host_emit_log(QEMU_HOST_LOG_DEBUG,
                       "embedding: UWP OpenGL display prepared");
    qemu_host_emit_log(QEMU_HOST_LOG_DEBUG,
                       "embedding: entering qemu_init");

    qemu_init(argc, argv);
    qemu_host_emit_log(QEMU_HOST_LOG_DEBUG,
                       "embedding: qemu_init completed");
    qemu_host_emit_log(QEMU_HOST_LOG_DEBUG,
                       runstate_is_running() ?
                       "embedding: virtual machine is running" :
                       "embedding: virtual machine is not running");
    xemu_start_embedded_display();
    bql_unlock();
    replay_mutex_unlock();
#if defined(XBOX) || defined(CONFIG_UWP)
    qemu_mutex_unlock_main_loop();
#endif

    g_mutex_lock(&host_state_lock);
    host_initializing = false;
    host_initialized = true;
    host_running = false;
    host_loop_exited = false;
    host_thread_started = false;
    host_step_active = false;
    host_cleaned = false;
    host_exit_status = 0;
    g_mutex_unlock(&host_state_lock);
    qemu_host_emit_log(QEMU_HOST_LOG_INFO, "xemu embedding initialized");
    return 0;
}

int qemu_host_start(void)
{
    g_mutex_lock(&host_state_lock);
    if (!host_initialized || host_cleaned || host_running ||
        host_thread_started || host_step_active || host_loop_exited) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    host_running = true;
    host_thread_started = true;
    g_mutex_unlock(&host_state_lock);

    qemu_thread_create(&host_loop_thread, "xemu_host_loop",
                       qemu_host_loop_thread, NULL, QEMU_THREAD_JOINABLE);
    return 0;
}

int qemu_host_render_frame(void)
{
    static bool first_frame = true;

    if (first_frame) {
        qemu_host_emit_log(QEMU_HOST_LOG_DEBUG,
                           "embedding: first render API call begin");
    }
    if (!host_is_ready()) {
        return -EINVAL;
    }
    if (first_frame) {
        qemu_host_emit_log(QEMU_HOST_LOG_DEBUG,
                           "embedding: first render host state ready");
    }
    xemu_render_embedded_frame();
    if (first_frame) {
        qemu_host_emit_log(QEMU_HOST_LOG_DEBUG,
                           "embedding: first render API call complete");
        first_frame = false;
    }
    return 0;
}

int qemu_host_main_loop_step(bool nonblocking, int *exit_status)
{
    int status = 0;
    bool exited;

    g_mutex_lock(&host_state_lock);
    if (!host_initialized || host_cleaned || host_thread_started) {
        g_mutex_unlock(&host_state_lock);
        return -EINVAL;
    }
    if (host_loop_exited) {
        if (exit_status) {
            *exit_status = host_exit_status;
        }
        g_mutex_unlock(&host_state_lock);
        return 1;
    }
    if (host_step_active) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    host_step_active = true;
    g_mutex_unlock(&host_state_lock);

#if defined(XBOX) || defined(CONFIG_UWP)
    qemu_mutex_lock_main_loop();
#endif
    replay_mutex_lock();
    bql_lock();
    exited = qemu_main_loop_step(nonblocking, &status);
    bql_unlock();
    replay_mutex_unlock();
#if defined(XBOX) || defined(CONFIG_UWP)
    qemu_mutex_unlock_main_loop();
#endif

    g_mutex_lock(&host_state_lock);
    host_step_active = false;
    if (exited) {
        host_exit_status = status;
        host_loop_exited = true;
    }
    g_mutex_unlock(&host_state_lock);
    if (exit_status) {
        *exit_status = exited ? status : 0;
    }
    return exited ? 1 : 0;
}

void qemu_host_wake_main_loop(void)
{
    qemu_notify_event();
}

int qemu_host_pause(void)
{
    int result;

    if (!host_is_ready()) {
        return -EINVAL;
    }
    replay_mutex_lock();
    bql_lock();
    if (runstate_check(RUN_STATE_PAUSED)) {
        result = 0;
    } else if (runstate_is_live(runstate_get())) {
        result = vm_stop(RUN_STATE_PAUSED);
    } else {
        result = -EBUSY;
    }
    bql_unlock();
    replay_mutex_unlock();
    return result;
}

int qemu_host_resume(void)
{
    int result;

    if (!host_is_ready()) {
        return -EINVAL;
    }
    replay_mutex_lock();
    bql_lock();
    if (runstate_check(RUN_STATE_RUNNING)) {
        result = 0;
    } else if (runstate_check(RUN_STATE_PAUSED)) {
        vm_start();
        result = runstate_check(RUN_STATE_RUNNING) ? 0 : -EIO;
    } else {
        result = -EBUSY;
    }
    bql_unlock();
    replay_mutex_unlock();
    return result;
}

int qemu_host_request_shutdown(void)
{
    if (!host_is_ready()) {
        return -EINVAL;
    }
    qemu_system_powerdown_request();
    return 0;
}

int qemu_host_request_stop(void)
{
    if (!host_is_ready()) {
        return -EINVAL;
    }
    shutdown_action = SHUTDOWN_ACTION_POWEROFF;
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
    qemu_notify_event();
    return 0;
}

int qemu_host_reset(void)
{
    if (!host_is_ready()) {
        return -EINVAL;
    }
    qemu_system_reset_request(SHUTDOWN_CAUSE_HOST_UI);
    qemu_notify_event();
    return 0;
}

int qemu_host_join(int *exit_status)
{
    g_mutex_lock(&host_state_lock);
    if (!host_thread_started) {
        g_mutex_unlock(&host_state_lock);
        return -EINVAL;
    }
    g_mutex_unlock(&host_state_lock);
    qemu_thread_join(&host_loop_thread);
    g_mutex_lock(&host_state_lock);
    host_thread_started = false;
    if (exit_status) {
        *exit_status = host_exit_status;
    }
    g_mutex_unlock(&host_state_lock);
    return 0;
}

int qemu_host_cleanup(void)
{
    QemuHostBrokeredStorageCallbacks brokered;
    g_autoptr(GArray) backend_handles =
        g_array_new(false, false, sizeof(int64_t));
    GPtrArray *mounts;
    GHashTableIter iter;
    gpointer value;
    void *brokered_opaque;
    guint i;
    int status;

    g_mutex_lock(&host_state_lock);
    if (!host_initialized || host_cleaned || host_running ||
        host_thread_started || host_step_active) {
        g_mutex_unlock(&host_state_lock);
        return -EINVAL;
    }
    status = host_exit_status;
    host_cleaned = true;
    g_mutex_unlock(&host_state_lock);
    xemu_stop_embedded_display();
    replay_mutex_lock();
    bql_lock();
    qemu_cleanup(status);
    bql_unlock();
    replay_mutex_unlock();
    xemu_shutdown_embedded_display();
    g_mutex_lock(&host_storage_lock);
    brokered = host_brokered_callbacks;
    brokered_opaque = host_brokered_opaque;
    if (host_brokered_handles) {
        g_hash_table_iter_init(&iter, host_brokered_handles);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            QemuHostBrokeredHandle *handle = value;

            g_array_append_val(backend_handles, handle->backend_handle);
        }
        g_clear_pointer(&host_brokered_handles, g_hash_table_unref);
    }
    mounts = host_brokered_mounts;
    host_brokered_mounts = NULL;
    g_mutex_unlock(&host_storage_lock);
    for (i = 0; brokered.close && i < backend_handles->len; i++) {
        brokered.close(brokered_opaque,
                       g_array_index(backend_handles, int64_t, i));
    }
    if (mounts) {
        g_ptr_array_unref(mounts);
    }
    g_mutex_lock(&host_storage_lock);
    memset(&host_brokered_callbacks, 0, sizeof(host_brokered_callbacks));
    host_brokered_opaque = NULL;
    memset(&host_storage_callbacks, 0, sizeof(host_storage_callbacks));
    host_storage_opaque = NULL;
    g_mutex_unlock(&host_storage_lock);
    qemu_host_register_log_callback(NULL, NULL);
    qemu_host_set_log_file(NULL);
    qemu_host_set_pipeline_cache_file(NULL);
    return 0;
}

uint32_t qemu_host_get_api_version(void)
{
    return QEMU_HOST_API_VERSION;
}

bool qemu_host_is_initialized(void)
{
    bool initialized;

    g_mutex_lock(&host_state_lock);
    initialized = host_initialized && !host_cleaned;
    g_mutex_unlock(&host_state_lock);
    return initialized;
}

bool qemu_host_is_running(void)
{
    bool running;

    g_mutex_lock(&host_state_lock);
    running = host_running || host_step_active;
    g_mutex_unlock(&host_state_lock);
    return running;
}

int qemu_host_get_exit_status(void)
{
    int status;

    g_mutex_lock(&host_state_lock);
    status = host_exit_status;
    g_mutex_unlock(&host_state_lock);
    return status;
}

int qemu_host_send_key_number(int key_number, bool down)
{
    if (!host_is_ready() || key_number < 0 ||
        !qemu_input_key_number_to_qcode(key_number)) {
        return -EINVAL;
    }
    qemu_input_event_send_key_number(NULL, key_number, down);
    qemu_notify_event();
    return 0;
}

int qemu_host_send_key_qcode(int qcode, bool down)
{
    if (!host_is_ready() || qcode <= Q_KEY_CODE_UNMAPPED ||
        qcode >= Q_KEY_CODE__MAX) {
        return -EINVAL;
    }
    qemu_input_event_send_key_qcode(NULL, (QKeyCode)qcode, down);
    qemu_notify_event();
    return 0;
}

int qemu_host_send_pointer_rel(int dx, int dy)
{
    if (!host_is_ready()) {
        return -EINVAL;
    }
    if (dx) {
        qemu_input_queue_rel(NULL, INPUT_AXIS_X, dx);
    }
    if (dy) {
        qemu_input_queue_rel(NULL, INPUT_AXIS_Y, dy);
    }
    qemu_input_event_sync();
    qemu_notify_event();
    return 0;
}

int qemu_host_send_pointer_abs(int x, int y, int width, int height)
{
    if (!host_is_ready() || !qemu_input_is_absolute(NULL) ||
        width <= 0 || height <= 0) {
        return -EINVAL;
    }
    x = MIN(MAX(x, 0), width);
    y = MIN(MAX(y, 0), height);
    qemu_input_queue_abs(NULL, INPUT_AXIS_X, x, 0, width);
    qemu_input_queue_abs(NULL, INPUT_AXIS_Y, y, 0, height);
    qemu_input_event_sync();
    qemu_notify_event();
    return 0;
}

int qemu_host_send_pointer_button(QemuHostPointerButton button, bool down)
{
    static const InputButton buttons[] = {
        INPUT_BUTTON_LEFT, INPUT_BUTTON_MIDDLE, INPUT_BUTTON_RIGHT,
        INPUT_BUTTON_WHEEL_UP, INPUT_BUTTON_WHEEL_DOWN,
        INPUT_BUTTON_SIDE, INPUT_BUTTON_EXTRA,
    };

    if (!host_is_ready() || button < 0 || button >= ARRAY_SIZE(buttons)) {
        return -EINVAL;
    }
    qemu_input_queue_btn(NULL, buttons[button], down);
    qemu_input_event_sync();
    qemu_notify_event();
    return 0;
}

bool qemu_host_pointer_is_absolute(void)
{
    return host_is_ready() && qemu_input_is_absolute(NULL);
}
