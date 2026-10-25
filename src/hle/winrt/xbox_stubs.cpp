// Windows.Xbox.Management.Deployment. Slot signatures from gameosproxy.dll's
// proxy data (FINDINGS §4c). The whole package is on disk, so every chunk is
// installed and transfer events never fire. Unknown slots trap.

#include <cstdlib>
#include <cstdio>
#include <set>
#include <cstring>
#include <vector>
#include "common/config.h"
#include "common/log.h"
#include "hle/winrt/activation.h"
#include "hle/winrt/collections.h"
#include "hle/winrt/object.h"

namespace fl::winrt {
namespace {

constexpr int S = kInspectableSlots;

// ---------------------------------------------------------------- PackageTransferManager
// Statics iid {D6CF4CAE-9EB1-4CBE-B426-7E75D429F846}; slot S+0 returns the
// current manager (call site rva 0x11d3874). The instance interface is unknown:
// all its slots trap until the game uses them.

constexpr GUID IID_IPackageTransferManagerStatics = {0xD6CF4CAE, 0x9EB1, 0x4CBE,
                                                     {0xB4, 0x26, 0x7E, 0x75, 0xD4, 0x29, 0xF8, 0x46}};
constexpr GUID IID_IPackageTransferManager = {0xC2D4B94B, 0x0413, 0x4380, {0xB2, 0xF2, 0xAB, 0x98, 0x72, 0xD0, 0x82, 0x85}};
constexpr GUID IID_IPackageTransferWatcherStatics = {0xA306E506, 0x12F9, 0x460A,
                                                     {0xB0, 0xAE, 0x50, 0xBA, 0x32, 0x8C, 0x78, 0x6C}};
constexpr GUID IID_IPackageTransferWatcher = {0xE74B8E43, 0xBA51, 0x4B09, {0xAD, 0x3F, 0x01, 0xBA, 0xFF, 0x31, 0xF9, 0x34}};

// The package's chunks, from Mount/hash.manifest: 12-byte records from
// offset 8, each starting with a u32 chunk id; ids >= 0x3fffffff are not
// content chunks (FINDINGS §5b; Azimuth builds its installed-chunk set the
// same way). The whole dump is on disk, so every listed chunk is installed.
const std::set<uint32_t>& installed_chunks() {
    static const std::set<uint32_t> chunks = [] {
        std::set<uint32_t> out;
        std::string path = fl::config().game_dir + "/hash.manifest";
        if (FILE* f = fopen(path.c_str(), "rb")) {
            std::vector<uint8_t> d;
            uint8_t buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
            fclose(f);
            for (size_t off = 8; off + 4 <= d.size(); off += 12) {
                uint32_t id;
                memcpy(&id, d.data() + off, 4);
                if (id < 0x3fffffff) out.insert(id);
            }
        }
        if (out.empty()) FL_ERROR("PackageTransferManager: no chunk list in %s", path.c_str());
        FL_LOG(winrt, "PackageTransferManager: %zu installed chunks from hash.manifest", out.size());
        return out;
    }();
    return chunks;
}

struct PackageTransferManager : Object {
    // S+1: HRESULT (uint32 chunk_id, bool* installed). The caller caches positive
    // answers (rva 0x11d03c4). The whole package is on disk, so: always installed.
    static WINAPI HRESULT IsChunkInstalled(Iface*, uint32_t chunk, uint8_t* out) {
        // Unknown ids answer "installed" too: the dump has every file.
        FL_LOG(winrt, "PackageTransferManager: chunk %u installed? -> yes%s", chunk,
               installed_chunks().count(chunk) ? "" : " (not in hash.manifest)");
        *out = 1;
        return S_OK;
    }
    // S+2: (IIterable<uint32> chunk ids, bool* installed).
    static WINAPI HRESULT AreChunksInstalled(Iface*, void* chunks, uint8_t* out) {
        *out = 1;
        return S_OK;
    }
    // S+0: (IIterable<uint32> chunk ids, enum) and nothing out (proxy data):
    // a request to move those chunks up the install order (game rva 0x11d0b1a,
    // once the FMOD audio engine runs). Everything is installed, so there is
    // nothing to reorder.
    static WINAPI HRESULT UpdateInstallOrder(Iface*, void* chunks, int32_t behavior) {
        FL_LOG(winrt, "PackageTransferManager: install order request (behavior %d): all installed", behavior);
        return S_OK;
    }
    static Vtable& vt() {
        static Vtable v =
            Vtable("IPackageTransferManager", 11)
                .set(S + 0, &UpdateInstallOrder)
                .set(S + 1, &IsChunkInstalled)
                .set(S + 2, &AreChunksInstalled);
        return v;
    }
    Iface imgr{vt(), this};
    PackageTransferManager() : Object(u"Windows.Xbox.Management.Deployment.PackageTransferManager") {
        expose(IID_IPackageTransferManager, &imgr);
    }
};

struct PackageTransferManagerStatics : Object {
    static WINAPI HRESULT get_Current(Iface*, void** out) {
        static PackageTransferManager* mgr = new PackageTransferManager();
        FL_LOG(winrt, "PackageTransferManager::Current");
        *out = mgr->default_iface();
        return S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IPackageTransferManagerStatics", S + 8).set(S + 0, &get_Current);
        return v;
    }
    Iface istatics{vt(), this};
    PackageTransferManagerStatics() : Object(u"Windows.Xbox.Management.Deployment.PackageTransferManager") {
        expose(IID_IPackageTransferManagerStatics, &istatics);
    }
};

// IChunkCompletedEventArgs {C1B7DCCE-7CCC-4A06-BA46-4B90E6A87D0B}: S+0 ChunkId.
constexpr GUID IID_IChunkCompletedEventArgs = {0xC1B7DCCE, 0x7CCC, 0x4A06, {0xBA, 0x46, 0x4B, 0x90, 0xE6, 0xA8, 0x7D, 0x0B}};
struct ChunkCompletedEventArgs : Object {
    uint32_t chunk;
    static WINAPI HRESULT get_ChunkId(Iface* i, uint32_t* out) {
        *out = i->as<ChunkCompletedEventArgs>()->chunk;
        return S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IChunkCompletedEventArgs", S + 1).set(S + 0, &get_ChunkId);
        return v;
    }
    Iface iargs{vt(), this};
    explicit ChunkCompletedEventArgs(uint32_t c)
        : Object(u"Windows.Xbox.Management.Deployment.ChunkCompletedEventArgs"), chunk(c) {
        expose(IID_IChunkCompletedEventArgs, &iargs);
    }
};

// ---------------------------------------------------------------- PackageTransferWatcher
// IPackageTransferWatcher (13 slots): S+0 chunk ids, then ChunkCompleted,
// ProgressChanged, TransferCompleted add/remove pairs. Nothing is transferring.
struct PackageTransferWatcher : Object {
    EventSource events[3];
    template <int E>
    static WINAPI HRESULT add(Iface* i, void* h, EventToken* t) {
        auto* w = i->as<PackageTransferWatcher>();
        HRESULT hr = w->events[E].add(h, t);
        // A whole-package watcher reports every chunk of the (fully installed)
        // package as completed when the game subscribes. The game keeps an
        // installed-chunk set fed by these events (handler rva 0x11ceaa0) and
        // its FMOD bank loader only consults that set (rva 0x1012110 ->
        // 0x11d0470): without it MasterBank.bank never loads, the title's
        // BASE_PressStartMusic snapshot never starts and the splash never ends.
        if (E == 0 && hr >= 0 && w->whole_package) {
            const auto& chunks = installed_chunks();
            FL_LOG(winrt, "PackageTransferWatcher: reporting %zu chunks completed", chunks.size());
            for (uint32_t c : chunks) {
                auto* args = new ChunkCompletedEventArgs(c);
                winrt::call_slot<HRESULT>(h, 3, &w->iwatcher, args->default_iface_noref());
                args->release();
            }
        }
        return hr;
    }
    template <int E>
    static WINAPI HRESULT remove(Iface* i, EventToken t) {
        return i->as<PackageTransferWatcher>()->events[E].remove(t);
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IPackageTransferWatcher", 13)
                              .set(S + 1, &add<0>)
                              .set(S + 2, &remove<0>)
                              .set(S + 3, &add<1>)
                              .set(S + 4, &remove<1>)
                              .set(S + 5, &add<2>)
                              .set(S + 6, &remove<2>);
        return v;
    }
    Iface iwatcher{vt(), this};
    bool whole_package = false;
    PackageTransferWatcher() : Object(u"Windows.Xbox.Management.Deployment.PackageTransferWatcher") {
        expose(IID_IPackageTransferWatcher, &iwatcher);
    }
};

// IPackageTransferWatcherStatics: S+0 Create(package, IIterable<uint32> chunks, out), S+1 Create(package, out).
struct PackageTransferWatcherStatics : Object {
    static WINAPI HRESULT CreateForChunks(Iface*, void* package, void* chunks, void** out) {
        FL_LOG(winrt, "PackageTransferWatcher::Create (chunks): nothing to watch, all installed");
        return *out = (new PackageTransferWatcher())->default_iface_noref(), S_OK;
    }
    static WINAPI HRESULT Create(Iface*, void* package, void** out) {
        FL_LOG(winrt, "PackageTransferWatcher::Create: nothing to watch, all installed");
        auto* w = new PackageTransferWatcher();
        w->whole_package = true;
        return *out = w->default_iface_noref(), S_OK;
    }
    static Vtable& vt() {
        static Vtable v =
            Vtable("IPackageTransferWatcherStatics", 8).set(S + 0, &CreateForChunks).set(S + 1, &Create);
        return v;
    }
    Iface istatics{vt(), this};
    PackageTransferWatcherStatics() : Object(u"Windows.Xbox.Management.Deployment.PackageTransferWatcher") {
        expose(IID_IPackageTransferWatcherStatics, &istatics);
    }
};

// ---------------------------------------------------------------- DownloadableContentPackageManager
// Created with `ref new` (IActivationFactory::ActivateInstance). No DLC is
// installed: FindPackages returns an empty list; install events never fire.
constexpr GUID IID_IDlcPackageManager = {0x971FEAAF, 0x98F8, 0x4038, {0xB8, 0xC1, 0x3A, 0x5F, 0x1A, 0x04, 0x3B, 0x97}};
constexpr GUID IID_IVectorView_IDlcPackage = {0xA4BC9CE8, 0xD7A3, 0x5781, {0x8D, 0xB4, 0xD2, 0x72, 0x07, 0x57, 0xA8, 0x72}};

struct DlcPackageManager : Object {
    EventSource events[2];
    // S+0: (enum32 scope, out IVectorView<IDownloadableContentPackage>).
    static WINAPI HRESULT FindPackages(Iface*, int32_t scope, void** out) {
        FL_LOG(winrt, "DownloadableContentPackageManager::FindPackages(%d) -> none", scope);
        return *out = (new VectorView(IID_IVectorView_IDlcPackage, {}))->default_iface_noref(), S_OK;
    }
    template <int E>
    static WINAPI HRESULT add(Iface* i, void* h, EventToken* t) {
        return i->as<DlcPackageManager>()->events[E].add(h, t);
    }
    template <int E>
    static WINAPI HRESULT remove(Iface* i, EventToken t) {
        return i->as<DlcPackageManager>()->events[E].remove(t);
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IDownloadableContentPackageManager", 11)
                              .set(S + 0, &FindPackages)
                              .set(S + 1, &add<0>)
                              .set(S + 2, &remove<0>)
                              .set(S + 3, &add<1>)
                              .set(S + 4, &remove<1>);
        return v;
    }
    Iface imgr{vt(), this};
    DlcPackageManager() : Object(u"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager") {
        expose(IID_IDlcPackageManager, &imgr);
    }
};

HRESULT get_dlc(const GUID& iid, void** out) {
    static auto* f = new ActivationFactory(u"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager",
                                           [] { return (new DlcPackageManager())->default_iface_noref(); });
    return f->query(iid, out);
}

HRESULT get_ptw(const GUID& iid, void** out) {
    static auto* s = new PackageTransferWatcherStatics();
    return s->query(iid, out);
}

HRESULT get_ptm(const GUID& iid, void** out) {
    static auto* s = new PackageTransferManagerStatics();
    return s->query(iid, out);
}

}  // namespace

WINRT_REGISTER_CLASS(u"Windows.Xbox.Management.Deployment.PackageTransferManager", get_ptm);
WINRT_REGISTER_CLASS(u"Windows.Xbox.Management.Deployment.PackageTransferWatcher", get_ptw);
WINRT_REGISTER_CLASS(u"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager", get_dlc);

}  // namespace fl::winrt
