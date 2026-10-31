// Windows.Xbox.Storage (ConnectedStorage) on local files, and
// Windows.Storage.Streams.Buffer (the IBuffer the blobs travel in).
//
// Layout on disk: <data_dir>/saves/<xuid>/<container>/<blob>, names
// percent-encoded. A SubmitUpdatesAsync builds the container's new contents in
// <container>.new (unchanged blobs hard-linked) and swaps it in with renames,
// so a crash leaves either the old or the new container, never a mix.
//
// Interfaces: gameosproxy proxy data (FINDINGS §4g). Behaviour that the proxy
// data can't show (ContainerInfo is {Name, TotalSize}; GetAsync of a missing
// blob fails with 0x80830008; only GetForUserAsync(user) is used; the space's
// S+2 bool is false) follows the Azimuth reference (CLAUDE.md Rule 1).
// All operations complete before they are returned.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/log.h"
#include "common/wstr.h"
#include "hle/winrt/activation.h"
#include "hle/winrt/async.h"
#include "hle/winrt/object.h"
#include "hle/winrt/xbox.h"

namespace fl::winrt {
namespace {

constexpr int S = kInspectableSlots;

// Windows.Storage.Streams (public Windows SDK interfaces)
constexpr GUID IID_IBuffer = {0x905A0FE0, 0xBC53, 0x11DF, {0x8C, 0x49, 0x00, 0x1E, 0x4F, 0xC6, 0x86, 0xDA}};
constexpr GUID IID_IBufferByteAccess = {0x905A0FEF, 0xBC53, 0x11DF, {0x8C, 0x49, 0x00, 0x1E, 0x4F, 0xC6, 0x86, 0xDA}};
constexpr GUID IID_IBufferFactory = {0x71AF914D, 0xC10F, 0x484B, {0xBC, 0x50, 0x14, 0xBC, 0x62, 0x3B, 0x3A, 0x27}};
// Collections over <String, IBuffer>
constexpr GUID IID_IMapView_String_IBuffer = {0x2CFEEC4F, 0xE261, 0x5F4C, {0xAE, 0xE1, 0xC7, 0x85, 0x18, 0xE9, 0xD5, 0xB9}};
constexpr GUID IID_IIterable_KVP = {0x3C9FFA92, 0x5123, 0x5AC4, {0xB1, 0x11, 0x03, 0xC2, 0x15, 0xF0, 0xC5, 0x1C}};
constexpr GUID IID_IIterator_KVP = {0x790ACB62, 0xC4B3, 0x57EA, {0xA1, 0x52, 0x9E, 0x21, 0x93, 0x71, 0xC6, 0xDC}};
constexpr GUID IID_IKeyValuePair_String_IBuffer = {0x9114F794, 0x2CEB, 0x5B03, {0x9B, 0x22, 0x36, 0x88, 0x4E, 0x1F, 0x58, 0xB3}};
// Windows.Xbox.Storage
constexpr GUID IID_IConnectedStorageSpaceStatics = {0x07020414, 0xB3E9, 0x47C8, {0xA3, 0x6F, 0xF2, 0x4E, 0x2D, 0xC6, 0x55, 0x73}};
constexpr GUID IID_IConnectedStorageSpaceStatics2 = {0xDF3FE390, 0x13FA, 0x4814, {0x91, 0xC1, 0xFB, 0x92, 0xF9, 0x0F, 0x64, 0x1D}};
constexpr GUID IID_IConnectedStorageSpace = {0xB48EEA29, 0x8323, 0x41BC, {0xBB, 0xAD, 0x2C, 0x26, 0xE2, 0x7B, 0xDA, 0x22}};
constexpr GUID IID_IConnectedStorageSpace2 = {0xBC5B91A7, 0x2AFC, 0x46FD, {0x81, 0xD4, 0x74, 0xB8, 0x18, 0x8D, 0x74, 0x8A}};
constexpr GUID IID_IConnectedStorageContainer = {0x885C65FE, 0xE884, 0x4CFF, {0xBA, 0x58, 0xFB, 0x82, 0xCC, 0x32, 0xB5, 0x52}};
constexpr GUID IID_IConnectedStorageContainer2 = {0xAF964B72, 0xEC65, 0x46DE, {0x93, 0x5C, 0x84, 0x5A, 0xD0, 0x19, 0x4F, 0x0D}};
constexpr GUID IID_IContainerInfoQueryResult = {0x4F937563, 0xBAE3, 0x4062, {0xA6, 0x7F, 0xCB, 0xEA, 0x61, 0xA2, 0xED, 0xC3}};
constexpr GUID IID_IVectorView_ContainerInfo = {0x8DBBED35, 0x29EE, 0x5C66, {0x80, 0xC7, 0xAB, 0x2E, 0x05, 0x6E, 0xBF, 0x13}};
constexpr GUID IID_IIterable_ContainerInfo = {0xCD84DCCA, 0xB42A, 0x5D4F, {0xBF, 0xDC, 0x32, 0x4F, 0x2E, 0xC1, 0x42, 0xBB}};
constexpr GUID IID_IIterator_ContainerInfo = {0x43CBA1FB, 0xE79C, 0x5528, {0xB5, 0xD6, 0x9D, 0xC2, 0x23, 0x93, 0x1E, 0x7D}};
// Async results
constexpr GUID IID_IAsyncAction = {0x5A648006, 0x843A, 0x4DA9, {0x86, 0x5B, 0x9D, 0x26, 0xE5, 0xDF, 0xAD, 0x7B}};
constexpr GUID IID_IAsyncOp_Space = {0xC901CBBF, 0x25EB, 0x51E4, {0x99, 0x22, 0x4A, 0x57, 0x0F, 0xED, 0x22, 0xA6}};
constexpr GUID IID_IAsyncOp_BufferMap = {0xC797EBDE, 0xB0B3, 0x57DF, {0x83, 0x09, 0x33, 0x16, 0x7A, 0x57, 0x3C, 0x21}};
constexpr GUID IID_IAsyncOp_ContainerInfoVector = {0x729B0242, 0x27B9, 0x5596, {0x8B, 0x26, 0x44, 0x68, 0x1B, 0x25, 0xD2, 0x05}};
constexpr GUID IID_IAsyncOp_UInt32 = {0xEF60385F, 0xBE78, 0x584B, {0xAA, 0xEF, 0x78, 0x29, 0xAD, 0xA2, 0xB0, 0xDE}};
constexpr GUID IID_IAsyncOp_Int32 = {0x968B9665, 0x06ED, 0x5774, {0x8F, 0x53, 0x8E, 0xDE, 0xAB, 0xD5, 0xF7, 0xB5}};
constexpr GUID IID_IAsyncOp_Int64 = {0xCC468085, 0x4BEF, 0x5584, {0x90, 0x7C, 0x92, 0x23, 0xD2, 0x67, 0x90, 0x19}};

constexpr HRESULT kBlobNotFound = static_cast<HRESULT>(0x80830008);  // ConnectedStorage: requested blob missing
constexpr HRESULT E_BOUNDS_ = static_cast<HRESULT>(0x8000000B);
constexpr int64_t kQuotaBytes = 64ll << 20;  // STUB: per-user quota (the console's value is not known)

std::mutex g_io;  // one save operation at a time

// Commits run on one host thread (write-behind): on the PS5 a commit to /data
// took seconds (writes, fsyncs, renames, unlinks), and the game's update
// thread waited for its autosaves, freezing the picture (FINDINGS 7.16).
// Everything else that touches save files takes io_lock(), which waits for
// the queued commits first, so it sees every update the game submitted.
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<std::function<void()>> g_queue;
bool g_busy = false;  // the commit thread is running a job

void queue_commit(std::function<void()> job) {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            for (;;) {
                std::function<void()> next;
                {
                    std::unique_lock q(g_queue_mutex);
                    g_queue_cv.wait(q, [] { return !g_queue.empty(); });
                    next = std::move(g_queue.front());
                    g_queue.pop_front();
                    g_busy = true;
                }
                next();
                {
                    std::lock_guard q(g_queue_mutex);
                    g_busy = false;
                }
                g_queue_cv.notify_all();
            }
        }).detach();
    });
    {
        std::lock_guard q(g_queue_mutex);
        g_queue.push_back(std::move(job));
    }
    g_queue_cv.notify_all();
}

std::unique_lock<std::mutex> io_lock() {
    {
        std::unique_lock q(g_queue_mutex);
        g_queue_cv.wait(q, [] { return g_queue.empty() && !g_busy; });
    }
    return std::unique_lock<std::mutex>(g_io);
}

// ---------------------------------------------------------------- names and files

// Percent-encode a container/blob name into one safe path component.
std::string encode_name(const std::u16string& n) {
    std::string out;
    for (size_t i = 0; i < n.size(); i++) {
        char16_t c = n[i];
        bool plain = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'_' ||
                     c == u'-' || (c == u'.' && i > 0) || c == u' ';
        if (plain) {
            out += char(c);
        } else {
            char buf[8];
            snprintf(buf, sizeof buf, "%%%04X", unsigned(c));
            out += buf;
        }
    }
    return out;
}

std::u16string decode_name(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 4 < s.size()) {
            out += char16_t(strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
            i += 4;
        } else {
            out += char16_t(static_cast<unsigned char>(s[i]));
        }
    }
    return out;
}

bool mkdirs(const std::string& path) {
    for (size_t p = 1; p <= path.size(); p++) {
        if (p == path.size() || path[p] == '/') {
            std::string d = path.substr(0, p);
            // An existing folder is fine whatever mkdir says: in a PS5 title,
            // mkdir of a folder above its own (/data) fails, but not with
            // EEXIST (the errno wasn't recorded; FINDINGS 7.11).
            struct stat st;
            if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST && !(stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode)))
                return false;
        }
    }
    return true;
}

std::vector<std::string> list_dir(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') out.push_back(e->d_name);
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void remove_tree(const std::string& dir) {
    for (const std::string& f : list_dir(dir)) unlink((dir + "/" + f).c_str());
    rmdir(dir.c_str());
}

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->insert(out->end(), buf, buf + n);
    fclose(f);
    return true;
}

bool write_file_synced(const std::string& path, const uint8_t* data, size_t n) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, data + done, n - done);
        if (w <= 0) {
            close(fd);
            return false;
        }
        done += size_t(w);
    }
    bool ok = fsync(fd) == 0;
    return close(fd) == 0 && ok;
}

void fsync_dir(const std::string& dir) {
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) fsync(fd), close(fd);
}

// A container directory, recovering from an interrupted commit first.
std::string container_dir(const std::string& space, const std::u16string& name) {
    std::string dir = space + "/" + encode_name(name);
    struct stat st;
    if (stat(dir.c_str(), &st) != 0) {
        if (stat((dir + ".old").c_str(), &st) == 0) rename((dir + ".old").c_str(), dir.c_str());
    } else if (stat((dir + ".old").c_str(), &st) == 0) {
        remove_tree(dir + ".old");
    }
    if (stat((dir + ".new").c_str(), &st) == 0) remove_tree(dir + ".new");
    return dir;
}

uint64_t dir_size(const std::string& dir) {
    uint64_t total = 0;
    for (const std::string& f : list_dir(dir)) {
        struct stat st;
        if (stat((dir + "/" + f).c_str(), &st) == 0) total += uint64_t(st.st_size);
    }
    return total;
}

// ---------------------------------------------------------------- Windows.Storage.Streams.Buffer

struct Buffer : Object {
    std::vector<uint8_t> bytes;  // capacity = size
    uint32_t length = 0;
    static Buffer* self(Iface* i) { return i->as<Buffer>(); }
    static WINAPI HRESULT get_Capacity(Iface* i, uint32_t* out) { return *out = uint32_t(self(i)->bytes.size()), S_OK; }
    static WINAPI HRESULT get_Length(Iface* i, uint32_t* out) { return *out = self(i)->length, S_OK; }
    static WINAPI HRESULT put_Length(Iface* i, uint32_t v) {
        if (v > self(i)->bytes.size()) return E_INVALIDARG;
        self(i)->length = v;
        return S_OK;
    }
    static WINAPI HRESULT Bytes(Iface* i, uint8_t** out) { return *out = self(i)->bytes.data(), S_OK; }
    static Vtable& vt() {
        static Vtable v = Vtable("IBuffer", S + 3).set(S + 0, &get_Capacity).set(S + 1, &get_Length).set(S + 2, &put_Length);
        return v;
    }
    static Vtable& vt_access() {
        static Vtable v = Vtable("IBufferByteAccess", 4, false).set(3, &Bytes);
        return v;
    }
    Iface ibuffer{vt(), this};
    Iface iaccess{vt_access(), this};
    explicit Buffer(uint32_t capacity) : Object(u"Windows.Storage.Streams.Buffer"), bytes(capacity) {
        expose(IID_IBuffer, &ibuffer);
        expose(IID_IBufferByteAccess, &iaccess);
    }
};

struct BufferFactory : Object {
    static WINAPI HRESULT Create(Iface*, uint32_t capacity, void** out) {
        *out = (new Buffer(capacity))->default_iface_noref();
        return S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IBufferFactory", S + 1).set(S + 0, &Create);
        return v;
    }
    Iface ifactory{vt(), this};
    BufferFactory() : Object(u"Windows.Storage.Streams.Buffer") { expose(IID_IBufferFactory, &ifactory); }
};

HRESULT get_buffer_factory(const GUID& iid, void** out) {
    static auto* f = new BufferFactory();
    return f->query(iid, out);
}

// The bytes of any IBuffer (ours or the game's): IBufferByteAccess + Length.
bool buffer_bytes(void* ibuffer, const uint8_t** data, uint32_t* len) {
    void* access = nullptr;
    if (!ibuffer || call_slot<HRESULT>(ibuffer, 0, &IID_IBufferByteAccess, &access) < 0 || !access) return false;
    uint8_t* p = nullptr;
    call_slot<HRESULT>(access, 3, &p);
    com_release(access);
    call_slot<HRESULT>(ibuffer, S + 1, len);
    *data = p;
    return true;
}

// ---------------------------------------------------------------- IMapView<String, IBuffer> (GetAsync results)

struct BufferMap;
struct BufferPair : Object {
    std::u16string key;
    Buffer* value;
    static WINAPI HRESULT get_Key(Iface* i, HSTRING* out) { return make_hstring(i->as<BufferPair>()->key.c_str(), out); }
    static WINAPI HRESULT get_Value(Iface* i, void** out) {
        *out = i->as<BufferPair>()->value->default_iface();
        return S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IKeyValuePair<String,IBuffer>", S + 2).set(S + 0, &get_Key).set(S + 1, &get_Value);
        return v;
    }
    Iface ipair{vt(), this};
    BufferPair(std::u16string k, Buffer* v) : Object(u"Windows.Foundation.Collections.IKeyValuePair`2"), key(std::move(k)), value(v) {
        value->add_ref();
        expose(IID_IKeyValuePair_String_IBuffer, &ipair);
    }
    ~BufferPair() override { value->release(); }
};

struct BufferMap : Object {
    std::vector<std::pair<std::u16string, Buffer*>> items;  // sorted by key, owning
    ~BufferMap() override {
        for (auto& [k, b] : items) b->release();
    }
    Buffer* find(const std::u16string& k) const {
        for (auto& [key, b] : items)
            if (key == k) return b;
        return nullptr;
    }
    static BufferMap* self(Iface* i) { return i->as<BufferMap>(); }
    static WINAPI HRESULT Lookup(Iface* i, HSTRING key, void** out) {
        Buffer* b = self(i)->find(hstring_str(key));
        *out = b ? b->default_iface() : nullptr;
        return b ? S_OK : E_BOUNDS_;
    }
    static WINAPI HRESULT get_Size(Iface* i, uint32_t* out) { return *out = uint32_t(self(i)->items.size()), S_OK; }
    static WINAPI HRESULT HasKey(Iface* i, HSTRING key, uint8_t* out) {
        return *out = self(i)->find(hstring_str(key)) != nullptr, S_OK;
    }
    static WINAPI HRESULT Split(Iface*, void** a, void** b) { return *a = *b = nullptr, S_OK; }
    static WINAPI HRESULT First(Iface* i, void** out);
    static Vtable& vt() {
        static Vtable v = Vtable("IMapView<String,IBuffer>", S + 4)
                              .set(S + 0, &Lookup)
                              .set(S + 1, &get_Size)
                              .set(S + 2, &HasKey)
                              .set(S + 3, &Split);
        return v;
    }
    static Vtable& vt_iterable() {
        static Vtable v = Vtable("IIterable<IKeyValuePair<String,IBuffer>>", S + 1).set(S + 0, &First);
        return v;
    }
    Iface imap{vt(), this};
    Iface iiterable{vt_iterable(), this};
    BufferMap() : Object(u"Windows.Foundation.Collections.IMapView`2") {
        expose(IID_IMapView_String_IBuffer, &imap);
        expose(IID_IIterable_KVP, &iiterable);
    }
};

struct BufferMapIterator : Object {
    BufferMap* map;
    size_t pos = 0;
    static BufferMapIterator* self(Iface* i) { return i->as<BufferMapIterator>(); }
    static WINAPI HRESULT get_Current(Iface* i, void** out) {
        auto* it = self(i);
        if (it->pos >= it->map->items.size()) return *out = nullptr, E_BOUNDS_;
        auto& [k, b] = it->map->items[it->pos];
        *out = (new BufferPair(k, b))->default_iface_noref();
        return S_OK;
    }
    static WINAPI HRESULT get_HasCurrent(Iface* i, uint8_t* out) {
        return *out = self(i)->pos < self(i)->map->items.size(), S_OK;
    }
    static WINAPI HRESULT MoveNext(Iface* i, uint8_t* out) {
        auto* it = self(i);
        if (it->pos < it->map->items.size()) it->pos++;
        return *out = it->pos < it->map->items.size(), S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IIterator<IKeyValuePair<String,IBuffer>>", S + 4)
                              .set(S + 0, &get_Current)
                              .set(S + 1, &get_HasCurrent)
                              .set(S + 2, &MoveNext);
        return v;
    }
    Iface iit{vt(), this};
    explicit BufferMapIterator(BufferMap* m) : Object(u"Windows.Foundation.Collections.IIterator`1"), map(m) {
        map->add_ref();
        expose(IID_IIterator_KVP, &iit);
    }
    ~BufferMapIterator() override { map->release(); }
};

WINAPI HRESULT BufferMap::First(Iface* i, void** out) {
    *out = (new BufferMapIterator(self(i)))->default_iface_noref();
    return S_OK;
}

// ---------------------------------------------------------------- reading the game's collections

// Iterate an IIterable (slot S+0 First); for each item call fn(current item
// pointer or value). Iterator slots: S+0 Current, S+1 HasCurrent, S+2 MoveNext.
template <class T, class Fn>
bool iterate(void* iterable, Fn fn) {
    void* it = nullptr;
    if (call_slot<HRESULT>(iterable, S + 0, &it) < 0 || !it) return false;
    uint8_t has = 0;
    call_slot<HRESULT>(it, S + 1, &has);
    while (has) {
        T cur{};
        if (call_slot<HRESULT>(it, S + 0, &cur) < 0) break;
        fn(cur);
        if (call_slot<HRESULT>(it, S + 2, &has) < 0) break;
    }
    com_release(it);
    return true;
}

// IMapView<String, IBuffer> from the game -> name -> bytes.
bool read_buffer_map(void* map_view, std::map<std::u16string, std::vector<uint8_t>>* out) {
    if (!map_view) return true;
    void* iterable = nullptr;
    if (call_slot<HRESULT>(map_view, 0, &IID_IIterable_KVP, &iterable) < 0 || !iterable) {
        FL_ERROR("ConnectedStorage: blob map has no IIterable<IKeyValuePair>");
        return false;
    }
    bool ok = true;
    iterate<void*>(iterable, [&](void* pair) {
        HSTRING key = nullptr;
        void* buf = nullptr;
        call_slot<HRESULT>(pair, S + 0, &key);
        call_slot<HRESULT>(pair, S + 1, &buf);
        const uint8_t* data = nullptr;
        uint32_t len = 0;
        if (buffer_bytes(buf, &data, &len))
            (*out)[hstring_str(key)].assign(data, data + len);
        else
            ok = false;
        if (buf) com_release(buf);
        com_release(pair);
    });
    com_release(iterable);
    return ok;
}

// IIterable<String> from the game -> names.
std::vector<std::u16string> read_string_iterable(void* iterable) {
    std::vector<std::u16string> out;
    if (iterable) iterate<HSTRING>(iterable, [&](HSTRING s) { out.push_back(hstring_str(s)); });
    return out;
}

void* action_done(HRESULT hr = S_OK) {
    return (new AsyncOperation(IID_IAsyncAction, nullptr, hr))->default_iface_noref();
}

// ---------------------------------------------------------------- ContainerInfo vector and query

struct ContainerInfo {  // {Name, TotalSize}: 16 bytes
    HSTRING name;
    uint64_t total_size;
};
static_assert(sizeof(ContainerInfo) == 16);

struct InfoEntry {
    std::u16string name;
    uint64_t size;
};

HRESULT fill_info(const InfoEntry& e, ContainerInfo* out) {
    out->total_size = e.size;
    return make_hstring(e.name.c_str(), &out->name);
}

struct InfoVector : Object {
    std::vector<InfoEntry> items;
    static InfoVector* self(Iface* i) { return i->as<InfoVector>(); }
    static WINAPI HRESULT GetAt(Iface* i, uint32_t idx, ContainerInfo* out) {
        *out = {};
        if (idx >= self(i)->items.size()) return E_BOUNDS_;
        return fill_info(self(i)->items[idx], out);
    }
    static WINAPI HRESULT get_Size(Iface* i, uint32_t* out) { return *out = uint32_t(self(i)->items.size()), S_OK; }
    static WINAPI HRESULT IndexOf(Iface* i, const ContainerInfo* v, uint32_t* idx, uint8_t* found) {
        std::u16string n = hstring_str(v->name);
        for (uint32_t k = 0; k < self(i)->items.size(); k++)
            if (self(i)->items[k].name == n) return *idx = k, *found = 1, S_OK;
        return *idx = 0, *found = 0, S_OK;
    }
    static WINAPI HRESULT GetMany(Iface* i, uint32_t start, uint32_t cap, ContainerInfo* items, uint32_t* actual) {
        uint32_t n = 0;
        for (uint32_t k = start; k < self(i)->items.size() && n < cap; k++, n++) fill_info(self(i)->items[k], &items[n]);
        return *actual = n, S_OK;
    }
    static WINAPI HRESULT First(Iface* i, void** out);
    static Vtable& vt() {
        static Vtable v = Vtable("IVectorView<ContainerInfo>", S + 4)
                              .set(S + 0, &GetAt)
                              .set(S + 1, &get_Size)
                              .set(S + 2, &IndexOf)
                              .set(S + 3, &GetMany);
        return v;
    }
    static Vtable& vt_iterable() {
        static Vtable v = Vtable("IIterable<ContainerInfo>", S + 1).set(S + 0, &First);
        return v;
    }
    Iface iview{vt(), this};
    Iface iiterable{vt_iterable(), this};
    explicit InfoVector(std::vector<InfoEntry> v) : Object(u"Windows.Foundation.Collections.IVectorView`1"), items(std::move(v)) {
        expose(IID_IVectorView_ContainerInfo, &iview);
        expose(IID_IIterable_ContainerInfo, &iiterable);
    }
};

struct InfoIterator : Object {
    InfoVector* vec;
    size_t pos = 0;
    static InfoIterator* self(Iface* i) { return i->as<InfoIterator>(); }
    static WINAPI HRESULT get_Current(Iface* i, ContainerInfo* out) {
        *out = {};
        if (self(i)->pos >= self(i)->vec->items.size()) return E_BOUNDS_;
        return fill_info(self(i)->vec->items[self(i)->pos], out);
    }
    static WINAPI HRESULT get_HasCurrent(Iface* i, uint8_t* out) {
        return *out = self(i)->pos < self(i)->vec->items.size(), S_OK;
    }
    static WINAPI HRESULT MoveNext(Iface* i, uint8_t* out) {
        if (self(i)->pos < self(i)->vec->items.size()) self(i)->pos++;
        return *out = self(i)->pos < self(i)->vec->items.size(), S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IIterator<ContainerInfo>", S + 4)
                              .set(S + 0, &get_Current)
                              .set(S + 1, &get_HasCurrent)
                              .set(S + 2, &MoveNext);
        return v;
    }
    Iface iit{vt(), this};
    explicit InfoIterator(InfoVector* v) : Object(u"Windows.Foundation.Collections.IIterator`1"), vec(v) {
        vec->add_ref();
        expose(IID_IIterator_ContainerInfo, &iit);
    }
    ~InfoIterator() override { vec->release(); }
};

WINAPI HRESULT InfoVector::First(Iface* i, void** out) {
    *out = (new InfoIterator(self(i)))->default_iface_noref();
    return S_OK;
}

struct Space;
std::vector<InfoEntry> list_containers(const std::string& space_dir, const std::u16string& prefix);

struct InfoQuery : Object {
    std::string space_dir;
    std::u16string prefix;
    static InfoQuery* self(Iface* i) { return i->as<InfoQuery>(); }
    static WINAPI HRESULT GetInfoRange(Iface* i, uint32_t start, uint32_t max, void** out) {
        std::vector<InfoEntry> all = list_containers(self(i)->space_dir, self(i)->prefix), part;
        for (size_t k = start; k < all.size() && part.size() < max; k++) part.push_back(all[k]);
        *out = async_result_ptr(IID_IAsyncOp_ContainerInfoVector, (new InfoVector(std::move(part)))->default_iface_noref());
        return S_OK;
    }
    static WINAPI HRESULT GetInfo(Iface* i, void** out) {
        *out = async_result_ptr(IID_IAsyncOp_ContainerInfoVector,
                                (new InfoVector(list_containers(self(i)->space_dir, self(i)->prefix)))->default_iface_noref());
        return S_OK;
    }
    static WINAPI HRESULT GetItemCount(Iface* i, void** out) {
        *out = async_result_u32(IID_IAsyncOp_UInt32, uint32_t(list_containers(self(i)->space_dir, self(i)->prefix).size()));
        return S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IContainerInfoQueryResult", S + 3)
                              .set(S + 0, &GetInfoRange)
                              .set(S + 1, &GetInfo)
                              .set(S + 2, &GetItemCount);
        return v;
    }
    Iface iquery{vt(), this};
    InfoQuery(std::string dir, std::u16string p)
        : Object(u"Windows.Xbox.Storage.ContainerInfoQueryResult"), space_dir(std::move(dir)), prefix(std::move(p)) {
        expose(IID_IContainerInfoQueryResult, &iquery);
    }
};

std::vector<InfoEntry> list_containers(const std::string& space_dir, const std::u16string& prefix) {
    auto lock = io_lock();
    std::vector<InfoEntry> out;
    for (const std::string& f : list_dir(space_dir)) {
        if (f.size() > 4 && (f.ends_with(".new") || f.ends_with(".old"))) continue;
        std::u16string name = decode_name(f);
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        out.push_back({name, dir_size(container_dir(space_dir, name))});
    }
    return out;
}

// ---------------------------------------------------------------- container

struct Container : Object {
    Space* space;
    std::u16string name;
    std::string dir;  // the container's directory (may not exist yet)

    static Container* self(Iface* i) { return i->as<Container>(); }
    static WINAPI HRESULT get_Name(Iface* i, HSTRING* out) { return make_hstring(self(i)->name.c_str(), out); }
    static WINAPI HRESULT get_OwningSpace(Iface* i, void** out);

    // Logs which step of a commit failed and why; returns E_FAIL.
    static HRESULT failed(const char* step, const std::string& path) {
        FL_ERROR("ConnectedStorage: %s %s failed: %s", step, path.c_str(), strerror(errno));
        return E_FAIL;
    }

    // Replace the container with (existing - deletes) + writes, atomically.
    HRESULT commit(const std::map<std::u16string, std::vector<uint8_t>>& writes, const std::vector<std::u16string>& deletes) {
        std::lock_guard lock(g_io);
        std::string parent = dir.substr(0, dir.rfind('/'));
        container_dir(parent, name);  // finish any interrupted commit
        std::string tmp = dir + ".new";
        if (!mkdirs(tmp)) return failed("mkdir", tmp);
        std::set<std::string> drop;
        for (auto& d : deletes) drop.insert(encode_name(d));
        for (auto& [k, v] : writes) drop.insert(encode_name(k));
        for (const std::string& f : list_dir(dir)) {
            if (drop.count(f)) continue;
            if (link((dir + "/" + f).c_str(), (tmp + "/" + f).c_str()) != 0) {
                std::vector<uint8_t> data;  // no hard links (other filesystem): copy
                if (!read_file(dir + "/" + f, &data)) return failed("read", dir + "/" + f), remove_tree(tmp), E_FAIL;
                if (!write_file_synced(tmp + "/" + f, data.data(), data.size()))
                    return failed("write", tmp + "/" + f), remove_tree(tmp), E_FAIL;
            }
        }
        for (auto& [k, v] : writes)
            if (!write_file_synced(tmp + "/" + encode_name(k), v.data(), v.size()))
                return failed("write", tmp + "/" + encode_name(k)), remove_tree(tmp), E_FAIL;
        fsync_dir(tmp);
        struct stat st;
        bool had = stat(dir.c_str(), &st) == 0;
        if (had && rename(dir.c_str(), (dir + ".old").c_str()) != 0)
            return failed("rename", dir), remove_tree(tmp), E_FAIL;
        if (rename(tmp.c_str(), dir.c_str()) != 0) return failed("rename", tmp);
        fsync_dir(parent);
        if (had) remove_tree(dir + ".old");
        return S_OK;
    }

    // S+2 SubmitUpdatesAsync(IMapView<String,IBuffer> blobsToWrite, IIterable<String> blobsToDelete)
    static WINAPI HRESULT SubmitUpdates(Iface* i, void* writes_map, void* deletes, void** out) {
        std::map<std::u16string, std::vector<uint8_t>> writes;
        if (!read_buffer_map(writes_map, &writes)) return *out = action_done(E_INVALIDARG), S_OK;
        std::vector<std::u16string> del = read_string_iterable(deletes);
        // Written behind (queue_commit): the game sees it done at once; a
        // failure is logged as an ERROR (it can't be reported to the game).
        Container* c = self(i);
        c->add_ref();
        queue_commit([c, writes = std::move(writes), del = std::move(del)] {
            double t0 = run_time();
            size_t bytes = 0;
            for (auto& [k, v] : writes) bytes += v.size();
            HRESULT hr = c->commit(writes, del);
            log_write("winrt", "ConnectedStorage: %s: wrote %zu blob(s) (%zu KiB), deleted %zu in %.0f ms -> 0x%08x",
                      utf16_to_utf8(c->name).c_str(), writes.size(), bytes >> 10, del.size(),
                      (run_time() - t0) * 1000, static_cast<uint32_t>(hr));
            if (hr < 0) FL_ERROR("ConnectedStorage: commit of %s failed", c->dir.c_str());
            c->release();
        });
        *out = action_done(S_OK);
        return S_OK;
    }
    // Container2 S+0 SubmitUpdatesAsync(writes, deletes, displayName)
    static WINAPI HRESULT SubmitUpdates2(Iface* i, void* writes_map, void* deletes, HSTRING, void** out) {
        return SubmitUpdates(i, writes_map, deletes, out);
    }

    // S+4 GetAsync(IIterable<String> blobsToRead) -> IMapView<String,IBuffer>
    static WINAPI HRESULT Get(Iface* i, void* names_iterable, void** out) {
        auto* c = self(i);
        std::vector<std::u16string> names = read_string_iterable(names_iterable);
        auto* map = new BufferMap();
        auto lock = io_lock();
        std::string parent = c->dir.substr(0, c->dir.rfind('/'));
        container_dir(parent, c->name);
        for (const auto& n : names) {
            std::vector<uint8_t> data;
            if (!read_file(c->dir + "/" + encode_name(n), &data)) {
                FL_LOG(winrt, "ConnectedStorage: %s/%s: no such blob", utf16_to_utf8(c->name).c_str(),
                       utf16_to_utf8(n).c_str());
                map->release();
                *out = async_failed(IID_IAsyncOp_BufferMap, kBlobNotFound);
                return S_OK;
            }
            auto* b = new Buffer(uint32_t(data.size()));
            memcpy(b->bytes.data(), data.data(), data.size());
            b->length = uint32_t(data.size());
            map->items.push_back({n, b});
        }
        std::sort(map->items.begin(), map->items.end(), [](auto& a, auto& b) { return a.first < b.first; });
        FL_LOG(winrt, "ConnectedStorage: %s: read %zu blob(s)", utf16_to_utf8(c->name).c_str(), names.size());
        *out = async_result_ptr(IID_IAsyncOp_BufferMap, map->default_iface_noref());
        return S_OK;
    }

    static Vtable& vt() {
        static Vtable v = Vtable("IConnectedStorageContainer", S + 6)
                              .set(S + 0, &get_Name)
                              .set(S + 1, &get_OwningSpace)
                              .set(S + 2, &SubmitUpdates)
                              .set(S + 4, &Get);  // S+3 ReadAsync, S+5 property sets: unused by the game
        return v;
    }
    static Vtable& vt2() {
        static Vtable v = Vtable("IConnectedStorageContainer2", S + 3).set(S + 0, &SubmitUpdates2);
        return v;
    }
    Iface icontainer{vt(), this};
    Iface icontainer2{vt2(), this};
    Container(Space* s, std::u16string n, std::string d);
    ~Container() override;
};

// ---------------------------------------------------------------- space

struct Space : Object {
    void* user;  // IUser (one reference)
    std::string dir;

    static Space* self(Iface* i) { return i->as<Space>(); }
    static WINAPI HRESULT get_User(Iface* i, void** out) {
        com_add_ref(self(i)->user);
        return *out = self(i)->user, S_OK;
    }
    static WINAPI HRESULT get_ServiceConfigurationId(Iface*, HSTRING* out) {
        static std::u16string scid = xbox_live_manifest_attr("PrimaryServiceConfigId");
        return make_hstring(scid.c_str(), out);
    }
    // S+2: a bool the reference answers false (name unknown).
    static WINAPI HRESULT get_Flag(Iface*, uint8_t* out) { return *out = 0, S_OK; }
    static WINAPI HRESULT CreateContainer(Iface* i, HSTRING name, void** out) {
        std::u16string n = hstring_str(name);
        if (n.empty()) return *out = nullptr, E_INVALIDARG;
        *out = (new Container(self(i), n, self(i)->dir + "/" + encode_name(n)))->default_iface_noref();
        return S_OK;
    }
    static WINAPI HRESULT DeleteContainer(Iface* i, HSTRING name, void** out) {
        std::u16string n = hstring_str(name);
        {
            auto lock = io_lock();
            remove_tree(container_dir(self(i)->dir, n));
        }
        FL_LOG(winrt, "ConnectedStorage: deleted container %s", utf16_to_utf8(n).c_str());
        *out = action_done();
        return S_OK;
    }
    static WINAPI HRESULT CreateContainerInfoQuery(Iface* i, HSTRING prefix, void** out) {
        *out = (new InfoQuery(self(i)->dir, hstring_str(prefix)))->default_iface_noref();
        return S_OK;
    }
    int64_t remaining() {
        uint64_t used = 0;
        for (const InfoEntry& e : list_containers(dir, u"")) used += e.size;
        return std::max<int64_t>(0, kQuotaBytes - int64_t(used));
    }
    static WINAPI HRESULT GetRemainingBytesInQuota(Iface* i, void** out) {
        FL_STUB_ONCE("ConnectedStorage quota: assumed 64 MiB per user");
        int64_t r = std::min<int64_t>(self(i)->remaining(), INT32_MAX);
        *out = async_result_u32(IID_IAsyncOp_Int32, uint32_t(r));
        return S_OK;
    }
    static WINAPI HRESULT GetRemainingBytesInQuota64(Iface* i, void** out) {
        FL_STUB_ONCE("ConnectedStorage quota: assumed 64 MiB per user");
        int64_t r = self(i)->remaining();
        *out = (new AsyncOperation(IID_IAsyncOp_Int64, [r](void* o) { memcpy(o, &r, 8); }))->default_iface_noref();
        return S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IConnectedStorageSpace", S + 7)
                              .set(S + 0, &get_User)
                              .set(S + 1, &get_ServiceConfigurationId)
                              .set(S + 2, &get_Flag)
                              .set(S + 3, &CreateContainer)
                              .set(S + 4, &DeleteContainer)
                              .set(S + 5, &CreateContainerInfoQuery)
                              .set(S + 6, &GetRemainingBytesInQuota);
        return v;
    }
    static Vtable& vt2() {
        static Vtable v = Vtable("IConnectedStorageSpace2", S + 1).set(S + 0, &GetRemainingBytesInQuota64);
        return v;
    }
    Iface ispace{vt(), this};
    Iface ispace2{vt2(), this};
    Space(void* u, std::string d) : Object(u"Windows.Xbox.Storage.ConnectedStorageSpace"), user(u), dir(std::move(d)) {
        com_add_ref(user);
        expose(IID_IConnectedStorageSpace, &ispace);
        expose(IID_IConnectedStorageSpace2, &ispace2);
    }
    ~Space() override { com_release(user); }
};

Container::Container(Space* s, std::u16string n, std::string d)
    : Object(u"Windows.Xbox.Storage.ConnectedStorageContainer"), space(s), name(std::move(n)), dir(std::move(d)) {
    space->add_ref();
    expose(IID_IConnectedStorageContainer, &icontainer);
    expose(IID_IConnectedStorageContainer2, &icontainer2);
}
Container::~Container() { space->release(); }

WINAPI HRESULT Container::get_OwningSpace(Iface* i, void** out) {
    *out = self(i)->space->default_iface();
    return S_OK;
}

// ---------------------------------------------------------------- statics

struct SpaceStatics : Object {
    // S+0 GetForUserAsync(IUser): the user's local space, keyed by XUID.
    static WINAPI HRESULT GetForUser(Iface*, void* user, void** out) {
        HSTRING xuid = nullptr;
        if (!user || call_slot<HRESULT>(user, S + 9 /* IUser::get_XboxUserId */, &xuid) < 0) {
            *out = async_failed(IID_IAsyncOp_Space, E_INVALIDARG);
            return S_OK;
        }
        std::string id = utf16_to_utf8(hstring_str(xuid));
        std::string dir = config().data_dir + "/saves/" + (id.empty() ? std::string("local") : id);
        if (!mkdirs(dir)) FL_ERROR("ConnectedStorage: cannot create %s", dir.c_str());
        FL_LOG(winrt, "ConnectedStorageSpace::GetForUserAsync -> %s", dir.c_str());
        *out = async_result_ptr(IID_IAsyncOp_Space, (new Space(user, dir))->default_iface_noref());
        return S_OK;
    }
    static Vtable& vt() {
        // S+1 (user, scid), S+2 GetForMachineAsync(), S+3 (scid): not used by this game; they trap.
        static Vtable v = Vtable("IConnectedStorageSpaceStatics", S + 4).set(S + 0, &GetForUser);
        return v;
    }
    static Vtable& vt2() {
        static Vtable v = Vtable("IConnectedStorageSpaceStatics2", S + 2);
        return v;
    }
    Iface istatics{vt(), this};
    Iface istatics2{vt2(), this};
    SpaceStatics() : Object(u"Windows.Xbox.Storage.ConnectedStorageSpace") {
        expose(IID_IConnectedStorageSpaceStatics, &istatics);
        expose(IID_IConnectedStorageSpaceStatics2, &istatics2);
    }
};

HRESULT get_space_factory(const GUID& iid, void** out) {
    static auto* s = new SpaceStatics();
    return s->query(iid, out);
}

}  // namespace

WINRT_REGISTER_CLASS(u"Windows.Xbox.Storage.ConnectedStorageSpace", get_space_factory);
WINRT_REGISTER_CLASS(u"Windows.Storage.Streams.Buffer", get_buffer_factory);

}  // namespace fl::winrt
