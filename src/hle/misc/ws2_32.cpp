// ws2_32: offline networking. The project runs the game without Xbox Live,
// so every attempt to create or use a socket fails with WSAENETDOWN.
// Byte-order helpers and address parsing work for real.

#include <arpa/inet.h>
#include <cstring>

#include "common/log.h"
#include "common/winabi.h"
#include "hle/kernelx/kobject.h"
#include "hle/kernelx/sync_objects.h"
#include "hle/registry.h"

namespace fl::ws2 {
namespace {

using SOCKET = uint64_t;
constexpr SOCKET INVALID_SOCKET = ~0ull;
constexpr int SOCKET_ERROR = -1;
constexpr int WSAENOTSOCK = 10038;
constexpr int WSAENETDOWN = 10050;
constexpr int WSAHOST_NOT_FOUND = 11001;

thread_local int t_wsa_error = 0;

int fail(int err) {
    t_wsa_error = err;
    return SOCKET_ERROR;
}

struct WsaData {
    WORD wVersion;
    WORD wHighVersion;
    uint16_t iMaxSockets;
    uint16_t iMaxUdpDg;
    char* lpVendorInfo;
    char szDescription[257];
    char szSystemStatus[129];
};

WINAPI int w_WSAStartup(WORD version, WsaData* data) {
    fl::log_write("net", "WSAStartup(0x%04x): networking is offline", version);
    if (data) {
        memset(data, 0, sizeof *data);
        data->wVersion = version;
        data->wHighVersion = 0x0202;
        strcpy(data->szDescription, "forza-linux offline winsock");
        strcpy(data->szSystemStatus, "Offline");
    }
    return 0;
}

WINAPI int w_WSACleanup() { return 0; }
WINAPI int w_WSAGetLastError() { return t_wsa_error; }
WINAPI void w_WSASetLastError(int e) { t_wsa_error = e; }

WINAPI SOCKET w_socket(int af, int type, int proto) {
    FL_LOG(net, "socket(%d, %d, %d) -> offline", af, type, proto);
    t_wsa_error = WSAENETDOWN;
    return INVALID_SOCKET;
}

WINAPI SOCKET w_WSASocketW(int af, int type, int proto, void* info, uint32_t group, DWORD flags) {
    return w_socket(af, type, proto);
}

// Anything that needs a real socket.
WINAPI int w_no_socket() { return fail(WSAENOTSOCK); }
WINAPI SOCKET w_accept(SOCKET, void*, int*) {
    t_wsa_error = WSAENOTSOCK;
    return INVALID_SOCKET;
}

WINAPI int w_select(int n, void* r, void* w, void* e, const void* timeout) { return fail(WSAENETDOWN); }
WINAPI int w_FDIsSet(SOCKET s, void* set) { return 0; }

WINAPI void* w_gethostbyname(const char* name) {
    FL_LOG(net, "gethostbyname(%s) -> offline", name ? name : "(null)");
    t_wsa_error = WSAHOST_NOT_FOUND;
    return nullptr;
}
WINAPI void* w_gethostbyaddr(const char*, int, int) {
    t_wsa_error = WSAHOST_NOT_FOUND;
    return nullptr;
}

WINAPI int w_getaddrinfo(const char* node, const char* service, const void* hints, void** res) {
    FL_LOG(net, "getaddrinfo(%s) -> offline", node ? node : "(null)");
    if (res) *res = nullptr;
    t_wsa_error = WSAHOST_NOT_FOUND;
    return WSAHOST_NOT_FOUND;
}
WINAPI void w_freeaddrinfo(void*) {}
WINAPI int w_getnameinfo(const void*, int, char*, DWORD, char*, DWORD, int) { return WSAHOST_NOT_FOUND; }

WINAPI uint32_t w_htonl(uint32_t v) { return htonl(v); }
WINAPI uint16_t w_htons(uint16_t v) { return htons(v); }
WINAPI uint32_t w_ntohl(uint32_t v) { return ntohl(v); }
WINAPI uint16_t w_ntohs(uint16_t v) { return ntohs(v); }
WINAPI uint32_t w_inet_addr(const char* s) { return inet_addr(s); }

WINAPI char* w_inet_ntoa(uint32_t addr) {
    static thread_local char buf[16];
    auto* b = reinterpret_cast<uint8_t*>(&addr);
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

// WSA events are ordinary manual-reset kernel events.
WINAPI HANDLE w_WSACreateEvent() { return kx::create_event(true, false); }
WINAPI BOOL w_WSACloseEvent(HANDLE h) { return kx::handle_close(h); }
WINAPI BOOL w_WSASetEvent(HANDLE h) { return kx::signal_event(h); }
WINAPI BOOL w_WSAResetEvent(HANDLE h) { return kx::reset_event(h); }
WINAPI DWORD w_WSAWaitForMultipleEvents(DWORD n, const HANDLE* h, BOOL all, DWORD ms, BOOL alertable) {
    return kx::wait_multiple(n, h, all, ms);
}

WINAPI BOOL w_WSAGetOverlappedResult(SOCKET, void*, DWORD*, BOOL, DWORD*) {
    t_wsa_error = WSAENOTSOCK;
    return FALSE_;
}

}  // namespace

HLE_EXPORT(ws2_32, WSAStartup, w_WSAStartup);
HLE_EXPORT(ws2_32, WSACleanup, w_WSACleanup);
HLE_EXPORT(ws2_32, WSAGetLastError, w_WSAGetLastError);
HLE_EXPORT(ws2_32, WSASetLastError, w_WSASetLastError);
HLE_EXPORT(ws2_32, socket, w_socket);
HLE_EXPORT(ws2_32, WSASocketW, w_WSASocketW);
HLE_EXPORT(ws2_32, accept, w_accept);
HLE_EXPORT(ws2_32, select, w_select);
HLE_EXPORT(ws2_32, __WSAFDIsSet, w_FDIsSet);
HLE_EXPORT(ws2_32, gethostbyname, w_gethostbyname);
HLE_EXPORT(ws2_32, gethostbyaddr, w_gethostbyaddr);
HLE_EXPORT(ws2_32, getaddrinfo, w_getaddrinfo);
HLE_EXPORT(ws2_32, freeaddrinfo, w_freeaddrinfo);
HLE_EXPORT(ws2_32, getnameinfo, w_getnameinfo);
HLE_EXPORT(ws2_32, htonl, w_htonl);
HLE_EXPORT(ws2_32, htons, w_htons);
HLE_EXPORT(ws2_32, ntohl, w_ntohl);
HLE_EXPORT(ws2_32, ntohs, w_ntohs);
HLE_EXPORT(ws2_32, inet_addr, w_inet_addr);
HLE_EXPORT(ws2_32, inet_ntoa, w_inet_ntoa);
HLE_EXPORT(ws2_32, WSACreateEvent, w_WSACreateEvent);
HLE_EXPORT(ws2_32, WSACloseEvent, w_WSACloseEvent);
HLE_EXPORT(ws2_32, WSASetEvent, w_WSASetEvent);
HLE_EXPORT(ws2_32, WSAResetEvent, w_WSAResetEvent);
HLE_EXPORT(ws2_32, WSAWaitForMultipleEvents, w_WSAWaitForMultipleEvents);
HLE_EXPORT(ws2_32, WSAGetOverlappedResult, w_WSAGetOverlappedResult);
// All of these need a valid socket, and none can exist.
HLE_EXPORT(ws2_32, bind, w_no_socket);
HLE_EXPORT(ws2_32, closesocket, w_no_socket);
HLE_EXPORT(ws2_32, connect, w_no_socket);
HLE_EXPORT(ws2_32, getpeername, w_no_socket);
HLE_EXPORT(ws2_32, getsockname, w_no_socket);
HLE_EXPORT(ws2_32, getsockopt, w_no_socket);
HLE_EXPORT(ws2_32, ioctlsocket, w_no_socket);
HLE_EXPORT(ws2_32, listen, w_no_socket);
HLE_EXPORT(ws2_32, recv, w_no_socket);
HLE_EXPORT(ws2_32, recvfrom, w_no_socket);
HLE_EXPORT(ws2_32, send, w_no_socket);
HLE_EXPORT(ws2_32, sendto, w_no_socket);
HLE_EXPORT(ws2_32, setsockopt, w_no_socket);
HLE_EXPORT(ws2_32, shutdown, w_no_socket);
HLE_EXPORT(ws2_32, WSASend, w_no_socket);
HLE_EXPORT(ws2_32, WSARecv, w_no_socket);
HLE_EXPORT(ws2_32, WSASendTo, w_no_socket);
HLE_EXPORT(ws2_32, WSARecvFrom, w_no_socket);
HLE_EXPORT(ws2_32, WSAEventSelect, w_no_socket);

}  // namespace fl::ws2
