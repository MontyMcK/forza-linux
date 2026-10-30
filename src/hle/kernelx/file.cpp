// kernelx: files, directories, find, I/O completion ports.
//
// Overlapped I/O completes synchronously inside ReadFile/WriteFile, then
// signals the OVERLAPPED event and posts to the IOCP, and returns
// ERROR_IO_PENDING. Correct, but blocks the caller: make it async if loading
// becomes a bottleneck.

#include <vector>
#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "common/config.h"
#include "common/log.h"
#include "hle/d3d11x/gpumem.h"
#include "common/wstr.h"
#include "hle/kernelx/kobject.h"
#include "hle/kernelx/sync_objects.h"
#include "hle/kernelx/vfs.h"
#include "hle/registry.h"
#include "platform/os/write_watch.h"

namespace fl::kx {
namespace {

constexpr DWORD GENERIC_READ = 0x80000000;
constexpr DWORD GENERIC_WRITE = 0x40000000;
constexpr DWORD CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4, TRUNCATE_EXISTING = 5;
constexpr DWORD FILE_FLAG_OVERLAPPED = 0x40000000;
constexpr DWORD FILE_ATTRIBUTE_READONLY = 0x1;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr DWORD FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr DWORD STATUS_SUCCESS = 0;
constexpr DWORD STATUS_END_OF_FILE = 0xC0000011;
constexpr uint64_t kEpochDelta = 116444736000000000ull;

struct Overlapped {
    ULONG_PTR Internal;      // NTSTATUS
    ULONG_PTR InternalHigh;  // bytes transferred
    DWORD Offset;
    DWORD OffsetHigh;
    HANDLE hEvent;
};
static_assert(sizeof(Overlapped) == 32);

struct Packet {
    DWORD bytes;
    ULONG_PTR key;
    Overlapped* ov;
};

struct Iocp : KObject {
    Iocp() : KObject(ObjType::Iocp) {}
    std::mutex m;
    std::condition_variable cv;
    std::deque<Packet> queue;

    void post(const Packet& p) {
        {
            std::lock_guard lock(m);
            queue.push_back(p);
        }
        cv.notify_one();
    }
};

struct File : KObject {
    File() : KObject(ObjType::File) {}
    ~File() override {
        if (fd >= 0) close(fd);
    }
    int fd = -1;
    bool overlapped = false;
    std::string path;
    std::shared_ptr<Iocp> port;
    ULONG_PTR key = 0;
};

DWORD errno_to_win(int e) {
    switch (e) {
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
        case EEXIST: return ERROR_FILE_EXISTS;
        case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
        default: return ERROR_INVALID_PARAMETER;
    }
}

void put_ft(FILETIME* f, const timespec& ts) {
    uint64_t v = static_cast<uint64_t>(ts.tv_sec) * 10000000ull + ts.tv_nsec / 100 + kEpochDelta;
    *f = {static_cast<DWORD>(v), static_cast<DWORD>(v >> 32)};
}

DWORD attrs_of(const struct stat& st) {
    DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a;
}

// ---------------------------------------------------------------- open/close

#ifdef FL_DEV_SKIP_MOVIES
// Developer builds only (CMake -DFL_DEV_SKIP_MOVIES=ON, off by default):
// FL_SKIP_MOVIES=name[,name...]: opening a .bk2 movie whose
// path contains one of the names (case-insensitive) fails with "file not
// found", so the game skips it (e.g. the long Anthem_Tone_Video after the
// title). "all" skips every movie.
bool skip_movie(const std::string& win) {
    static const char* env = getenv("FL_SKIP_MOVIES");
    if (!env || !*env) return false;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::string path = lower(win);
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".bk2") != 0) return false;
    std::string list = lower(env);
    for (size_t pos = 0; pos <= list.size();) {
        size_t comma = list.find(',', pos);
        std::string name = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (name == "all" || (!name.empty() && path.find(name) != std::string::npos)) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}
#endif

HANDLE open_file(const std::string& win, DWORD access, DWORD disposition, DWORD flags) {
#ifdef FL_DEV_SKIP_MOVIES
    if (skip_movie(win)) {
        ::fl::log_write("file", "skipping movie '%s' (FL_SKIP_MOVIES, developer build)", win.c_str());
        set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
#endif
    bool write = access & (GENERIC_WRITE | 0x2 /* FILE_WRITE_DATA */ | 0x4 /* FILE_APPEND_DATA */);
    std::string host = vfs::to_host(win, disposition != OPEN_EXISTING && disposition != TRUNCATE_EXISTING);
    if (write && host.rfind(config().game_dir, 0) == 0)
        FL_ERROR("game opens a package file for WRITING: %s (needs an overlay)", host.c_str());

    int oflags = write ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
        case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS: oflags |= O_CREAT; break;
        case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
        default: break;
    }
    if (oflags & O_CREAT) std::filesystem::create_directories(std::filesystem::path(host).parent_path());

    struct stat st;
    bool existed = stat(host.c_str(), &st) == 0;
    int fd = existed && S_ISDIR(st.st_mode) ? open(host.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)
                                            : open(host.c_str(), oflags | O_CLOEXEC, 0644);
    if (fd < 0) {
        int e = errno;
        FL_LOG(file, "open '%s' -> %s FAILED (%s)", win.c_str(), host.c_str(), strerror(e));
        set_last_error(errno_to_win(e));
        return INVALID_HANDLE_VALUE;
    }
    auto f = std::make_shared<File>();
    f->fd = fd;
    f->overlapped = flags & FILE_FLAG_OVERLAPPED;
    f->path = host;
    FL_LOG(file, "open '%s' -> %s fd %d%s%s", win.c_str(), host.c_str(), fd, write ? " (write)" : "",
           f->overlapped ? " (overlapped)" : "");
    // Windows sets ERROR_ALREADY_EXISTS when CREATE_ALWAYS/OPEN_ALWAYS hit an existing file.
    set_last_error(existed && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) ? ERROR_ALREADY_EXISTS
                                                                                         : ERROR_SUCCESS);
    return handle_add(f);
}

WINAPI HANDLE kx_CreateFileW(LPCWSTR name, DWORD access, DWORD share, void* sa, DWORD disposition, DWORD flags,
                             HANDLE tmpl) {
    return open_file(utf16_to_utf8(name), access, disposition, flags);
}

WINAPI HANDLE kx_CreateFileA(LPCSTR name, DWORD access, DWORD share, void* sa, DWORD disposition, DWORD flags,
                             HANDLE tmpl) {
    return open_file(name, access, disposition, flags);
}

struct CreateFile2Params {
    DWORD dwSize;
    DWORD dwFileAttributes;
    DWORD dwFileFlags;
    DWORD dwSecurityQosFlags;
    void* lpSecurityAttributes;
    HANDLE hTemplateFile;
};

WINAPI HANDLE kx_CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition, CreateFile2Params* p) {
    return open_file(utf16_to_utf8(name), access, disposition, p ? p->dwFileFlags : 0);
}

// ---------------------------------------------------------------- read/write

std::shared_ptr<File> get_file(HANDLE h) { return handle_get_as<File>(h, ObjType::File); }

void complete(File& f, Overlapped* ov, DWORD status, DWORD bytes) {
    ov->Internal = status;
    ov->InternalHigh = bytes;
    // Low bit of hEvent set = "don't post to the completion port".
    auto ev = reinterpret_cast<HANDLE>(reinterpret_cast<uint64_t>(ov->hEvent) & ~uint64_t(1));
    if (ev) signal_event(ev);
    if (f.port && !(reinterpret_cast<uint64_t>(ov->hEvent) & 1)) f.port->post({bytes, f.key, ov});
}

BOOL do_io(HANDLE h, void* buf, DWORD n, DWORD* done, Overlapped* ov, bool is_write) {
    auto f = get_file(h);
    if (!f) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    if (done) *done = 0;
    if (ov) {
        off_t off = (static_cast<off_t>(ov->OffsetHigh) << 32) | ov->Offset;
        if (!is_write) os::write_watch_before_kernel_write(buf, n);
        ssize_t r = is_write ? pwrite(f->fd, buf, n, off) : pread(f->fd, buf, n, off);
        if (r < 0) {
            set_last_error(errno_to_win(errno));
            return FALSE_;
        }
        DWORD status = (!is_write && r == 0 && n > 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
        FL_LOG(fileio, "%s fd %d off 0x%llx len 0x%x -> 0x%zx (overlapped)", is_write ? "write" : "read", f->fd,
               (unsigned long long)off, n, r);
        if (status == STATUS_END_OF_FILE && !f->overlapped) return set_last_error(ERROR_HANDLE_EOF), FALSE_;
        complete(*f, ov, status, static_cast<DWORD>(r));
        if (!f->overlapped) {
            if (done) *done = static_cast<DWORD>(r);
            return TRUE_;
        }
        if (status == STATUS_END_OF_FILE) return set_last_error(ERROR_HANDLE_EOF), FALSE_;
        set_last_error(ERROR_IO_PENDING);
        return FALSE_;
    }
    if (!is_write) os::write_watch_before_kernel_write(buf, n);
    size_t total = 0;
    while (total < n) {
        ssize_t r = is_write ? write(f->fd, static_cast<uint8_t*>(buf) + total, n - total)
                             : read(f->fd, static_cast<uint8_t*>(buf) + total, n - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            set_last_error(errno_to_win(errno));
            return FALSE_;
        }
        if (r == 0) break;
        total += r;
    }
    if (done) *done = static_cast<DWORD>(total);
    return TRUE_;
}

WINAPI BOOL kx_ReadFile(HANDLE h, void* buf, DWORD n, DWORD* done, Overlapped* ov) {
    return do_io(h, buf, n, done, ov, false);
}

WINAPI BOOL kx_WriteFile(HANDLE h, const void* buf, DWORD n, DWORD* done, Overlapped* ov) {
    return do_io(h, const_cast<void*>(buf), n, done, ov, true);
}

WINAPI BOOL kx_GetOverlappedResultEx(HANDLE h, Overlapped* ov, DWORD* bytes, DWORD ms, BOOL alertable) {
    // We complete synchronously, so the result is always ready.
    if (ov->Internal == 0x103 /* STATUS_PENDING */) fl::fatal("GetOverlappedResultEx: I/O still pending?");
    *bytes = static_cast<DWORD>(ov->InternalHigh);
    if (ov->Internal == STATUS_END_OF_FILE) return set_last_error(ERROR_HANDLE_EOF), FALSE_;
    return ov->Internal == STATUS_SUCCESS ? TRUE_ : FALSE_;
}

WINAPI BOOL kx_SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER* newpos, DWORD method) {
    auto f = get_file(h);
    if (!f) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    off_t r = lseek(f->fd, dist.QuadPart, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) return set_last_error(ERROR_INVALID_PARAMETER), FALSE_;
    if (newpos) newpos->QuadPart = r;
    return TRUE_;
}

WINAPI DWORD kx_SetFilePointer(HANDLE h, LONG lo, LONG* hi, DWORD method) {
    LARGE_INTEGER d, n;
    d.QuadPart = hi ? ((static_cast<int64_t>(*hi) << 32) | static_cast<uint32_t>(lo)) : lo;
    if (!kx_SetFilePointerEx(h, d, &n, method)) return 0xFFFFFFFF;  // INVALID_SET_FILE_POINTER
    if (hi) *hi = n.HighPart;
    return n.LowPart;
}

WINAPI BOOL kx_GetFileSizeEx(HANDLE h, LARGE_INTEGER* size) {
    auto f = get_file(h);
    if (!f) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    struct stat st;
    fstat(f->fd, &st);
    size->QuadPart = st.st_size;
    return TRUE_;
}

WINAPI BOOL kx_FlushFileBuffers(HANDLE h) {
    auto f = get_file(h);
    if (!f) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    double t0 = run_time();
    fsync(f->fd);
    FL_LOG(file, "FlushFileBuffers(fd %d): fsync %.1f ms", f->fd, (run_time() - t0) * 1000);
    return TRUE_;
}

WINAPI BOOL kx_SetEndOfFile(HANDLE h) {
    auto f = get_file(h);
    if (!f) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    return ftruncate(f->fd, pos) == 0 ? TRUE_ : FALSE_;
}

struct FileBasicInfo {
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    DWORD FileAttributes;
};
struct FileStandardInfo {
    LARGE_INTEGER AllocationSize, EndOfFile;
    DWORD NumberOfLinks;
    BOOLEAN DeletePending, Directory;
};

WINAPI BOOL kx_GetFileInformationByHandleEx(HANDLE h, int cls, void* out, DWORD size) {
    auto f = get_file(h);
    if (!f) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    struct stat st;
    fstat(f->fd, &st);
    auto ft = [](const timespec& ts) {
        LARGE_INTEGER v;
        v.QuadPart = static_cast<int64_t>(ts.tv_sec) * 10000000 + ts.tv_nsec / 100 + kEpochDelta;
        return v;
    };
    if (cls == 0 && size >= sizeof(FileBasicInfo)) {
        auto* b = static_cast<FileBasicInfo*>(out);
        b->CreationTime = b->ChangeTime = ft(st.st_ctim);
        b->LastAccessTime = ft(st.st_atim);
        b->LastWriteTime = ft(st.st_mtim);
        b->FileAttributes = attrs_of(st);
        return TRUE_;
    }
    if (cls == 1 && size >= sizeof(FileStandardInfo)) {
        auto* s = static_cast<FileStandardInfo*>(out);
        s->AllocationSize.QuadPart = st.st_blocks * 512;
        s->EndOfFile.QuadPart = st.st_size;
        s->NumberOfLinks = static_cast<DWORD>(st.st_nlink);
        s->DeletePending = 0;
        s->Directory = S_ISDIR(st.st_mode);
        return TRUE_;
    }
    fl::fatal("UNIMPLEMENTED GetFileInformationByHandleEx class %d", cls);
}

// ---------------------------------------------------------------- attributes / directories

struct Win32FileAttributeData {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
};

BOOL file_attributes(const std::string& win, Win32FileAttributeData* out) {
    std::string host = vfs::to_host(win);
    struct stat st;
    if (stat(host.c_str(), &st) != 0) {
        FL_LOG(file, "attributes '%s' -> not found", win.c_str());
        set_last_error(errno_to_win(errno));
        return FALSE_;
    }
    out->dwFileAttributes = attrs_of(st);
    put_ft(&out->ftCreationTime, st.st_ctim);
    put_ft(&out->ftLastAccessTime, st.st_atim);
    put_ft(&out->ftLastWriteTime, st.st_mtim);
    out->nFileSizeHigh = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
    out->nFileSizeLow = static_cast<DWORD>(st.st_size);
    return TRUE_;
}

WINAPI BOOL kx_GetFileAttributesExW(LPCWSTR name, int level, Win32FileAttributeData* out) {
    return file_attributes(utf16_to_utf8(name), out);
}
WINAPI BOOL kx_GetFileAttributesExA(LPCSTR name, int level, Win32FileAttributeData* out) {
    return file_attributes(name, out);
}

BOOL delete_file(const std::string& win) {
    std::string host = vfs::to_host(win);
    if (unlink(host.c_str()) != 0) return set_last_error(errno_to_win(errno)), FALSE_;
    return TRUE_;
}
WINAPI BOOL kx_DeleteFileW(LPCWSTR name) { return delete_file(utf16_to_utf8(name)); }
WINAPI BOOL kx_DeleteFileA(LPCSTR name) { return delete_file(name); }

BOOL create_dir(const std::string& win) {
    std::string host = vfs::to_host(win, true);
    if (mkdir(host.c_str(), 0755) != 0) {
        set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : errno_to_win(errno));
        return FALSE_;
    }
    return TRUE_;
}
WINAPI BOOL kx_CreateDirectoryW(LPCWSTR name, void* sa) { return create_dir(utf16_to_utf8(name)); }
WINAPI BOOL kx_CreateDirectoryA(LPCSTR name, void* sa) { return create_dir(name); }

WINAPI BOOL kx_RemoveDirectoryA(LPCSTR name) {
    if (rmdir(vfs::to_host(name).c_str()) != 0) return set_last_error(errno_to_win(errno)), FALSE_;
    return TRUE_;
}

WINAPI BOOL kx_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    std::string a = vfs::to_host(utf16_to_utf8(from)), b = vfs::to_host(utf16_to_utf8(to), true);
    if (rename(a.c_str(), b.c_str()) != 0) return set_last_error(errno_to_win(errno)), FALSE_;
    return TRUE_;
}

WINAPI HRESULT kx_CopyFile2(LPCWSTR from, LPCWSTR to, void* params) {
    std::error_code ec;
    std::filesystem::copy_file(vfs::to_host(utf16_to_utf8(from)), vfs::to_host(utf16_to_utf8(to), true),
                               std::filesystem::copy_options::overwrite_existing, ec);
    return ec ? E_FAIL : S_OK;
}

WINAPI BOOL kx_GetDiskFreeSpaceExW(LPCWSTR dir, uint64_t* avail, uint64_t* total, uint64_t* free_bytes) {
    struct statvfs vs;
    if (statvfs(config().data_dir.c_str(), &vs) != 0) return FALSE_;
    if (avail) *avail = static_cast<uint64_t>(vs.f_bavail) * vs.f_frsize;
    if (total) *total = static_cast<uint64_t>(vs.f_blocks) * vs.f_frsize;
    if (free_bytes) *free_bytes = static_cast<uint64_t>(vs.f_bfree) * vs.f_frsize;
    return TRUE_;
}

// ---------------------------------------------------------------- find

struct Win32FindDataA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    DWORD dwReserved0, dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
};
static_assert(sizeof(Win32FindDataA) == 320);

struct FindFile : KObject {
    FindFile() : KObject(ObjType::FindFile) {}
    std::string dir;
    std::vector<std::string> names;
    size_t next = 0;
};

bool fill_find(FindFile& ff, Win32FindDataA* out) {
    while (ff.next < ff.names.size()) {
        const std::string& n = ff.names[ff.next++];
        struct stat st;
        if (stat((ff.dir + "/" + n).c_str(), &st) != 0) continue;
        memset(out, 0, sizeof *out);
        out->dwFileAttributes = attrs_of(st);
        put_ft(&out->ftCreationTime, st.st_ctim);
        put_ft(&out->ftLastAccessTime, st.st_atim);
        put_ft(&out->ftLastWriteTime, st.st_mtim);
        out->nFileSizeHigh = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
        out->nFileSizeLow = static_cast<DWORD>(st.st_size);
        snprintf(out->cFileName, sizeof out->cFileName, "%s", n.c_str());
        return true;
    }
    return false;
}

WINAPI HANDLE kx_FindFirstFileExA(LPCSTR pattern, int level, Win32FindDataA* out, int op, void* filter, DWORD flags) {
    std::string pat = pattern;
    for (char& c : pat)
        if (c == '\\') c = '/';
    size_t slash = pat.find_last_of('/');
    std::string dir_win = slash == std::string::npos ? "" : pat.substr(0, slash);
    std::string glob = slash == std::string::npos ? pat : pat.substr(slash + 1);
    if (glob == "*.*") glob = "*";

    auto ff = std::make_shared<FindFile>();
    ff->dir = vfs::to_host(dir_win);
    if (DIR* d = opendir(ff->dir.c_str())) {
        while (dirent* e = readdir(d))
            if (fnmatch(glob.c_str(), e->d_name, FNM_CASEFOLD) == 0) ff->names.push_back(e->d_name);
        closedir(d);
    }
    FL_LOG(file, "FindFirstFile '%s' -> %s: %zu matches", pattern, ff->dir.c_str(), ff->names.size());
    if (!fill_find(*ff, out)) return set_last_error(ERROR_FILE_NOT_FOUND), INVALID_HANDLE_VALUE;
    return handle_add(ff);
}

WINAPI BOOL kx_FindNextFileA(HANDLE h, Win32FindDataA* out) {
    auto ff = handle_get_as<FindFile>(h, ObjType::FindFile);
    if (!ff) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    if (!fill_find(*ff, out)) return set_last_error(ERROR_NO_MORE_FILES), FALSE_;
    return TRUE_;
}

WINAPI BOOL kx_FindClose(HANDLE h) { return handle_close(h) ? TRUE_ : FALSE_; }

// ---------------------------------------------------------------- IOCP

WINAPI HANDLE kx_CreateIoCompletionPort(HANDLE file, HANDLE existing, ULONG_PTR key, DWORD threads) {
    std::shared_ptr<Iocp> port;
    HANDLE port_handle = existing;
    if (existing) {
        port = handle_get_as<Iocp>(existing, ObjType::Iocp);
        if (!port) return set_last_error(ERROR_INVALID_PARAMETER), nullptr;
    } else {
        port = std::make_shared<Iocp>();
        port_handle = handle_add(port);
    }
    if (file != INVALID_HANDLE_VALUE) {
        auto f = get_file(file);
        if (!f) return set_last_error(ERROR_INVALID_HANDLE), nullptr;
        f->port = port;
        f->key = key;
    }
    return port_handle;
}

WINAPI BOOL kx_PostQueuedCompletionStatus(HANDLE h, DWORD bytes, ULONG_PTR key, Overlapped* ov) {
    auto port = handle_get_as<Iocp>(h, ObjType::Iocp);
    if (!port) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    port->post({bytes, key, ov});
    return TRUE_;
}

WINAPI BOOL kx_GetQueuedCompletionStatus(HANDLE h, DWORD* bytes, ULONG_PTR* key, Overlapped** ov, DWORD ms) {
    auto port = handle_get_as<Iocp>(h, ObjType::Iocp);
    if (!port) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    std::unique_lock lock(port->m);
    auto ready = [&] { return !port->queue.empty(); };
    if (ms == INFINITE)
        port->cv.wait(lock, ready);
    else if (!port->cv.wait_for(lock, std::chrono::milliseconds(ms), ready)) {
        *ov = nullptr;
        set_last_error(WAIT_TIMEOUT);
        return FALSE_;
    }
    Packet p = port->queue.front();
    port->queue.pop_front();
    *bytes = p.bytes;
    *key = p.key;
    *ov = p.ov;
    if (p.ov && p.ov->Internal != STATUS_SUCCESS) {
        set_last_error(p.ov->Internal == STATUS_END_OF_FILE ? ERROR_HANDLE_EOF : ERROR_INVALID_PARAMETER);
        return FALSE_;
    }
    return TRUE_;
}

}  // namespace

HLE_EXPORT(kernelx, CreateFileW, kx_CreateFileW);
HLE_EXPORT(kernelx, CreateFileA, kx_CreateFileA);
HLE_EXPORT(kernelx, CreateFile2, kx_CreateFile2);
HLE_EXPORT(kernelx, ReadFile, kx_ReadFile);
HLE_EXPORT(kernelx, WriteFile, kx_WriteFile);
HLE_EXPORT(kernelx, GetOverlappedResultEx, kx_GetOverlappedResultEx);
HLE_EXPORT(kernelx, SetFilePointerEx, kx_SetFilePointerEx);
HLE_EXPORT(kernelx, SetFilePointer, kx_SetFilePointer);
HLE_EXPORT(kernelx, GetFileSizeEx, kx_GetFileSizeEx);
HLE_EXPORT(kernelx, FlushFileBuffers, kx_FlushFileBuffers);
HLE_EXPORT(kernelx, SetEndOfFile, kx_SetEndOfFile);
HLE_EXPORT(kernelx, GetFileInformationByHandleEx, kx_GetFileInformationByHandleEx);
HLE_EXPORT(kernelx, GetFileAttributesExW, kx_GetFileAttributesExW);
HLE_EXPORT(kernelx, GetFileAttributesExA, kx_GetFileAttributesExA);
HLE_EXPORT(kernelx, DeleteFileW, kx_DeleteFileW);
HLE_EXPORT(kernelx, DeleteFileA, kx_DeleteFileA);
HLE_EXPORT(kernelx, CreateDirectoryW, kx_CreateDirectoryW);
HLE_EXPORT(kernelx, CreateDirectoryA, kx_CreateDirectoryA);
HLE_EXPORT(kernelx, RemoveDirectoryA, kx_RemoveDirectoryA);
HLE_EXPORT(kernelx, MoveFileExW, kx_MoveFileExW);
HLE_EXPORT(kernelx, CopyFile2, kx_CopyFile2);
HLE_EXPORT(kernelx, GetDiskFreeSpaceExW, kx_GetDiskFreeSpaceExW);
HLE_EXPORT(kernelx, FindFirstFileExA, kx_FindFirstFileExA);
HLE_EXPORT(kernelx, FindNextFileA, kx_FindNextFileA);
HLE_EXPORT(kernelx, FindClose, kx_FindClose);
HLE_EXPORT(kernelx, CreateIoCompletionPort, kx_CreateIoCompletionPort);
HLE_EXPORT(kernelx, PostQueuedCompletionStatus, kx_PostQueuedCompletionStatus);
HLE_EXPORT(kernelx, GetQueuedCompletionStatus, kx_GetQueuedCompletionStatus);

}  // namespace fl::kx
