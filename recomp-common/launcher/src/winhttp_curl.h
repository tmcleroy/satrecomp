// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
#pragma once
// curl's arguments, made by the program itself on Windows (WinHTTP) instead of by the system's curl.exe started with no
// window: a program that starts a hidden curl.exe to fetch from the internet is what downloaders do, and antivirus
// programs' machine learning took the ports for one (Windows Defender's "Program:Win32/Wacapew.A!ml"). The requests
// the ports make (run_program's callers: update.cpp, online.cpp, a game's online play) use only the arguments below;
// any other one, a SOCKS proxy or a URL other than http(s) (the tests' file://) and the system's curl.exe is started
// as before. A project turns it on with RECOMP_WINHTTP and links winhttp (launcher.cmake does, for the launcher).
// curl_request() is the part every system builds, for tests/curl_args_test.cpp.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace launcher {

struct CurlRequest {
    bool ok = false;           // (false: an argument this does not make; the system's curl does it)
    std::string url;
    std::string method;        // ("" : GET, or POST with a body)
    std::vector<std::string> headers;
    std::string body;
    bool has_body = false;
    std::string user_agent;
    std::string output_file;   // (-o; "": the output)
    std::string write_out;     // (-w, with \n and %{http_code} as curl reads them)
    bool fail = false;         // (-f: an HTTP error status is exit code 22, and no body)
    bool follow = false;       // (-L)
    bool show_error = false;   // (-S)
    int connect_timeout = 0, max_time = 0, speed_time = 0, retry = 0;  // (seconds; 0: none)
};

inline CurlRequest curl_request(const std::vector<std::string>& args) {
    CurlRequest r;
    auto number = [](const std::string& text, int& to) {
        char* end = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        if (text.empty() || *end || value < 0 || value > 86400) {
            return false;
        }
        to = static_cast<int>(value);
        return true;
    };
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        const bool more = i + 1 < args.size();
        int ignored = 0;
        if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
            // (short ones: -fsSL, -sS, and -X, -H, -A, -o, -w with their value next)
            if (a.size() == 2 && std::string("XHAow").find(a[1]) != std::string::npos) {
                if (!more) {
                    return r;
                }
                const std::string& value = args[++i];
                switch (a[1]) {
                case 'X': r.method = value; break;
                case 'H': r.headers.push_back(value); break;
                case 'A': r.user_agent = value; break;
                case 'o': r.output_file = value; break;
                case 'w': r.write_out = value; break;
                }
                continue;
            }
            for (size_t c = 1; c < a.size(); c++) {
                switch (a[c]) {
                case 'f': r.fail = true; break;
                case 's': break;
                case 'S': r.show_error = true; break;
                case 'L': r.follow = true; break;
                default: return r;
                }
            }
        } else if (a == "--max-time" && more && number(args[i + 1], r.max_time)) {
            i++;
        } else if (a == "--connect-timeout" && more && number(args[i + 1], r.connect_timeout)) {
            i++;
        } else if (a == "--speed-time" && more && number(args[i + 1], r.speed_time)) {
            i++;
        } else if (a == "--speed-limit" && more && number(args[i + 1], ignored)) {
            i++;  // (a download slower than it for --speed-time: WinHTTP's receive timeout stands for the pair)
        } else if (a == "--retry" && more && number(args[i + 1], r.retry)) {
            i++;
        } else if (a == "--data-binary" && more) {
            r.body = args[++i];
            r.has_body = true;
        } else if (a.rfind("http://", 0) == 0 || a.rfind("https://", 0) == 0) {
            if (!r.url.empty()) {
                return r;
            }
            r.url = a;
        } else {
            return r;
        }
    }
    if (r.url.empty() || (!r.body.empty() && r.body[0] == '@')) {
        return r;  // (no URL, or curl's "@file" body: curl's own)
    }
    if (r.method.empty()) {
        r.method = r.has_body ? "POST" : "GET";
    }
    std::string out;
    for (size_t i = 0; i < r.write_out.size(); i++) {
        if (r.write_out[i] == '\\' && i + 1 < r.write_out.size() && (r.write_out[i + 1] == 'n' || r.write_out[i + 1] == 'r' || r.write_out[i + 1] == 't')) {
            out += r.write_out[i + 1] == 'n' ? '\n' : r.write_out[i + 1] == 'r' ? '\r' : '\t';
            i++;
        } else if (r.write_out.compare(i, 2, "%{") == 0) {
            const size_t end = r.write_out.find('}', i);
            if (end == std::string::npos || r.write_out.compare(i, end - i + 1, "%{http_code}") != 0) {
                return r;  // (only the status is written out)
            }
            out += '\1';  // (the status's place)
            i = end;
        } else {
            out += r.write_out[i];
        }
    }
    r.write_out = out;
    r.ok = true;
    return r;
}

// -w's text with the status in it (curl writes it as three digits, 000 for none)
inline std::string curl_write_out(const std::string& prepared, int status) {
    std::string code = std::to_string(status);
    code.insert(0, code.size() < 3 ? 3 - code.size() : 0, '0');
    std::string out;
    for (char c : prepared) {
        if (c == '\1') {
            out += code;
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace launcher

#if defined(_WIN32) && defined(RECOMP_WINHTTP)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace launcher {
namespace winhttp_detail {

inline std::wstring wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() {
        if (h) {
            WinHttpCloseHandle(h);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

// curl's exit code for a WinHTTP error
inline int curl_code(DWORD error) {
    switch (error) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return 6;
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR: return 7;
    case ERROR_WINHTTP_TIMEOUT: return 28;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED: return 60;
    case ERROR_WINHTTP_REDIRECT_FAILED: return 47;
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return 3;
    default: return 56;
    }
}

// The proxy: the one the player or recomp::UseSystemProxy() put in HTTPS_PROXY/HTTP_PROXY (an http:// one), else
// Windows' own setting and its automatic configuration. false for a proxy WinHTTP cannot use (SOCKS).
inline bool open_session(const CurlRequest& r, HINTERNET& session) {
    const std::wstring agent = wide(r.user_agent.empty() ? "curl" : r.user_agent);
    std::string proxy;
    for (const char* name : {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy", "ALL_PROXY", "all_proxy"}) {
        if (const char* set = std::getenv(name); set && *set) {
            proxy = set;
            break;
        }
    }
    if (!proxy.empty()) {
        if (proxy.rfind("http://", 0) == 0) {
            proxy = proxy.substr(7);
        } else if (proxy.find("://") != std::string::npos) {
            return false;
        }
        while (!proxy.empty() && proxy.back() == '/') {
            proxy.pop_back();
        }
        session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_NAMED_PROXY, wide(proxy).c_str(), L"<local>", 0);
    } else {
        // (Windows 8.1 and later: the system's proxy, its PAC script included; earlier, the default)
        session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
    }
    return true;
}

// One try: curl's exit code; the body into `output` or the file, the status into `status`
inline int request_once(const CurlRequest& r, std::string* output, int& status, std::string& error) {
    status = 0;
    HINTERNET raw = nullptr;
    if (!open_session(r, raw)) {
        return -2;  // (the system's curl does it)
    }
    Handle session(raw);
    if (!session) {
        error = "could not start WinHTTP";
        return 2;
    }
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const int ms_connect = (r.connect_timeout ? r.connect_timeout : r.max_time ? r.max_time : 60) * 1000;
    const int ms_receive = (r.speed_time ? r.speed_time : r.max_time ? r.max_time : 300) * 1000;
    WinHttpSetTimeouts(session.h, ms_connect, ms_connect, ms_receive, ms_receive);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // (Windows 10 before TLS 1.3)
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }

    std::wstring url = wide(r.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        error = "bad URL";
        return 3;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) {
        path = L"/";
    }
    Handle connection(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
    if (!connection) {
        return curl_code(GetLastError());
    }
    Handle request(WinHttpOpenRequest(connection.h, wide(r.method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        return curl_code(GetLastError());
    }
    DWORD redirects = r.follow ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof redirects);
    std::wstring headers;
    for (const std::string& header : r.headers) {
        headers += wide(header) + L"\r\n";
    }
    if (r.has_body && r.headers.end() == std::find_if(r.headers.begin(), r.headers.end(), [](const std::string& h) {
            return h.size() >= 13 && _strnicmp(h.c_str(), "content-type:", 13) == 0;
        })) {
        headers += L"Content-Type: application/x-www-form-urlencoded\r\n";  // (curl's, for --data-binary)
    }
    if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L),
                            r.has_body ? const_cast<char*>(r.body.data()) : WINHTTP_NO_REQUEST_DATA,
                            static_cast<DWORD>(r.body.size()), static_cast<DWORD>(r.body.size()), 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        return curl_code(GetLastError());
    }
    DWORD code = 0, size = sizeof code;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                        WINHTTP_NO_HEADER_INDEX);
    status = static_cast<int>(code);
    if (r.fail && status >= 400) {
        error = "The requested URL returned error: " + std::to_string(status);
        return 22;
    }
    FILE* file = nullptr;
    if (!r.output_file.empty()) {
        file = _wfopen(wide(r.output_file).c_str(), L"wb");
        if (!file) {
            error = "could not write " + r.output_file;
            return 23;
        }
    }
    int result = 0;
    std::vector<char> buffer(64 * 1024);
    for (;;) {
        if (r.max_time && clock::now() - start > std::chrono::seconds(r.max_time)) {
            error = "Operation timed out";
            result = 28;
            break;
        }
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) {
            result = curl_code(GetLastError());
            break;
        }
        if (got == 0) {
            break;
        }
        if (file) {
            if (fwrite(buffer.data(), 1, got, file) != got) {
                error = "could not write " + r.output_file;
                result = 23;
                break;
            }
        } else if (output) {
            output->append(buffer.data(), got);
        }
    }
    if (file && fclose(file) != 0 && result == 0) {
        result = 23;
    }
    return result;
}

}  // namespace winhttp_detail

// The request curl's `args` ask for, made with WinHTTP: curl's exit code, and what curl would write to its output
// (the body, unless -o, then -w's text; with -S, an error's message). -1: not one this makes (the caller starts curl).
inline int winhttp_curl(const std::vector<std::string>& args, std::string* output) {
    const CurlRequest r = curl_request(args);
    if (!r.ok) {
        return -1;
    }
    std::string body, error;
    int status = 0, code = 0;
    for (int attempt = 0;; attempt++) {
        body.clear();
        error.clear();
        code = winhttp_detail::request_once(r, output ? &body : nullptr, status, error);
        if (code == -2) {
            return -1;
        }
        // (curl's --retry: a timeout, a connection that failed, or a 408, 429 or 5xx status; after 1 s, 2 s, ...)
        const bool transient = code == 6 || code == 7 || code == 28 || code == 56 || status == 408 || status == 429 || (status >= 500 && status < 600);
        if (attempt >= r.retry || !transient) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
    }
    if (output) {
        *output = std::move(body);
        if (code != 0 && r.show_error) {
            *output += "curl: (" + std::to_string(code) + ") " + (error.empty() ? "request failed" : error) + "\n";
        }
        if (!r.write_out.empty()) {
            *output += curl_write_out(r.write_out, status);
        }
    }
    return code;
}

}  // namespace launcher
#endif
