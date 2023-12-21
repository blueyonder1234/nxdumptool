/*
 * hfs_dev.c
 *
 * Loosely based on fs_dev.c from libnx, et al.
 *
 * Copyright (c) 2020-2023, DarkMatterCore <pabloacurielz@gmail.com>.
 *
 * This file is part of nxdumptool (https://github.com/DarkMatterCore/nxdumptool).
 *
 * nxdumptool is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nxdumptool is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "nxdt_utils.h"
#include "nxdt_devoptab.h"
#include "ro_dev.h"

/* Helper macros. */

#define HFS_DEV_INIT_VARS       DEVOPTAB_INIT_VARS(HashFileSystemContext)
#define HFS_DEV_INIT_FILE_VARS  DEVOPTAB_INIT_FILE_VARS(HashFileSystemContext, HashFileSystemFileState)
#define HFS_DEV_INIT_DIR_VARS   DEVOPTAB_INIT_DIR_VARS(HashFileSystemContext, HashFileSystemDirectoryState)
#define HFS_DEV_INIT_FS_ACCESS  DEVOPTAB_DECL_FS_CTX(HashFileSystemContext)

/* Type definitions. */

typedef struct {
    u32 index;                      ///< Hash FS entry index.
    HashFileSystemEntry *hfs_entry; ///< Hash FS entry metadata.
    const char *name;               ///< Entry name.
    u64 offset;                     ///< Current offset within Hash FS entry data.
} HashFileSystemFileState;

typedef struct {
    u8 state;   ///< 0: "." entry; 1: ".." entry; 2: actual Hash FS entry.
    u32 index;  ///< Current Hash FS entry index.
} HashFileSystemDirectoryState;

/* Function prototypes. */

static int       hfsdev_open(struct _reent *r, void *fd, const char *path, int flags, int mode);
static int       hfsdev_close(struct _reent *r, void *fd);
static ssize_t   hfsdev_read(struct _reent *r, void *fd, char *ptr, size_t len);
static off_t     hfsdev_seek(struct _reent *r, void *fd, off_t pos, int dir);
static int       hfsdev_fstat(struct _reent *r, void *fd, struct stat *st);
static int       hfsdev_stat(struct _reent *r, const char *file, struct stat *st);
static DIR_ITER* hfsdev_diropen(struct _reent *r, DIR_ITER *dirState, const char *path);
static int       hfsdev_dirreset(struct _reent *r, DIR_ITER *dirState);
static int       hfsdev_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat);
static int       hfsdev_dirclose(struct _reent *r, DIR_ITER *dirState);
static int       hfsdev_statvfs(struct _reent *r, const char *path, struct statvfs *buf);

static const char *hfsdev_get_truncated_path(struct _reent *r, const char *path);

static void hfsdev_fill_stat(struct stat *st, u32 index, const HashFileSystemEntry *hfs_entry, time_t mount_time);

/* Global variables. */

static const devoptab_t hfsdev_devoptab = {
    .name         = NULL,
    .structSize   = sizeof(HashFileSystemFileState),
    .open_r       = hfsdev_open,
    .close_r      = hfsdev_close,
    .write_r      = rodev_write,                                ///< Not supported by Hash FS sections.
    .read_r       = hfsdev_read,
    .seek_r       = hfsdev_seek,
    .fstat_r      = hfsdev_fstat,
    .stat_r       = hfsdev_stat,
    .link_r       = rodev_link,                                 ///< Not supported by Hash FS sections.
    .unlink_r     = rodev_unlink,                               ///< Not supported by Hash FS sections.
    .chdir_r      = rodev_chdir,                                ///< No need to deal with cwd shenanigans, so we won't support it.
    .rename_r     = rodev_rename,                               ///< Not supported by Hash FS sections.
    .mkdir_r      = rodev_mkdir,                                ///< Not supported by Hash FS sections.
    .dirStateSize = sizeof(HashFileSystemDirectoryState),
    .diropen_r    = hfsdev_diropen,                             ///< Hash FS sections don't support directories, but we'll allow operations on the FS root.
    .dirreset_r   = hfsdev_dirreset,                            ///< Hash FS sections don't support directories, but we'll allow operations on the FS root.
    .dirnext_r    = hfsdev_dirnext,                             ///< Hash FS sections don't support directories, but we'll allow operations on the FS root.
    .dirclose_r   = hfsdev_dirclose,                            ///< Hash FS sections don't support directories, but we'll allow operations on the FS root.
    .statvfs_r    = hfsdev_statvfs,
    .ftruncate_r  = rodev_ftruncate,                            ///< Not supported by Hash FS sections.
    .fsync_r      = rodev_fsync,                                ///< Not supported by Hash FS sections.
    .deviceData   = NULL,
    .chmod_r      = rodev_chmod,                                ///< Not supported by Hash FS sections.
    .fchmod_r     = rodev_fchmod,                               ///< Not supported by Hash FS sections.
    .rmdir_r      = rodev_rmdir,                                ///< Not supported by Hash FS sections.
    .lstat_r      = hfsdev_stat,                                ///< Symlinks aren't supported, so we'll just alias lstat() to stat().
    .utimes_r     = rodev_utimes,                               ///< Not supported by Hash FS sections.
    .fpathconf_r  = rodev_fpathconf,                            ///< Not supported by Hash FS sections.
    .pathconf_r   = rodev_pathconf,                             ///< Not supported by Hash FS sections.
    .symlink_r    = rodev_symlink,                              ///< Not supported by Hash FS sections.
    .readlink_r   = rodev_readlink                              ///< Not supported by Hash FS sections.
};

const devoptab_t *hfsdev_get_devoptab()
{
    return &hfsdev_devoptab;
}

static int hfsdev_open(struct _reent *r, void *fd, const char *path, int flags, int mode)
{
    NX_IGNORE_ARG(mode);

    HFS_DEV_INIT_FILE_VARS;
    HFS_DEV_INIT_FS_ACCESS;

    /* Validate input. */
    if (!file || (flags & (O_WRONLY | O_RDWR | O_APPEND | O_CREAT | O_TRUNC | O_EXCL))) DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);

    /* Get truncated path. */
    if (!(path = hfsdev_get_truncated_path(r, path))) DEVOPTAB_EXIT;

    LOG_MSG_DEBUG("Opening \"%s:/%s\" with flags 0x%X.", dev_ctx->name, path, flags);

    /* Reset file descriptor. */
    memset(file, 0, sizeof(HashFileSystemFileState));

    /* Get information about the requested Partition FS entry. */
    if (!hfsGetEntryIndexByName(fs_ctx, path, &(file->index)) || !(file->hfs_entry = hfsGetEntryByIndex(fs_ctx, file->index)) || \
        !(file->name = hfsGetEntryNameByIndex(fs_ctx, file->index))) DEVOPTAB_SET_ERROR(ENOENT);

end:
    DEVOPTAB_DEINIT_VARS;
    DEVOPTAB_RETURN_INT(0);
}

static int hfsdev_close(struct _reent *r, void *fd)
{
    HFS_DEV_INIT_FILE_VARS;

    /* Sanity check. */
    if (!file) DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);

    LOG_MSG_DEBUG("Closing \"%s:/%s\".", dev_ctx->name, file->name);

    /* Reset file descriptor. */
    memset(file, 0, sizeof(HashFileSystemFileState));

end:
    DEVOPTAB_DEINIT_VARS;
    DEVOPTAB_RETURN_INT(0);
}

static ssize_t hfsdev_read(struct _reent *r, void *fd, char *ptr, size_t len)
{
    HFS_DEV_INIT_FILE_VARS;
    HFS_DEV_INIT_FS_ACCESS;

    /* Sanity check. */
    if (!file || !ptr || !len) DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);

    LOG_MSG_DEBUG("Reading 0x%lX byte(s) at offset 0x%lX from \"%s:/%s\".", len, file->offset, dev_ctx->name, file->name);

    /* Read file data. */
    if (!hfsReadEntryData(fs_ctx, file->hfs_entry, ptr, len, file->offset)) DEVOPTAB_SET_ERROR_AND_EXIT(EIO);

    /* Adjust offset. */
    file->offset += len;

end:
    DEVOPTAB_DEINIT_VARS;
    DEVOPTAB_RETURN_INT((ssize_t)len);
}

static off_t hfsdev_seek(struct _reent *r, void *fd, off_t pos, int dir)
{
    off_t offset = 0;

    HFS_DEV_INIT_FILE_VARS;

    /* Sanity check. */
    if (!file) DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);

    /* Find the offset to seek from. */
    switch(dir)
    {
        case SEEK_SET:  /* Set absolute position relative to zero (start offset). */
            break;
        case SEEK_CUR:  /* Set position relative to the current position. */
            offset = (off_t)file->offset;
            break;
        case SEEK_END:  /* Set position relative to EOF. */
            offset = (off_t)file->hfs_entry->size;
            break;
        default:        /* Invalid option. */
            DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);
    }

    /* Don't allow negative seeks beyond the beginning of file. */
    if (pos < 0 && offset < -pos) DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);

    /* Calculate actual offset. */
    offset += pos;

    /* Don't allow positive seeks beyond the end of file. */
    if (offset > (off_t)file->hfs_entry->size) DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);

    LOG_MSG_DEBUG("Seeking to offset 0x%lX from \"%s:/%s\".", offset, dev_ctx->name, file->name);

    /* Adjust offset. */
    file->offset = (u64)offset;

end:
    DEVOPTAB_DEINIT_VARS;
    DEVOPTAB_RETURN_INT(offset);
}

static int hfsdev_fstat(struct _reent *r, void *fd, struct stat *st)
{
    HFS_DEV_INIT_FILE_VARS;

    /* Sanity check. */
    if (!file || !st) DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);

    LOG_MSG_DEBUG("Getting file stats for \"%s:/%s\".", dev_ctx->name, file->name);

    /* Fill stat info. */
    hfsdev_fill_stat(st, file->index, file->hfs_entry, dev_ctx->mount_time);

end:
    DEVOPTAB_DEINIT_VARS;
    DEVOPTAB_RETURN_INT(0);
}

static int hfsdev_stat(struct _reent *r, const char *file, struct stat *st)
{
    u32 index = 0;
    HashFileSystemEntry *hfs_entry = NULL;

    HFS_DEV_INIT_VARS;
    HFS_DEV_INIT_FS_ACCESS;

    /* Sanity check. */
    if (!st) DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);

    /* Get truncated path. */
    if (!(file = hfsdev_get_truncated_path(r, file))) DEVOPTAB_EXIT;

    LOG_MSG_DEBUG("Getting file stats for \"%s:/%s\".", dev_ctx->name, file);

    /* Get information about the requested Partition FS entry. */
    if (!hfsGetEntryIndexByName(fs_ctx, file, &index) || !(hfs_entry = hfsGetEntryByIndex(fs_ctx, index))) DEVOPTAB_SET_ERROR(ENOENT);

    /* Fill stat info. */
    hfsdev_fill_stat(st, index, hfs_entry, dev_ctx->mount_time);

end:
    DEVOPTAB_DEINIT_VARS;
    DEVOPTAB_RETURN_INT(0);
}

static DIR_ITER *hfsdev_diropen(struct _reent *r, DIR_ITER *dirState, const char *path)
{
    DIR_ITER *ret = NULL;

    HFS_DEV_INIT_DIR_VARS;

    /* Get truncated path. */
    /* We can only work with the FS root here, so we won't accept anything else. */
    if (!(path = hfsdev_get_truncated_path(r, path))) DEVOPTAB_EXIT;
    if (*path) DEVOPTAB_SET_ERROR_AND_EXIT(ENOENT);

    LOG_MSG_DEBUG("Opening directory \"%s:/\".", dev_ctx->name);

    /* Reset directory state. */
    memset(dir, 0, sizeof(HashFileSystemDirectoryState));

    /* Update return value. */
    ret = dirState;

end:
    DEVOPTAB_DEINIT_VARS;
    DEVOPTAB_RETURN_PTR(ret);
}

static int hfsdev_dirreset(struct _reent *r, DIR_ITER *dirState)
{
    HFS_DEV_INIT_DIR_VARS;

    LOG_MSG_DEBUG("Resetting directory state for \"%s:/\".", dev_ctx->name);

    /* Reset directory state. */
    dir->state = 0;
    dir->index = 0;

end:
    DEVOPTAB_DEINIT_VARS;
    DEVOPTAB_RETURN_INT(0);
}

static int hfsdev_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat)
{
    HashFileSystemEntry *hfs_entry = NULL;
    const char *fname = NULL;

    HFS_DEV_INIT_DIR_VARS;
    HFS_DEV_INIT_FS_ACCESS;

    /* Sanity check. */
    if (!filename || !filestat) DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);

    LOG_MSG_DEBUG("Getting info for next directory entry in \"%s:/\" (state %u, index %u).", dev_ctx->name, dir->state, dir->index);

    if (dir->state < 2)
    {
        /* Fill bogus directory entry. */
        memset(filestat, 0, sizeof(struct stat));

        filestat->st_nlink = 1;
        filestat->st_mode = (S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH);
        filestat->st_atime = filestat->st_mtime = filestat->st_ctime = dev_ctx->mount_time;

        strcpy(filename, dir->state == 0 ? "." : "..");

        /* Update state. */
        dir->state++;

        DEVOPTAB_EXIT;
    }

    /* Check if we haven't reached EOD. */
    if (dir->index >= hfsGetEntryCount(fs_ctx)) DEVOPTAB_SET_ERROR_AND_EXIT(ENOENT);

    /* Get Hash FS entry. */
    if (!(hfs_entry = hfsGetEntryByIndex(fs_ctx, dir->index)) || !(fname = hfsGetEntryName(fs_ctx, hfs_entry))) DEVOPTAB_SET_ERROR_AND_EXIT(EIO);

    /* Copy filename. */
    strcpy(filename, fname);

    /* Fill stat info. */
    hfsdev_fill_stat(filestat, dir->index, hfs_entry, dev_ctx->mount_time);

    /* Adjust index. */
    dir->index++;

end:
    DEVOPTAB_DEINIT_VARS;
    DEVOPTAB_RETURN_INT(0);
}

static int hfsdev_dirclose(struct _reent *r, DIR_ITER *dirState)
{
    HFS_DEV_INIT_DIR_VARS;

    LOG_MSG_DEBUG("Closing directory \"%s:/\".", dev_ctx->name);

    /* Reset directory state. */
    memset(dir, 0, sizeof(HashFileSystemDirectoryState));

end:
    DEVOPTAB_DEINIT_VARS;
    DEVOPTAB_RETURN_INT(0);
}

static int hfsdev_statvfs(struct _reent *r, const char *path, struct statvfs *buf)
{
    NX_IGNORE_ARG(path);

    u64 ext_fs_size = 0;

    HFS_DEV_INIT_VARS;
    HFS_DEV_INIT_FS_ACCESS;

    /* Sanity check. */
    if (!buf) DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);

    LOG_MSG_DEBUG("Getting filesystem stats for \"%s:\"", dev_ctx->name);

    /* Get Hash FS total data size. */
    if (!hfsGetTotalDataSize(fs_ctx, &ext_fs_size)) DEVOPTAB_SET_ERROR_AND_EXIT(EIO);

    /* Fill filesystem stats. */
    memset(buf, 0, sizeof(struct statvfs));

    buf->f_bsize = 1;
    buf->f_frsize = 1;
    buf->f_blocks = ext_fs_size;
    buf->f_bfree = 0;
    buf->f_bavail = 0;
    buf->f_files = 0;
    buf->f_ffree = 0;
    buf->f_favail = 0;
    buf->f_fsid = 0;
    buf->f_flag = ST_NOSUID;
    buf->f_namemax = FS_MAX_PATH;

end:
    DEVOPTAB_DEINIT_VARS;
    DEVOPTAB_RETURN_INT(0);
}

static const char *hfsdev_get_truncated_path(struct _reent *r, const char *path)
{
    const u8 *p = (const u8*)path;
    ssize_t units = 0;
    u32 code = 0;
    size_t len = 0;
    bool path_sep_skipped = false;

    DEVOPTAB_DECL_ERROR_STATE;

    if (!r || !path || !*path) DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);

    LOG_MSG_DEBUG("Input path: \"%s\".", path);

    /* Move the path pointer to the start of the actual path. */
    do {
        units = decode_utf8(&code, p);
        if (units < 0) DEVOPTAB_SET_ERROR_AND_EXIT(EILSEQ);
        p += units;
    } while(code >= ' ' && code != ':');

    /* We found a colon; p points to the actual path. */
    if (code == ':') path = (const char*)p;

    /* Skip the leading slash, if available. */
    if (path[0] == '/')
    {
        path++;
        path_sep_skipped = true;
    }

    /* Make sure there are no more colons or slashes and that the remainder of the string is valid UTF-8. */
    p = (const u8*)path;

    do {
        units = decode_utf8(&code, p);
        if (units < 0) DEVOPTAB_SET_ERROR_AND_EXIT(EILSEQ);
        if (code == ':' || code == '/') DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);
        p += units;
    } while(code >= ' ');

    /* Verify fixed path length. */
    len = strlen(path);
    if (!len && !path_sep_skipped) DEVOPTAB_SET_ERROR_AND_EXIT(EINVAL);
    if (len >= FS_MAX_PATH) DEVOPTAB_SET_ERROR_AND_EXIT(ENAMETOOLONG);

    LOG_MSG_DEBUG("Truncated path: \"%s\".", path);

end:
    DEVOPTAB_RETURN_PTR(path);
}

static void hfsdev_fill_stat(struct stat *st, u32 index, const HashFileSystemEntry *hfs_entry, time_t mount_time)
{
    /* Clear stat struct. */
    memset(st, 0, sizeof(struct stat));

    /* Fill stat struct. */
    /* We're always dealing with a file entry. */
    st->st_ino = index;
    st->st_mode = (S_IFREG | S_IRUSR | S_IRGRP | S_IROTH);
    st->st_nlink = 1;
    st->st_size = (off_t)hfs_entry->size;
    st->st_atime = st->st_mtime = st->st_ctime = mount_time;
}
