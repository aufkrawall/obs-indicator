#define _CRT_SECURE_NO_WARNINGS
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

#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wincrypt.h>
#include <uxtheme.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <ctime>
#include <algorithm>
#include <sstream>

// --- CONSTANTS ---
#define WM_TRAY (WM_USER + 1)
#define WM_OBS  (WM_USER + 2)
#define IDI_MAIN_ICON 101
#define TIMER_WARN 999

// Palette
#define COL_BG          RGB(20, 20, 20)
#define COL_GRP_BORDER  RGB(80, 80, 80)
#define COL_TEXT        RGB(200, 200, 200)
#define COL_INPUT_BG    RGB(30, 30, 30)
#define COL_ACCENT      RGB(60, 100, 160)
#define COL_BTN_FACE    RGB(50, 50, 50)
#define COL_STATUS_ERR  RGB(255, 50, 50)
#define COL_STATUS_REC  RGB(0, 255, 0)
#define COL_STATUS_WS   RGB(100, 150, 255)
#define COL_WARN_TEXT   RGB(255, 20, 20)

// Modes
enum AppMode { MODE_INFO = 0, MODE_WARN = 1, MODE_WARN_ONLY = 2 };

struct AppState {
    int size = 30;
    int pad = 20;
    int pos = 0; // 0=BR, 1=BL, 2=TR, 3=TL
    bool circle = false;
    bool autoStart = false;
    COLORREF col = RGB(255, 0, 0);
    int mode = MODE_INFO;
    std::string processList = ""; // Pipe delimited in memory/ini, newline in edit
} cfg;

struct { int x=-1, y=-1, s=-1; bool vis=false; } lastOv;
std::atomic<bool> isRec(false), isStream(false), isConn(false), isTest(false);
bool showLic = false;
bool showProcList = false;
float g_Scale = 1.0f;
HWND hMain = NULL, hOv = NULL, hWarn = NULL;
HWND hEditPad = NULL, hEditSize = NULL, hEditPass = NULL, hEditLic = NULL, hEditProcs = NULL;
NOTIFYICONDATA nid = { sizeof(NOTIFYICONDATA) };

// Warning Logic State
bool warnActive = false;
bool warnTargetFocused = false;
DWORD warnCycleStart = 0;
bool warnVisible = false;

// Full license text
const char* LIC_TEXT = "MIT License\r\n\r\nCopyright (c) 2025 aufkrawall\r\n\r\nPermission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the \"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:\r\n\r\nThe above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.\r\n\r\nTHE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.";

// --- HELPERS ---
int S(int v) { return (int)(v * g_Scale); }
void Wipe(std::string& s) { SecureZeroMemory(&s[0], s.size()); s.clear(); }

// --- STRING HELPER ---
std::string ReplaceAll(std::string str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return str;
}

// --- FOCUS CHECK ---
bool IsForegroundTarget() {
    if (cfg.processList.empty()) return false;

    HWND hFg = GetForegroundWindow();
    if (!hFg) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hFg, &pid);
    if (pid == 0) return false;

    // Cache results to ensure zero overhead while the same window is focused
    static DWORD lastPid = 0;
    static bool lastRes = false;
    static DWORD lastCheckTime = 0;

    // Re-validate string every 2 seconds in case config changed, otherwise trust cache
    if (pid == lastPid && (GetTickCount() - lastCheckTime < 2000)) return lastRes;

    bool match = false;
    // PROCESS_QUERY_LIMITED_INFORMATION is sufficient and requires fewer rights
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        char exePath[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameA(hProcess, 0, exePath, &size)) {
            std::string exeName = exePath;
            size_t lastSlash = exeName.find_last_of("\\/");
            if (lastSlash != std::string::npos) exeName = exeName.substr(lastSlash + 1);
            std::transform(exeName.begin(), exeName.end(), exeName.begin(), ::tolower);

            std::stringstream ss(cfg.processList);
            std::string item;
            while(std::getline(ss, item, '|')) {
                if(!item.empty()) {
                    std::string t = item;
                    std::transform(t.begin(), t.end(), t.begin(), ::tolower);
                    if (exeName == t) { match = true; break; }
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
std::string Base64(const std::vector<BYTE>& data) {
    if (data.empty()) return "";
    DWORD len = 0;
    if (!CryptBinaryToStringA(data.data(), (DWORD)data.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &len)) return "";
    std::string res(len, 0);
    if (!CryptBinaryToStringA(data.data(), (DWORD)data.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &res[0], &len)) return "";
    while (!res.empty() && res.back() == '\0') res.pop_back();
    return res;
}

std::vector<BYTE> Sha256(const std::string& data) {
    HCRYPTPROV hP; HCRYPTHASH hH;
    std::vector<BYTE> hash(32);
    if (CryptAcquireContext(&hP, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hP, CALG_SHA_256, 0, 0, &hH)) {
            CryptHashData(hH, (BYTE*)data.data(), (DWORD)data.size(), 0);
            DWORD len = 32;
            CryptGetHashParam(hH, HP_HASHVAL, hash.data(), &len, 0);
            CryptDestroyHash(hH);
        }
        CryptReleaseContext(hP, 0);
    }
    return hash;
}

std::string Auth(std::string& pass, const std::string& salt, const std::string& chal) {
    std::string secret = Base64(Sha256(pass + salt));
    std::string res = Base64(Sha256(secret + chal));
    Wipe(pass); Wipe(secret);
    return res;
}

// --- CONFIG ---
std::string ManagePass(const char* newPass = nullptr) {
    char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
    strcpy(strrchr(path, '.'), ".ini");

    if (newPass) {
        std::string enc = Base64(std::vector<BYTE>(newPass, newPass + strlen(newPass)));
        WritePrivateProfileStringA("S", "Pw", enc.c_str(), path);
        return "";
    } else {
        char buf[512] = {0};
        GetPrivateProfileStringA("S", "Pw", "", buf, 512, path);
        if (strlen(buf) == 0) return "";
        DWORD len = 0;
        CryptStringToBinaryA(buf, 0, CRYPT_STRING_BASE64, NULL, &len, 0, 0);
        std::string dec(len, 0);
        CryptStringToBinaryA(buf, 0, CRYPT_STRING_BASE64, (BYTE*)&dec[0], &len, 0, 0);
        return dec;
    }
}

void IOCfg(bool save) {
    char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
    strcpy(strrchr(path, '.'), ".ini");
    if(save) {
        WritePrivateProfileStringA("S", "Sz", std::to_string(cfg.size).c_str(), path);
        WritePrivateProfileStringA("S", "Pd", std::to_string(cfg.pad).c_str(), path);
        WritePrivateProfileStringA("S", "Ps", std::to_string(cfg.pos).c_str(), path);
        WritePrivateProfileStringA("S", "Cr", cfg.circle ? "1" : "0", path);
        WritePrivateProfileStringA("S", "Au", cfg.autoStart ? "1" : "0", path);
        WritePrivateProfileStringA("S", "Cl", std::to_string(cfg.col).c_str(), path);
        WritePrivateProfileStringA("S", "Md", std::to_string(cfg.mode).c_str(), path);
        WritePrivateProfileStringA("S", "Pr", cfg.processList.c_str(), path);
    } else {
        cfg.size = GetPrivateProfileIntA("S", "Sz", 30, path);
        cfg.pad = GetPrivateProfileIntA("S", "Pd", 20, path);
        cfg.pos = GetPrivateProfileIntA("S", "Ps", 0, path);
        cfg.circle = GetPrivateProfileIntA("S", "Cr", 0, path) == 1;
        cfg.autoStart = GetPrivateProfileIntA("S", "Au", 0, path) == 1;
        cfg.col = GetPrivateProfileIntA("S", "Cl", RGB(255, 0, 0), path);
        cfg.mode = GetPrivateProfileIntA("S", "Md", 0, path);
        char buf[4096] = {0};
        GetPrivateProfileStringA("S", "Pr", "", buf, 4096, path);
        cfg.processList = buf;
    }
}

// --- NETWORKING ---
std::string JsonVal(const std::string& json, const std::string& key) {
    std::string q = "\"" + key + "\"";
    size_t p = json.find(q); if (p == -1) return "";
    p = json.find(":", p) + 1;
    while (p < json.size() && (isspace(json[p]) || json[p] == '"')) p++;
    size_t e = p;
    while (e < json.size() && json[e] != '"' && json[e] != ',' && json[e] != '}') e++;
    return json.substr(p, e - p);
}

void SendWS(SOCKET s, const std::string& txt) {
    std::vector<BYTE> f = { 0x81 };
    if(txt.size() > 125) {
        f.push_back(0x80 | 126);
        unsigned short l = htons((unsigned short)txt.size());
        f.insert(f.end(), (BYTE*)&l, (BYTE*)&l + 2);
    } else f.push_back(0x80 | (BYTE)txt.size());

    DWORD m = GetTickCount(); BYTE mask[4]; memcpy(mask, &m, 4);
    f.insert(f.end(), mask, mask+4);
    for(size_t i=0; i<txt.size(); i++) f.push_back(txt[i] ^ mask[i%4]);
    send(s, (char*)f.data(), (int)f.size(), 0);
}

void NetThread() {
    while (true) {
        Sleep(1000);
        WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a = { AF_INET, htons(4455) };
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);

        if(isConn) { isConn = false; isRec = false; isStream = false; PostMessage(hMain, WM_OBS, 0, 0); }

        if (connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
            std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
            std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
            send(s, req.c_str(), (int)req.size(), 0);

            char buf[4096];
            if (recv(s, buf, 4096, 0) > 0 && strstr(buf, "101")) {
                int n = recv(s, buf, 4096, 0);
                std::string hello(buf, n > 0 ? n : 0);
                std::string salt = JsonVal(hello, "salt"), chal = JsonVal(hello, "challenge");

                std::string id = "{\"op\":1,\"d\":{\"rpcVersion\":1,\"eventSubscriptions\":65";
                if (!salt.empty() && !chal.empty()) {
                    std::string p = ManagePass();
                    id += ",\"authentication\":\"" + Auth(p, salt, chal) + "\"";
                }
                id += "}}";
                SendWS(s, id);

                isConn = true; PostMessage(hMain, WM_OBS, 0, 0);

                DWORD lastPollRec = 0;
                DWORD lastPollStr = 0;

                while (true) {
                    DWORD now = GetTickCount();
                    // Interleave polling to avoid packet merging/collision issues in simple parser
                    if (now - lastPollRec > 3000) {
                        SendWS(s, "{\"op\":6,\"d\":{\"requestType\":\"GetRecordStatus\",\"requestId\":\"pollR\"}}");
                        lastPollRec = now;
    }
    if (now - lastPollStr > 3000 && now - lastPollRec > 1500) {
        SendWS(s, "{\"op\":6,\"d\":{\"requestType\":\"GetStreamStatus\",\"requestId\":\"pollS\"}}");
        lastPollStr = now;
        }

        timeval tv = { 0, 100000 };
        fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
        if (select(0, &fds, NULL, NULL, &tv) > 0) {
            int r = recv(s, buf, 4096, 0);
            if (r <= 0) break;
            std::string chunk(buf, r);

            // Check Recording Status
            if (chunk.find("RecordStateChanged") != -1 || chunk.find("GetRecordStatus") != -1) {
                bool act = chunk.find("\"outputActive\":true") != -1;
                if (act != isRec) { isRec = act; PostMessage(hMain, WM_OBS, 0, 0); }
            }

            // Check Streaming Status
            if (chunk.find("StreamStateChanged") != -1 || chunk.find("GetStreamStatus") != -1) {
                bool act = chunk.find("\"outputActive\":true") != -1;
                if (act != isStream) { isStream = act; PostMessage(hMain, WM_OBS, 0, 0); }
            }
        }
        }
        }
        }
        closesocket(s); WSACleanup();
        if(isConn) { isConn = false; isRec = false; isStream = false; PostMessage(hMain, WM_OBS, 0, 0); }
        Sleep(1000);
        }
        }

        // --- UI ---
        HFONT hF, hFb; HBRUSH hBb, hBp, hBr, hBbtn, hBa, hBi;
        struct Hit { int id; RECT r; };
        std::vector<Hit> hits;

        void InitGDI() {
            hF = CreateFontA(-S(16), 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET, 0,0,0,0, "Segoe UI");
            hFb = CreateFontA(-S(16), 0,0,0, FW_BOLD, 0,0,0, DEFAULT_CHARSET, 0,0,0,0, "Segoe UI");
            hBb = CreateSolidBrush(COL_BG);
            hBp = CreateSolidBrush(COL_INPUT_BG);
            hBr = CreateSolidBrush(COL_GRP_BORDER);
            hBbtn = CreateSolidBrush(COL_BTN_FACE);
            hBa = CreateSolidBrush(COL_ACCENT);
            hBi = CreateSolidBrush(COL_INPUT_BG);
        }

        void DrawUI(HDC hdc, int w, int h) {
            HDC dc = CreateCompatibleDC(hdc); HBITMAP bm = CreateCompatibleBitmap(hdc, w, h);
            SelectObject(dc, bm); hits.clear();

            FillRect(dc, &RECT{0,0,w,h}, hBb);
            SetBkMode(dc, TRANSPARENT); SelectObject(dc, hF); SetTextColor(dc, COL_TEXT);

            if (showLic || showProcList) {
                // Back Button Logic for Sub-screens
                RECT b = {S(20), h-S(55), S(170), h-S(20)};
                FillRect(dc, &b, hBp);
                RECT i = {b.left+S(4), b.top+S(4), b.right-S(4), b.bottom-S(4)}; FillRect(dc, &i, hBa);
                DrawTextA(dc, "Back", -1, &b, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                hits.push_back({99, b});

                if (showProcList) {
                    RECT rT = {S(20), S(20), S(600), S(50)};
                    SelectObject(dc, hFb);
                    DrawTextA(dc, "Process List (one per line, e.g. game.exe)", -1, &rT, DT_LEFT|DT_TOP);
                    SelectObject(dc, hF);
                }
            } else {
                auto Grp = [&](int x, int y, int gw, int gh, const char* t) {
                    RECT r = {S(x), S(y), S(x+gw), S(y+gh)};
                    FrameRect(dc, &r, hBr);
                    RECT tr = {r.left+S(10), r.top-S(10), r.left+S(10)+S((int)strlen(t)*9), r.top+S(5)};
                    FillRect(dc, &tr, hBb);
                    TextOutA(dc, r.left+S(15), r.top-S(8), t, strlen(t));
                };
                auto Tick = [&](int id, int x, int y, const char* t, bool c) {
                    RECT r = {S(x), S(y), S(x+20), S(y+20)};
                    FillRect(dc, &r, hBi);
                    if(c) {
                        RECT i = {r.left+S(4), r.top+S(4), r.right-S(4), r.bottom-S(4)};
                        FillRect(dc, &i, hBa);
                    }
                    TextOutA(dc, S(x+30), S(y), t, strlen(t));
                    hits.push_back({id, {S(x), S(y), S(x+150), S(y+20)}});
                };
                auto Btn = [&](int id, int x, int y, const char* t, bool active=false) {
                    RECT r = {S(x), S(y), S(x+120), S(y+35)};
                    FillRect(dc, &r, active ? hBa : hBbtn);
                    DrawTextA(dc, t, -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                    hits.push_back({id, r});
                };

                int C1 = 20, C2 = 240, C3 = 450;

                Grp(C1, 30, 180, 200, "Corner Position");
                Tick(10, C1+20, 50, "Top Left", cfg.pos==3);
                Tick(11, C1+20, 90, "Top Right", cfg.pos==2);
                Tick(12, C1+20, 130, "Bottom Left", cfg.pos==1);
                Tick(13, C1+20, 170, "Bottom Right", cfg.pos==0);

                TextOutA(dc, S(C2), S(30), "Padding:", 8);
                TextOutA(dc, S(C2), S(70), "Size:", 5);

                Grp(C2, 110, 160, 100, "Shape");
                Tick(20, C2+20, 130, "Square", !cfg.circle);
                Tick(21, C2+20, 170, "Circle", cfg.circle);

                Grp(C3, 30, 140, 140, "Color");
                Tick(30, C3+20, 50, "Red", cfg.col==RGB(255,0,0));
                Tick(31, C3+20, 90, "Green", cfg.col==RGB(0,255,0));
                Tick(32, C3+20, 130, "Blue", cfg.col==RGB(0,0,255));

                Grp(C1, 260, 400, 120, "Mode");
                Tick(50, C1+20, 280, "Information Indicator", cfg.mode == MODE_INFO);
                Tick(51, C1+20, 310, "Warning Indicator", cfg.mode == MODE_WARN);
                Tick(52, C1+20, 340, "Warning Only", cfg.mode == MODE_WARN_ONLY);

                Btn(53, C2+20, 300, "Edit Processes", false);

                Btn(41, C3, 220, isTest ? "Stop Test" : "Test Overlay", isTest);
                Tick(40, C3, 280, "Launch on Startup", cfg.autoStart);
                Btn(42, C3, 320, "View License", false);

                TextOutA(dc, S(C2), S(220)+S(8), "WebSocket Password:", 19);

                // Status Line
                char st[100]; COLORREF stCol;
                if(isTest) { strcpy(st, "Status: TEST MODE"); stCol = COL_ACCENT; }
                else if(!isConn) { strcpy(st, "Status: Disconnected"); stCol = COL_STATUS_ERR; }
                else if(isRec || isStream) {
                    if (isRec && isStream) strcpy(st, "Status: Rec & Stream Active");
                    else if (isRec) strcpy(st, "Status: Recording Active");
                    else strcpy(st, "Status: Streaming Active");
                    stCol = COL_STATUS_REC;
                }
                else { strcpy(st, "Status: Idle (WebSocket)"); stCol = COL_STATUS_WS; }

                SetTextColor(dc, stCol);
                TextOutA(dc, S(C1), S(390), st, strlen(st));
            }

            BitBlt(hdc, 0, 0, w, h, dc, 0, 0, SRCCOPY);
            DeleteObject(bm); DeleteDC(dc);
        }

        void UpdateOv() {
            // Normal Indicator
            int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN), p = cfg.pad, s = cfg.size;
            int x[] = {sw-s-p, p, sw-s-p, p}, y[] = {sh-s-p, sh-s-p, p, p};
            int cx = x[cfg.pos], cy = y[cfg.pos];

            // Logic for Info Indicator Visibility
            bool active = isRec || isStream;

            bool showInd = false;
            if (isTest) showInd = true;
            else if (active) {
                if (cfg.mode != MODE_WARN_ONLY) showInd = true;
            }

            if (showInd != lastOv.vis || (showInd && (cx!=lastOv.x || cy!=lastOv.y || s!=lastOv.s))) {
                SetWindowPos(hOv, HWND_TOPMOST, cx, cy, s, s, SWP_NOACTIVATE | (showInd?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
                if(showInd) InvalidateRect(hOv, NULL, TRUE);
                lastOv = {cx, cy, s, showInd};
            }

            // Warning Overlay Update
            if (warnVisible) {
                SetWindowPos(hWarn, HWND_TOPMOST, 0, 0, sw, sh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                // Force repaint to handle dynamic text positioning updates
                InvalidateRect(hWarn, NULL, TRUE);
            } else {
                SetWindowPos(hWarn, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_HIDEWINDOW);
            }
        }

        LRESULT CALLBACK P(HWND h, UINT m, WPARAM w, LPARAM l) {
            if(m == WM_PAINT) {
                PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
                RECT rc; GetClientRect(h, &rc);
                HBRUSH hBlack = CreateSolidBrush(RGB(0,0,0)); FillRect(dc, &rc, hBlack); DeleteObject(hBlack);
                HBRUSH b = CreateSolidBrush(cfg.col); SelectObject(dc, b); SelectObject(dc, GetStockObject(NULL_PEN));
                cfg.circle ? Ellipse(dc, 0, 0, cfg.size, cfg.size) : Rectangle(dc, 0, 0, cfg.size, cfg.size);
                DeleteObject(b); EndPaint(h, &ps); return 0;
            }
            return DefWindowProc(h, m, w, l);
        }

        // Warning Window Proc
        LRESULT CALLBACK W(HWND h, UINT m, WPARAM w, LPARAM l) {
            if(m == WM_PAINT) {
                PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
                RECT rc; GetClientRect(h, &rc);
                // Fill with black key color
                HBRUSH hK = CreateSolidBrush(RGB(1,1,1)); FillRect(dc, &rc, hK); DeleteObject(hK);

                // Smaller font for corner warning (-S(40) instead of -S(80))
                HFONT hFontBig = CreateFontA(-S(40), 0,0,0, FW_BOLD, 0,0,0, DEFAULT_CHARSET, 0,0,0,0, "Arial");
                SelectObject(dc, hFontBig); SetTextColor(dc, COL_WARN_TEXT); SetBkMode(dc, TRANSPARENT);

                const char* msg = "OBS INACTIVE"; // Changed text to cover both Rec & Stream

                // Calculate text position based on cfg.pos (same corners as indicator)
                // Indicator Posiitons: 0=BR, 1=BL, 2=TR, 3=TL
                // Text needs to be near the corner but not overlapping the indicator (approx size+pad)

                int p = cfg.pad;
                int s = cfg.size;
                int off = p + s + S(10); // Offset to clear the indicator

                RECT rT = rc; // Start with full screen rect
                UINT format = 0;

                if (cfg.pos == 3) { // Top Left
                    rT.left = off; rT.top = p;
                    format = DT_LEFT | DT_TOP;
                } else if (cfg.pos == 2) { // Top Right
                    rT.right = rc.right - off; rT.top = p;
                    format = DT_RIGHT | DT_TOP;
                } else if (cfg.pos == 1) { // Bottom Left
                    rT.left = off; rT.bottom = rc.bottom - p;
                    // DrawText doesn't support DT_BOTTOM easily for single line without calc, so we position top manually for bottom
                    rT.top = rc.bottom - p - S(40); // Approx height
                    format = DT_LEFT | DT_TOP;
                } else { // Bottom Right
                    rT.right = rc.right - off;
                    rT.top = rc.bottom - p - S(40);
                    format = DT_RIGHT | DT_TOP;
                }

                DrawTextA(dc, msg, -1, &rT, format | DT_NOCLIP);

                DeleteObject(hFontBig);
                EndPaint(h, &ps); return 0;
            }
            return DefWindowProc(h, m, w, l);
        }

        LRESULT CALLBACK M(HWND h, UINT m, WPARAM w, LPARAM l) {
            if(m == WM_CREATE) {
                auto Edit = [&](int id, const char* t, bool p=0) {
                    return CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", t, WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|(p?ES_PASSWORD:ES_NUMBER|ES_CENTER), 0,0,0,0, h, (HMENU)(INT_PTR)id, 0, 0);
                };
                hEditPad = Edit(100, std::to_string(cfg.pad).c_str());
                hEditSize = Edit(101, std::to_string(cfg.size).c_str());
                std::string p = ManagePass();
                hEditPass = Edit(102, p.c_str(), 1);
                Wipe(p);

                hEditLic = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", LIC_TEXT,
                                           WS_CHILD|WS_BORDER|WS_VSCROLL|ES_MULTILINE|ES_READONLY,
                                           0,0,0,0, h, (HMENU)103, 0, 0);
                SetWindowTheme(hEditLic, L"DarkMode_Explorer", NULL);
                SendMessage(hEditLic, WM_SETFONT, (WPARAM)hF, 1);

                // Process List Edit
                std::string pl = ReplaceAll(cfg.processList, "|", "\r\n");
                hEditProcs = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", pl.c_str(),
                                             WS_CHILD|WS_BORDER|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN,
                                             0,0,0,0, h, (HMENU)104, 0, 0);
                SetWindowTheme(hEditProcs, L"DarkMode_Explorer", NULL);
                SendMessage(hEditProcs, WM_SETFONT, (WPARAM)hF, 1);

                SendMessage(hEditPad, WM_SETFONT, (WPARAM)hF, 1); SendMessage(hEditSize, WM_SETFONT, (WPARAM)hF, 1); SendMessage(hEditPass, WM_SETFONT, (WPARAM)hF, 1);

                // Start Timer for Warning Logic
                SetTimer(h, TIMER_WARN, 100, NULL);
            }
            else if(m == WM_CTLCOLOREDIT || m == WM_CTLCOLORSTATIC) {
                HDC hdc = (HDC)w; SetBkColor(hdc, COL_INPUT_BG); SetTextColor(hdc, COL_TEXT);
                return (LRESULT)hBp;
            }
            else if(m == WM_SIZE) {
                if(w == SIZE_MINIMIZED) ShowWindow(h, SW_HIDE);
                else if(showLic) {
                    ShowWindow(hEditPad, 0); ShowWindow(hEditSize, 0); ShowWindow(hEditPass, 0); ShowWindow(hEditProcs, 0);
                    MoveWindow(hEditLic, S(20), S(20), S(600), S(320), 1);
                    ShowWindow(hEditLic, SW_SHOW);
                } else if(showProcList) {
                    ShowWindow(hEditPad, 0); ShowWindow(hEditSize, 0); ShowWindow(hEditPass, 0); ShowWindow(hEditLic, 0);
                    MoveWindow(hEditProcs, S(20), S(50), S(600), S(320), 1);
                    ShowWindow(hEditProcs, SW_SHOW);
                } else {
                    ShowWindow(hEditLic, SW_HIDE); ShowWindow(hEditProcs, SW_HIDE);
                    int C2 = S(240);
                    MoveWindow(hEditPad, C2+S(80), S(28), S(60), S(24), 1);
                    MoveWindow(hEditSize, C2+S(80), S(68), S(60), S(24), 1);
                    MoveWindow(hEditPass, C2, S(220)+S(30), S(160), S(24), 1);
                    ShowWindow(hEditPad, 1); ShowWindow(hEditSize, 1); ShowWindow(hEditPass, 1);
                }
            }
            else if(m == WM_COMMAND && HIWORD(w) == EN_CHANGE) {
                char b[4096]; GetWindowTextA((HWND)l, b, 4096);
                if((HWND)l == hEditPad) cfg.pad = atoi(b);
                if((HWND)l == hEditSize) cfg.size = atoi(b);
                if((HWND)l == hEditPass) ManagePass(b);
                if((HWND)l == hEditProcs) {
                    std::string s = b;
                    s = ReplaceAll(s, "\r\n", "|");
                    cfg.processList = s;
                }
                IOCfg(true); UpdateOv();
                if(isTest) InvalidateRect(hOv, NULL, FALSE);
            }
            else if(m == WM_LBUTTONDOWN) {
                POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                for(auto& hit : hits) if(PtInRect(&hit.r, p)) {
                    if(hit.id==10) cfg.pos=3; if(hit.id==11) cfg.pos=2; if(hit.id==12) cfg.pos=1; if(hit.id==13) cfg.pos=0;
                    if(hit.id==20) cfg.circle=0; if(hit.id==21) cfg.circle=1;
                    if(hit.id>=30 && hit.id<=32) { COLORREF c[]={RGB(255,0,0),RGB(0,255,0),RGB(0,0,255)}; cfg.col=c[hit.id-30]; }
                    if(hit.id==40) {
                        cfg.autoStart = !cfg.autoStart;
                        HKEY k; RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k);
                        if(cfg.autoStart) { char p[MAX_PATH], q[MAX_PATH]; GetModuleFileNameA(0,p,MAX_PATH); sprintf(q,"\"%s\" --tray", p); RegSetValueExA(k,"OBSInd",0,REG_SZ,(BYTE*)q,strlen(q)+1); }
                        else RegDeleteValueA(k, "OBSInd");
                        RegCloseKey(k);
                    }
                    if(hit.id==41) { isTest = !isTest; UpdateOv(); InvalidateRect(h,0,0); }
                    if(hit.id==42) showLic=1; if(hit.id==99) { showLic=0; showProcList=0; }

                    // Mode Select
                    if(hit.id==50) cfg.mode = MODE_INFO;
                    if(hit.id==51) cfg.mode = MODE_WARN;
                    if(hit.id==52) cfg.mode = MODE_WARN_ONLY;
                    if(hit.id==53) showProcList = 1;

                    IOCfg(true); UpdateOv(); InvalidateRect(h,0,0); SendMessage(h, WM_SIZE, 0, 0);
                    if(isTest) InvalidateRect(hOv, NULL, FALSE);
                }
            }
            else if(m == WM_TIMER && w == TIMER_WARN) {
                if (cfg.mode == MODE_WARN || cfg.mode == MODE_WARN_ONLY) {
                    DWORD now = GetTickCount();

                    // Check Focus Every Timer Tick (cached, low overhead)
                    warnTargetFocused = IsForegroundTarget();

                    // Warning Condition: Process Focused AND Not Recording AND Not Streaming (and not in Test mode)
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
            }
            else if(m == WM_OBS) { UpdateOv(); InvalidateRect(h,0,0); }
            else if(m == WM_PAINT) {
                PAINTSTRUCT ps; HDC dc = BeginPaint(h,&ps); RECT rc; GetClientRect(h, &rc);
                DrawUI(ps.hdc, rc.right, rc.bottom); EndPaint(h,&ps);
            }
            else if(m == WM_TRAY && l == WM_LBUTTONUP) { ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); }
            else if(m == WM_CLOSE) PostQuitMessage(0);
            return DefWindowProc(h, m, w, l);
        }

        int WINAPI WinMain(HINSTANCE hI, HINSTANCE, LPSTR p, int) {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            if(CreateMutex(0,1,"OBSIndM") && GetLastError()==ERROR_ALREADY_EXISTS) return 0;
            IOCfg(false); g_Scale = GetDpiForSystem() / 96.0f; InitGDI();

            WNDCLASS wc={0}; wc.lpfnWndProc=P; wc.hInstance=hI; wc.lpszClassName="O"; RegisterClass(&wc);
            hOv = CreateWindowEx(WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW,"O","",WS_POPUP,0,0,0,0,0,0,hI,0);
            SetLayeredWindowAttributes(hOv, 0, 0, LWA_COLORKEY);

            // Warning Window Class
            WNDCLASS wcw={0}; wcw.lpfnWndProc=W; wcw.hInstance=hI; wcw.lpszClassName="WARN"; RegisterClass(&wcw);
            hWarn = CreateWindowEx(WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW,"WARN","",WS_POPUP,0,0,0,0,0,0,hI,0);
            // Use RGB(1,1,1) as key for warning window transparency
            SetLayeredWindowAttributes(hWarn, RGB(1,1,1), 0, LWA_COLORKEY);

            wc.lpfnWndProc=M; wc.lpszClassName="C"; wc.hbrBackground=hBb; wc.hIcon=LoadIcon(hI, MAKEINTRESOURCE(IDI_MAIN_ICON)); wc.hCursor=LoadCursor(0, IDC_ARROW); RegisterClass(&wc);

            DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            RECT wr = { 0, 0, S(640), S(450) }; AdjustWindowRect(&wr, style, FALSE);

            int scrW = GetSystemMetrics(SM_CXSCREEN);
            int scrH = GetSystemMetrics(SM_CYSCREEN);
            int winW = wr.right - wr.left;
            int winH = wr.bottom - wr.top;
            int x = (scrW - winW) / 2;
            int y = (scrH - winH) / 2;

            hMain = CreateWindow("C", "OBS Indicator Settings", style, x, y, winW, winH, 0,0,hI,0);

            nid.hWnd=hMain; nid.uID=1; nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP; nid.uCallbackMessage=WM_TRAY; nid.hIcon=wc.hIcon; strcpy(nid.szTip, "OBS Indicator");
            Shell_NotifyIcon(NIM_ADD, &nid);

            BOOL d=1; DwmSetWindowAttribute(hMain, 20, &d, 4);
            std::thread(NetThread).detach();
            if(strstr(p, "--tray")) ShowWindow(hMain, SW_HIDE); else ShowWindow(hMain, SW_SHOW);

            MSG m; while(GetMessage(&m,0,0,0)) { TranslateMessage(&m); DispatchMessage(&m); }
            Shell_NotifyIcon(NIM_DELETE, &nid); return 0;
        }
