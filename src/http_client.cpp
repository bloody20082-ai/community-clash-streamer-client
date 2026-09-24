#include "http_client.h"
#include <windows.h>
#include <winhttp.h>
#include <stdexcept>
#include <vector>
#include <string>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

static std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

static std::string json_string(const std::string& body, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  auto p = body.find(needle);
  if (p == std::string::npos) return {};
  p = body.find(':', p + needle.size());
  if (p == std::string::npos) return {};
  p = body.find('"', p + 1);
  if (p == std::string::npos) return {};
  ++p;

  std::string out;
  bool esc = false;
  for (; p < body.size(); ++p) {
    char c = body[p];
    if (esc) {
      switch (c) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        default: out += c; break;
      }
      esc = false;
    } else if (c == '\\') {
      esc = true;
    } else if (c == '"') {
      break;
    } else {
      out += c;
    }
  }
  return out;
}

TokenResponse fetch_streamer_token(
    const std::wstring& base_url,
    const std::string& username,
    const std::string& password) {

  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256]{};
  wchar_t path[1024]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 255;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 1023;

  if (!WinHttpCrackUrl(base_url.c_str(), 0, 0, &parts)) {
    throw std::runtime_error("Ungueltige Server-URL.");
  }

  HINTERNET ses = WinHttpOpen(
      L"CommunityClashStreamer/1.0",
      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) throw std::runtime_error("WinHttpOpen fehlgeschlagen.");

  HINTERNET con = WinHttpConnect(
      ses,
      std::wstring(host, parts.dwHostNameLength).c_str(),
      parts.nPort, 0);
  if (!con) {
    WinHttpCloseHandle(ses);
    throw std::runtime_error("WinHttpConnect fehlgeschlagen.");
  }

  std::wstring endpoint = L"/api/token/streamer";
  DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS
      ? WINHTTP_FLAG_SECURE : 0;

  HINTERNET req = WinHttpOpenRequest(
      con, L"POST", endpoint.c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!req) {
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    throw std::runtime_error("WinHttpOpenRequest fehlgeschlagen.");
  }

  std::string body =
      "{\"identity\":\"" + json_escape(username) +
      "\",\"password\":\"" + json_escape(password) +
      "\",\"team_name\":null}";

  const wchar_t* headers = L"Content-Type: application/json\r\n";
  BOOL ok = WinHttpSendRequest(
      req, headers, -1L,
      (LPVOID)body.data(), (DWORD)body.size(),
      (DWORD)body.size(), 0);

  if (ok) ok = WinHttpReceiveResponse(req, nullptr);

  if (!ok) {
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    throw std::runtime_error("API-Anfrage fehlgeschlagen.");
  }

  DWORD status = 0, status_len = sizeof(status);
  WinHttpQueryHeaders(
      req,
      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX,
      &status, &status_len, WINHTTP_NO_HEADER_INDEX);

  std::string response;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(req, &available) || available == 0) break;
    std::vector<char> buf(available);
    DWORD read = 0;
    if (!WinHttpReadData(req, buf.data(), available, &read)) break;
    response.append(buf.data(), read);
  }

  WinHttpCloseHandle(req);
  WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);

  if (status < 200 || status >= 300) {
    std::string detail = json_string(response, "detail");
    throw std::runtime_error(
        detail.empty() ? "Login fehlgeschlagen (HTTP " + std::to_string(status) + ")"
                       : detail);
  }

  TokenResponse out;
  out.token = json_string(response, "token");
  out.identity = json_string(response, "identity");
  out.display_name = json_string(response, "display_name");
  out.livekit_url = json_string(response, "livekit_url");
  if (out.livekit_url.empty()) out.livekit_url = json_string(response, "url");
  if (out.livekit_url.empty()) out.livekit_url = json_string(response, "server_url");
  if (out.livekit_url.empty()) out.livekit_url = "wss://community-clash.de";

  if (out.token.empty()) {
    throw std::runtime_error("Backend hat keinen LiveKit-Token geliefert.");
  }

  return out;
}
