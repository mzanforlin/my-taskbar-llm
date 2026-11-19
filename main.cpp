#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// Identificadores de Comandos do Menu
#define IDM_DOCK_TASKBAR    1001
#define IDM_START_BOOT      1002
#define IDM_ABOUT           1003
#define IDM_EXIT            1004

#define TIMER_UPDATE_ID     1

// Dimensões da janela do monitor (compacta para a barra do Windows 10)
const int WND_WIDTH  = 480;
const int WND_HEIGHT = 36;

// Paleta de Cores Escura e Moderna
const COLORREF COLOR_BG         = RGB(24, 24, 28);      // Fundo escuro fosco
const COLORREF COLOR_BORDER     = RGB(50, 50, 60);      // Borda sutil
const COLORREF COLOR_BAR_BG     = RGB(40, 40, 48);      // Fundo dos medidores
const COLORREF COLOR_CPU        = RGB(56, 189, 248);    // Ciano vibrante
const COLORREF COLOR_RAM        = RGB(168, 85, 247);    // Violeta vibrante
const COLORREF COLOR_NET_DOWN   = RGB(34, 197, 94);     // Verde esmeralda (Down)
const COLORREF COLOR_NET_UP     = RGB(251, 146, 60);    // Laranja suave (Up)
const COLORREF COLOR_TEXT_LABEL = RGB(148, 163, 184);   // Cinza suave
const COLORREF COLOR_TEXT_VAL   = RGB(248, 250, 252);   // Branco nítido

// Estrutura de Métricas
typedef struct {
    double cpuPercent;
    double ramPercent;
    double ramUsedGB;
    double ramTotalGB;
    double netDownSpeedKB;
    double netUpSpeedKB;
    bool dockedToTaskbar;
    bool autoStartEnabled;
    // Limite de sessao (5h) do Claude, lido de ~/.claude/claude-usage.txt
    bool claudeHasData;
    double claudePercent;
    unsigned __int64 claudeResetsAt; // epoch em segundos
    // Limite de 5h do Antigravity (via `agy -p "/usage"`): grupo Gemini e grupo Claude/GPT
    bool agyGemHas;
    double agyGemUsedPct;
    unsigned __int64 agyGemResets;
    bool agyThirdHas;
    double agyThirdUsedPct;
    unsigned __int64 agyThirdResets;
} SystemPerfData;

static SystemPerfData g_perfData = {0};
static FILETIME g_prevIdleTime = {0};
static FILETIME g_prevKernelTime = {0};
static FILETIME g_prevUserTime = {0};
static unsigned __int64 g_prevInBytes = 0;
static unsigned __int64 g_prevOutBytes = 0;
static DWORD g_prevTickCount = 0;
static HFONT g_hFontBold = NULL;
static HFONT g_hFontSmall = NULL;

static unsigned __int64 FileTimeToU64(FILETIME ft) {
    return ((unsigned __int64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// Verifica se está configurado para inicializar com o Windows
static bool CheckAutoStart() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0;
        LONG res = RegQueryValueExW(hKey, L"TaskbarPerfMonitor", NULL, &type, NULL, NULL);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS);
    }
    return false;
}

// Ativa ou desativa inicialização com o Windows
static void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            RegSetValueExW(hKey, L"TaskbarPerfMonitor", 0, REG_SZ, (const BYTE*)exePath, (DWORD)((wcslen(exePath) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"TaskbarPerfMonitor");
        }
        RegCloseKey(hKey);
    }
}

// Le o arquivo gravado pela statusline do Claude Code ("<pct> <resets_at_epoch>")
static void UpdateClaudeUsage() {
    wchar_t profile[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    wchar_t path[MAX_PATH + 40];
    swprintf_s(path, MAX_PATH + 40, L"%s\\.claude\\claude-usage.txt", profile);

    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return;
    double pct = 0.0;
    unsigned long long resets = 0;
    int got = fscanf(f, "%lf %llu", &pct, &resets);
    fclose(f);
    if (got == 2) {
        g_perfData.claudePercent = pct;
        g_perfData.claudeResetsAt = resets;
        g_perfData.claudeHasData = true;
    }
}

static void AgyOutPath(wchar_t* out, size_t n) {
    wchar_t tmp[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, tmp);
    if (len == 0 || len >= MAX_PATH) tmp[0] = 0;
    swprintf_s(out, n, L"%staskbarperf-agy.json", tmp);
}

// Dispara `agy -p "/usage"` em segundo plano (sem janela), saida vai para arquivo
static void LaunchAgyUsage() {
    wchar_t local[MAX_PATH], outPath[MAX_PATH + 40];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    AgyOutPath(outPath, MAX_PATH + 40);

    wchar_t cmd[2 * MAX_PATH + 160];
    swprintf_s(cmd, 2 * MAX_PATH + 160,
        L"cmd.exe /c \"\"%s\\agy\\bin\\agy.exe\" -p \"/usage\" --output-format json > \"%s\"\"",
        local, outPath);

    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// Acha o bucket `id` no JSON e extrai remaining_fraction + reset_time (ISO UTC -> epoch)
static bool ParseAgyBucket(const char* txt, const char* id, double* usedPct, unsigned __int64* resets) {
    char key[64];
    sprintf_s(key, sizeof(key), "\"id\":\"%s\"", id);
    const char* p = strstr(txt, key);
    if (!p) return false;
    const char* end = strchr(p, '}');
    const char* rf = strstr(p, "\"remaining_fraction\":");
    const char* rt = strstr(p, "\"reset_time\":\"");
    if (!rf || !rt || !end || rf > end || rt > end) return false;

    double frac = atof(rf + 21);
    SYSTEMTIME st = {0};
    int Y, M, D, h, m, s;
    if (sscanf_s(rt + 14, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return false;
    st.wYear = (WORD)Y; st.wMonth = (WORD)M; st.wDay = (WORD)D;
    st.wHour = (WORD)h; st.wMinute = (WORD)m; st.wSecond = (WORD)s;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return false;

    double used = (1.0 - frac) * 100.0;
    if (used < 0.0) used = 0.0;
    if (used > 100.0) used = 100.0;
    *usedPct = used;
    *resets = (FileTimeToU64(ft) - 116444736000000000ULL) / 10000000ULL;
    return true;
}

static void UpdateAgyUsage() {
    wchar_t path[MAX_PATH + 40];
    AgyOutPath(path, MAX_PATH + 40);
    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return;
    static char buf[16384];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = 0;

    double pct; unsigned __int64 rs;
    if (ParseAgyBucket(buf, "gemini-5h", &pct, &rs)) {
        g_perfData.agyGemUsedPct = pct; g_perfData.agyGemResets = rs; g_perfData.agyGemHas = true;
    }
    if (ParseAgyBucket(buf, "3p-5h", &pct, &rs)) {
        g_perfData.agyThirdUsedPct = pct; g_perfData.agyThirdResets = rs; g_perfData.agyThirdHas = true;
    }
}

// Coleta métricas de CPU, RAM e Rede
static void UpdateMetrics() {
    UpdateAgyUsage();
    UpdateClaudeUsage();

    // 1. CPU via GetSystemTimes (Zero impacto de CPU)
    FILETIME idleTime, kernelTime, userTime;
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        unsigned __int64 idle = FileTimeToU64(idleTime);
        unsigned __int64 kernel = FileTimeToU64(kernelTime);
        unsigned __int64 user = FileTimeToU64(userTime);

        unsigned __int64 prevIdle = FileTimeToU64(g_prevIdleTime);
        unsigned __int64 prevKernel = FileTimeToU64(g_prevKernelTime);
        unsigned __int64 prevUser = FileTimeToU64(g_prevUserTime);

        unsigned __int64 sysDiff = (kernel - prevKernel) + (user - prevUser);
        unsigned __int64 idleDiff = (idle - prevIdle);

        if (sysDiff > 0 && prevKernel > 0) {
            double cpu = 0.0;
            if (sysDiff >= idleDiff) {
                cpu = (double)(sysDiff - idleDiff) * 100.0 / (double)sysDiff;
            }
            if (cpu < 0.0) cpu = 0.0;
            if (cpu > 100.0) cpu = 100.0;
            g_perfData.cpuPercent = cpu;
        }

        g_prevIdleTime = idleTime;
        g_prevKernelTime = kernelTime;
        g_prevUserTime = userTime;
    }

    // 2. RAM via GlobalMemoryStatusEx
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&mem)) {
        g_perfData.ramPercent = (double)mem.dwMemoryLoad;
        double totalGB = (double)mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
        double availGB = (double)mem.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);
        g_perfData.ramTotalGB = totalGB;
        g_perfData.ramUsedGB = totalGB - availGB;
    }

    // 3. REDE via GetIfTable (Compatibilidade nativa total)
    DWORD currentTick = GetTickCount();
    DWORD deltaMs = currentTick - g_prevTickCount;
    if (deltaMs == 0) deltaMs = 1000;

    DWORD dwSize = sizeof(MIB_IFTABLE) + sizeof(MIB_IFROW) * 64;
    MIB_IFTABLE* pTable = (MIB_IFTABLE*)malloc(dwSize);
    if (pTable) {
        if (GetIfTable(pTable, &dwSize, FALSE) == ERROR_INSUFFICIENT_BUFFER) {
            free(pTable);
            pTable = (MIB_IFTABLE*)malloc(dwSize);
        }

        if (pTable && GetIfTable(pTable, &dwSize, FALSE) == NO_ERROR) {
            unsigned __int64 totalIn = 0;
            unsigned __int64 totalOut = 0;

            for (DWORD i = 0; i < pTable->dwNumEntries; ++i) {
                MIB_IFROW* row = &pTable->table[i];
                // Interfaces ativas tipo Ethernet ou Wi-Fi (71 = IEEE 802.11)
                if (row->dwOperStatus == MIB_IF_OPER_STATUS_OPERATIONAL &&
                    (row->dwType == MIB_IF_TYPE_ETHERNET || row->dwType == 71 || row->dwType == MIB_IF_TYPE_PPP)) {
                    totalIn += row->dwInOctets;
                    totalOut += row->dwOutOctets;
                }
            }

            if (g_prevTickCount > 0 && totalIn >= g_prevInBytes && totalOut >= g_prevOutBytes) {
                double deltaSec = (double)deltaMs / 1000.0;
                g_perfData.netDownSpeedKB = ((double)(totalIn - g_prevInBytes) / 1024.0) / deltaSec;
                g_perfData.netUpSpeedKB = ((double)(totalOut - g_prevOutBytes) / 1024.0) / deltaSec;
            }

            g_prevInBytes = totalIn;
            g_prevOutBytes = totalOut;
            g_prevTickCount = currentTick;
        }

        if (pTable) free(pTable);
    }
}

// Alinha perfeitamente a janela na barra de tarefas do Windows 10
static void DockToTaskbar(HWND hwnd) {
    HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!hTaskbar) return;

    RECT rcTaskbar, rcTray;
    GetWindowRect(hTaskbar, &rcTaskbar);

    HWND hTray = FindWindowExW(hTaskbar, NULL, L"TrayNotifyWnd", NULL);
    if (hTray && GetWindowRect(hTray, &rcTray)) {
        int x, y;
        int tbHeight = rcTaskbar.bottom - rcTaskbar.top;
        
        // Se a barra estiver na parte inferior (padrão do Windows)
        if (rcTaskbar.top > 0) {
            x = rcTray.left - WND_WIDTH - 6;
            y = rcTaskbar.top + (tbHeight - WND_HEIGHT) / 2;
        } else {
            x = rcTray.left - WND_WIDTH - 6;
            y = rcTaskbar.top + 2;
        }

        SetWindowPos(hwnd, HWND_TOPMOST, x, y, WND_WIDTH, WND_HEIGHT, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// Formata exibição da velocidade de rede
static void FormatSpeed(double kbSpeed, wchar_t* buffer, size_t bufSize) {
    if (kbSpeed >= 1024.0) {
        swprintf_s(buffer, bufSize, L"%.1f MB/s", kbSpeed / 1024.0);
    } else {
        swprintf_s(buffer, bufSize, L"%.0f KB/s", kbSpeed);
    }
}

// Desenha a mini-barra horizontal de progresso
static void DrawMiniBar(HDC hdc, int x, int y, int width, int height, double percent, COLORREF barColor) {
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;

    HBRUSH hBgBrush = CreateSolidBrush(COLOR_BAR_BG);
    RECT rcBg = { x, y, x + width, y + height };
    FillRect(hdc, &rcBg, hBgBrush);
    DeleteObject(hBgBrush);

    int fillW = (int)((double)width * (percent / 100.0));
    if (fillW > 0) {
        HBRUSH hFillBrush = CreateSolidBrush(barColor);
        RECT rcFill = { x, y, x + fillW, y + height };
        FillRect(hdc, &rcFill, hFillBrush);
        DeleteObject(hFillBrush);
    }
}

// Renderização Double-Buffer em Memória (Sem flickering, 0% CPU)
static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdcWin = BeginPaint(hwnd, &ps);

    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    int w = rcClient.right - rcClient.left;
    int h = rcClient.bottom - rcClient.top;

    HDC hdcMem = CreateCompatibleDC(hdcWin);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcWin, w, h);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBmp);

    // Fundo
    HBRUSH hBgBrush = CreateSolidBrush(COLOR_BG);
    FillRect(hdcMem, &rcClient, hBgBrush);
    DeleteObject(hBgBrush);

    // Borda arredondada suave
    HPEN hPenBorder = CreatePen(PS_SOLID, 1, COLOR_BORDER);
    HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPenBorder);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
    RoundRect(hdcMem, 0, 0, w, h, 6, 6);
    SelectObject(hdcMem, hOldPen);
    SelectObject(hdcMem, hOldBrush);
    DeleteObject(hPenBorder);

    SetBkMode(hdcMem, TRANSPARENT);

    // -------------------------------------------------------------
    // BLOCO 1: CPU
    // -------------------------------------------------------------
    int xCpu = 8;
    SelectObject(hdcMem, g_hFontSmall);
    SetTextColor(hdcMem, COLOR_TEXT_LABEL);
    TextOutW(hdcMem, xCpu, 3, L"CPU", 3);

    wchar_t szCpu[16];
    swprintf_s(szCpu, 16, L"%2.0f%%", g_perfData.cpuPercent);
    SelectObject(hdcMem, g_hFontBold);
    SetTextColor(hdcMem, COLOR_CPU);
    TextOutW(hdcMem, xCpu + 24, 3, szCpu, (int)wcslen(szCpu));

    DrawMiniBar(hdcMem, xCpu, 22, 60, 5, g_perfData.cpuPercent, COLOR_CPU);

    // Separador vertical 1
    HPEN hSepPen = CreatePen(PS_SOLID, 1, COLOR_BORDER);
    SelectObject(hdcMem, hSepPen);
    MoveToEx(hdcMem, 74, 5, NULL);
    LineTo(hdcMem, 74, 30);

    // -------------------------------------------------------------
    // BLOCO 2: RAM
    // -------------------------------------------------------------
    int xRam = 80;
    SelectObject(hdcMem, g_hFontSmall);
    SetTextColor(hdcMem, COLOR_TEXT_LABEL);
    TextOutW(hdcMem, xRam, 3, L"RAM", 3);

    wchar_t szRam[16];
    swprintf_s(szRam, 16, L"%2.0f%%", g_perfData.ramPercent);
    SelectObject(hdcMem, g_hFontBold);
    SetTextColor(hdcMem, COLOR_RAM);
    TextOutW(hdcMem, xRam + 26, 3, szRam, (int)wcslen(szRam));

    DrawMiniBar(hdcMem, xRam, 22, 60, 5, g_perfData.ramPercent, COLOR_RAM);

    // Separador vertical 2
    MoveToEx(hdcMem, 146, 5, NULL);
    LineTo(hdcMem, 146, 30);
    DeleteObject(hSepPen);

    // -------------------------------------------------------------
    // BLOCO 3: REDE (Down & Up)
    // -------------------------------------------------------------
    int xNet = 152;
    SelectObject(hdcMem, g_hFontSmall);

    wchar_t szDown[20], szUp[20];
    FormatSpeed(g_perfData.netDownSpeedKB, szDown, 20);
    FormatSpeed(g_perfData.netUpSpeedKB, szUp, 20);

    // Download (D:)
    SetTextColor(hdcMem, COLOR_NET_DOWN);
    TextOutW(hdcMem, xNet, 2, L"D:", 2);
    SetTextColor(hdcMem, COLOR_TEXT_VAL);
    TextOutW(hdcMem, xNet + 16, 2, szDown, (int)wcslen(szDown));

    // Upload (U:)
    SetTextColor(hdcMem, COLOR_NET_UP);
    TextOutW(hdcMem, xNet, 18, L"U:", 2);
    SetTextColor(hdcMem, COLOR_TEXT_VAL);
    TextOutW(hdcMem, xNet + 16, 18, szUp, (int)wcslen(szUp));

    // Separador vertical 3
    HPEN hSepPen3 = CreatePen(PS_SOLID, 1, COLOR_BORDER);
    SelectObject(hdcMem, hSepPen3);
    MoveToEx(hdcMem, 236, 5, NULL);
    LineTo(hdcMem, 236, 30);
    SelectObject(hdcMem, hOldPen);
    DeleteObject(hSepPen3);

    // -------------------------------------------------------------
    // BLOCO 4: Limite de sessao do Claude (5h) + horario do reset
    // -------------------------------------------------------------
    int xCl = 242;
    SelectObject(hdcMem, g_hFontSmall);
    SetTextColor(hdcMem, COLOR_TEXT_LABEL);
    TextOutW(hdcMem, xCl, 3, L"Claude", 6);

    wchar_t szCl[16] = L"--";
    wchar_t szReset[32] = L"sem dados";
    double clPct = 0.0;

    if (g_perfData.claudeHasData) {
        FILETIME nowFt;
        GetSystemTimeAsFileTime(&nowFt);
        unsigned __int64 nowEpoch = (FileTimeToU64(nowFt) - 116444736000000000ULL) / 10000000ULL;

        if (nowEpoch >= g_perfData.claudeResetsAt) {
            // Janela ja resetou: sessao zerada ate a proxima atualizacao
            clPct = 0.0;
            wcscpy_s(szCl, 16, L"0%");
            wcscpy_s(szReset, 32, L"resetou");
        } else {
            clPct = g_perfData.claudePercent;
            swprintf_s(szCl, 16, L"%2.0f%%", clPct);

            unsigned __int64 t = g_perfData.claudeResetsAt * 10000000ULL + 116444736000000000ULL;
            FILETIME utcFt, localFt;
            utcFt.dwLowDateTime = (DWORD)(t & 0xFFFFFFFF);
            utcFt.dwHighDateTime = (DWORD)(t >> 32);
            SYSTEMTIME st;
            if (FileTimeToLocalFileTime(&utcFt, &localFt) && FileTimeToSystemTime(&localFt, &st)) {
                swprintf_s(szReset, 32, L"reseta %02d:%02d", st.wHour, st.wMinute);
            }
        }
    }

    COLORREF clColor = (clPct >= 90.0) ? RGB(239, 68, 68) :
                       (clPct >= 70.0) ? RGB(251, 191, 36) : RGB(217, 119, 87);
    SelectObject(hdcMem, g_hFontBold);
    SetTextColor(hdcMem, clColor);
    TextOutW(hdcMem, xCl + 44, 3, szCl, (int)wcslen(szCl));

    DrawMiniBar(hdcMem, xCl, 22, 40, 5, clPct, clColor);
    SelectObject(hdcMem, g_hFontSmall);
    SetTextColor(hdcMem, COLOR_TEXT_VAL);
    TextOutW(hdcMem, xCl + 46, 18, szReset, (int)wcslen(szReset));

    // Separador vertical 4
    HPEN hSepPen4 = CreatePen(PS_SOLID, 1, COLOR_BORDER);
    SelectObject(hdcMem, hSepPen4);
    MoveToEx(hdcMem, 354, 5, NULL);
    LineTo(hdcMem, 354, 30);
    SelectObject(hdcMem, hOldPen);
    DeleteObject(hSepPen4);

    // -------------------------------------------------------------
    // BLOCO 5: Antigravity (limite de 5h): Gemini e Claude/GPT
    // -------------------------------------------------------------
    {
        FILETIME nf;
        GetSystemTimeAsFileTime(&nf);
        unsigned __int64 nowE = (FileTimeToU64(nf) - 116444736000000000ULL) / 10000000ULL;

        struct Row { const wchar_t* label; bool has; double pct; unsigned __int64 resets; int y; COLORREF col; };
        Row rows[2] = {
            { L"Gem",   g_perfData.agyGemHas,   g_perfData.agyGemUsedPct,   g_perfData.agyGemResets,   2,  RGB(96, 165, 250) },
            { L"Cl/GPT", g_perfData.agyThirdHas, g_perfData.agyThirdUsedPct, g_perfData.agyThirdResets, 18, RGB(52, 211, 153) }
        };
        int xAg = 360;
        SelectObject(hdcMem, g_hFontSmall);
        for (int i = 0; i < 2; ++i) {
            SetTextColor(hdcMem, rows[i].col);
            TextOutW(hdcMem, xAg, rows[i].y, rows[i].label, (int)wcslen(rows[i].label));

            wchar_t txt[40] = L"--";
            if (rows[i].has) {
                double pct = rows[i].pct;
                if (nowE >= rows[i].resets) {
                    wcscpy_s(txt, 40, L"0%");
                } else {
                    unsigned __int64 t = rows[i].resets * 10000000ULL + 116444736000000000ULL;
                    FILETIME u, l; SYSTEMTIME st;
                    u.dwLowDateTime = (DWORD)(t & 0xFFFFFFFF);
                    u.dwHighDateTime = (DWORD)(t >> 32);
                    if (FileTimeToLocalFileTime(&u, &l) && FileTimeToSystemTime(&l, &st))
                        swprintf_s(txt, 40, L"%.0f%%  %02d:%02d", pct, st.wHour, st.wMinute);
                    else
                        swprintf_s(txt, 40, L"%.0f%%", pct);
                }
            }
            SetTextColor(hdcMem, COLOR_TEXT_VAL);
            TextOutW(hdcMem, xAg + 40, rows[i].y, txt, (int)wcslen(txt));
        }
    }

    // Copia para a janela em 1 operação de vídeo
    BitBlt(hdcWin, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);

    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);

    EndPaint(hwnd, &ps);
}

// Procedimento de Janela
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_perfData.dockedToTaskbar = true;
            g_perfData.autoStartEnabled = CheckAutoStart();

            // Fontes Segoe UI limpas
            g_hFontBold = CreateFontW(-12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            g_hFontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            LaunchAgyUsage();
            UpdateMetrics();
            DockToTaskbar(hwnd);
            SetTimer(hwnd, TIMER_UPDATE_ID, 1000, NULL);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == TIMER_UPDATE_ID) {
                static int agyTicks = 0;
                if (++agyTicks >= 120) { agyTicks = 0; LaunchAgyUsage(); } // atualiza Antigravity a cada 2 min
                UpdateMetrics();
                if (g_perfData.dockedToTaskbar) {
                    DockToTaskbar(hwnd);
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            // Permite reposicionar livremente arrastando
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            g_perfData.dockedToTaskbar = false;
            return 0;
        }

        case WM_RBUTTONUP: {
            POINT pt;
            GetCursorPos(&pt);

            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, g_perfData.dockedToTaskbar ? MF_CHECKED : MF_UNCHECKED, 
                IDM_DOCK_TASKBAR, L"📌 Travar na Barra de Tarefas");
            AppendMenuW(hMenu, g_perfData.autoStartEnabled ? MF_CHECKED : MF_UNCHECKED, 
                IDM_START_BOOT, L"🚀 Iniciar com o Windows");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            
            wchar_t memInfo[64];
            swprintf_s(memInfo, 64, L"Memória: %.1f GB / %.1f GB", g_perfData.ramUsedGB, g_perfData.ramTotalGB);
            AppendMenuW(hMenu, MF_GRAYED, 0, memInfo);
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

            AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"ℹ️ Sobre o TaskbarPerf");
            AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"❌ Fechar");

            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);

            if (cmd == IDM_DOCK_TASKBAR) {
                g_perfData.dockedToTaskbar = !g_perfData.dockedToTaskbar;
                if (g_perfData.dockedToTaskbar) {
                    DockToTaskbar(hwnd);
                }
            } else if (cmd == IDM_START_BOOT) {
                g_perfData.autoStartEnabled = !g_perfData.autoStartEnabled;
                SetAutoStart(g_perfData.autoStartEnabled);
            } else if (cmd == IDM_ABOUT) {
                MessageBoxW(hwnd, 
                    L"TaskbarPerf Monitor v1.0\n\n- Monitor de CPU, RAM e Tráfego de Rede\n- Consumo estimado: < 3 MB de RAM\n- 0% de uso de CPU\n\nDica: Clique com o botão esquerdo para arrastar livremente para onde quiser!", 
                    L"Sobre o TaskbarPerf", MB_OK | MB_ICONINFORMATION);
            } else if (cmd == IDM_EXIT) {
                DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_PAINT:
            OnPaint(hwnd);
            return 0;

        case WM_DESTROY: {
            KillTimer(hwnd, TIMER_UPDATE_ID);
            if (g_hFontBold) DeleteObject(g_hFontBold);
            if (g_hFontSmall) DeleteObject(g_hFontSmall);
            PostQuitMessage(0);
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// Entrada da aplicação
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"TaskbarPerf_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"O TaskbarPerf já está rodando na sua barra de tarefas!", L"Aviso", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"TaskbarPerfMonitorClass";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;

    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        wc.lpszClassName,
        L"TaskbarPerf Monitor",
        WS_POPUP,
        100, 100, WND_WIDTH, WND_HEIGHT,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return (int)msg.wParam;
}
