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

// --- CONSTANTS ---
#define WM_TRAY (WM_USER + 1)
#define WM_OBS  (WM_USER + 2)
#define IDI_MAIN_ICON 101

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

struct AppState {
    int size = 30;
    int pad = 20;
    int pos = 0; // 0=BR, 1=BL, 2=TR, 3=TL
    bool circle = false;
    bool autoStart = false;
    COLORREF col = RGB(255, 0, 0);
} cfg;

struct { int x=-1, y=-1, s=-1; bool vis=false; } lastOv;
std::atomic<bool> isRec(false), isConn(false), isTest(false);
bool showLic = false;
float g_Scale = 1.0f;
HWND hMain = NULL, hOv = NULL;
HWND hEditPad = NULL, hEditSize = NULL, hEditPass = NULL, hEditLic = NULL;
NOTIFYICONDATA nid = { sizeof(NOTIFYICONDATA) };

// Full license text with Windows-style line breaks for Edit control
const char* LIC_TEXT = "MIT License\r\n\r\nCopyright (c) 2025 aufkrawall\r\n\r\nPermission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the \"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:\r\n\r\nThe above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.\r\n\r\nTHE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.";

// --- HELPERS ---
int S(int v) { return (int)(v * g_Scale); }
void Wipe(std::string& s) { SecureZeroMemory(&s[0], s.size()); s.clear(); }

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
    } else {
        cfg.size = GetPrivateProfileIntA("S", "Sz", 30, path);
        cfg.pad = GetPrivateProfileIntA("S", "Pd", 20, path);
        cfg.pos = GetPrivateProfileIntA("S", "Ps", 0, path);
        cfg.circle = GetPrivateProfileIntA("S", "Cr", 0, path) == 1;
        cfg.autoStart = GetPrivateProfileIntA("S", "Au", 0, path) == 1;
        cfg.col = GetPrivateProfileIntA("S", "Cl", RGB(255, 0, 0), path);
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

        if(isConn) { isConn = false; isRec = false; PostMessage(hMain, WM_OBS, 0, 0); }

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

                DWORD lastPoll = 0;
                while (true) {
                    DWORD now = GetTickCount();
                    if (now - lastPoll > 3000) {
                        SendWS(s, "{\"op\":6,\"d\":{\"requestType\":\"GetRecordStatus\",\"requestId\":\"poll\"}}");
                        lastPoll = now;
    }

    timeval tv = { 0, 100000 };
    fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
    if (select(0, &fds, NULL, NULL, &tv) > 0) {
        int r = recv(s, buf, 4096, 0);
        if (r <= 0) break;
        std::string chunk(buf, r);
        if (chunk.find("RecordStateChanged") != -1 || chunk.find("GetRecordStatus") != -1) {
            bool act = chunk.find("\"outputActive\":true") != -1;
            if (act != isRec) { isRec = act; PostMessage(hMain, WM_OBS, 0, 0); }
        }
    }
}
}
}
closesocket(s); WSACleanup();
if(isConn) { isConn = false; isRec = false; PostMessage(hMain, WM_OBS, 0, 0); }
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

    if (showLic) {
        // Text is now handled by hEditLic control

        // Increased height to 35 and margin for better fit
        RECT b = {S(20), h-S(55), S(170), h-S(20)};
        FillRect(dc, &b, hBp);
        RECT i = {b.left+S(4), b.top+S(4), b.right-S(4), b.bottom-S(4)}; FillRect(dc, &i, hBa);
        DrawTextA(dc, "Back", -1, &b, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        hits.push_back({99, b});
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

        int C1 = 20, C2 = 240, C3 = 420;

        Grp(C1, 30, 180, 200, "Corner Position");
        Tick(10, C1+20, 50, "Top Left", cfg.pos==3);
        Tick(11, C1+20, 90, "Top Right", cfg.pos==2);
        Tick(12, C1+20, 130, "Bottom Left", cfg.pos==1);
        Tick(13, C1+20, 170, "Bottom Right", cfg.pos==0);

        TextOutA(dc, S(C2), S(30), "Padding:", 8);
        TextOutA(dc, S(C2), S(70), "Size:", 5);

        Grp(C2, 130, 160, 100, "Shape");
        Tick(20, C2+20, 150, "Square", !cfg.circle);
        Tick(21, C2+20, 190, "Circle", cfg.circle);

        Grp(C3, 30, 140, 140, "Color");
        Tick(30, C3+20, 50, "Red", cfg.col==RGB(255,0,0));
        Tick(31, C3+20, 90, "Green", cfg.col==RGB(0,255,0));
        Tick(32, C3+20, 130, "Blue", cfg.col==RGB(0,0,255));

        Btn(41, C1, 260, isTest ? "Stop Test" : "Test Overlay", isTest);
        Tick(40, C3, 260, "Launch on Startup", cfg.autoStart);

        // Changed License Checkbox to Button
        Btn(42, C3, 300, "View License", false);

        TextOutA(dc, S(C2), S(260)+S(8), "WebSocket Password:", 19);

        // Status Line
        char st[100]; COLORREF stCol;
        if(isTest) { strcpy(st, "Status: TEST MODE"); stCol = COL_ACCENT; }
        else if(!isConn) { strcpy(st, "Status: Disconnected"); stCol = COL_STATUS_ERR; }
        else if(isRec) { strcpy(st, "Status: Recording (WebSocket)"); stCol = COL_STATUS_REC; }
        else { strcpy(st, "Status: Idle (WebSocket)"); stCol = COL_STATUS_WS; }

        SetTextColor(dc, stCol);
        TextOutA(dc, S(C1), S(260)+S(40), st, strlen(st));
    }

    BitBlt(hdc, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    DeleteObject(bm); DeleteDC(dc);
}

void UpdateOv() {
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN), p = cfg.pad, s = cfg.size;
    int x[] = {sw-s-p, p, sw-s-p, p}, y[] = {sh-s-p, sh-s-p, p, p};
    int cx = x[cfg.pos], cy = y[cfg.pos];
    bool v = isRec || isTest;
    if (v != lastOv.vis || (v && (cx!=lastOv.x || cy!=lastOv.y || s!=lastOv.s))) {
        SetWindowPos(hOv, HWND_TOPMOST, cx, cy, s, s, SWP_NOACTIVATE | (v?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
        if(v) InvalidateRect(hOv, NULL, TRUE);
        lastOv = {cx, cy, s, v};
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

        // APPLY DARK MODE THEME TO SCROLLBAR
        SetWindowTheme(hEditLic, L"DarkMode_Explorer", NULL);

        SendMessage(hEditPad, WM_SETFONT, (WPARAM)hF, 1); SendMessage(hEditSize, WM_SETFONT, (WPARAM)hF, 1); SendMessage(hEditPass, WM_SETFONT, (WPARAM)hF, 1);
        SendMessage(hEditLic, WM_SETFONT, (WPARAM)hF, 1);
    }
    else if(m == WM_CTLCOLOREDIT || m == WM_CTLCOLORSTATIC) {
        HDC hdc = (HDC)w; SetBkColor(hdc, COL_INPUT_BG); SetTextColor(hdc, COL_TEXT);
        return (LRESULT)hBp;
    }
    else if(m == WM_SIZE) {
        if(w == SIZE_MINIMIZED) ShowWindow(h, SW_HIDE);
        else if(!showLic) {
            int C2 = S(240);
            MoveWindow(hEditPad, C2+S(80), S(28), S(60), S(24), 1);
            MoveWindow(hEditSize, C2+S(80), S(68), S(60), S(24), 1);
            MoveWindow(hEditPass, C2, S(260)+S(30), S(160), S(24), 1);
            ShowWindow(hEditPad, 1); ShowWindow(hEditSize, 1); ShowWindow(hEditPass, 1);
            ShowWindow(hEditLic, SW_HIDE);
        } else {
            ShowWindow(hEditPad, 0); ShowWindow(hEditSize, 0); ShowWindow(hEditPass, 0);
            MoveWindow(hEditLic, S(20), S(20), S(600), S(320), 1);
            ShowWindow(hEditLic, SW_SHOW);
        }
    }
    else if(m == WM_COMMAND && HIWORD(w) == EN_CHANGE) {
        char b[256]; GetWindowTextA((HWND)l, b, 256);
        if((HWND)l == hEditPad) cfg.pad = atoi(b);
        if((HWND)l == hEditSize) cfg.size = atoi(b);
        if((HWND)l == hEditPass) ManagePass(b);
        IOCfg(true); UpdateOv();
        if(isTest) InvalidateRect(hOv, NULL, FALSE); // Force repaint on edit change
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
            if(hit.id==42) showLic=1; if(hit.id==99) showLic=0;
            IOCfg(true); UpdateOv(); InvalidateRect(h,0,0); SendMessage(h, WM_SIZE, 0, 0);
            if(isTest) InvalidateRect(hOv, NULL, FALSE); // Force repaint on click
        }
    }
    else if(m == WM_TIMER) { KillTimer(h,1); UpdateOv(); InvalidateRect(h,0,0); }
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

    wc.lpfnWndProc=M; wc.lpszClassName="C"; wc.hbrBackground=hBb; wc.hIcon=LoadIcon(hI, MAKEINTRESOURCE(IDI_MAIN_ICON)); wc.hCursor=LoadCursor(0, IDC_ARROW); RegisterClass(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr = { 0, 0, S(640), S(400) }; AdjustWindowRect(&wr, style, FALSE);

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