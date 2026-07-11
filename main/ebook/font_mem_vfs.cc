// font_mem_vfs.cc — 见头文件说明。

#include "font_mem_vfs.h"

#include <esp_vfs.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_log.h>

namespace font_mem_vfs {

namespace {

constexpr char TAG[] = "font_mem_vfs";
constexpr char kBase[] = "/fmem";
constexpr int kMaxFiles = 4;  // 同时在世字体 blob ≤2（现役+退休），留冗余
constexpr int kMaxFds = 8;    // 每 blob 一个 FT_Face → 一个持久 fd；留冗余

struct File {
    bool used = false;
    char name[32] = {0};  // "g1.ttf"
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct Fd {
    bool used = false;
    int file_idx = -1;
    off_t pos = 0;
};

File s_files[kMaxFiles];
Fd s_fds[kMaxFds];
SemaphoreHandle_t s_mtx = nullptr;
bool s_registered = false;

struct Guard {
    Guard() { xSemaphoreTake(s_mtx, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(s_mtx); }
};

int FindFileLocked(const char* name) {
    for (int i = 0; i < kMaxFiles; i++) {
        if (s_files[i].used && strcmp(s_files[i].name, name) == 0) return i;
    }
    return -1;
}

// ---- VFS 回调（ESP_VFS_FLAG_DEFAULT：无 context 指针）------------------------

int VfsOpen(const char* path, int flags, int /*mode*/) {
    if ((flags & O_ACCMODE) != O_RDONLY) {
        errno = EROFS;
        return -1;
    }
    while (*path == '/') path++;  // VFS 已剥 "/fmem" 前缀，余 "/g1.ttf" → 去前导斜杠
    Guard g;
    int fi = FindFileLocked(path);
    if (fi < 0) {
        errno = ENOENT;
        return -1;
    }
    for (int i = 0; i < kMaxFds; i++) {
        if (!s_fds[i].used) {
            s_fds[i].used = true;
            s_fds[i].file_idx = fi;
            s_fds[i].pos = 0;
            return i;
        }
    }
    errno = ENFILE;
    return -1;
}

ssize_t VfsRead(int fd, void* dst, size_t size) {
    Guard g;
    if (fd < 0 || fd >= kMaxFds || !s_fds[fd].used) {
        errno = EBADF;
        return -1;
    }
    const File& f = s_files[s_fds[fd].file_idx];
    off_t pos = s_fds[fd].pos;
    if (pos >= static_cast<off_t>(f.size)) return 0;  // EOF
    size_t avail = f.size - static_cast<size_t>(pos);
    size_t n = size < avail ? size : avail;
    memcpy(dst, f.data + pos, n);
    s_fds[fd].pos = pos + static_cast<off_t>(n);
    return static_cast<ssize_t>(n);
}

off_t VfsLseek(int fd, off_t offset, int whence) {
    Guard g;
    if (fd < 0 || fd >= kMaxFds || !s_fds[fd].used) {
        errno = EBADF;
        return -1;
    }
    const File& f = s_files[s_fds[fd].file_idx];
    off_t base;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = s_fds[fd].pos; break;
        case SEEK_END: base = static_cast<off_t>(f.size); break;
        default: errno = EINVAL; return -1;
    }
    off_t np = base + offset;
    if (np < 0) {
        errno = EINVAL;
        return -1;
    }
    s_fds[fd].pos = np;  // 允许 seek 超尾（下次 read 返回 EOF），符合 POSIX
    return np;
}

int VfsClose(int fd) {
    Guard g;
    if (fd < 0 || fd >= kMaxFds || !s_fds[fd].used) {
        errno = EBADF;
        return -1;
    }
    s_fds[fd].used = false;
    s_fds[fd].file_idx = -1;
    s_fds[fd].pos = 0;
    return 0;
}

int VfsFstat(int fd, struct stat* st) {
    Guard g;
    if (fd < 0 || fd >= kMaxFds || !s_fds[fd].used) {
        errno = EBADF;
        return -1;
    }
    const File& f = s_files[s_fds[fd].file_idx];
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_size = static_cast<off_t>(f.size);
    st->st_blksize = 4096;  // 让 newlib 用 4KB 全缓冲，减少 read 回调次数
    return 0;
}

}  // namespace

bool Register() {
    if (s_registered) return true;
    if (s_mtx == nullptr) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == nullptr) return false;
    }
    esp_vfs_t vfs = {};
    vfs.flags = ESP_VFS_FLAG_DEFAULT;
    vfs.open = &VfsOpen;
    vfs.read = &VfsRead;
    vfs.lseek = &VfsLseek;
    vfs.close = &VfsClose;
    vfs.fstat = &VfsFstat;
    esp_err_t err = esp_vfs_register(kBase, &vfs, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_register(%s) failed: %s", kBase, esp_err_to_name(err));
        return false;
    }
    s_registered = true;
    ESP_LOGI(TAG, "registered %s", kBase);
    return true;
}

bool Add(const char* name, const uint8_t* data, size_t size) {
    if (!s_registered || name == nullptr || data == nullptr) return false;
    if (strlen(name) >= sizeof(s_files[0].name)) return false;
    Guard g;
    if (FindFileLocked(name) >= 0) return false;  // 重名
    for (int i = 0; i < kMaxFiles; i++) {
        if (!s_files[i].used) {
            s_files[i].used = true;
            strlcpy(s_files[i].name, name, sizeof(s_files[i].name));
            s_files[i].data = data;
            s_files[i].size = size;
            return true;
        }
    }
    return false;  // 表满
}

void Remove(const char* name) {
    if (name == nullptr) return;
    Guard g;
    int fi = FindFileLocked(name);
    if (fi < 0) return;
    for (int i = 0; i < kMaxFds; i++) {  // 防御：正常不该有残留 fd（fence 时序保证）
        if (s_fds[i].used && s_fds[i].file_idx == fi) {
            ESP_LOGW(TAG, "Remove(%s) with open fd %d still referencing it", name, i);
            s_fds[i].used = false;
            s_fds[i].file_idx = -1;
        }
    }
    s_files[fi] = File{};
}

void PathFor(const char* name, char* out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", kBase, name);
}

}  // namespace font_mem_vfs
