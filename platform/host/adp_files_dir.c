// SPDX-License-Identifier: Apache-2.0
//
// fsync(), opendir() and friends are POSIX, not C11: see host_time.c for why
// the feature-test macro comes before the first header.
#define _POSIX_C_SOURCE 200809L

#include "adp_files_dir.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_MODE 0755
#define FILE_MODE 0644

// The root, a '/', and a port path, terminated.
#define FULL_PATH_MAX (ADP_FILES_DIR_ROOT_MAX + PORT_FILES_PATH_MAX + 1u)

static adp_files_dir_t *self_of(port_files_t *port)
{
    return (adp_files_dir_t *)port->impl;
}

static bool full_path(const adp_files_dir_t *d, const char *rel, char *out)
{
    const size_t root_len = strlen(d->root);
    const size_t rel_len = strlen(rel);
    if (root_len + 1u + rel_len + 1u > FULL_PATH_MAX) {
        return false;
    }
    memcpy(out, d->root, root_len);
    out[root_len] = '/';
    memcpy(out + root_len + 1u, rel, rel_len + 1u);
    return true;
}

static hal_status_t from_errno(int err)
{
    switch (err) {
        case ENOSPC:
        case EDQUOT:
            return HAL_ERR_NO_MEM;
        case ENOENT:
            return HAL_ERR_NOT_FOUND;
        default:
            return HAL_ERR_IO;
    }
}

static bool known_open(const adp_files_dir_t *d, const port_file_t *file)
{
    if (file == NULL) {
        return false;
    }
    for (size_t i = 0u; i < ADP_FILES_DIR_MAX_OPEN; ++i) {
        if (&d->files[i] == file) {
            return file->used;
        }
    }
    return false;
}

static port_file_t *open_by_path(adp_files_dir_t *d, const char *path)
{
    for (size_t i = 0u; i < ADP_FILES_DIR_MAX_OPEN; ++i) {
        if (d->files[i].used && strcmp(d->files[i].path, path) == 0) {
            return &d->files[i];
        }
    }
    return NULL;
}

static hal_status_t files_open_append(port_files_t *port, const char *path, port_file_t **out)
{
    adp_files_dir_t *d = self_of(port);
    if (out == NULL || !port_files_path_ok(path)) {
        return HAL_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!d->present) {
        return HAL_ERR_NOT_READY;
    }
    if (open_by_path(d, path) != NULL) {
        return HAL_ERR_BUSY;
    }
    port_file_t *slot = NULL;
    for (size_t i = 0u; i < ADP_FILES_DIR_MAX_OPEN && slot == NULL; ++i) {
        if (!d->files[i].used) {
            slot = &d->files[i];
        }
    }
    if (slot == NULL) {
        return HAL_ERR_BUSY;
    }

    char full[FULL_PATH_MAX];
    if (!full_path(d, path, full)) {
        return HAL_ERR_INVALID_ARG;
    }
    const char *slash = strchr(path, '/');
    if (slash != NULL) {
        char dir[FULL_PATH_MAX];
        const size_t dir_len = strlen(d->root) + 1u + (size_t)(slash - path);
        memcpy(dir, full, dir_len);
        dir[dir_len] = '\0';
        if (mkdir(dir, DIR_MODE) != 0 && errno != EEXIST) {
            return from_errno(errno);
        }
    }

    const int fd = open(full, O_WRONLY | O_CREAT | O_APPEND, FILE_MODE);
    if (fd < 0) {
        return from_errno(errno);
    }
    slot->used = true;
    slot->fd = fd;
    memcpy(slot->path, path, strlen(path) + 1u);
    *out = slot;
    return HAL_OK;
}

static hal_status_t files_append(port_files_t *port, port_file_t *file, const void *data,
                                 size_t len)
{
    adp_files_dir_t *d = self_of(port);
    if (!known_open(d, file) || (data == NULL && len > 0u)) {
        return HAL_ERR_INVALID_ARG;
    }
    if (!d->present || file->fd < 0) {
        return HAL_ERR_NOT_READY;
    }
    if (d->limit_bytes != 0u && d->appended_bytes + len > d->limit_bytes) {
        return HAL_ERR_NO_MEM;
    }
    const unsigned char *p = (const unsigned char *)data;
    size_t left = len;
    while (left > 0u) {
        const ssize_t n = write(file->fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return from_errno(errno);
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    d->appended_bytes += len;
    return HAL_OK;
}

static hal_status_t files_flush(port_files_t *port, port_file_t *file)
{
    adp_files_dir_t *d = self_of(port);
    if (!known_open(d, file)) {
        return HAL_ERR_INVALID_ARG;
    }
    if (!d->present || file->fd < 0) {
        return HAL_ERR_NOT_READY;
    }
    return (fsync(file->fd) == 0) ? HAL_OK : from_errno(errno);
}

static hal_status_t files_close(port_files_t *port, port_file_t *file)
{
    adp_files_dir_t *d = self_of(port);
    if (!known_open(d, file)) {
        return HAL_ERR_INVALID_ARG;
    }
    hal_status_t status = HAL_OK;
    if (!d->present || file->fd < 0) {
        status = HAL_ERR_NOT_READY;
    } else if (fsync(file->fd) != 0) {
        status = from_errno(errno);
    }
    if (file->fd >= 0 && close(file->fd) != 0 && status == HAL_OK) {
        status = from_errno(errno);
    }
    memset(file, 0, sizeof(*file));
    file->fd = -1;
    return status;
}

static hal_status_t files_list(port_files_t *port, const char *dir, port_files_visit_fn visit,
                               void *ctx)
{
    adp_files_dir_t *d = self_of(port);
    if (!port_files_dir_ok(dir) || visit == NULL) {
        return HAL_ERR_INVALID_ARG;
    }
    if (!d->present) {
        return HAL_ERR_NOT_READY;
    }
    char full[FULL_PATH_MAX];
    if (!full_path(d, dir, full)) {
        return HAL_ERR_INVALID_ARG;
    }
    DIR *handle = opendir(full);
    if (handle == NULL) {
        return (errno == ENOENT) ? HAL_OK : from_errno(errno);
    }
    const size_t dir_len = strlen(full);
    for (struct dirent *e = readdir(handle); e != NULL; e = readdir(handle)) {
        const size_t name_len = strlen(e->d_name);
        if (dir_len + 1u + name_len + 1u > FULL_PATH_MAX) {
            continue;
        }
        char entry[FULL_PATH_MAX];
        memcpy(entry, full, dir_len);
        entry[dir_len] = '/';
        memcpy(entry + dir_len + 1u, e->d_name, name_len + 1u);
        struct stat st;
        if (stat(entry, &st) == 0 && S_ISREG(st.st_mode)) {
            visit(ctx, e->d_name, (uint64_t)st.st_size);
        }
    }
    closedir(handle);
    return HAL_OK;
}

static hal_status_t files_remove(port_files_t *port, const char *path)
{
    adp_files_dir_t *d = self_of(port);
    if (!port_files_path_ok(path)) {
        return HAL_ERR_INVALID_ARG;
    }
    if (!d->present) {
        return HAL_ERR_NOT_READY;
    }
    if (open_by_path(d, path) != NULL) {
        return HAL_ERR_BUSY;
    }
    char full[FULL_PATH_MAX];
    if (!full_path(d, path, full)) {
        return HAL_ERR_INVALID_ARG;
    }
    return (unlink(full) == 0) ? HAL_OK : from_errno(errno);
}

hal_status_t adp_files_dir_init(adp_files_dir_t *d, const char *root)
{
    memset(d, 0, sizeof(*d));
    for (size_t i = 0u; i < ADP_FILES_DIR_MAX_OPEN; ++i) {
        d->files[i].fd = -1;
    }
    if (root == NULL || strlen(root) + 1u > sizeof(d->root) || root[0] == '\0') {
        return HAL_ERR_INVALID_ARG;
    }
    memcpy(d->root, root, strlen(root) + 1u);
    if (mkdir(d->root, DIR_MODE) != 0 && errno != EEXIST) {
        return HAL_ERR_IO;
    }
    d->present = true;

    d->port.open_append = files_open_append;
    d->port.append = files_append;
    d->port.flush = files_flush;
    d->port.close = files_close;
    d->port.list = files_list;
    d->port.remove = files_remove;
    d->port.impl = d;
    return HAL_OK;
}

port_files_t *adp_files_dir_port(adp_files_dir_t *d)
{
    return &d->port;
}

void adp_files_dir_deinit(adp_files_dir_t *d)
{
    for (size_t i = 0u; i < ADP_FILES_DIR_MAX_OPEN; ++i) {
        if (d->files[i].used) {
            (void)files_close(&d->port, &d->files[i]);
        }
    }
}

void adp_files_dir_set_present(adp_files_dir_t *d, bool present)
{
    if (!present) {
        // What a card pulled out does to its open files: they are gone, and
        // only close() frees their handles.
        for (size_t i = 0u; i < ADP_FILES_DIR_MAX_OPEN; ++i) {
            if (d->files[i].used && d->files[i].fd >= 0) {
                (void)close(d->files[i].fd);
                d->files[i].fd = -1;
            }
        }
    }
    d->present = present;
}

void adp_files_dir_set_limit(adp_files_dir_t *d, uint64_t limit_bytes)
{
    d->limit_bytes = limit_bytes;
    d->appended_bytes = 0u;
}
