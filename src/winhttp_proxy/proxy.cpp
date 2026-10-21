// wrc10mp - WinHTTP proxy DLL for WRC 10.
//
// Placed next to WRC10.exe as winhttp.dll. Every export is forwarded to the
// system winhttp.dll; the request/response functions are wrapped so that all
// HTTP(S) traffic of the game (KtOS backend) is logged in plain text and
// selected responses can be replaced by local files (see wrc10mp.ini).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "forwards.h"
#include "crash.h"
#include "lan.h"
#include "pause.h"

#ifndef WINHTTP_QUERY_MODIFIER_FLAGS_MASK
#define WINHTTP_QUERY_MODIFIER_FLAGS_MASK 0xF0000000
#endif

#pragma comment(linker, "/export:WinHttpConnect=Proxy_WinHttpConnect")
#pragma comment(linker, "/export:WinHttpOpenRequest=Proxy_WinHttpOpenRequest")
#pragma comment(linker, "/export:WinHttpAddRequestHeaders=Proxy_WinHttpAddRequestHeaders")
#pragma comment(linker, "/export:WinHttpSendRequest=Proxy_WinHttpSendRequest")
#pragma comment(linker, "/export:WinHttpReceiveResponse=Proxy_WinHttpReceiveResponse")
#pragma comment(linker, "/export:WinHttpQueryHeaders=Proxy_WinHttpQueryHeaders")
#pragma comment(linker, "/export:WinHttpQueryDataAvailable=Proxy_WinHttpQueryDataAvailable")
#pragma comment(linker, "/export:WinHttpReadData=Proxy_WinHttpReadData")
#pragma comment(linker, "/export:WinHttpCloseHandle=Proxy_WinHttpCloseHandle")

namespace {

// ---------------------------------------------------------------------------
// Real winhttp.dll

struct RealApi {
    decltype(&::WinHttpConnect) Connect;
    decltype(&::WinHttpOpenRequest) OpenRequest;
    decltype(&::WinHttpAddRequestHeaders) AddRequestHeaders;
    decltype(&::WinHttpSendRequest) SendRequest;
    decltype(&::WinHttpReceiveResponse) ReceiveResponse;
    decltype(&::WinHttpQueryHeaders) QueryHeaders;
    decltype(&::WinHttpQueryDataAvailable) QueryDataAvailable;
    decltype(&::WinHttpReadData) ReadData;
    decltype(&::WinHttpCloseHandle) CloseHandle;
};

// ---------------------------------------------------------------------------
// Configuration

enum class When { Always, OnFail };

struct Rule {
    std::string name;
    std::string host;    // lower-case substring, empty = any
    std::string path;    // lower-case substring, required
    std::string method;  // lower-case, empty = any
    When when = When::Always;
    DWORD status = 200;
    std::wstring bodyFile;
};

struct Config {
    bool log = true;
    size_t logMaxBody = 65536;
    std::string hostIp;
    std::string hostPort;
    std::vector<Rule> rules;
};

// ---------------------------------------------------------------------------
// Per-handle state

struct ConnectInfo {
    std::wstring host;
    INTERNET_PORT port = 0;
};

struct Request {
    unsigned id = 0;
    std::wstring host;
    INTERNET_PORT port = 0;
    bool secure = false;
    std::wstring verb;
    std::wstring path;
    std::wstring headers;
    std::string body;
    const Rule* rule = nullptr;  // matching rewrite rule, if any

    // Set once the response has been captured by ReceiveResponse; reads are
    // then served from `response` instead of the real handle.
    bool buffered = false;
    bool rewritten = false;  // body/status replaced by a rule
    bool faked = false;      // no real response exists at all
    DWORD status = 0;
    std::string response;
    size_t readPos = 0;
};

RealApi g_real{};
Config g_cfg;
std::wstring g_dir;  // folder of this DLL (= game folder)
std::once_flag g_initOnce;
std::mutex g_mutex;  // guards the maps, the log and g_nextId
std::map<HINTERNET, ConnectInfo> g_connects;
std::map<HINTERNET, std::shared_ptr<Request>> g_requests;
unsigned g_nextId = 1;
FILE* g_log = nullptr;

// ---------------------------------------------------------------------------
// Helpers

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return {};
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring HeaderString(LPCWSTR headers, DWORD length) {
    if (!headers) return {};
    if (length == (DWORD)-1L) return headers;
    return std::wstring(headers, length);
}

std::wstring IniString(const wchar_t* section, const wchar_t* key, const wchar_t* def, const std::wstring& file) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(section, key, def, buf, (DWORD)std::size(buf), file.c_str());
    return Trim(buf);
}

void LogLine(const std::string& text) {
    // Caller holds g_mutex.
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text.c_str());
    fflush(g_log);
}

void Log(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LogLine(text);
}

std::string BodyForLog(const std::string& body) {
    if (body.size() <= g_cfg.logMaxBody) return body;
    return body.substr(0, g_cfg.logMaxBody) + "\n... (" + std::to_string(body.size() - g_cfg.logMaxBody) + " more bytes truncated)";
}

std::string Url(const Request& r) {
    std::string url = r.secure ? "https://" : "http://";
    url += ToUtf8(r.host);
    if (r.port && r.port != (r.secure ? 443 : 80)) url += ":" + std::to_string(r.port);
    return url + ToUtf8(r.path);
}

void LoadConfig() {
    std::wstring ini = g_dir + L"wrc10mp.ini";
    g_cfg.log = GetPrivateProfileIntW(L"general", L"log", 1, ini.c_str()) != 0;
    g_cfg.logMaxBody = GetPrivateProfileIntW(L"general", L"log_max_body", 65536, ini.c_str());
    g_cfg.hostIp = ToUtf8(IniString(L"general", L"host_ip", L"", ini));
    g_cfg.hostPort = ToUtf8(IniString(L"general", L"host_port", L"", ini));

    for (int i = 1; i <= 32; ++i) {
        std::wstring section = L"rule" + std::to_wstring(i);
        if (!GetPrivateProfileIntW(section.c_str(), L"enabled", 0, ini.c_str())) continue;
        Rule rule;
        rule.name = ToUtf8(section);
        rule.host = Lower(ToUtf8(IniString(section.c_str(), L"host", L"", ini)));
        rule.path = Lower(ToUtf8(IniString(section.c_str(), L"path", L"", ini)));
        rule.method = Lower(ToUtf8(IniString(section.c_str(), L"method", L"", ini)));
        rule.when = Lower(ToUtf8(IniString(section.c_str(), L"when", L"always", ini))) == "on_fail" ? When::OnFail : When::Always;
        rule.status = GetPrivateProfileIntW(section.c_str(), L"status", 200, ini.c_str());
        rule.bodyFile = IniString(section.c_str(), L"body_file", L"", ini);
        if (rule.path.empty() || rule.bodyFile.empty()) continue;
        if (rule.bodyFile.size() < 2 || rule.bodyFile[1] != L':') rule.bodyFile = g_dir + rule.bodyFile;
        g_cfg.rules.push_back(rule);
    }
}

void Init() {
    wchar_t path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&Init, &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);

    LoadConfig();
    if (g_cfg.log) _wfopen_s(&g_log, (g_dir + L"wrc10mp_http.log").c_str(), L"ab");

    GetSystemDirectoryW(path, MAX_PATH);
    HMODULE real = LoadLibraryW((std::wstring(path) + L"\\winhttp.dll").c_str());
    g_real.Connect = (decltype(g_real.Connect))GetProcAddress(real, "WinHttpConnect");
    g_real.OpenRequest = (decltype(g_real.OpenRequest))GetProcAddress(real, "WinHttpOpenRequest");
    g_real.AddRequestHeaders = (decltype(g_real.AddRequestHeaders))GetProcAddress(real, "WinHttpAddRequestHeaders");
    g_real.SendRequest = (decltype(g_real.SendRequest))GetProcAddress(real, "WinHttpSendRequest");
    g_real.ReceiveResponse = (decltype(g_real.ReceiveResponse))GetProcAddress(real, "WinHttpReceiveResponse");
    g_real.QueryHeaders = (decltype(g_real.QueryHeaders))GetProcAddress(real, "WinHttpQueryHeaders");
    g_real.QueryDataAvailable = (decltype(g_real.QueryDataAvailable))GetProcAddress(real, "WinHttpQueryDataAvailable");
    g_real.ReadData = (decltype(g_real.ReadData))GetProcAddress(real, "WinHttpReadData");
    g_real.CloseHandle = (decltype(g_real.CloseHandle))GetProcAddress(real, "WinHttpCloseHandle");

    std::string msg = "==== wrc10mp loaded, " + std::to_string(g_cfg.rules.size()) + " rule(s) active";
    for (const auto& r : g_cfg.rules) msg += "\n     " + r.name + ": path contains '" + r.path + "' -> " + ToUtf8(r.bodyFile);
    Log(msg);
}

void EnsureInit() { std::call_once(g_initOnce, Init); }

std::shared_ptr<Request> FindRequest(HINTERNET h) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_requests.find(h);
    return it == g_requests.end() ? nullptr : it->second;
}

const Rule* MatchRule(const Request& r) {
    std::string host = Lower(ToUtf8(r.host));
    std::string path = Lower(ToUtf8(r.path));
    std::string verb = Lower(ToUtf8(r.verb));
    for (const auto& rule : g_cfg.rules) {
        if (!rule.host.empty() && host.find(rule.host) == std::string::npos) continue;
        if (!rule.method.empty() && rule.method != verb) continue;
        if (rule.path != "*" && path.find(rule.path) == std::string::npos) continue;
        return &rule;
    }
    return nullptr;
}

void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) s.replace(pos, from.size(), to);
}

// Reads the rule's body file fresh on every request, so it can be edited while
// the game is running. Supports {HOST_IP} and {HOST_PORT} placeholders.
std::string RuleBody(const Rule& rule) {
    std::string body;
    FILE* f = nullptr;
    if (_wfopen_s(&f, rule.bodyFile.c_str(), L"rb") == 0 && f) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
        fclose(f);
    } else {
        Log("!!! " + rule.name + ": cannot open body file " + ToUtf8(rule.bodyFile));
    }
    ReplaceAll(body, "{HOST_IP}", g_cfg.hostIp);
    ReplaceAll(body, "{HOST_PORT}", g_cfg.hostPort);
    return body;
}

std::wstring RealRawHeaders(HINTERNET h) {
    DWORD size = 0;
    g_real.QueryHeaders(h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
    std::wstring headers(size / sizeof(wchar_t), L'\0');
    if (!g_real.QueryHeaders(h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, headers.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    headers.resize(size / sizeof(wchar_t));
    return Trim(headers);
}

// Answers a header query from our own data. Follows WinHTTP's buffer contract.
BOOL AnswerHeader(DWORD infoLevel, const std::wstring& value, DWORD number, LPVOID buffer, LPDWORD bufferLength) {
    if (infoLevel & WINHTTP_QUERY_FLAG_NUMBER) {
        if (!buffer || *bufferLength < sizeof(DWORD)) {
            *bufferLength = sizeof(DWORD);
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        *(DWORD*)buffer = number;
        *bufferLength = sizeof(DWORD);
        return TRUE;
    }
    DWORD needed = (DWORD)((value.size() + 1) * sizeof(wchar_t));
    if (!buffer || *bufferLength < needed) {
        *bufferLength = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(buffer, value.c_str(), needed);
    *bufferLength = needed - sizeof(wchar_t);
    return TRUE;
}

}  // namespace

// ---------------------------------------------------------------------------
// Exports

extern "C" HINTERNET WINAPI Proxy_WinHttpConnect(HINTERNET hSession, LPCWSTR pswzServerName, INTERNET_PORT nServerPort, DWORD dwReserved) {
    EnsureInit();
    HINTERNET h = g_real.Connect(hSession, pswzServerName, nServerPort, dwReserved);
    if (h) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_connects[h] = {pswzServerName ? pswzServerName : L"", nServerPort};
    }
    return h;
}

extern "C" HINTERNET WINAPI Proxy_WinHttpOpenRequest(HINTERNET hConnect, LPCWSTR pwszVerb, LPCWSTR pwszObjectName, LPCWSTR pwszVersion,
                                                     LPCWSTR pwszReferrer, LPCWSTR* ppwszAcceptTypes, DWORD dwFlags) {
    EnsureInit();
    HINTERNET h = g_real.OpenRequest(hConnect, pwszVerb, pwszObjectName, pwszVersion, pwszReferrer, ppwszAcceptTypes, dwFlags);
    if (h) {
        auto r = std::make_shared<Request>();
        r->verb = pwszVerb ? pwszVerb : L"GET";
        r->path = pwszObjectName ? pwszObjectName : L"/";
        if (r->path.empty() || r->path[0] != L'/') r->path = L"/" + r->path;
        r->secure = (dwFlags & WINHTTP_FLAG_SECURE) != 0;
        std::lock_guard<std::mutex> lock(g_mutex);
        auto c = g_connects.find(hConnect);
        if (c != g_connects.end()) {
            r->host = c->second.host;
            r->port = c->second.port;
        }
        r->id = g_nextId++;
        g_requests[h] = r;
    }
    return h;
}

extern "C" BOOL WINAPI Proxy_WinHttpAddRequestHeaders(HINTERNET hRequest, LPCWSTR lpszHeaders, DWORD dwHeadersLength, DWORD dwModifiers) {
    EnsureInit();
    if (auto r = FindRequest(hRequest)) r->headers += Trim(HeaderString(lpszHeaders, dwHeadersLength)) + L"\r\n";
    return g_real.AddRequestHeaders(hRequest, lpszHeaders, dwHeadersLength, dwModifiers);
}

extern "C" BOOL WINAPI Proxy_WinHttpSendRequest(HINTERNET hRequest, LPCWSTR lpszHeaders, DWORD dwHeadersLength, LPVOID lpOptional,
                                                DWORD dwOptionalLength, DWORD dwTotalLength, DWORD_PTR dwContext) {
    EnsureInit();
    auto r = FindRequest(hRequest);
    if (r) {
        std::wstring extra = Trim(HeaderString(lpszHeaders, dwHeadersLength));
        if (!extra.empty()) r->headers += extra + L"\r\n";
        if (lpOptional && dwOptionalLength) r->body.assign((const char*)lpOptional, dwOptionalLength);
        r->rule = MatchRule(*r);
        Log("#" + std::to_string(r->id) + " >>> " + ToUtf8(r->verb) + " " + Url(*r) + "\n" + ToUtf8(Trim(r->headers)) +
            (r->body.empty() ? "" : "\n\n" + BodyForLog(r->body)));
    }

    BOOL ok = g_real.SendRequest(hRequest, lpszHeaders, dwHeadersLength, lpOptional, dwOptionalLength, dwTotalLength, dwContext);
    if (!ok && r) {
        DWORD err = GetLastError();
        Log("#" + std::to_string(r->id) + " !!! send failed, error " + std::to_string(err));
        if (!r->rule) {
            SetLastError(err);
            return FALSE;
        }
        r->faked = true;  // the rule will answer in ReceiveResponse
        return TRUE;
    }
    return ok;
}

extern "C" BOOL WINAPI Proxy_WinHttpReceiveResponse(HINTERNET hRequest, LPVOID lpReserved) {
    EnsureInit();
    auto r = FindRequest(hRequest);
    if (!r) return g_real.ReceiveResponse(hRequest, lpReserved);

    std::wstring rawHeaders;
    if (!r->faked) {
        if (!g_real.ReceiveResponse(hRequest, lpReserved)) {
            DWORD err = GetLastError();
            Log("#" + std::to_string(r->id) + " !!! receive failed, error " + std::to_string(err));
            if (!r->rule) {
                SetLastError(err);
                return FALSE;
            }
            r->faked = true;
        } else {
            DWORD status = 0, size = sizeof(status);
            g_real.QueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                &size, WINHTTP_NO_HEADER_INDEX);
            r->status = status;
            rawHeaders = RealRawHeaders(hRequest);
            DWORD avail = 0;
            std::vector<char> chunk;
            while (g_real.QueryDataAvailable(hRequest, &avail) && avail > 0) {
                chunk.resize(avail);
                DWORD read = 0;
                if (!g_real.ReadData(hRequest, chunk.data(), avail, &read) || read == 0) break;
                r->response.append(chunk.data(), read);
            }
        }
    }

    std::string original = r->response;
    DWORD originalStatus = r->status;
    if (r->rule && (r->faked || r->rule->when == When::Always)) {
        r->response = RuleBody(*r->rule);
        r->status = r->rule->status;
        r->rewritten = true;
    }
    r->buffered = true;
    r->readPos = 0;

    std::string msg = "#" + std::to_string(r->id) + " <<< " + (r->faked ? std::string("(no server response)") : std::to_string(originalStatus)) + " " +
                      Url(*r);
    if (!rawHeaders.empty()) msg += "\n" + ToUtf8(rawHeaders);
    if (!r->faked) msg += "\n\n" + BodyForLog(original);
    if (r->rewritten) msg += "\n--- replaced by " + r->rule->name + " (status " + std::to_string(r->status) + "):\n" + BodyForLog(r->response);
    Log(msg);
    return TRUE;
}

extern "C" BOOL WINAPI Proxy_WinHttpQueryHeaders(HINTERNET hRequest, DWORD dwInfoLevel, LPCWSTR pwszName, LPVOID lpBuffer,
                                                 LPDWORD lpdwBufferLength, LPDWORD lpdwIndex) {
    EnsureInit();
    auto r = FindRequest(hRequest);
    if (!r || !r->rewritten) return g_real.QueryHeaders(hRequest, dwInfoLevel, pwszName, lpBuffer, lpdwBufferLength, lpdwIndex);

    DWORD level = dwInfoLevel & ~WINHTTP_QUERY_MODIFIER_FLAGS_MASK;
    switch (level) {
        case WINHTTP_QUERY_STATUS_CODE:
            return AnswerHeader(dwInfoLevel, std::to_wstring(r->status), r->status, lpBuffer, lpdwBufferLength);
        case WINHTTP_QUERY_CONTENT_LENGTH:
            return AnswerHeader(dwInfoLevel, std::to_wstring(r->response.size()), (DWORD)r->response.size(), lpBuffer, lpdwBufferLength);
    }
    if (!r->faked) return g_real.QueryHeaders(hRequest, dwInfoLevel, pwszName, lpBuffer, lpdwBufferLength, lpdwIndex);

    switch (level) {
        case WINHTTP_QUERY_STATUS_TEXT:
            return AnswerHeader(dwInfoLevel, L"OK", 0, lpBuffer, lpdwBufferLength);
        case WINHTTP_QUERY_CONTENT_TYPE:
            return AnswerHeader(dwInfoLevel, L"application/json", 0, lpBuffer, lpdwBufferLength);
        case WINHTTP_QUERY_RAW_HEADERS_CRLF:
            return AnswerHeader(dwInfoLevel,
                                L"HTTP/1.1 " + std::to_wstring(r->status) + L" OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                    std::to_wstring(r->response.size()) + L"\r\n\r\n",
                                0, lpBuffer, lpdwBufferLength);
    }
    SetLastError(ERROR_WINHTTP_HEADER_NOT_FOUND);
    return FALSE;
}

extern "C" BOOL WINAPI Proxy_WinHttpQueryDataAvailable(HINTERNET hRequest, LPDWORD lpdwNumberOfBytesAvailable) {
    EnsureInit();
    auto r = FindRequest(hRequest);
    if (!r || !r->buffered) return g_real.QueryDataAvailable(hRequest, lpdwNumberOfBytesAvailable);
    if (lpdwNumberOfBytesAvailable) *lpdwNumberOfBytesAvailable = (DWORD)(r->response.size() - r->readPos);
    return TRUE;
}

extern "C" BOOL WINAPI Proxy_WinHttpReadData(HINTERNET hRequest, LPVOID lpBuffer, DWORD dwNumberOfBytesToRead, LPDWORD lpdwNumberOfBytesRead) {
    EnsureInit();
    auto r = FindRequest(hRequest);
    if (!r || !r->buffered) return g_real.ReadData(hRequest, lpBuffer, dwNumberOfBytesToRead, lpdwNumberOfBytesRead);
    DWORD n = (DWORD)min((size_t)dwNumberOfBytesToRead, r->response.size() - r->readPos);
    memcpy(lpBuffer, r->response.data() + r->readPos, n);
    r->readPos += n;
    if (lpdwNumberOfBytesRead) *lpdwNumberOfBytesRead = n;
    return TRUE;
}

extern "C" BOOL WINAPI Proxy_WinHttpCloseHandle(HINTERNET hInternet) {
    EnsureInit();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_requests.erase(hInternet);
        g_connects.erase(hInternet);
    }
    return g_real.CloseHandle(hInternet);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        // The game's import table is already bound at this point, and the
        // hooks must be in place before the game opens its first socket.
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(hinst, path, MAX_PATH);
        std::wstring dir = path;
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        InstallCrashLogger(dir);
        InstallLanHooks(dir);
        InstallPauseWatch(dir);
    }
    return TRUE;
}
