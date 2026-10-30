#include "loader/imports.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "common/log.h"
#include "common/winabi.h"
#include "hle/registry.h"
#include "loader/crash.h"
#include "loader/pe_structs.h"
#include "platform/os/memory.h"

extern "C" void fl_trace_entry();

namespace fl::loader {
namespace {

std::deque<ImportInfo> g_infos;  // deque: stable addresses
std::unordered_map<std::string, void*> g_dynamic;
std::mutex g_mutex;
int g_trace = 0;  // 0 off, 1 first call, 2 every call

// Executable thunk arena.
uint8_t* g_arena = nullptr;
size_t g_arena_used = 0;
constexpr size_t kArenaSize = 1 << 20;

uint8_t* alloc_thunk(size_t n) {
    if (!g_arena) {
        // Writable first, then executable: the PS5 refuses execute at map time.
        g_arena = static_cast<uint8_t*>(os::map_anon(nullptr, kArenaSize, os::kRead | os::kWrite));
        if (!g_arena || !os::protect(g_arena, kArenaSize, os::kRead | os::kWrite | os::kExec))
            fatal("thunk arena mmap failed");
    }
    if (g_arena_used + n > kArenaSize) fatal("thunk arena exhausted");
    uint8_t* p = g_arena + g_arena_used;
    g_arena_used += (n + 15) & ~size_t(15);
    return p;
}

void emit_mov_imm64(uint8_t*& p, uint8_t rex, uint8_t op, uint64_t v) {
    *p++ = rex;
    *p++ = op;
    memcpy(p, &v, 8);
    p += 8;
}

[[noreturn]] WINAPI void unimplemented_trap(ImportInfo* info, void* ret) {
    uint64_t rva = main_image().contains(ret) ? main_image().rva_of(ret) : 0;
    fatal("UNIMPLEMENTED %s!%s (called from %p, game rva 0x%llx)", info->dll.c_str(), info->name.c_str(), ret,
          static_cast<unsigned long long>(rva));
}

// mov rcx, info ; mov rdx, [rsp] ; mov rax, unimplemented_trap ; jmp rax
void* make_trap(ImportInfo* info) {
    uint8_t* start = alloc_thunk(32);
    uint8_t* p = start;
    emit_mov_imm64(p, 0x48, 0xB9, reinterpret_cast<uint64_t>(info));
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0x14; *p++ = 0x24;
    emit_mov_imm64(p, 0x48, 0xB8, reinterpret_cast<uint64_t>(&unimplemented_trap));
    *p++ = 0xFF; *p++ = 0xE0;
    return start;
}

// mov r11, info ; mov rax, fl_trace_entry ; jmp rax
void* make_trace_thunk(ImportInfo* info) {
    uint8_t* start = alloc_thunk(32);
    uint8_t* p = start;
    emit_mov_imm64(p, 0x49, 0xBB, reinterpret_cast<uint64_t>(info));
    emit_mov_imm64(p, 0x48, 0xB8, reinterpret_cast<uint64_t>(&fl_trace_entry));
    *p++ = 0xFF; *p++ = 0xE0;
    return start;
}

// MSVC-mangled names for variables: "?name@scope@@3..." (global) or "...@@2..."
// (static member), and "??_7...@6B@" (vftable). Plus known CRT data symbols.
bool is_data_import(const std::string& name) {
    if (name.rfind("??_7", 0) == 0) return true;
    static const char* const kCData[] = {"_acmdln", "_wcmdln", "_fmode", "_commode", "_FInf", "_FNan",
                                         "_Inf", "_Nan", "_Denorm", "_FDenorm", "__argc", "__argv"};
    for (const char* d : kCData)
        if (name == d) return true;
    if (name.size() > 2 && name[0] == '?' && name[1] != '?') {
        size_t at = name.find("@@");
        if (at != std::string::npos && at + 2 < name.size() && (name[at + 2] == '3' || name[at + 2] == '2'))
            return true;
    }
    return false;
}

// ws2_32 is imported by ordinal; these are the standard Winsock ordinals.
std::string ordinal_name(const std::string& dll, const std::string& name) {
    if (name.empty() || name[0] != '#' || hle::normalize_dll(dll) != "ws2_32") return name;
    static const std::pair<int, const char*> kWs2[] = {
        {1, "accept"}, {2, "bind"}, {3, "closesocket"}, {4, "connect"}, {5, "getpeername"}, {6, "getsockname"},
        {7, "getsockopt"}, {8, "htonl"}, {9, "htons"}, {10, "ioctlsocket"}, {11, "inet_addr"}, {12, "inet_ntoa"},
        {13, "listen"}, {14, "ntohl"}, {15, "ntohs"}, {16, "recv"}, {17, "recvfrom"}, {18, "select"}, {19, "send"},
        {20, "sendto"}, {21, "setsockopt"}, {22, "shutdown"}, {23, "socket"}, {51, "gethostbyaddr"},
        {52, "gethostbyname"}, {57, "gethostname"}, {111, "WSAGetLastError"}, {112, "WSASetLastError"},
        {115, "WSAStartup"}, {116, "WSACleanup"}, {151, "__WSAFDIsSet"}};
    int ord = std::stoi(name.substr(1));
    for (auto& [o, n] : kWs2)
        if (o == ord) return n;
    return name;
}

void* bind_import(const std::string& dll, const std::string& raw_name, bool* resolved) {
    std::string name = ordinal_name(dll, raw_name);
    void* fn = hle::lookup(dll, name);
    *resolved = fn != nullptr;
    if (is_data_import(name)) {
        // Data imports point at the object itself; never wrap them in a thunk.
        return fn ? fn : make_data_trap(hle::normalize_dll(dll), name);
    }
    ImportInfo& info = g_infos.emplace_back();
    info.dll = hle::normalize_dll(dll);
    info.name = name;
    info.target = fn ? fn : make_trap(&info);
    if (fn && g_trace) return make_trace_thunk(&info);
    return info.target;
}

}  // namespace

int resolve_imports(PeImage& img) {
    if (const char* t = getenv("FL_TRACE_IMPORTS")) g_trace = strcmp(t, "all") == 0 ? 2 : 1;
    std::lock_guard lock(g_mutex);
    int missing = 0, total = 0;
    for (auto* d = img.at<ImageImportDescriptor>(img.import_dir_rva); d->Name; d++) {
        std::string dll = img.at<char>(d->Name);
        auto* names = img.at<uint64_t>(d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk);
        auto* iat = img.at<uint64_t>(d->FirstThunk);
        int dll_missing = 0, dll_total = 0;
        for (size_t i = 0; names[i]; i++) {
            std::string name;
            if (names[i] & (1ull << 63))
                name = "#" + std::to_string(names[i] & 0xFFFF);
            else
                name = img.at<char>(static_cast<uint32_t>(names[i]) + 2);  // skip Hint
            bool ok;
            iat[i] = reinterpret_cast<uint64_t>(bind_import(dll, name, &ok));
            dll_total++;
            if (!ok) {
                dll_missing++;
                FL_LOG(missing, "%s!%s", dll.c_str(), name.c_str());
            }
        }
        FL_LOG(loader, "imports %-20s %3d/%3d implemented", dll.c_str(), dll_total - dll_missing, dll_total);
        missing += dll_missing;
        total += dll_total;
    }
    fl::log_write("loader", "imports: %d/%d implemented (%zu HLE exports registered)%s", total - missing, total,
                  hle::count(), g_trace ? ", tracing on" : "");
    return missing;
}

void* make_named_trap(const std::string& what, const std::string& name) {
    std::lock_guard lock(g_mutex);
    ImportInfo& info = g_infos.emplace_back();
    info.dll = what;
    info.name = name;
    info.target = make_trap(&info);
    return info.target;
}

void* resolve_dynamic(const std::string& dll, const std::string& name) {
    std::lock_guard lock(g_mutex);
    std::string key = hle::normalize_dll(dll) + "!" + name;
    auto it = g_dynamic.find(key);
    if (it != g_dynamic.end()) return it->second;
    if (!hle::lookup(dll, name)) return nullptr;
    bool ok;
    void* p = bind_import(dll, name, &ok);
    g_dynamic[key] = p;
    return p;
}

}  // namespace fl::loader

extern "C" WINAPI void fl_trace_hit(fl::loader::ImportInfo* info) {
    uint64_t n = info->calls.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || fl::loader::g_trace == 2)
        fl::log_write("import", "%s!%s%s", info->dll.c_str(), info->name.c_str(), n == 0 ? " (first call)" : "");
}
