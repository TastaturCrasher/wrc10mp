// wrc10mp - LAN discovery over the internet.
//
// WRC 10's LAN mode finds lobbies with a RakNet broadcast ping to UDP 60000
// and then connects to whoever answers. Broadcasts don't cross the internet,
// so this module hooks sendto/recvfrom in WRC10.exe's import table: every
// search broadcast is additionally sent by unicast to the host configured in
// wrc10mp.ini ([lan] host_ip, e.g. a Tailscale IP). The host answers directly
// and the game connects to it like to a LAN lobby. RakNet offline messages
// (search, pong, connection handshake) are logged to wrc10mp_net.log.
//
// A searching client only accepts a pong whose name (text before the first
// ';') equals its own "WRC10Kylo" + password. The password is "KyloPwd" for a
// private lobby, and the client takes it from the last lobby it created in
// this session, not from the search (WRC10.exe 1.6.32.2: pong handler at
// 0x140f98350, search start at 0x140a350f0). A freshly started client thus
// rejects every private lobby. For each lobby pong the hook also delivers a
// copy with the password added or removed, so one of the two always matches.
//
// Steam mode: RakNet's Lobby2 Steam stores lobby parameters as Steam lobby
// data with "%u" (0x140e86dc9) and reads them back with atoi (0x140e87320).
// A parameter of -1 ("any") turns into "4294967295", which atoi clamps to
// INT_MAX; the lobby menu then looks up a rally with that index and crashes
// (0x14061fc80 -> 0x1407223c0). The atoi hook ([steam] atoi_wrap=1) returns
// unsigned 32-bit numbers above INT_MAX as their int value instead.
//
// Optionally ([lan] alias=1) Tailscale peers (100.64.0.0/10) are shown to the
// game under an alias in the PC's own subnet (x.y.z.250, .249, ...). Packets
// the game sends to an alias go to the real Tailscale address. The game does
// not filter by address, so this is off by default.

#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "lan.h"

namespace {

const unsigned char kOfflineMagic[16] = {0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe,
                                         0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78};

using SendToFn = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using RecvFromFn = int(WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using AtoiFn = int(__cdecl*)(const char*);

SendToFn g_realSendTo = nullptr;
RecvFromFn g_realRecvFrom = nullptr;
AtoiFn g_realAtoi = nullptr;

sockaddr_in g_host{};  // unicast target for search pings, sin_port 0 = disabled
unsigned short g_lanPort = 60000;
FILE* g_netLog = nullptr;
std::mutex g_netMutex;

// Pong copies waiting to be returned by the next recvfrom on that socket.
struct PendingPacket {
    std::string data;
    sockaddr_in from;
};
bool g_pongVariants = true;
std::mutex g_pendingMutex;
std::map<SOCKET, PendingPacket> g_pending;

const char kLobbyName[] = "WRC10Kylo";
const char kLobbyPassword[] = "KyloPwd";
const int kPongHeader = 33;  // id, 8-byte time, 8-byte guid, 16-byte magic

// Alias table, addresses in host byte order. Guarded by g_aliasMutex.
bool g_aliasEnabled = false;
std::mutex g_aliasMutex;
std::map<unsigned long, unsigned long> g_realToAlias, g_aliasToReal;
unsigned long g_aliasSubnet = 0;  // x.y.z.0 of the PC's main address, 0 = not yet known
unsigned long g_nextAlias = 250;

bool IsTailscale(unsigned long ip) { return (ip & 0xFFC00000) == 0x64400000; }

// Address Windows would use to reach the internet; no packet is sent.
unsigned long PrimaryIp() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = htonl(0x01010101);
    sockaddr_in me{};
    int len = sizeof(me);
    unsigned long ip = 0;
    if (connect(s, (sockaddr*)&dst, sizeof(dst)) == 0 && getsockname(s, (sockaddr*)&me, &len) == 0) ip = ntohl(me.sin_addr.s_addr);
    closesocket(s);
    return ip;
}

// Caller holds g_aliasMutex.
unsigned long AliasForLocked(unsigned long real) {
    auto it = g_realToAlias.find(real);
    if (it != g_realToAlias.end()) return it->second;
    if (!g_aliasSubnet) {
        unsigned long me = PrimaryIp();
        g_aliasSubnet = (me && !IsTailscale(me)) ? (me & 0xFFFFFF00) : 0xC0A8FA00;  // fallback 192.168.250.0
        if ((me & 0xFF) >= 240) g_nextAlias = 239;  // keep clear of our own address
    }
    unsigned long alias = g_aliasSubnet | g_nextAlias--;
    g_realToAlias[real] = alias;
    g_aliasToReal[alias] = real;
    return alias;
}

std::string Addr(const sockaddr* sa) {
    if (!sa || sa->sa_family != AF_INET) return "?";
    auto* in = (const sockaddr_in*)sa;
    const unsigned char* b = (const unsigned char*)&in->sin_addr;
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], ntohs(in->sin_port));
    return buf;
}

const char* PacketName(unsigned char id) {
    switch (id) {
        case 0x01: return "UNCONNECTED_PING";
        case 0x02: return "UNCONNECTED_PING_OPEN";
        case 0x05: return "OPEN_CONNECTION_REQUEST_1";
        case 0x06: return "OPEN_CONNECTION_REPLY_1";
        case 0x07: return "OPEN_CONNECTION_REQUEST_2";
        case 0x08: return "OPEN_CONNECTION_REPLY_2";
        case 0x1C: return "UNCONNECTED_PONG";
        default: return nullptr;
    }
}

// RakNet datagrams of an established connection have the high bit set; only
// the low-volume offline messages (search and handshake) are logged.
void LogPacket(const char* dir, const sockaddr* peer, const char* data, int len, const char* note = "") {
    if (!g_netLog || len <= 0 || ((unsigned char)data[0] & 0x80)) return;
    const char* name = PacketName((unsigned char)data[0]);
    char id[16];
    if (!name) {
        snprintf(id, sizeof(id), "id 0x%02x", (unsigned char)data[0]);
        name = id;
    }
    std::string extra;
    if ((unsigned char)data[0] == 0x1C && len > 33) extra = " \"" + std::string(data + 33, len - 33) + "\"";
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> lock(g_netMutex);
    fprintf(g_netLog, "[%02d:%02d:%02d.%03d] %s %s %s (%d bytes)%s%s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, dir,
            Addr(peer).c_str(), name, len, extra.c_str(), note);
    fflush(g_netLog);
}

bool IsSearchPing(const char* data, int len) {
    return len >= 25 && (data[0] == 0x01 || data[0] == 0x02) && memcmp(data + 9, kOfflineMagic, 16) == 0;
}

bool IsBroadcast(const sockaddr_in* to) {
    return ((const unsigned char*)&to->sin_addr)[3] == 255;  // 255.255.255.255 or a subnet broadcast
}

int WSAAPI HookSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
    // Packets to an alias go to the real Tailscale address.
    sockaddr_in real{};
    const sockaddr* target = to;
    if (g_aliasEnabled && to && to->sa_family == AF_INET) {
        std::lock_guard<std::mutex> lock(g_aliasMutex);
        auto it = g_aliasToReal.find(ntohl(((const sockaddr_in*)to)->sin_addr.s_addr));
        if (it != g_aliasToReal.end()) {
            real = *(const sockaddr_in*)to;
            real.sin_addr.s_addr = htonl(it->second);
            target = (const sockaddr*)&real;
        }
    }
    int ret = g_realSendTo(s, buf, len, flags, target, tolen);
    int err = WSAGetLastError();
    std::string note = target != to ? " (alias of " + Addr(target) + ")" : "";
    LogPacket("send ->", to, buf, len, (note + (ret == SOCKET_ERROR ? " FAILED" : "")).c_str());
    if (g_host.sin_port && to && to->sa_family == AF_INET) {
        auto* in = (const sockaddr_in*)to;
        unsigned short port = ntohs(in->sin_port);
        // The search scans a port range (60000-60015), since a host whose
        // port is taken moves to the next one. Mirror each probe to the host.
        if (port >= g_lanPort && port < g_lanPort + 16 && IsBroadcast(in) && IsSearchPing(buf, len)) {
            sockaddr_in host = g_host;
            host.sin_port = in->sin_port;
            int r = g_realSendTo(s, buf, len, flags, (const sockaddr*)&host, sizeof(host));
            LogPacket("send ->", (const sockaddr*)&host, buf, len, r == SOCKET_ERROR ? " (copy to host) FAILED" : " (copy to host)");
        }
    }
    WSASetLastError(err);
    return ret;
}

// For a lobby pong "WRC10Kylo[KyloPwd];params" returns the same datagram with
// the password toggled, or an empty string if `data` is no lobby pong.
std::string PongVariant(const char* data, int len) {
    if (len <= kPongHeader || (unsigned char)data[0] != 0x1C || memcmp(data + 17, kOfflineMagic, 16) != 0) return {};
    std::string payload(data + kPongHeader, len - kPongHeader);
    size_t nameLen = payload.find(';');
    if (nameLen == std::string::npos) nameLen = payload.size();
    std::string name = payload.substr(0, nameLen);
    const size_t base = sizeof(kLobbyName) - 1, pwd = sizeof(kLobbyPassword) - 1;
    if (name.compare(0, base, kLobbyName) != 0) return {};
    if (name.size() >= base + pwd && name.compare(name.size() - pwd, pwd, kLobbyPassword) == 0)
        name.erase(name.size() - pwd);
    else
        name += kLobbyPassword;
    return std::string(data, kPongHeader) + name + payload.substr(nameLen);
}

int WSAAPI HookRecvFrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
    if (g_pongVariants && !(flags & MSG_PEEK)) {
        PendingPacket p;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            auto it = g_pending.find(s);
            if (it != g_pending.end()) {
                p = it->second;
                g_pending.erase(it);
                have = true;
            }
        }
        if (have && (int)p.data.size() <= len && (!from || !fromlen || *fromlen >= (int)sizeof(sockaddr_in))) {
            memcpy(buf, p.data.data(), p.data.size());
            if (from && fromlen) {
                memcpy(from, &p.from, sizeof(p.from));
                *fromlen = sizeof(p.from);
            }
            LogPacket("recv <-", (const sockaddr*)&p.from, buf, (int)p.data.size(), " (pong copy, password toggled)");
            WSASetLastError(0);
            return (int)p.data.size();
        }
    }
    int ret = g_realRecvFrom(s, buf, len, flags, from, fromlen);
    if (ret > 0) {
        int err = WSAGetLastError();
        std::string note;
        if (g_aliasEnabled && from && from->sa_family == AF_INET) {
            auto* in = (sockaddr_in*)from;
            unsigned long ip = ntohl(in->sin_addr.s_addr);
            if (IsTailscale(ip)) {
                note = " (real " + Addr(from) + ")";
                std::lock_guard<std::mutex> lock(g_aliasMutex);
                in->sin_addr.s_addr = htonl(AliasForLocked(ip));
            }
        }
        LogPacket("recv <-", from, buf, ret, note.c_str());
        if (g_pongVariants && !(flags & MSG_PEEK) && from && from->sa_family == AF_INET) {
            std::string variant = PongVariant(buf, ret);
            if (!variant.empty()) {
                std::lock_guard<std::mutex> lock(g_pendingMutex);
                g_pending[s] = {variant, *(sockaddr_in*)from};
            }
        }
        WSASetLastError(err);
    }
    return ret;
}

// "4294967295" -> -1: an unsigned 32-bit number written with "%u" reads back
// as the int it came from. Everything else keeps atoi's behaviour.
int __cdecl HookAtoi(const char* s) {
    if (s) {
        const char* p = s;
        while (*p == ' ' || *p == '\t') ++p;
        unsigned long long v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 11) {
            v = v * 10 + (*p++ - '0');
            ++digits;
        }
        if (digits > 0 && digits <= 10 && (*p < '0' || *p > '9') && v > 0x7FFFFFFFull && v <= 0xFFFFFFFFull)
            return (int)(unsigned int)v;
    }
    return g_realAtoi(s);
}

}  // namespace

// Replaces one entry of `module`'s import table for `dll`. Imports by ordinal
// are matched by the resolved address, so names in the import table don't matter.
bool PatchImport(HMODULE module, const char* dll, void* target, void* hook) {
    auto* base = (BYTE*)module;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        for (auto* thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); thunk->u1.Function; ++thunk) {
            if ((void*)thunk->u1.Function != target) continue;
            DWORD old;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            thunk->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

void InstallLanHooks(const std::wstring& dir) {
    std::wstring ini = dir + L"wrc10mp.ini";
    wchar_t ip[64];
    GetPrivateProfileStringW(L"lan", L"host_ip", L"", ip, 64, ini.c_str());
    g_lanPort = (unsigned short)GetPrivateProfileIntW(L"lan", L"port", 60000, ini.c_str());
    g_aliasEnabled = GetPrivateProfileIntW(L"lan", L"alias", 0, ini.c_str()) != 0;
    g_pongVariants = GetPrivateProfileIntW(L"lan", L"pong_variants", 1, ini.c_str()) != 0;
    if (GetPrivateProfileIntW(L"lan", L"log", 1, ini.c_str())) _wfopen_s(&g_netLog, (dir + L"wrc10mp_net.log").c_str(), L"ab");

    char ipA[64] = {};
    WideCharToMultiByte(CP_ACP, 0, ip, -1, ipA, sizeof(ipA), nullptr, nullptr);
    unsigned a, b, c, d;
    if (sscanf_s(ipA, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 && d < 256) {
        g_host.sin_family = AF_INET;
        g_host.sin_port = htons(g_lanPort);
        g_host.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
    }

    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    HMODULE exe = GetModuleHandleW(nullptr);
    bool okSend = false, okRecv = false;
    if (ws2) {
        g_realSendTo = (SendToFn)GetProcAddress(ws2, "sendto");
        g_realRecvFrom = (RecvFromFn)GetProcAddress(ws2, "recvfrom");
        okSend = g_realSendTo && PatchImport(exe, "WS2_32.dll", (void*)g_realSendTo, (void*)&HookSendTo);
        okRecv = g_realRecvFrom && PatchImport(exe, "WS2_32.dll", (void*)g_realRecvFrom, (void*)&HookRecvFrom);
    }
    bool okAtoi = false;
    HMODULE crt = GetModuleHandleW(L"ucrtbase.dll");  // target of the api-ms-win-crt-* forwarders
    if (GetPrivateProfileIntW(L"steam", L"atoi_wrap", 1, ini.c_str()) && crt) {
        g_realAtoi = (AtoiFn)GetProcAddress(crt, "atoi");
        okAtoi = g_realAtoi && PatchImport(exe, "api-ms-win-crt-convert-l1-1-0.dll", (void*)g_realAtoi, (void*)&HookAtoi);
    }
    if (g_netLog) {
        fprintf(g_netLog, "==== wrc10mp lan: hooks sendto=%s recvfrom=%s atoi=%s, port %u, search copy to %s, tailscale alias %s, pong variants %s\n",
                okSend ? "ok" : "FAILED", okRecv ? "ok" : "FAILED", okAtoi ? "ok" : "off", g_lanPort,
                g_host.sin_port ? Addr((const sockaddr*)&g_host).c_str() : "(none - host mode)", g_aliasEnabled ? "on" : "off",
                g_pongVariants ? "on" : "off");
        fflush(g_netLog);
    }
}
