#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "uxtheme.lib")

#include <algorithm>
#include <atomic>
#include <ctime>
#include <dwmapi.h>
#include <shellapi.h>
#include <sstream>
#include <string>
#include <thread>
#include <uxtheme.h>
#include <vector>
#include <functional>
#include <wincrypt.h>
#include <windows.h>
#include <windowsx.h>
#include <winsock2.h>
#include <ws2tcpip.h>

// --- CONSTANTS ---
#define WM_TRAY (WM_USER + 1)
#define WM_OBS (WM_USER + 2)
#define IDI_MAIN_ICON 101
#define TIMER_WARN 999

// Palette
#define COL_BG RGB(20, 20, 20)
#define COL_GRP_BORDER RGB(80, 80, 80)
#define COL_TEXT RGB(200, 200, 200)
#define COL_INPUT_BG RGB(30, 30, 30)
#define COL_ACCENT RGB(60, 100, 160)
#define COL_BTN_FACE RGB(50, 50, 50)
#define COL_STATUS_ERR RGB(255, 50, 50)
#define COL_STATUS_REC RGB(0, 255, 0)
#define COL_STATUS_WS RGB(100, 150, 255)
#define COL_WARN_TEXT RGB(255, 20, 20)

// Modes
enum AppMode { MODE_INFO = 0, MODE_WARN = 1, MODE_WARN_ONLY = 2 };

struct AppState {
  int size = 30;
  int pad = 20;
  int pos = 0; // 0=BR, 1=BL, 2=TR, 3=TL
  bool autoStart = false;
  int mode = MODE_INFO;
  std::string processList = ""; // Pipe delimited in memory/ini, newline in edit
  bool showOverloadWarn = false;
  bool ghostMode = false;
} cfg;

struct {
  int x = -1, y = -1, s = -1;
  bool vis = false;
  bool ghost = false;
} lastOv;
bool lastWarnVis = false; // Optimize warning window updates
std::atomic<bool> isRec(false), isStream(false), isConn(false), isTest(false);
std::atomic<bool> g_shutdown(false);  // Graceful shutdown flag
std::atomic<DWORD> overloadWarnUntil(0);
bool showLic = false;
bool showProcList = false;
float g_Scale = 1.0f;
// Windows
HWND hMain = NULL, hOv = NULL, hWarn = NULL;
HWND hEditPad = NULL, hEditSize = NULL, hEditPass = NULL, hEditLic = NULL,
     hEditProcs = NULL;
NOTIFYICONDATA nid = {sizeof(NOTIFYICONDATA)};

// Warning Logic State
bool warnActive = false;
bool warnTargetFocused = false;
DWORD warnCycleStart = 0;
bool warnVisible = false;

// Full license text
const char *LIC_TEXT =
    "MIT License\r\n\r\nCopyright (c) 2025 aufkrawall\r\n\r\nPermission is "
    "hereby granted, free of charge, to any person obtaining a copy of this "
    "software and associated documentation files (the \"Software\"), to deal "
    "in the Software without restriction, including without limitation the "
    "rights to use, copy, modify, merge, publish, distribute, sublicense, "
    "and/or sell copies of the Software, and to permit persons to whom the "
    "Software is furnished to do so, subject to the following "
    "conditions:\r\n\r\nThe above copyright notice and this permission notice "
    "shall be included in all copies or substantial portions of the "
    "Software.\r\n\r\nTHE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF "
    "ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES "
    "OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. "
    "IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY "
    "CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT "
    "OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR "
    "THE USE OR OTHER DEALINGS IN THE SOFTWARE.";

// --- HELPERS ---
int S(int v) { return (int)(v * g_Scale); }
void Wipe(std::string &s) {
  SecureZeroMemory(&s[0], s.size());
  s.clear();
}

// --- STRING HELPER ---
std::string ReplaceAll(std::string str, const std::string &from,
                       const std::string &to) {
  size_t start_pos = 0;
  while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
    str.replace(start_pos, from.length(), to);
    start_pos += to.length();
  }
  return str;
}

// --- FOCUS CHECK ---
bool IsForegroundTarget() {
  if (cfg.processList.empty())
    return false;

  HWND hFg = GetForegroundWindow();
  if (!hFg)
    return false;

  DWORD pid = 0;
  GetWindowThreadProcessId(hFg, &pid);
  if (pid == 0)
    return false;

  // Cache results to ensure zero overhead while the same window is focused
  static DWORD lastPid = 0;
  static bool lastRes = false;
  static DWORD lastCheckTime = 0;

  // Re-validate string every 2 seconds in case config changed, otherwise trust
  // cache
  if (pid == lastPid && (GetTickCount() - lastCheckTime < 2000))
    return lastRes;

  bool match = false;
  // PROCESS_QUERY_LIMITED_INFORMATION is sufficient and requires fewer rights
  HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (hProcess) {
    char exePath[MAX_PATH];
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameA(hProcess, 0, exePath, &size)) {
      std::string exeName = exePath;
      size_t lastSlash = exeName.find_last_of("\\/");
      if (lastSlash != std::string::npos)
        exeName = exeName.substr(lastSlash + 1);
      std::transform(exeName.begin(), exeName.end(), exeName.begin(),
                     ::tolower);

      std::stringstream ss(cfg.processList);
      std::string item;
      while (std::getline(ss, item, '|')) {
        if (!item.empty()) {
          std::string t = item;
          std::transform(t.begin(), t.end(), t.begin(), ::tolower);
          if (exeName == t) {
            match = true;
            break;
          }
        }
      }
    }
    CloseHandle(hProcess);
  }

  lastPid = pid;
  lastRes = match;
  lastCheckTime = GetTickCount();
  return match;
}

// --- CRYPTO ---
std::string Base64(const std::vector<BYTE> &data) {
  if (data.empty())
    return "";
  DWORD len = 0;
  if (!CryptBinaryToStringA(data.data(), (DWORD)data.size(),
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL,
                            &len))
    return "";
  std::string res(len, 0);
  if (!CryptBinaryToStringA(data.data(), (DWORD)data.size(),
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &res[0],
                            &len))
    return "";
  while (!res.empty() && res.back() == '\0')
    res.pop_back();
  return res;
}

std::vector<BYTE> Sha256(const std::string &data) {
  HCRYPTPROV hP;
  HCRYPTHASH hH;
  std::vector<BYTE> hash(32);
  if (CryptAcquireContext(&hP, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
    if (CryptCreateHash(hP, CALG_SHA_256, 0, 0, &hH)) {
      CryptHashData(hH, (BYTE *)data.data(), (DWORD)data.size(), 0);
      DWORD len = 32;
      CryptGetHashParam(hH, HP_HASHVAL, hash.data(), &len, 0);
      CryptDestroyHash(hH);
    }
    CryptReleaseContext(hP, 0);
  }
  return hash;
}

std::string Auth(std::string &pass, const std::string &salt,
                 const std::string &chal) {
  std::string secret = Base64(Sha256(pass + salt));
  std::string res = Base64(Sha256(secret + chal));
  Wipe(pass);
  Wipe(secret);
  return res;
}

// --- CONFIG ---
std::string ManagePass(const char *newPass = nullptr) {
  char path[MAX_PATH];
  GetModuleFileNameA(NULL, path, MAX_PATH);
  char *dot = strrchr(path, '.');
  if (!dot) return "";  // Safety check
  strcpy(dot, ".ini");

  if (newPass) {
    std::string enc =
        Base64(std::vector<BYTE>(newPass, newPass + strlen(newPass)));
    WritePrivateProfileStringA("S", "Pw", enc.c_str(), path);
    return "";
  } else {
    char buf[512] = {0};
    GetPrivateProfileStringA("S", "Pw", "", buf, 512, path);
    if (strlen(buf) == 0)
      return "";
    DWORD len = 0;
    CryptStringToBinaryA(buf, 0, CRYPT_STRING_BASE64, NULL, &len, 0, 0);
    std::string dec(len, 0);
    CryptStringToBinaryA(buf, 0, CRYPT_STRING_BASE64, (BYTE *)&dec[0], &len, 0,
                         0);
    return dec;
  }
}

void IOCfg(bool save) {
  char path[MAX_PATH];
  GetModuleFileNameA(NULL, path, MAX_PATH);
  char *dot = strrchr(path, '.');
  if (!dot) return;  // Safety check
  strcpy(dot, ".ini");
  if (save) {
    WritePrivateProfileStringA("S", "Sz", std::to_string(cfg.size).c_str(),
                               path);
    WritePrivateProfileStringA("S", "Pd", std::to_string(cfg.pad).c_str(),
                               path);
    WritePrivateProfileStringA("S", "Ps", std::to_string(cfg.pos).c_str(),
                               path);
    WritePrivateProfileStringA("S", "Au", cfg.autoStart ? "1" : "0", path);
    WritePrivateProfileStringA("S", "Md", std::to_string(cfg.mode).c_str(),
                               path);
    WritePrivateProfileStringA("S", "Ow", cfg.showOverloadWarn ? "1" : "0",
                               path);
    WritePrivateProfileStringA("S", "Gm", cfg.ghostMode ? "1" : "0", path);
    WritePrivateProfileStringA("S", "Pr", cfg.processList.c_str(), path);
  } else {
    cfg.size = GetPrivateProfileIntA("S", "Sz", 30, path);
    cfg.pad = GetPrivateProfileIntA("S", "Pd", 20, path);
    cfg.pos = GetPrivateProfileIntA("S", "Ps", 0, path);
    cfg.autoStart = GetPrivateProfileIntA("S", "Au", 0, path) == 1;
    cfg.showOverloadWarn = GetPrivateProfileIntA("S", "Ow", 0, path) == 1;
    cfg.ghostMode = GetPrivateProfileIntA("S", "Gm", 0, path) == 1;
    cfg.mode = GetPrivateProfileIntA("S", "Md", 0, path);
    char buf[4096] = {0};
    GetPrivateProfileStringA("S", "Pr", "", buf, 4096, path);
    cfg.processList = buf;
  }
}

// --- NETWORKING ---
std::string JsonVal(const std::string &json, const std::string &key) {
  std::string q = "\"" + key + "\"";
  size_t p = json.find(q);
  if (p == std::string::npos)
    return "";
  p = json.find(":", p) + 1;
  while (p < json.size() && (isspace(json[p]) || json[p] == '"'))
    p++;
  size_t e = p;
  while (e < json.size() && json[e] != '"' && json[e] != ',' && json[e] != '}')
    e++;
  return json.substr(p, e - p);
}

void SendWS(SOCKET s, const std::string &txt) {
  std::vector<BYTE> f = {0x81};
  if (txt.size() > 125) {
    f.push_back(0x80 | 126);
    unsigned short l = htons((unsigned short)txt.size());
    f.insert(f.end(), (BYTE *)&l, (BYTE *)&l + 2);
  } else
    f.push_back(0x80 | (BYTE)txt.size());

  DWORD m = GetTickCount();
  BYTE mask[4];
  memcpy(mask, &m, 4);
  f.insert(f.end(), mask, mask + 4);
  for (size_t i = 0; i < txt.size(); i++)
    f.push_back(txt[i] ^ mask[i % 4]);
  send(s, (char *)f.data(), (int)f.size(), 0);
}

void NetThread() {
  WSADATA w;
  WSAStartup(MAKEWORD(2, 2), &w);  // Initialize once at thread start
  
  while (!g_shutdown) {
    Sleep(1000);
    if (g_shutdown) break;
    
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {AF_INET, htons(4455)};
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);

    if (isConn) {
      isConn = false;
      isRec = false;
      isStream = false;
      PostMessage(hMain, WM_OBS, 0, 0);
    }

    if (connect(s, (sockaddr *)&a, sizeof(a)) == 0) {
      std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
      std::string req =
          "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: "
          "websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
          key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
      send(s, req.c_str(), (int)req.size(), 0);

      // Robust Handshake Handling
      char buf[4096];
      std::string bufStr;
      bool handshakeDone = false;

      // Read until we get the full header
      while (!handshakeDone) {
        int r = recv(s, buf, sizeof(buf), 0);
        if (r <= 0)
          break;
        bufStr.append(buf, r);

        if (bufStr.find("\r\n\r\n") != std::string::npos) {
          handshakeDone = true;
        }
      }

      if (handshakeDone && bufStr.find("101") != std::string::npos) {
        // Check if we have extra data (the body) in the buffer
        size_t headerEnd = bufStr.find("\r\n\r\n") + 4;
        std::string hello;

        if (headerEnd < bufStr.size()) {
          // Body was partially or fully read with headers
          hello = bufStr.substr(headerEnd);
        } else {
          // Body needs to be read
          int n = recv(s, buf, 4096, 0);
          if (n > 0)
            hello.assign(buf, n);
        }

        std::string salt = JsonVal(hello, "salt"),
                    chal = JsonVal(hello, "challenge");

        std::string id =
            "{\"op\":1,\"d\":{\"rpcVersion\":1,\"eventSubscriptions\":65";
        if (!salt.empty() && !chal.empty()) {
          std::string p = ManagePass();
          id += ",\"authentication\":\"" + Auth(p, salt, chal) + "\"";
        }
        id += "}}";
        SendWS(s, id);

        // Wait for OpCode 2 (Identified)
        bool authenticated = false;

        DWORD lastPollRec = 0;
        DWORD lastPollStr = 0;
        DWORD lastPollStats = 0;
        int lastSkippedFrames = -1;

        while (true) {
          DWORD now = GetTickCount();

          if (authenticated) {
            // Interleave polling to avoid packet merging/collision issues in
            // simple parser
            if (now - lastPollRec > 3000) {
              SendWS(s, "{\"op\":6,\"d\":{\"requestType\":\"GetRecordStatus\","
                        "\"requestId\":\"pollR\"}}");
              lastPollRec = now;
            }
            if (now - lastPollStr > 3000 && now - lastPollRec > 1500) {
              SendWS(s, "{\"op\":6,\"d\":{\"requestType\":\"GetStreamStatus\","
                        "\"requestId\":\"pollS\"}}");
              lastPollStr = now;
            }
            if (now - lastPollStats > 2000) {
              SendWS(s, "{\"op\":6,\"d\":{\"requestType\":\"GetStats\","
                        "\"requestId\":\"pollSt\"}}");
              lastPollStats = now;
            }
          }

          timeval tv = {0, 100000};
          fd_set fds;
          FD_ZERO(&fds);
          FD_SET(s, &fds);
          if (select(0, &fds, NULL, NULL, &tv) > 0) {
            int r = recv(s, buf, 4096, 0);
            if (r <= 0)
              break;
            std::string chunk(buf, r);

            // Check for Identification Success (OpCode 2)
            if (!authenticated && chunk.find("\"op\":2") != std::string::npos) {
              authenticated = true;
              isConn = true;
              PostMessage(hMain, WM_OBS, 0, 0);
              // Reset timers to poll immediately
              lastPollRec = 0;
              lastPollStr = 0;
              lastPollStats = 0;
            }

            // Check Recording Status
            if (chunk.find("RecordStateChanged") != std::string::npos ||
                chunk.find("GetRecordStatus") != std::string::npos) {
              bool act = chunk.find("\"outputActive\":true") != std::string::npos;
              if (act != isRec) {
                isRec = act;
                PostMessage(hMain, WM_OBS, 0, 0);
              }
            }

            // Check Streaming Status
            if (chunk.find("StreamStateChanged") != std::string::npos ||
                chunk.find("GetStreamStatus") != std::string::npos) {
              bool act = chunk.find("\"outputActive\":true") != std::string::npos;
              if (act != isStream) {
                isStream = act;
                PostMessage(hMain, WM_OBS, 0, 0);
              }
            }

            // Check Stats (Encoder Overload)
            if (chunk.find("GetStats") != std::string::npos) {
              std::string skipS = JsonVal(chunk, "outputSkippedFrames");
              if (!skipS.empty()) {
                int skip = atoi(skipS.c_str());
                if (lastSkippedFrames != -1 && skip > lastSkippedFrames) {
                  DWORD n = GetTickCount();
                  if (n < overloadWarnUntil)
                    overloadWarnUntil += 5000;
                  else
                    overloadWarnUntil = n + 5000;
                  PostMessage(hMain, WM_OBS, 0, 0);
                }
                lastSkippedFrames = skip;
              }
            }
          }
        }
      }
    }
    closesocket(s);
    if (isConn) {
      isConn = false;
      isRec = false;
      isStream = false;
      PostMessage(hMain, WM_OBS, 0, 0);
    }
    overloadWarnUntil = 0;
    if (!g_shutdown) Sleep(1000);
  }
  
  WSACleanup();  // Cleanup once at thread end
}

// --- UI ---
HFONT hF, hFb;
// Cached Warning Resources
HFONT g_hFontWarn = NULL;
HBITMAP g_hBmWarn = NULL;
HDC g_hdcWarn = NULL;
SIZE g_sizeWarn = {0, 0};
std::string g_lastWarnMsg = "";

HBRUSH hBb, hBp, hBr, hBbtn, hBa, hBi;
struct Hit {
  int id;
  RECT r;
};
std::vector<Hit> hits;

void InitGDI() {
  hF = CreateFontA(-S(16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                   0, 0, "Segoe UI");
  hFb = CreateFontA(-S(16), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0,
                    0, "Segoe UI");
  hBb = CreateSolidBrush(COL_BG);
  hBp = CreateSolidBrush(COL_INPUT_BG);
  hBr = CreateSolidBrush(COL_GRP_BORDER);
  hBbtn = CreateSolidBrush(COL_BTN_FACE);
  hBa = CreateSolidBrush(COL_ACCENT);
  hBi = CreateSolidBrush(COL_INPUT_BG);

  // Initialize Cached Warning Resources
  g_hFontWarn = CreateFontA(-S(40), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                            0, 0, 0, 0, "Arial");
  HDC hdcScreen = GetDC(NULL);
  g_hdcWarn = CreateCompatibleDC(hdcScreen);
  ReleaseDC(NULL, hdcScreen);
}

void DrawUI(HDC hdc, int w, int h) {
  HDC dc = CreateCompatibleDC(hdc);
  HBITMAP bm = CreateCompatibleBitmap(hdc, w, h);
  SelectObject(dc, bm);
  hits.clear();

  RECT rBg = {0, 0, w, h};
  FillRect(dc, &rBg, hBb);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, hF);
  SetTextColor(dc, COL_TEXT);

  if (showLic || showProcList) {
    // Back Button Logic for Sub-screens
    RECT b = {S(20), h - S(55), S(170), h - S(20)};
    FillRect(dc, &b, hBp);
    RECT i = {b.left + S(4), b.top + S(4), b.right - S(4), b.bottom - S(4)};
    FillRect(dc, &i, hBa);
    DrawTextA(dc, "Back", -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    hits.push_back({99, b});

    if (showProcList) {
      RECT rT = {S(20), S(20), S(600), S(50)};
      SelectObject(dc, hFb);
      DrawTextA(dc, "Process List (one per line, e.g. game.exe)", -1, &rT,
                DT_LEFT | DT_TOP);
      SelectObject(dc, hF);
    }
  } else {
    auto Grp = [&](int x, int y, int gw, int gh, const char *t) {
      RECT r = {S(x), S(y), S(x + gw), S(y + gh)};
      FrameRect(dc, &r, hBr);
      RECT tr = {r.left + S(10), r.top - S(10),
                 r.left + S(10) + S((int)strlen(t) * 12), r.top + S(5)};
      FillRect(dc, &tr, hBb);
      TextOutA(dc, r.left + S(15), r.top - S(8), t, strlen(t));
    };
    auto Tick = [&](int id, int x, int y, const char *t, bool c) {
      RECT r = {S(x), S(y), S(x + 20), S(y + 20)};
      FillRect(dc, &r, hBi);
      if (c) {
        RECT i = {r.left + S(4), r.top + S(4), r.right - S(4), r.bottom - S(4)};
        FillRect(dc, &i, hBa);
      }
      TextOutA(dc, S(x + 30), S(y), t, strlen(t));
      // Dynamic Hit Width calculation to avoid overlap
      int estW = S(30 + (int)(strlen(t) * 10)); // 30px checkbox + ~10px per char
      hits.push_back({id, {S(x), S(y), S(x) + estW, S(y + 20)}});
    };
    auto Btn = [&](int id, int x, int y, const char *t, bool active = false) {
      RECT r = {S(x), S(y), S(x + 120), S(y + 35)};
      FillRect(dc, &r, active ? hBa : hBbtn);
      DrawTextA(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      hits.push_back({id, r});
    };

    int C1 = 20, C2 = 260, C3 = 450;

    Grp(C1, 30, 180, 200, "Corner Position");
    Tick(10, C1 + 20, 50, "Top Left", cfg.pos == 3);
    Tick(11, C1 + 20, 90, "Top Right", cfg.pos == 2);
    Tick(12, C1 + 20, 130, "Bottom Left", cfg.pos == 1);
    Tick(13, C1 + 20, 170, "Bottom Right", cfg.pos == 0);

    // Move Padding/Size labels slightly right (C2 + 20)
    TextOutA(dc, S(C2 + 20), S(30), "Padding:", 8);
    TextOutA(dc, S(C2 + 20), S(70), "Size:", 5);

    // Group Padding, Size, Ghost Mode, Overload (Wider and Taller to fit note)
    Grp(C2, 10, 360, 270, "Appearance"); // H=270, Bottom=280
    
    // Ghost Mode (Always Render)
    Tick(55, C2 + 20, 130, "Always Render (Invisible when idle)", cfg.ghostMode);
    
    // Show encoder overload warnings
    Tick(54, C2 + 20, 170, "Show encoder overload warnings", cfg.showOverloadWarn);
    
    // Info text for overload warning
    {
      // Checkbox is at 170. Note at 200.
      RECT rInfo = {S(C2 + 40), S(200), S(C2 + 350), S(270)}; // Increased H to avoid truncation
      SetTextColor(dc, RGB(150, 150, 150));
      DrawTextA(dc,
                "Note: May show false positives caused by\nhigh CPU load "
                "(e.g. on loading screens)",
                -1, &rInfo, DT_LEFT | DT_TOP);
      SetTextColor(dc, COL_TEXT);
    }

    // Draw Mode Group (Narrowed to fit in Col 1)
    Grp(C1, 260, 220, 120, "Mode");
    Tick(50, C1 + 20, 280, "Information Indicator", cfg.mode == MODE_INFO);
    Tick(51, C1 + 20, 310, "Warning Indicator", cfg.mode == MODE_WARN);
    Tick(52, C1 + 20, 340, "Warning Only", cfg.mode == MODE_WARN_ONLY);

    // Controls Below Appearance Group (Y > 280)
    
    // WebSocket Password (Label at 290)
    TextOutA(dc, S(C2), S(290), "WebSocket Password:", 19);

    // Buttons
    // Test Overlay (Align with Password Edit at Y=312)
    Btn(41, C3, 312, isTest ? "Stop Test" : "Test Overlay", isTest);
    
    // Edit Processes (Below Password)
    Btn(53, C2, 350, "Edit Processes", false);
    
    Tick(40, C3, 355, "Launch on Startup", cfg.autoStart);
    Btn(42, C3, 395, "View License", false);

    // Status Line
    char st[100];
    COLORREF stCol;
    if (isTest) {
      strcpy(st, "Status: TEST MODE");
      stCol = COL_ACCENT;
    } else if (!isConn) {
      strcpy(st, "Status: Disconnected");
      stCol = COL_STATUS_ERR;
    } else if (isRec || isStream) {
      if (isRec && isStream)
        strcpy(st, "Status: Rec & Stream Active");
      else if (isRec)
        strcpy(st, "Status: Recording Active");
      else
        strcpy(st, "Status: Streaming Active");
      stCol = COL_STATUS_REC;
    } else {
      strcpy(st, "Status: Idle (WebSocket)");
      stCol = COL_STATUS_WS;
    }

    SetTextColor(dc, stCol);
    TextOutA(dc, S(C1), S(440), st, strlen(st));
  }

  BitBlt(hdc, 0, 0, w, h, dc, 0, 0, SRCCOPY);
  DeleteObject(bm);
  DeleteDC(dc);
}

// --- RENDERING HELPER ---
void UpdateLayered(HWND hWnd, int x, int y, int w, int h, BYTE alpha,
                   std::function<void(HDC)> drawFn) {
  HDC hdcScreen = GetDC(NULL);
  HDC hdcMem = CreateCompatibleDC(hdcScreen);
  
  // NOTE: CreateCompatibleBitmap with a DC that might be screen.
  // We need to ensure we have a valid bitmap.
  HBITMAP hBm = CreateCompatibleBitmap(hdcScreen, w, h);
  HBITMAP hOldBm = (HBITMAP)SelectObject(hdcMem, hBm);

  drawFn(hdcMem);

  POINT ptDst = {x, y};
  SIZE size = {w, h};
  POINT ptSrc = {0, 0};
  
  // Fix: If alpha is 255, we can drop ULW_ALPHA to rely purely on ColorKey if desired,
  // but using both is valid.
  // Ensure alpha is strictly BYTE.
  BLENDFUNCTION blend = {AC_SRC_OVER, 0, alpha, 0};
  
  // Always use both flags for consistent behavior
  DWORD flags = ULW_COLORKEY | ULW_ALPHA;

  // Use Black as Color Key
  UpdateLayeredWindow(hWnd, hdcScreen, &ptDst, &size, hdcMem, &ptSrc,
                      RGB(0, 0, 0), &blend, flags);

  SelectObject(hdcMem, hOldBm);
  DeleteObject(hBm);
  DeleteDC(hdcMem);
  ReleaseDC(NULL, hdcScreen);
}

// --- UPDATED UpdateOv logic for Padded Window strategy ---
void UpdateOv() {
  int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN),
      p = cfg.pad, s = cfg.size;
  
  // Full Size of the window (Size + Padding) to touch the corner
  int fullS = s + p;
  
  // Window Positions (Anchored to Screen Corners)
  // Correct Map: 0=BR, 1=BL, 2=TR, 3=TL
  int winX = 0, winY = 0;
  if (cfg.pos == 3) { winX = 0; winY = 0; } // TL
  else if (cfg.pos == 2) { winX = sw - fullS; winY = 0; } // TR
  else if (cfg.pos == 1) { winX = 0; winY = sh - fullS; } // BL
  else { winX = sw - fullS; winY = sh - fullS; } // BR
  
  // Indicator Offsets (Relative to Window)
  int indX = 0, indY = 0;
  if (cfg.pos == 3) { indX = p; indY = p; } // TL
  else if (cfg.pos == 2) { indX = 0; indY = p; } // TR
  else if (cfg.pos == 1) { indX = p; indY = 0; } // BL
  else { indX = 0; indY = 0; } // BR
  
  // Ghost Pixel Offsets (Relative to Window - Furthest Corner)
  int pixX = 0, pixY = 0;
  if (cfg.pos == 3) { pixX = 0; pixY = 0; } // TL -> 0,0 (Screen Corner)
  else if (cfg.pos == 2) { pixX = fullS - 1; pixY = 0; } // TR -> WinW-1, 0
  else if (cfg.pos == 1) { pixX = 0; pixY = fullS - 1; } // BL -> 0, WinH-1
  else { pixX = fullS - 1; pixY = fullS - 1; } // BR -> WinW-1, WinH-1

  // Logic for Info Indicator Visibility
  bool active = isRec || isStream;
  bool showInd = false;
  if (isTest)
    showInd = true;
  else if (active && cfg.mode != MODE_WARN_ONLY)
    showInd = true;

  // Determine Alpha
  BYTE indAlpha = 0;
  bool doUpdateInd = false;

  if (cfg.ghostMode) {
    indAlpha = showInd ? 255 : 1;
    // Update if Alpha changed, GhostMode toggled, OR Geometry changed
    // Note: We track winX/winY/fullS in lastOv now effectively?
    // Using s, p, pos to track geom changes.
    // Let's use generic check.
    if (indAlpha != (lastOv.vis ? 255 : 1) || cfg.ghostMode != lastOv.ghost ||
        lastOv.x != winX || lastOv.y != winY || lastOv.s != fullS) {
        doUpdateInd = true;
    }
  } else {
    // Classic Mode
    if (showInd) {
        indAlpha = 255;
        if (!lastOv.vis || lastOv.x != winX || lastOv.y != winY || lastOv.s != fullS || lastOv.ghost)
            doUpdateInd = true;
    } else {
        if (lastOv.vis || lastOv.ghost) {
            indAlpha = 0;
            doUpdateInd = true;
        }
    }
  }
  
  if (doUpdateInd) {
    if (indAlpha > 0) {
         if (!IsWindowVisible(hOv)) ShowWindow(hOv, SW_SHOWNA);
         // Move to PADDED position
         SetWindowPos(hOv, HWND_TOPMOST, winX, winY, fullS, fullS, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
         if (!cfg.ghostMode) 
             SetWindowPos(hOv, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
    }
    
    UpdateLayered(hOv, winX, winY, fullS, fullS, indAlpha, [&](HDC dc) {
      if (indAlpha == 1) {
          // Draw 1x1 Pixel at Calculated Offset
          HBRUSH hInv = CreateSolidBrush(RGB(0, 0, 1));
          RECT rPixel = {pixX, pixY, pixX + 1, pixY + 1};
          FillRect(dc, &rPixel, hInv);
          DeleteObject(hInv);
          return;
      }
      
      // Fill Black (Key)
      HBRUSH hBlack = CreateSolidBrush(RGB(0, 0, 0));
      RECT r = {0, 0, fullS, fullS};
      FillRect(dc, &r, hBlack);
      DeleteObject(hBlack);

      // Draw Red Circle / White Border at Indicator Offset
      HPEN hPen = CreatePen(PS_SOLID, S(2), RGB(255, 255, 255));
      HBRUSH hBrush = CreateSolidBrush(RGB(255, 0, 0));
      HPEN hOldPen = (HPEN)SelectObject(dc, hPen);
      HBRUSH hOldBrush = (HBRUSH)SelectObject(dc, hBrush);
      // Circle Rect: indX, indY, indX+s, indY+s
      // Adjust for Pen (S(1) offset inside the circle rect?)
      // Original: Ellipse(dc, S(1), S(1), s - S(1), s - S(1)); 
      // We need to shift this by indX, indY
      Ellipse(dc, indX + S(1), indY + S(1), indX + s - S(1), indY + s - S(1));
      
      SelectObject(dc, hOldPen);
      SelectObject(dc, hOldBrush);
      DeleteObject(hBrush);
      DeleteObject(hPen);
    });
    
    lastOv = {winX, winY, fullS, showInd, cfg.ghostMode};
  }

  // Warning Overlay Update
  bool showOverload = cfg.showOverloadWarn && (GetTickCount() < overloadWarnUntil);
  bool showW = warnVisible || showOverload;
  BYTE warnAlpha = 0;
  bool doUpdateWarn = false;
  
  const char *msg = "OBS INACTIVE";
  if (cfg.showOverloadWarn && GetTickCount() < overloadWarnUntil)
      msg = "Encoder overloaded!";

  // We need to calculate position and size every time because text might change or pos might change
  // But we can cache the last text/state to avoid ULW calls.
  static std::string lastMsg = "";

  if (cfg.ghostMode) {
      // In ghost mode: warning window uses alpha=0 when hidden (100% invisible)
      // Only the indicator overlay's 1x1 pixel uses alpha=1 for MPO prevention
      warnAlpha = showW ? 255 : 0;
      if ((warnAlpha > 0) != lastWarnVis || cfg.ghostMode != lastOv.ghost || msg != lastMsg)
          doUpdateWarn = true;
  } else {
      if (showW) {
          warnAlpha = 255;
          if (!lastWarnVis || msg != lastMsg || lastOv.ghost)
              doUpdateWarn = true;
      } else {
          if (lastWarnVis || lastOv.ghost) {
            warnAlpha = 0;
            doUpdateWarn = true;
          }
      }
  }

  if (doUpdateWarn) {
    if (warnAlpha > 0) {
        if (!IsWindowVisible(hWarn)) ShowWindow(hWarn, SW_SHOWNA);
        SetWindowPos(hWarn, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        // Hide warning window when not visible (even in ghost mode)
        SetWindowPos(hWarn, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
    }

    // Check Cache Validity
    bool cacheStale = (msg != g_lastWarnMsg) || (g_hBmWarn == NULL);
    
    // Position/Size must be recalculated if message changes OR scale changes (which we don't handle dynamic DPI yet fully, but assuming S stays same)
    // Actually, position depends on screen size too. We calculate Pos every frame anyway.

    if (cacheStale) {
        // 1. Calculate Size
        HDC hdcScreen = GetDC(NULL);
        // Use temp DC for measurment
        HDC dc = CreateCompatibleDC(hdcScreen);
        SelectObject(dc, g_hFontWarn); // Use cached font
        RECT rText = {0, 0, 0, 0};
        DrawTextA(dc, msg, -1, &rText, DT_CALCRECT);
        int wW = rText.right - rText.left + S(20);
        int wH = rText.bottom - rText.top + S(10);
        DeleteDC(dc);
        
        // 2. Re-allocate Bitmap if needed (or if size grew? Just strict realloc for simplicity)
        if (g_hBmWarn) DeleteObject(g_hBmWarn);
        g_hBmWarn = CreateCompatibleBitmap(hdcScreen, wW, wH);
        
        // 3. Draw into Cached Bitmap
        SelectObject(g_hdcWarn, g_hBmWarn);
        SelectObject(g_hdcWarn, g_hFontWarn);
        SetTextColor(g_hdcWarn, COL_WARN_TEXT);
        SetBkMode(g_hdcWarn, TRANSPARENT);
        
        // Fill Black (Key)
        RECT rFill = {0, 0, wW, wH};
        HBRUSH hK = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(g_hdcWarn, &rFill, hK);
        DeleteObject(hK);
        
        RECT rT = {S(10), S(5), wW, wH};
        DrawTextA(g_hdcWarn, msg, -1, &rT, DT_LEFT | DT_TOP | DT_NOCLIP);
        
        ReleaseDC(NULL, hdcScreen);
        
        g_sizeWarn = {wW, wH};
        g_lastWarnMsg = msg;
    }
    
    // Calculate Position based on Corner (always dynamic)
    int wx = 0, wy = 0;
    int off = s + p + S(10);
    int wW = g_sizeWarn.cx;
    int wH = g_sizeWarn.cy;

    if (cfg.pos == 3) { // Top Left
        wx = off; wy = p;
    } else if (cfg.pos == 2) { // Top Right
        wx = sw - off - wW; wy = p;
    } else if (cfg.pos == 1) { // Bottom Left
        wx = off; wy = sh - p - wH; // approx
    } else { // Bottom Right
        wx = sw - off - wW; wy = sh - p - wH - S(40);
    }
    
    // UpdateLayeredWindow using CACHED DC/Bitmap
    POINT ptDst = {wx, wy};
    SIZE size = {wW, wH};
    POINT ptSrc = {0, 0};
    
    // Always use both flags for consistent behavior
    DWORD flags = ULW_COLORKEY | ULW_ALPHA;

    BLENDFUNCTION blend = {AC_SRC_OVER, 0, warnAlpha, 0};
    UpdateLayeredWindow(hWarn, NULL, &ptDst, &size, g_hdcWarn, &ptSrc, RGB(0,0,0), &blend, flags);

    lastWarnVis = warnAlpha > 0;
  }
}


LRESULT CALLBACK P(HWND h, UINT m, WPARAM w, LPARAM l) {
  // WM_PAINT handled by UpdateLayeredWindow
  return DefWindowProc(h, m, w, l);
}

// Warning Window Proc
LRESULT CALLBACK W(HWND h, UINT m, WPARAM w, LPARAM l) {
  // WM_PAINT handled by UpdateLayeredWindow
  return DefWindowProc(h, m, w, l);
}

LRESULT CALLBACK M(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CREATE) {
    auto Edit = [&](int id, const char *t, bool p = 0) {
      return CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", t,
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL |
                                 (p ? ES_PASSWORD : ES_NUMBER | ES_CENTER),
                             0, 0, 0, 0, h, (HMENU)(INT_PTR)id, 0, 0);
    };
    hEditPad = Edit(100, std::to_string(cfg.pad).c_str());
    hEditSize = Edit(101, std::to_string(cfg.size).c_str());
    std::string p = ManagePass();
    hEditPass = Edit(102, p.c_str(), 1);
    Wipe(p);

    hEditLic = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", LIC_TEXT,
                               WS_CHILD | WS_BORDER | WS_VSCROLL |
                                   ES_MULTILINE | ES_READONLY,
                               0, 0, 0, 0, h, (HMENU)103, 0, 0);
    SetWindowTheme(hEditLic, L"DarkMode_Explorer", NULL);
    SendMessage(hEditLic, WM_SETFONT, (WPARAM)hF, 1);

    // Process List Edit
    std::string pl = ReplaceAll(cfg.processList, "|", "\r\n");
    hEditProcs =
        CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", pl.c_str(),
                        WS_CHILD | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
                            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                        0, 0, 0, 0, h, (HMENU)104, 0, 0);
    SetWindowTheme(hEditProcs, L"DarkMode_Explorer", NULL);
    SendMessage(hEditProcs, WM_SETFONT, (WPARAM)hF, 1);

    SendMessage(hEditPad, WM_SETFONT, (WPARAM)hF, 1);
    SendMessage(hEditSize, WM_SETFONT, (WPARAM)hF, 1);
    SendMessage(hEditPass, WM_SETFONT, (WPARAM)hF, 1);

    // Start Timer for Warning Logic
    SetTimer(h, TIMER_WARN, 100, NULL);
  } else if (m == WM_CTLCOLOREDIT || m == WM_CTLCOLORSTATIC) {
    HDC hdc = (HDC)w;
    SetBkColor(hdc, COL_INPUT_BG);
    SetTextColor(hdc, COL_TEXT);
    return (LRESULT)hBp;
  } else if (m == WM_SIZE) {
    if (w == SIZE_MINIMIZED)
      ShowWindow(h, SW_HIDE);
    else if (showLic) {
      ShowWindow(hEditPad, 0);
      ShowWindow(hEditSize, 0);
      ShowWindow(hEditPass, 0);
      ShowWindow(hEditProcs, 0);
      MoveWindow(hEditLic, S(20), S(20), S(600), S(320), 1);
      ShowWindow(hEditLic, SW_SHOW);
    } else if (showProcList) {
      ShowWindow(hEditPad, 0);
      ShowWindow(hEditSize, 0);
      ShowWindow(hEditPass, 0);
      ShowWindow(hEditLic, 0);
      MoveWindow(hEditProcs, S(20), S(50), S(600), S(320), 1);
      ShowWindow(hEditProcs, SW_SHOW);
    } else {
      ShowWindow(hEditLic, SW_HIDE);
      ShowWindow(hEditProcs, SW_HIDE);
      int C2 = S(260); // Shifted from 240
      // Move Edit Boxes to new positions
      MoveWindow(hEditPad, C2 + S(100), S(28), S(60), S(24), 1);
      MoveWindow(hEditSize, C2 + S(100), S(68), S(60), S(24), 1);
      // Password Edit: Y=312 (Below 290 Label)
      MoveWindow(hEditPass, C2, S(312), S(160), S(24), 1);
      ShowWindow(hEditPad, 1);
      ShowWindow(hEditSize, 1);
      ShowWindow(hEditPass, 1);
    }
  } else if (m == WM_COMMAND && HIWORD(w) == EN_CHANGE) {
    char b[4096];
    GetWindowTextA((HWND)l, b, 4096);
    if ((HWND)l == hEditPad)
      cfg.pad = atoi(b);
    if ((HWND)l == hEditSize)
      cfg.size = atoi(b);
    if ((HWND)l == hEditPass)
      ManagePass(b);
    if ((HWND)l == hEditProcs) {
      std::string s = b;
      s = ReplaceAll(s, "\r\n", "|");
      cfg.processList = s;
    }
    IOCfg(true);
    UpdateOv();
    if (isTest)
      InvalidateRect(hOv, NULL, FALSE);
  } else if (m == WM_LBUTTONDOWN) {
    POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    for (auto &hit : hits)
      if (PtInRect(&hit.r, p)) {
        if (hit.id == 10)
          cfg.pos = 3;
        if (hit.id == 11)
          cfg.pos = 2;
        if (hit.id == 12)
          cfg.pos = 1;
        if (hit.id == 13)
          cfg.pos = 0;
        // Removed Shape(20,21) and Color(30-32) handlers
        if (hit.id == 40) {
          cfg.autoStart = !cfg.autoStart;
          HKEY k;
          RegOpenKeyExA(HKEY_CURRENT_USER,
                        "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                        KEY_SET_VALUE, &k);
          if (cfg.autoStart) {
            char p[MAX_PATH], q[MAX_PATH];
            GetModuleFileNameA(0, p, MAX_PATH);
            sprintf(q, "\"%s\" --tray", p);
            RegSetValueExA(k, "OBSInd", 0, REG_SZ, (BYTE *)q, strlen(q) + 1);
          } else
            RegDeleteValueA(k, "OBSInd");
          RegCloseKey(k);
        }
        if (hit.id == 41) {
          isTest = !isTest;
          UpdateOv();
          InvalidateRect(h, 0, 0);
        }
        if (hit.id == 42)
          showLic = 1;
        if (hit.id == 99) {
          showLic = 0;
          showProcList = 0;
        }

        // Mode Select
        if (hit.id == 50)
          cfg.mode = MODE_INFO;
        if (hit.id == 51)
          cfg.mode = MODE_WARN;
        if (hit.id == 52)
          cfg.mode = MODE_WARN_ONLY;
        if (hit.id == 53)
          showProcList = 1;
        if (hit.id == 54)
          cfg.showOverloadWarn = !cfg.showOverloadWarn;
        if (hit.id == 55)
          cfg.ghostMode = !cfg.ghostMode;

        IOCfg(true);
        UpdateOv();
        InvalidateRect(h, 0, 0);
        SendMessage(h, WM_SIZE, 0, 0);
        if (isTest)
          InvalidateRect(hOv, NULL, FALSE);
      }
  } else if (m == WM_TIMER && w == TIMER_WARN) {
    if (cfg.mode == MODE_WARN || cfg.mode == MODE_WARN_ONLY) {
      DWORD now = GetTickCount();

      // Check Focus Every Timer Tick (cached, low overhead)
      warnTargetFocused = IsForegroundTarget();

      // Warning Condition: Process Focused AND Not Recording AND Not Streaming
      // (and not in Test mode)
      bool active = isRec || isStream;
      bool condition = warnTargetFocused && !active && !isTest;

      if (condition) {
        // If just started warning state
        if (!warnActive) {
          warnActive = true;
          warnCycleStart = now;
          warnVisible = true;
          UpdateOv();
        } else {
          // Cycle: 2s ON, 1s OFF = 3s total
          DWORD elapsed = now - warnCycleStart;
          DWORD cycleTime = elapsed % 3000;
          bool shouldBeVisible = (cycleTime < 2000);
          if (warnVisible != shouldBeVisible) {
            warnVisible = shouldBeVisible;
            UpdateOv();
          }
        }
      } else {
        if (warnActive || warnVisible) {
          warnActive = false;
          warnVisible = false;
          UpdateOv();
        }
      }
    } else {
      // Mode changed, reset
      if (warnActive || warnVisible) {
        warnActive = false;
        warnVisible = false;
        UpdateOv();
      }
    }

    // ALSO Check Overload Expiry (to hide window when time is up)
    if (cfg.showOverloadWarn && overloadWarnUntil > 0) {
      // Just triggering an update occasionally or checking if we need to hide
      if (GetTickCount() > overloadWarnUntil)
        UpdateOv();
    }
  } else if (m == WM_OBS) {
    UpdateOv();
    InvalidateRect(h, 0, 0);
  } else if (m == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    DrawUI(ps.hdc, rc.right, rc.bottom);
    EndPaint(h, &ps);
  } else if (m == WM_TRAY && l == WM_LBUTTONUP) {
    ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
  } else if (m == WM_GETMINMAXINFO) {
    MINMAXINFO *mmi = (MINMAXINFO *)l;
    // Lock Size to current or initial
    // We want fixed client size 640x480 (scaled)
    // We'll calculate the window rect for that and enforcing it.
    // Or simpler: lock to the size set in WinMain.
    // However, M doesn't know WinMain vars readily. 
    // We can just recalculate quickly or use static.
    static POINT sz = {0, 0};
    if (sz.x == 0) {
       DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
       RECT wr = {0, 0, S(640), S(480)};
       AdjustWindowRect(&wr, style, FALSE);
       sz.x = wr.right - wr.left;
       sz.y = wr.bottom - wr.top;
    }
    mmi->ptMinTrackSize = sz;
    mmi->ptMaxTrackSize = sz;
    return 0;
  } else if (m == WM_CLOSE)
    PostQuitMessage(0);
  return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hI, HINSTANCE, LPSTR p, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (CreateMutex(0, 1, "OBSIndM") && GetLastError() == ERROR_ALREADY_EXISTS)
    return 0;
  IOCfg(false);
  g_Scale = GetDpiForSystem() / 96.0f;
  InitGDI();

  WNDCLASS wc = {0};
  wc.lpfnWndProc = P;
  wc.hInstance = hI;
  wc.lpszClassName = "O";
  RegisterClass(&wc);
  hOv = CreateWindowEx(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                           WS_EX_TOOLWINDOW,
                       "O", "", WS_POPUP, 0, 0, 0, 0, 0, 0, hI, 0);
  // Do NOT set LWA logic here if we use UpdateLayeredWindow immediately or consistently.
  // But ULW overrides it anyway.


  // Warning Window Class
  WNDCLASS wcw = {0};
  wcw.lpfnWndProc = W;
  wcw.hInstance = hI;
  wcw.lpszClassName = "WARN";
  RegisterClass(&wcw);
  hWarn = CreateWindowEx(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                             WS_EX_TOOLWINDOW,
                         "WARN", "", WS_POPUP, 0, 0, 0, 0, 0, 0, hI, 0);

  wc.lpfnWndProc = M;
  wc.lpszClassName = "C";
  wc.hbrBackground = hBb;
  wc.hIcon = LoadIcon(hI, MAKEINTRESOURCE(IDI_MAIN_ICON));
  wc.hCursor = LoadCursor(0, IDC_ARROW);
  RegisterClass(&wc);

  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
  // Increased Window Height to 480
  RECT wr = {0, 0, S(640), S(480)};
  AdjustWindowRect(&wr, style, FALSE);

  int scrW = GetSystemMetrics(SM_CXSCREEN);
  int scrH = GetSystemMetrics(SM_CYSCREEN);
  int winW = wr.right - wr.left;
  int winH = wr.bottom - wr.top;
  int x = (scrW - winW) / 2;
  int y = (scrH - winH) / 2;

  hMain = CreateWindow("C", "OBS Indicator Settings", style, x, y, winW, winH,
                       0, 0, hI, 0);

  nid.hWnd = hMain;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_TRAY;
  nid.hIcon = wc.hIcon;
  strcpy(nid.szTip, "OBS Indicator");
  Shell_NotifyIcon(NIM_ADD, &nid);

  BOOL d = 1;
  DwmSetWindowAttribute(hMain, 20, &d, 4);
  std::thread(NetThread).detach();
  if (strstr(p, "--tray"))
    ShowWindow(hMain, SW_HIDE);
  else
    ShowWindow(hMain, SW_SHOW);

  MSG m;
  while (GetMessage(&m, 0, 0, 0)) {
    TranslateMessage(&m);
    DispatchMessage(&m);
  }
  Shell_NotifyIcon(NIM_DELETE, &nid);
  return 0;
}
