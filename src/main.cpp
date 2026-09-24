#include <windows.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <cwctype>
#include <cstdlib>
#include <iterator>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "livekit/livekit.h"
#include "http_client.h"
#include "desktop_capture.h"
#include "loopback_audio.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {

constexpr COLORREF C_BG      = RGB(0, 0, 0);
constexpr COLORREF C_PANEL   = RGB(16, 16, 16);
constexpr COLORREF C_PANEL_2 = RGB(21, 21, 21);
constexpr COLORREF C_BORDER  = RGB(43, 43, 43);
constexpr COLORREF C_GREEN   = RGB(0, 247, 0);
constexpr COLORREF C_GREEN_2 = RGB(0, 255, 0);
constexpr COLORREF C_TEXT    = RGB(247, 247, 247);
constexpr COLORREF C_MUTED   = RGB(178, 178, 178);
constexpr COLORREF C_MUTED_2 = RGB(119, 119, 119);
constexpr COLORREF C_DANGER  = RGB(255, 93, 105);

constexpr int ID_USERNAME = 1001;
constexpr int ID_PASSWORD = 1002;
constexpr int ID_LOGIN = 1003;
constexpr int ID_TEAM = 1004;
constexpr int ID_GAME = 1005;
constexpr int ID_SOUND = 1006;
constexpr int ID_STREAM = 1007;
constexpr int ID_STATUS = 1008;
constexpr int ID_METRICS = 1009;
constexpr int ID_DETAIL = 1010;
constexpr int ID_GAME_DETAIL = 1011;

constexpr UINT WM_APP_LOGIN_OK = WM_APP + 1;
constexpr UINT WM_APP_ERROR = WM_APP + 2;
constexpr UINT WM_APP_STREAM_LIVE = WM_APP + 3;
constexpr UINT WM_APP_STREAM_STOPPED = WM_APP + 4;

struct WowTarget {
  HWND hwnd = nullptr;
  DWORD pid = 0;
  std::wstring title;
  std::wstring exe_name;
  DesktopOutput output;
  bool valid = false;
  bool fullscreen = false;
};

struct AppState {
  HWND hwnd = nullptr;
  HWND username = nullptr;
  HWND password = nullptr;
  HWND login = nullptr;
  HWND team = nullptr;
  HWND game = nullptr;
  HWND game_detail = nullptr;
  HWND sound = nullptr;
  HWND stream = nullptr;
  HWND status = nullptr;
  HWND metrics = nullptr;
  HWND detail = nullptr;

  HFONT font = nullptr;
  HFONT font_bold = nullptr;
  HFONT font_small = nullptr;
  HFONT font_brand = nullptr;
  HFONT font_kicker = nullptr;
  HFONT font_game = nullptr;

  HBRUSH edit_brush = nullptr;
  HBRUSH panel_brush = nullptr;
  HBRUSH game_brush = nullptr;
  HBRUSH stats_brush = nullptr;

  std::vector<DesktopOutput> outputs;
  TokenResponse creds;
  std::mutex wow_mutex;
  WowTarget wow_target;
  std::atomic<bool> wow_paused{false};

  ULONG_PTR gdiplus_token = 0;
  std::unique_ptr<Gdiplus::Image> logo;

  std::atomic<bool> logged_in{false};
  std::atomic<bool> login_busy{false};
  std::atomic<bool> streaming{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> livekit_initialized{false};
  std::atomic<bool> close_pending{false};
  bool sound_enabled = true;

  std::thread login_thread;
  std::thread stream_thread;
  std::shared_ptr<DesktopCapture> desktop;
  std::shared_ptr<LoopbackAudioCapture> audio;

};

AppState g;

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (len <= 0) return std::wstring(s.begin(), s.end());
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
  return out;
}

std::string wide_to_utf8(const std::wstring& s) {
  if (s.empty()) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
  return out;
}

std::wstring get_text(HWND h) {
  int len = GetWindowTextLengthW(h);
  std::wstring out(static_cast<size_t>(len + 1), L'\0');
  if (len > 0) GetWindowTextW(h, out.data(), len + 1);
  out.resize(static_cast<size_t>(len));
  return out;
}

void set_text(HWND h, const std::wstring& s) {
  if (h) SetWindowTextW(h, s.c_str());
}

std::wstring lower_copy(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towlower(c));
  });
  return s;
}

std::wstring exe_directory() {
  wchar_t path[MAX_PATH]{};
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (!n || n >= MAX_PATH) return L".";
  return std::filesystem::path(path).parent_path().wstring();
}

bool is_wow_executable_name(const std::wstring& name) {
  std::wstring n = lower_copy(name);
  return n == L"wow.exe" || n == L"wowclassic.exe" ||
         n == L"wowclassict.exe" || n == L"wowt.exe" ||
         n == L"wowbeta.exe";
}

bool query_process_name(DWORD pid, std::wstring& outName) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return false;
  wchar_t path[32768]{};
  DWORD size = static_cast<DWORD>(std::size(path));
  bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
  CloseHandle(process);
  if (!ok) return false;
  outName = std::filesystem::path(std::wstring(path, size)).filename().wstring();
  return true;
}

bool is_wow_window(HWND hwnd, DWORD* pidOut = nullptr, std::wstring* exeOut = nullptr) {
  if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid) return false;

  std::wstring exe;
  if (!query_process_name(pid, exe) || !is_wow_executable_name(exe)) return false;

  wchar_t titleBuf[512]{};
  GetWindowTextW(hwnd, titleBuf, static_cast<int>(std::size(titleBuf)));
  std::wstring title = lower_copy(titleBuf);
  if (title.find(L"world of warcraft") == std::wstring::npos && title.find(L"warcraft") == std::wstring::npos) {
    return false;
  }

  if (pidOut) *pidOut = pid;
  if (exeOut) *exeOut = exe;
  return true;
}

bool target_is_foreground(const WowTarget& target) {
  HWND fg = GetForegroundWindow();
  if (!fg) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid != 0 && pid == target.pid;
}

bool target_still_valid(const WowTarget& target) {
  if (!target.valid || !target.hwnd || !IsWindow(target.hwnd)) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(target.hwnd, &pid);
  if (!pid || pid != target.pid) return false;
  std::wstring exe;
  return query_process_name(pid, exe) && is_wow_executable_name(exe);
}

bool window_is_fullscreen_on_monitor(HWND hwnd, const RECT& monitorRect) {
  RECT wr{};
  HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr));
  if (FAILED(hr)) GetWindowRect(hwnd, &wr);
  constexpr int tolerance = 18;
  return std::abs(wr.left - monitorRect.left) <= tolerance &&
         std::abs(wr.top - monitorRect.top) <= tolerance &&
         std::abs(wr.right - monitorRect.right) <= tolerance &&
         std::abs(wr.bottom - monitorRect.bottom) <= tolerance;
}

BOOL CALLBACK enum_wow_window(HWND hwnd, LPARAM lParam) {
  auto* result = reinterpret_cast<WowTarget*>(lParam);
  if (!result || result->valid) return FALSE;

  DWORD pid = 0;
  std::wstring exe;
  if (!is_wow_window(hwnd, &pid, &exe)) return TRUE;

  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
  if (!mon) return TRUE;

  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(mon, &mi)) return TRUE;

  DesktopOutput matched{};
  bool foundOutput = false;
  for (const auto& out : g.outputs) {
    if (_wcsicmp(out.name.c_str(), mi.szDevice) == 0) {
      matched = out;
      foundOutput = true;
      break;
    }
  }
  if (!foundOutput) return TRUE;

  wchar_t title[512]{};
  GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));

  result->hwnd = hwnd;
  result->pid = pid;
  result->title = title;
  result->exe_name = exe;
  result->output = matched;
  result->valid = true;
  result->fullscreen = window_is_fullscreen_on_monitor(hwnd, mi.rcMonitor);
  return FALSE;
}

WowTarget detect_wow_target() {
  WowTarget result{};
  EnumWindows(enum_wow_window, reinterpret_cast<LPARAM>(&result));
  return result;
}

std::wstring wow_detail_text(const WowTarget& target) {
  if (!target.valid) return L"Starte World of Warcraft. Der Client findet das Spiel automatisch.";
  if (!target.fullscreen) return L"WoW erkannt - bitte 'Fenster (Vollbild)' oder Vollbild aktivieren.";
  return L"";
}

void refresh_wow_target() {
  if (g.streaming.load()) return;
  WowTarget target = detect_wow_target();
  {
    std::lock_guard<std::mutex> lock(g.wow_mutex);
    g.wow_target = target;
  }

  if (!target.valid) {
    set_text(g.game, L"World of Warcraft nicht gefunden");
  } else if (!target.fullscreen) {
    set_text(g.game, L"World of Warcraft erkannt");
  } else {
    set_text(g.game, L"World of Warcraft bereit");
  }
  set_text(g.game_detail, wow_detail_text(target));
  if (g.game) InvalidateRect(g.game, nullptr, TRUE);
  if (g.game_detail) InvalidateRect(g.game_detail, nullptr, TRUE);
}


HFONT make_font(int pt, int weight, const wchar_t* family) {
  HDC dc = GetDC(nullptr);
  int dpi = GetDeviceCaps(dc, LOGPIXELSY);
  ReleaseDC(nullptr, dc);
  int height = -MulDiv(pt, dpi, 72);
  return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
}

void apply_font(HWND h, HFONT f) {
  SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
}

HWND make_static(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                 int id = 0, DWORD extra_style = 0) {
  HWND wnd = CreateWindowExW(0, L"STATIC", text,
      WS_CHILD | WS_VISIBLE | extra_style,
      x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  apply_font(wnd, g.font);
  return wnd;
}

HWND make_edit(HWND parent, int id, int x, int y, int w, int h, DWORD style = 0) {
  HWND wnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | style,
      x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  apply_font(wnd, g.font);
  return wnd;
}

HWND make_owner_button(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
  HWND wnd = CreateWindowExW(0, L"BUTTON", text,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
      x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  apply_font(wnd, g.font_bold);
  return wnd;
}

void draw_round_rect(HDC dc, const RECT& r, COLORREF fill, COLORREF border, int radius = 14) {
  HBRUSH b = CreateSolidBrush(fill);
  HPEN p = CreatePen(PS_SOLID, 1, border);
  HGDIOBJ ob = SelectObject(dc, b);
  HGDIOBJ op = SelectObject(dc, p);
  RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
  SelectObject(dc, op);
  SelectObject(dc, ob);
  DeleteObject(p);
  DeleteObject(b);
}

void draw_button(const DRAWITEMSTRUCT* dis) {
  RECT r = dis->rcItem;
  bool disabled = (dis->itemState & ODS_DISABLED) != 0;
  bool pressed = (dis->itemState & ODS_SELECTED) != 0;
  int id = static_cast<int>(dis->CtlID);

  COLORREF fill = C_PANEL_2;
  COLORREF border = C_BORDER;
  COLORREF text = C_TEXT;

  if (id == ID_STREAM) {
    if (g.streaming.load()) {
      fill = pressed ? RGB(70, 15, 18) : RGB(43, 16, 18);
      border = C_DANGER;
      text = RGB(255, 156, 164);
    } else {
      fill = pressed ? RGB(80, 255, 80) : C_GREEN;
      border = C_GREEN_2;
      text = RGB(0, 0, 0);
    }
  } else if (id == ID_LOGIN) {
    if (g.logged_in.load()) {
      fill = pressed ? RGB(25, 25, 25) : C_PANEL_2;
      border = C_GREEN;
      text = C_GREEN;
    } else {
      fill = pressed ? RGB(20, 20, 20) : C_PANEL;
      border = RGB(55, 55, 55);
      text = C_TEXT;
    }
  } else if (id == ID_SOUND) {
    fill = C_PANEL_2;
    border = C_BORDER;
    text = C_TEXT;
  }

  if (disabled) {
    fill = RGB(20, 20, 20);
    border = RGB(40, 40, 40);
    text = RGB(90, 90, 90);
  }

  draw_round_rect(dis->hDC, r, fill, border, 12);

  SetBkMode(dis->hDC, TRANSPARENT);
  SetTextColor(dis->hDC, text);
  SelectObject(dis->hDC, g.font_bold);

  wchar_t buf[256]{};
  GetWindowTextW(dis->hwndItem, buf, 255);

  if (id == ID_SOUND) {
    RECT label = r;
    label.left += 16;
    label.right -= 76;
    DrawTextW(dis->hDC, buf, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT toggle{r.right - 58, r.top + 10, r.right - 14, r.bottom - 10};
    COLORREF toggleFill = g.sound_enabled ? C_GREEN : RGB(51, 51, 51);
    draw_round_rect(dis->hDC, toggle, toggleFill, toggleFill, 18);
    int knob = 18;
    int kx = g.sound_enabled ? toggle.right - knob - 3 : toggle.left + 3;
    int ky = toggle.top + (toggle.bottom - toggle.top - knob) / 2;
    HBRUSH kb = CreateSolidBrush(g.sound_enabled ? RGB(0, 0, 0) : RGB(170, 170, 170));
    HGDIOBJ old = SelectObject(dis->hDC, kb);
    Ellipse(dis->hDC, kx, ky, kx + knob, ky + knob);
    SelectObject(dis->hDC, old);
    DeleteObject(kb);
  } else {
    DrawTextW(dis->hDC, buf, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  if (dis->itemState & ODS_FOCUS) {
    RECT f = r;
    InflateRect(&f, -4, -4);
    DrawFocusRect(dis->hDC, &f);
  }
}

void set_status(const wchar_t* mainText, const wchar_t* detailText) {
  set_text(g.status, mainText);
  set_text(g.detail, detailText);
  InvalidateRect(g.status, nullptr, TRUE);
}

void refresh_enabled_state() {
  bool busy = g.login_busy.load();
  bool live = g.streaming.load();
  bool logged = g.logged_in.load();
  bool wowReady = false;
  {
    std::lock_guard<std::mutex> lock(g.wow_mutex);
    wowReady = g.wow_target.valid && g.wow_target.fullscreen;
  }

  EnableWindow(g.username, !busy && !logged && !live);
  EnableWindow(g.password, !busy && !logged && !live);
  EnableWindow(g.login, !busy && !live);
  EnableWindow(g.sound, logged && !live);
  EnableWindow(g.stream, live || (logged && wowReady && !busy));

  if (busy) set_text(g.login, L"ANMELDUNG ...");
  else if (logged) set_text(g.login, L"ABMELDEN");
  else set_text(g.login, L"ANMELDEN");

  if (live) set_text(g.stream, L"STREAM BEENDEN");
  else set_text(g.stream, wowReady ? L"WOW-STREAM STARTEN" : L"WOW NOCH NICHT BEREIT");

  InvalidateRect(g.login, nullptr, TRUE);
  InvalidateRect(g.stream, nullptr, TRUE);
  InvalidateRect(g.sound, nullptr, TRUE);
}

void logout_user() {
  if (g.streaming.load()) return;
  g.creds = {};
  g.logged_in = false;
  set_text(g.team, L"Noch nicht angemeldet");
  set_text(g.password, L"");
  set_status(L"OFFLINE", L"Melde dich an. Der Teamname wird automatisch aus dem Dashboard geladen.");
  set_text(g.metrics, L"Capture: --   |   Frames: --   |   Spielsound: --");
  refresh_enabled_state();
}

void begin_login() {
  if (g.login_busy.exchange(true)) return;

  std::wstring usernameW = get_text(g.username);
  std::wstring passwordW = get_text(g.password);

  if (usernameW.empty() || passwordW.empty()) {
    g.login_busy = false;
    set_status(L"EINGABE FEHLT", L"Bitte Benutzername und Passwort eingeben.");
    refresh_enabled_state();
    return;
  }

  if (g.login_thread.joinable()) g.login_thread.join();

  set_status(L"VERBINDE", L"Anmeldung am Community-Clash Backend ...");
  refresh_enabled_state();

  g.login_thread = std::thread([username = wide_to_utf8(usernameW), password = wide_to_utf8(passwordW)]() mutable {
    try {
      auto result = fetch_streamer_token(L"https://community-clash.de", username, password);
      SecureZeroMemory(password.data(), password.size());
      auto* heapResult = new TokenResponse(std::move(result));
      PostMessageW(g.hwnd, WM_APP_LOGIN_OK, 0, reinterpret_cast<LPARAM>(heapResult));
    } catch (const std::exception& e) {
      SecureZeroMemory(password.data(), password.size());
      auto* msg = new std::wstring(utf8_to_wide(e.what()));
      PostMessageW(g.hwnd, WM_APP_ERROR, 1, reinterpret_cast<LPARAM>(msg));
    }
  });
}

template <typename Options>
void prefer_hardware_encoder(Options& options) {
  if constexpr (requires(Options& o) { o.video_encoder; }) {
    using Opt = std::remove_reference_t<decltype(options.video_encoder)>;
    using Backend = typename Opt::value_type;
    options.video_encoder = static_cast<Backend>(2);
  }
}

void begin_stream() {
  if (!g.logged_in.load() || g.streaming.load()) return;

  WowTarget wow;
  {
    std::lock_guard<std::mutex> lock(g.wow_mutex);
    wow = g.wow_target;
  }
  if (!wow.valid) {
    set_status(L"NICHT BEREIT", L"World of Warcraft wurde nicht gefunden.");
    return;
  }
  if (!wow.fullscreen) {
    set_status(L"NICHT BEREIT", L"Bitte World of Warcraft auf Fenster (Vollbild) oder Vollbild stellen.");
    return;
  }
  if (!target_still_valid(wow)) {
    refresh_wow_target();
    set_status(L"NICHT BEREIT", L"World of Warcraft ist nicht mehr verfügbar.");
    return;
  }

  if (g.stream_thread.joinable()) g.stream_thread.join();

  g.stop_requested = false;
  g.streaming = true;
  refresh_enabled_state();
  set_status(L"VERBINDE", L"WoW-Stream wird vorbereitet ...");

  TokenResponse creds = g.creds;
  DesktopOutput output = wow.output;
  bool useAudio = g.sound_enabled;

  g.desktop = std::make_shared<DesktopCapture>();
  g.audio = std::make_shared<LoopbackAudioCapture>();

  auto desktop = g.desktop;
  auto audio = g.audio;

  g.stream_thread = std::thread([creds = std::move(creds), output, wow, useAudio, desktop, audio]() mutable {
    std::unique_ptr<livekit::Room> room;
    try {
      if (!g.livekit_initialized.load()) {
        if (!livekit::initialize(livekit::LogLevel::Info)) {
          throw std::runtime_error("LiveKit Initialisierung fehlgeschlagen.");
        }
        g.livekit_initialized = true;
      }

      room = std::make_unique<livekit::Room>();
      livekit::RoomOptions roomOptions;
      roomOptions.auto_subscribe = false;
      roomOptions.dynacast = false;

      if (!room->connect(creds.livekit_url, creds.token, roomOptions)) {
        throw std::runtime_error("Verbindung zu LiveKit fehlgeschlagen.");
      }

      auto participant = room->localParticipant().lock();
      if (!participant) throw std::runtime_error("Lokaler LiveKit-Teilnehmer fehlt.");

      if (!creds.display_name.empty()) participant->setName(creds.display_name);

      auto videoSource = std::make_shared<livekit::VideoSource>(
          static_cast<int>(output.width), static_cast<int>(output.height));
      auto videoTrack = livekit::LocalVideoTrack::createLocalVideoTrack(
          "community-clash-screen", videoSource);

      livekit::TrackPublishOptions videoOptions;
      videoOptions.source = livekit::TrackSource::SOURCE_SCREENSHARE;
      videoOptions.stream = std::string("community-clash-screen");
      videoOptions.simulcast = false;
      videoOptions.video_codec = livekit::VideoCodec::H264;
      videoOptions.video_encoding = livekit::VideoEncodingOptions{12'000'000, 60.0};
      videoOptions.degradation_preference = livekit::DegradationPreference::Balanced;
      prefer_hardware_encoder(videoOptions);
      participant->publishTrack(videoTrack, videoOptions);

      std::shared_ptr<livekit::AudioSource> audioSource;
      std::shared_ptr<livekit::LocalAudioTrack> audioTrack;
      std::atomic<bool> audioPublished{false};

      if (useAudio) {
        audio->start([&](int rate, int channels, std::vector<std::int16_t>&& pcm) {
          try {
            if (!audioPublished.exchange(true)) {
              audioSource = std::make_shared<livekit::AudioSource>(rate, channels, 0);
              audioTrack = livekit::LocalAudioTrack::createLocalAudioTrack(
                  "community-clash-screen-audio", audioSource);

              livekit::TrackPublishOptions audioOptions;
              audioOptions.source = livekit::TrackSource::SOURCE_SCREENSHARE_AUDIO;
              audioOptions.stream = std::string("community-clash-screen");
              audioOptions.audio_encoding = livekit::AudioEncodingOptions{192'000};
              audioOptions.dtx = false;
              audioOptions.red = false;
              participant->publishTrack(audioTrack, audioOptions);
            }

            if (audioSource) {
              const int samplesPerChannel = static_cast<int>(pcm.size()) / channels;
              livekit::AudioFrame frame(std::move(pcm), rate, channels, samplesPerChannel);
              audioSource->captureFrame(frame, 0);
            }
          } catch (...) {
          }
        });
      }

      desktop->start_window(wow.hwnd,
        [&](unsigned w, unsigned h, std::vector<std::uint8_t>&& bgra, std::int64_t timestampUs) {
          try {
            if (!target_still_valid(wow)) {
              g.stop_requested = true;
              return;
            }
            g.wow_paused = false;
            livekit::VideoFrame frame(
                static_cast<int>(w), static_cast<int>(h),
                livekit::VideoBufferType::BGRA, std::move(bgra));
            videoSource->captureFrame(frame, timestampUs);
          } catch (...) {
          }
        });

      g.wow_paused = false;
      PostMessageW(g.hwnd, WM_APP_STREAM_LIVE, 0, 0);

      while (!g.stop_requested.load()) {
        if (!target_still_valid(wow)) {
          g.stop_requested = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      desktop->stop();
      if (useAudio) audio->stop();
      room->disconnect();
      room.reset();
      PostMessageW(g.hwnd, WM_APP_STREAM_STOPPED, 0, 0);
    } catch (const std::exception& e) {
      try { desktop->stop(); } catch (...) {}
      try { if (useAudio) audio->stop(); } catch (...) {}
      try { if (room) room->disconnect(); } catch (...) {}
      auto* msg = new std::wstring(utf8_to_wide(e.what()));
      PostMessageW(g.hwnd, WM_APP_ERROR, 2, reinterpret_cast<LPARAM>(msg));
    }
  });
}

void stop_stream() {
  if (!g.streaming.load()) return;
  g.stop_requested = true;
  set_text(g.stream, L"WIRD GESTOPPT ...");
  EnableWindow(g.stream, FALSE);
  set_status(L"STOPPE", L"Stream wird sauber beendet ...");
  InvalidateRect(g.stream, nullptr, TRUE);
}

void paint_background(HWND hwnd) {
  PAINTSTRUCT ps{};
  HDC dc = BeginPaint(hwnd, &ps);
  RECT client{};
  GetClientRect(hwnd, &client);
  FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

  // Subtle website-like grid.
  HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(0, 13, 0));
  HGDIOBJ oldPen = SelectObject(dc, gridPen);
  for (int x = 0; x < client.right; x += 42) {
    MoveToEx(dc, x, 142, nullptr); LineTo(dc, x, client.bottom);
  }
  for (int y = 142; y < client.bottom; y += 42) {
    MoveToEx(dc, 0, y, nullptr); LineTo(dc, client.right, y);
  }
  SelectObject(dc, oldPen);
  DeleteObject(gridPen);

  RECT topLine{0, 0, client.right, 3};
  HBRUSH green = CreateSolidBrush(C_GREEN);
  FillRect(dc, &topLine, green);
  DeleteObject(green);

  RECT header{24, 20, 936, 140};
  RECT loginPanel{24, 158, 448, 526};
  RECT streamPanel{464, 158, 936, 526};
  RECT gamePanel{488, 220, 912, 306};
  RECT statsPanel{24, 544, 936, 636};
  draw_round_rect(dc, header, RGB(5, 5, 5), RGB(24, 56, 24), 18);
  draw_round_rect(dc, loginPanel, C_PANEL, RGB(35, 35, 35), 18);
  draw_round_rect(dc, streamPanel, C_PANEL, RGB(35, 35, 35), 18);
  draw_round_rect(dc, gamePanel, RGB(7, 7, 7), RGB(31, 64, 31), 14);
  draw_round_rect(dc, statsPanel, RGB(7, 7, 7), RGB(32, 32, 32), 16);

  HICON embeddedLogo = reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, 96, 96, LR_DEFAULTCOLOR));
  if (embeddedLogo) {
    DrawIconEx(dc, 38, 30, embeddedLogo, 96, 96, 0, nullptr, DI_NORMAL);
    DestroyIcon(embeddedLogo);
  }

  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g.font_brand);
  SetTextColor(dc, C_TEXT);
  RECT brand{150, 34, 560, 72};
  DrawTextW(dc, L"COMMUNITY CLASH", -1, &brand, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

  SelectObject(dc, g.font_kicker);
  SetTextColor(dc, C_GREEN);
  RECT sub{152, 76, 560, 100};
  DrawTextW(dc, L"VON CIARO  /  STREAMER CLIENT", -1, &sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

  RECT badge{690, 43, 912, 82};
  draw_round_rect(dc, badge, RGB(0, 24, 0), RGB(0, 105, 0), 12);
  SelectObject(dc, g.font_kicker);
  SetTextColor(dc, C_GREEN);
  DrawTextW(dc, L"WORLD OF WARCRAFT ONLY", -1, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

  SelectObject(dc, g.font_kicker);
  SetTextColor(dc, C_GREEN);
  RECT ltitle{48, 178, 404, 202};
  DrawTextW(dc, L"STREAMER LOGIN", -1, &ltitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  RECT rtitle{488, 178, 900, 202};
  DrawTextW(dc, L"WORLD OF WARCRAFT STREAM", -1, &rtitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

  EndPaint(hwnd, &ps);
}

void create_ui(HWND hwnd) {
  g.hwnd = hwnd;
  g.font = make_font(11, FW_NORMAL, L"Segoe UI");
  g.font_bold = make_font(11, FW_BOLD, L"Segoe UI");
  g.font_small = make_font(9, FW_NORMAL, L"Segoe UI");
  g.font_brand = make_font(27, FW_BOLD, L"Impact");
  g.font_kicker = make_font(9, FW_BOLD, L"Segoe UI");
  g.font_game = make_font(13, FW_BOLD, L"Segoe UI");
  g.edit_brush = CreateSolidBrush(RGB(7, 7, 7));
  g.panel_brush = CreateSolidBrush(C_PANEL);
  g.game_brush = CreateSolidBrush(RGB(7, 7, 7));
  g.stats_brush = CreateSolidBrush(RGB(7, 7, 7));

  try { g.outputs = DesktopCapture::enumerate(); } catch (...) { g.outputs.clear(); }

  HWND userLabel = make_static(hwnd, L"BENUTZERNAME", 48, 220, 350, 20);
  apply_font(userLabel, g.font_kicker);
  g.username = make_edit(hwnd, ID_USERNAME, 48, 244, 376, 38);

  HWND passLabel = make_static(hwnd, L"PASSWORT", 48, 300, 350, 20);
  apply_font(passLabel, g.font_kicker);
  g.password = make_edit(hwnd, ID_PASSWORD, 48, 324, 376, 38, ES_PASSWORD);
  SendMessageW(g.password, EM_SETPASSWORDCHAR, static_cast<WPARAM>(L'\x2022'), 0);

  g.login = make_owner_button(hwnd, ID_LOGIN, L"ANMELDEN", 48, 382, 376, 46);

  HWND teamLabel = make_static(hwnd, L"TEAMNAME AUS DEM DASHBOARD", 48, 450, 350, 20);
  apply_font(teamLabel, g.font_kicker);
  g.team = make_edit(hwnd, ID_TEAM, 48, 474, 376, 38, ES_READONLY);
  set_text(g.team, L"Noch nicht angemeldet");

  g.game = make_static(hwnd, L"World of Warcraft wird gesucht ...", 510, 232, 378, 28, ID_GAME);
  apply_font(g.game, g.font_game);
  g.game_detail = make_static(hwnd, L"Starte World of Warcraft. Der Client findet das Spiel automatisch.",
                              510, 270, 378, 34, ID_GAME_DETAIL);
  apply_font(g.game_detail, g.font_small);

  g.sound = make_owner_button(hwnd, ID_SOUND, L"SPIELSOUND UEBERTRAGEN", 488, 326, 424, 48);

  HWND audioHint = make_static(hwnd,
      L"WoW bleibt auch beim Alt-Tab im Stream. Mikrofon bleibt aus.",
      488, 384, 424, 30);
  apply_font(audioHint, g.font_small);

  g.stream = make_owner_button(hwnd, ID_STREAM, L"WOW NOCH NICHT BEREIT", 488, 428, 424, 56);

  HWND qualityInfo = make_static(hwnd,
      L"Fensteraufnahme: Andere Programme werden beim Alt-Tab nicht gezeigt.",
      488, 494, 424, 30);
  apply_font(qualityInfo, g.font_small);

  g.status = make_static(hwnd, L"OFFLINE", 48, 565, 220, 24, ID_STATUS);
  apply_font(g.status, g.font_bold);
  g.metrics = make_static(hwnd, L"Capture: --   |   Frames: --   |   Spielsound: --",
                          300, 565, 588, 24, ID_METRICS, SS_RIGHT);
  apply_font(g.metrics, g.font_small);
  g.detail = make_static(hwnd,
      L"Melde dich an. Der Teamname wird automatisch aus dem Dashboard geladen.",
      48, 600, 840, 28, ID_DETAIL);
  apply_font(g.detail, g.font_small);

  refresh_wow_target();
  refresh_enabled_state();
  SetTimer(hwnd, 1, 500, nullptr);
}

void cleanup() {
  g.stop_requested = true;
  if (g.stream_thread.joinable()) g.stream_thread.join();
  if (g.login_thread.joinable()) g.login_thread.join();
  if (g.livekit_initialized.load()) {
    livekit::shutdown();
    g.livekit_initialized = false;
  }
  if (g.font) DeleteObject(g.font);
  if (g.font_bold) DeleteObject(g.font_bold);
  if (g.font_small) DeleteObject(g.font_small);
  if (g.font_brand) DeleteObject(g.font_brand);
  if (g.font_kicker) DeleteObject(g.font_kicker);
  if (g.font_game) DeleteObject(g.font_game);
  if (g.edit_brush) DeleteObject(g.edit_brush);
  if (g.panel_brush) DeleteObject(g.panel_brush);
  if (g.game_brush) DeleteObject(g.game_brush);
  if (g.stats_brush) DeleteObject(g.stats_brush);
  g.logo.reset();
  if (g.gdiplus_token) { Gdiplus::GdiplusShutdown(g.gdiplus_token); g.gdiplus_token = 0; }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE: {
      BOOL dark = TRUE;
      DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
      create_ui(hwnd);
      return 0;
    }

    case WM_COMMAND: {
      int id = LOWORD(wParam);
      if (id == ID_LOGIN && HIWORD(wParam) == BN_CLICKED) {
        if (g.logged_in.load()) logout_user();
        else begin_login();
        return 0;
      }
      if (id == ID_SOUND && HIWORD(wParam) == BN_CLICKED) {
        if (!g.streaming.load()) {
          g.sound_enabled = !g.sound_enabled;
          InvalidateRect(g.sound, nullptr, TRUE);
        }
        return 0;
      }
      if (id == ID_STREAM && HIWORD(wParam) == BN_CLICKED) {
        if (g.streaming.load()) stop_stream();
        else begin_stream();
        return 0;
      }
      break;
    }

    case WM_DRAWITEM: {
      auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
      if (dis->CtlType == ODT_BUTTON) {
        draw_button(dis);
        return TRUE;
      }
      if (dis->CtlType == ODT_COMBOBOX) {
        HDC dc = dis->hDC;
        RECT r = dis->rcItem;
        bool sel = (dis->itemState & ODS_SELECTED) != 0;
        HBRUSH b = CreateSolidBrush(sel ? RGB(20, 40, 20) : RGB(7, 7, 7));
        FillRect(dc, &r, b);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, sel ? C_GREEN : C_TEXT);
        SelectObject(dc, g.font);
        if (dis->itemID != static_cast<UINT>(-1)) {
          wchar_t text[512]{};
          SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, reinterpret_cast<LPARAM>(text));
          r.left += 10;
          DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        return TRUE;
      }
      break;
    }

    case WM_MEASUREITEM: {
      auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
      if (mi->CtlType == ODT_COMBOBOX) {
        mi->itemHeight = 30;
        return TRUE;
      }
      break;
    }

    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      SetTextColor(dc, C_TEXT);
      SetBkColor(dc, RGB(7, 7, 7));
      return reinterpret_cast<LRESULT>(g.edit_brush);
    }

    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      HWND ctl = reinterpret_cast<HWND>(lParam);
      int id = GetDlgCtrlID(ctl);
      SetBkMode(dc, OPAQUE);

      HBRUSH brush = g.panel_brush;
      COLORREF bg = C_PANEL;
      if (id == ID_GAME || id == ID_GAME_DETAIL) {
        brush = g.game_brush;
        bg = RGB(7, 7, 7);
      } else if (id == ID_STATUS || id == ID_METRICS || id == ID_DETAIL) {
        brush = g.stats_brush;
        bg = RGB(7, 7, 7);
      }
      SetBkColor(dc, bg);

      if (id == ID_STATUS) {
        SetTextColor(dc, g.streaming.load() ? C_GREEN : C_MUTED);
      } else if (id == ID_GAME) {
        WowTarget t; { std::lock_guard<std::mutex> lock(g.wow_mutex); t = g.wow_target; }
        SetTextColor(dc, (t.valid && t.fullscreen) ? C_GREEN : (t.valid ? RGB(255, 210, 87) : C_MUTED));
      } else {
        SetTextColor(dc, C_MUTED);
      }
      return reinterpret_cast<LRESULT>(brush);
    }

    case WM_TIMER: {
      if (wParam == 1) {
        if (!g.streaming.load()) {
          refresh_wow_target();
          refresh_enabled_state();
        }

        auto desktop = g.desktop;
        auto audio = g.audio;
        if (g.streaming.load() && desktop) {
          int fps = static_cast<int>(desktop->fps() + 0.5);
          std::uint64_t frames = desktop->frames();
          std::uint64_t audioBlocks = (g.sound_enabled && audio) ? audio->blocks() : 0;
          std::wstring m = L"Capture: " + std::to_wstring(fps) + L" FPS   |   Frames: " +
                           std::to_wstring(frames) + L"   |   Spielsound: " +
                           (g.sound_enabled ? (audioBlocks > 0 ? L"AKTIV" : L"WARTE") : L"AUS");
          set_text(g.metrics, m);

          set_status(L"LIVE - WOW ONLY", L"World of Warcraft wird direkt als Fenster aufgenommen und bleibt auch beim Alt-Tab sichtbar.");
        }
      }
      return 0;
    }

    case WM_APP_LOGIN_OK: {
      std::unique_ptr<TokenResponse> result(reinterpret_cast<TokenResponse*>(lParam));
      g.login_busy = false;
      g.creds = std::move(*result);
      g.logged_in = true;
      set_text(g.password, L"");
      std::wstring team = utf8_to_wide(g.creds.display_name);
      if (team.empty()) team = utf8_to_wide(g.creds.identity);
      if (team.empty()) team = L"Teamname nicht hinterlegt";
      set_text(g.team, team);
      set_status(L"BEREIT", L"Angemeldet. Teamname wurde automatisch aus dem Dashboard uebernommen.");
      refresh_enabled_state();
      return 0;
    }

    case WM_APP_STREAM_LIVE: {
      set_status(L"LIVE - WOW ONLY", L"World of Warcraft wird direkt als Fenster aufgenommen und bleibt auch beim Alt-Tab sichtbar.");
      refresh_enabled_state();
      return 0;
    }

    case WM_APP_STREAM_STOPPED: {
      g.streaming = false;
      g.stop_requested = false;
      g.wow_paused = false;
      if (g.stream_thread.joinable() && g.stream_thread.get_id() != std::this_thread::get_id()) {
        g.stream_thread.join();
      }
      g.desktop.reset();
      g.audio.reset();
      set_text(g.metrics, L"Capture: --   |   Frames: --   |   Spielsound: --");
      if (g.close_pending.load()) {
        DestroyWindow(hwnd);
        return 0;
      }
      set_status(L"BEREIT", L"Stream beendet. Du kannst jederzeit erneut starten.");
      refresh_wow_target();
      refresh_enabled_state();
      return 0;
    }

    case WM_APP_ERROR: {
      std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lParam));
      if (wParam == 1) g.login_busy = false;
      if (wParam == 2) {
        g.streaming = false;
        g.stop_requested = false;
        g.desktop.reset();
        g.audio.reset();
      }
      if (g.close_pending.load()) {
        DestroyWindow(hwnd);
        return 0;
      }
      set_status(L"FEHLER", error->c_str());
      refresh_enabled_state();
      MessageBoxW(hwnd, error->c_str(), L"Community Clash Streamer", MB_OK | MB_ICONERROR);
      return 0;
    }

    case WM_PAINT:
      paint_background(hwnd);
      return 0;

    case WM_CLOSE:
      if (g.streaming.load()) {
        if (!g.close_pending.exchange(true)) {
          g.stop_requested = true;
          EnableWindow(hwnd, FALSE);
          set_text(g.stream, L"WIRD BEENDET ...");
          set_status(L"BEENDE", L"Stream und LiveKit-Verbindung werden sauber getrennt ...");
          InvalidateRect(g.stream, nullptr, TRUE);
        }
        return 0;
      }
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      KillTimer(hwnd, 1);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&icc);

  const wchar_t* className = L"CommunityClashStreamerWindow";
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = reinterpret_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
  wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = className;
  RegisterClassExW(&wc);

  RECT r{0, 0, 960, 660};
  AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);

  HWND hwnd = CreateWindowExW(
      0, className, L"Community Clash Streamer",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
      nullptr, nullptr, hInstance, nullptr);

  if (!hwnd) return 1;

  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  cleanup();
  return static_cast<int>(msg.wParam);
}
