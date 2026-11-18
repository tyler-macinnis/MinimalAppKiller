// MinimalAppKiller - lightweight tray-based GUI app that automatically
// terminates user-selected processes.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "resource.h"
#include "version.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "advapi32.lib")
#endif

namespace
{
    constexpr wchar_t kWindowClass[] = L"MinimalAppKillerWnd";
    constexpr wchar_t kWindowTitle[] = L"Minimal App Killer";
    constexpr wchar_t kMutexName[] = L"Local\\MinimalAppKillerSingleInstance";
    constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kRunValueName[] = L"MinimalAppKiller";
    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr UINT WM_KILLCOUNT = WM_APP + 2; // posted by the worker after it kills something
    constexpr UINT kTrayIconId = 1;
    constexpr unsigned int kScanIntervalMs = 1000;

    struct AppEntry
    {
        std::wstring name;
        bool enabled = true;
    };

    struct AppState
    {
        std::vector<AppEntry> apps;
        bool active = true;
    };

    // UI / shared state
    HINSTANCE g_instance = nullptr;
    HWND g_mainWindow = nullptr;
    HWND g_listView = nullptr;
    HWND g_editApp = nullptr;
    HWND g_statusLabel = nullptr;
    HWND g_stateLabel = nullptr;
    HWND g_toggleButton = nullptr;
    HWND g_startupCheck = nullptr;
    // Windows 11 surface and text colours; the app follows the system light/dark setting.
    struct Theme
    {
        COLORREF window, card, text, subtle, active, paused;
    };
    constexpr Theme kLight{RGB(243, 243, 243), RGB(255, 255, 255), RGB(27, 27, 27),
                           RGB(96, 96, 96), RGB(15, 123, 15), RGB(157, 93, 0)};
    constexpr Theme kDark{RGB(32, 32, 32), RGB(43, 43, 43), RGB(255, 255, 255),
                          RGB(200, 200, 200), RGB(108, 203, 95), RGB(252, 225, 0)};
    const Theme *g_theme = &kLight;
    HBRUSH g_backgroundBrush = nullptr;
    HBRUSH g_cardBrush = nullptr;
    // shortcut: sized for the primary monitor's DPI only, handle WM_DPICHANGED if mixed-DPI setups matter
    int g_dpi = 96;
    NOTIFYICONDATAW g_trayIcon{};
    bool g_trayBalloonShown = false;
    bool g_suppressListNotifications = false;

    AppState g_state;
    std::mutex g_stateMutex;
    std::atomic<unsigned long long> g_killCount{0};
    HANDLE g_workerStopEvent = nullptr;

    // Processes that must never be added as kill targets.
    const std::unordered_set<std::wstring> kProtectedProcesses = {
        L"minimalappkiller.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe",
        L"services.exe", L"lsass.exe", L"smss.exe", L"svchost.exe",
        L"system", L"system idle process", L"dwm.exe"};

    // Scales a 96-DPI pixel measurement to the display.
    int S(int px)
    {
        return MulDiv(px, g_dpi, 96);
    }

    std::wstring Trim(const std::wstring &value)
    {
        size_t start = 0;
        while (start < value.size() && ::iswspace(value[start]))
        {
            ++start;
        }
        size_t end = value.size();
        while (end > start && ::iswspace(value[end - 1]))
        {
            --end;
        }
        return value.substr(start, end - start);
    }

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
                       { return static_cast<wchar_t>(::towlower(ch)); });
        return value;
    }

    std::wstring Utf8ToWide(const std::string &text)
    {
        if (text.empty())
        {
            return std::wstring();
        }
        const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring result(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), needed);
        return result;
    }

    std::string WideToUtf8(const std::wstring &text)
    {
        if (text.empty())
        {
            return std::string();
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), needed, nullptr, nullptr);
        return result;
    }

    // ---------------- Settings persistence ----------------

    // Empty on failure, which simply makes the file streams below fail to open.
    std::filesystem::path SettingsPath()
    {
        wchar_t appData[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData)))
        {
            return {};
        }
        const std::wstring dir = std::wstring(appData) + L"\\MinimalAppKiller";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\settings.ini";
    }

    void LoadSettings(AppState &state)
    {
        std::ifstream file(SettingsPath());
        std::unordered_set<std::wstring> seen;
        std::string rawLine;
        while (std::getline(file, rawLine))
        {
            std::wstring line = Trim(Utf8ToWide(rawLine));
            if (line.empty() || line[0] == L'#')
            {
                continue;
            }

            if (line.rfind(L"active=", 0) == 0)
            {
                state.active = line.substr(7) != L"0";
                continue;
            }

            if (line.rfind(L"app=", 0) == 0)
            {
                std::wstring payload = line.substr(4);
                bool enabled = true;
                const size_t sep = payload.rfind(L'|');
                if (sep != std::wstring::npos)
                {
                    enabled = payload.substr(sep + 1) != L"0";
                    payload = payload.substr(0, sep);
                }
                payload = Trim(payload);
                if (payload.empty())
                {
                    continue;
                }
                if (seen.insert(ToLower(payload)).second)
                {
                    state.apps.push_back({payload, enabled});
                }
            }
        }
    }

    void SaveSettings(const AppState &state)
    {
        std::ofstream file(SettingsPath(), std::ios::trunc);
        file << "# MinimalAppKiller settings\n";
        file << "active=" << (state.active ? 1 : 0) << "\n";
        for (const AppEntry &app : state.apps)
        {
            file << "app=" << WideToUtf8(app.name) << "|" << (app.enabled ? 1 : 0) << "\n";
        }
    }

    // ---------------- Run-on-startup (HKCU Run key) ----------------

    bool IsStartupEnabled()
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        {
            return false;
        }
        const bool exists = RegQueryValueExW(key, kRunValueName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        RegCloseKey(key);
        return exists;
    }

    void SetStartupEnabled(bool enable)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        {
            return;
        }

        if (enable)
        {
            wchar_t modulePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            const std::wstring command = L"\"" + std::wstring(modulePath) + L"\" --minimized";
            RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                           reinterpret_cast<const BYTE *>(command.c_str()),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            RegDeleteValueW(key, kRunValueName);
        }
        RegCloseKey(key);
    }

    // ---------------- Process scanning / killing ----------------

    std::vector<DWORD> FindTargetPids(const std::unordered_set<std::wstring> &targets)
    {
        std::vector<DWORD> pids;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return pids;
        }

        PROCESSENTRY32W process{};
        process.dwSize = sizeof(process);
        if (Process32FirstW(snapshot, &process))
        {
            do
            {
                if (targets.count(ToLower(process.szExeFile)) != 0)
                {
                    pids.push_back(process.th32ProcessID);
                }
            } while (Process32NextW(snapshot, &process));
        }

        CloseHandle(snapshot);
        return pids;
    }

    bool KillProcessByPid(DWORD pid)
    {
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (process == nullptr)
        {
            return false;
        }
        const BOOL terminated = TerminateProcess(process, 1);
        CloseHandle(process);
        return terminated != FALSE;
    }

    void WorkerLoop()
    {
        // Scan, then sleep one interval; the stop event ends the wait (and the loop) at once.
        do
        {
            std::unordered_set<std::wstring> targets;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                if (g_state.active)
                {
                    for (const AppEntry &app : g_state.apps)
                    {
                        if (app.enabled)
                        {
                            targets.insert(ToLower(app.name));
                        }
                    }
                }
            }

            if (targets.empty())
            {
                continue;
            }

            unsigned int killed = 0;
            for (const DWORD pid : FindTargetPids(targets))
            {
                if (KillProcessByPid(pid))
                {
                    ++killed;
                }
            }
            if (killed > 0)
            {
                g_killCount += killed;
                PostMessageW(g_mainWindow, WM_KILLCOUNT, 0, 0);
            }
        } while (WaitForSingleObject(g_workerStopEvent, kScanIntervalMs) == WAIT_TIMEOUT);
    }

    // ---------------- Tray icon ----------------

    void AddTrayIcon(HWND hwnd)
    {
        g_trayIcon = {};
        g_trayIcon.cbSize = sizeof(g_trayIcon);
        g_trayIcon.hWnd = hwnd;
        g_trayIcon.uID = kTrayIconId;
        g_trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        g_trayIcon.uCallbackMessage = WM_TRAYICON;
        g_trayIcon.hIcon = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        lstrcpynW(g_trayIcon.szTip, kWindowTitle, ARRAYSIZE(g_trayIcon.szTip));
        Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
    }

    void RemoveTrayIcon()
    {
        Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
        if (g_trayIcon.hIcon != nullptr)
        {
            DestroyIcon(g_trayIcon.hIcon);
            g_trayIcon.hIcon = nullptr;
        }
    }

    void ShowTrayBalloonOnce()
    {
        if (g_trayBalloonShown)
        {
            return;
        }
        g_trayBalloonShown = true;
        g_trayIcon.uFlags = NIF_INFO;
        g_trayIcon.dwInfoFlags = NIIF_INFO;
        lstrcpynW(g_trayIcon.szInfoTitle, kWindowTitle, ARRAYSIZE(g_trayIcon.szInfoTitle));
        lstrcpynW(g_trayIcon.szInfo, L"Still running in the background. Double-click the tray icon to open.",
                  ARRAYSIZE(g_trayIcon.szInfo));
        Shell_NotifyIconW(NIM_MODIFY, &g_trayIcon);
        g_trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    }

    void ShowTrayMenu(HWND hwnd)
    {
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            active = g_state.active;
        }

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, L"&Open Minimal App Killer");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (active ? MF_CHECKED : 0), IDM_TRAY_TOGGLE, L"&Active");
        AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : 0), IDM_TRAY_STARTUP, L"Run on &startup");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"E&xit");
        SetMenuDefaultItem(menu, IDM_TRAY_OPEN, FALSE);

        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(hwnd);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }

    // ---------------- UI helpers ----------------

    void UpdateStatus()
    {
        size_t total = 0;
        size_t enabled = 0;
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            total = g_state.apps.size();
            active = g_state.active;
            for (const AppEntry &app : g_state.apps)
            {
                if (app.enabled)
                {
                    ++enabled;
                }
            }
        }

        wchar_t status[160];
        wsprintfW(status, L"Watching %u of %u app(s)  \u2022  %u process(es) terminated",
                  static_cast<unsigned>(enabled), static_cast<unsigned>(total),
                  static_cast<unsigned>(g_killCount.load()));
        SetWindowTextW(g_statusLabel, status);

        SetWindowTextW(g_stateLabel, active ? L"\u25CF Active" : L"\u23F8 Paused");
        SetWindowTextW(g_toggleButton, active ? L"Pause" : L"Resume");
        InvalidateRect(g_stateLabel, nullptr, TRUE);
    }

    void RefreshListView()
    {
        g_suppressListNotifications = true;
        ListView_DeleteAllItems(g_listView);

        std::lock_guard<std::mutex> lock(g_stateMutex);
        int index = 0;
        for (const AppEntry &app : g_state.apps)
        {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = index;
            item.pszText = const_cast<wchar_t *>(app.name.c_str());
            ListView_InsertItem(g_listView, &item);
            ListView_SetCheckState(g_listView, index, app.enabled ? TRUE : FALSE);
            ++index;
        }
        g_suppressListNotifications = false;
    }

    void PersistState()
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        SaveSettings(g_state);
    }

    void AddAppFromEdit(HWND hwnd)
    {
        wchar_t buffer[260]{};
        GetWindowTextW(g_editApp, buffer, ARRAYSIZE(buffer));
        std::wstring name = Trim(buffer);
        if (name.empty())
        {
            return;
        }

        if (name.find(L'.') == std::wstring::npos)
        {
            name += L".exe";
        }

        const std::wstring lowered = ToLower(name);
        if (kProtectedProcesses.count(lowered) != 0)
        {
            MessageBoxW(hwnd, L"That process is protected and cannot be added.", kWindowTitle,
                        MB_OK | MB_ICONWARNING);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            for (const AppEntry &app : g_state.apps)
            {
                if (ToLower(app.name) == lowered)
                {
                    MessageBoxW(hwnd, L"That app is already in the list.", kWindowTitle,
                                MB_OK | MB_ICONINFORMATION);
                    return;
                }
            }
            g_state.apps.push_back({name, true});
        }

        SetWindowTextW(g_editApp, L"");
        PersistState();
        RefreshListView();
        UpdateStatus();
    }

    void RemoveSelectedApps()
    {
        std::vector<int> selected;
        int item = -1;
        while ((item = ListView_GetNextItem(g_listView, item, LVNI_SELECTED)) != -1)
        {
            selected.push_back(item);
        }
        if (selected.empty())
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            for (auto it = selected.rbegin(); it != selected.rend(); ++it)
            {
                if (*it >= 0 && static_cast<size_t>(*it) < g_state.apps.size())
                {
                    g_state.apps.erase(g_state.apps.begin() + *it);
                }
            }
        }

        PersistState();
        RefreshListView();
        UpdateStatus();
    }

    void ToggleActive()
    {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.active = !g_state.active;
        }
        PersistState();
        UpdateStatus();
    }

    // Re-reads the system light/dark setting and restyles the window and every control.
    void ApplyTheme(HWND hwnd)
    {
        DWORD light = 1;
        DWORD size = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        const BOOL dark = light == 0;
        g_theme = dark ? &kDark : &kLight;

        DeleteObject(g_backgroundBrush);
        DeleteObject(g_cardBrush);
        g_backgroundBrush = CreateSolidBrush(g_theme->window);
        g_cardBrush = CreateSolidBrush(g_theme->card);

        // Title bar: dark caption buttons, and on Windows 11 the same colour as the window body.
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &g_theme->window, sizeof(COLORREF));

        EnumChildWindows(hwnd, [](HWND child, LPARAM dark) -> BOOL
                         {
                             SetWindowTheme(child, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
                             return TRUE;
                         },
                         dark);
        SetWindowTheme(g_listView, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        if (dark)
        {
            SetWindowTheme(g_editApp, L"DarkMode_CFD", nullptr);
        }
        ListView_SetBkColor(g_listView, g_theme->card);
        ListView_SetTextBkColor(g_listView, g_theme->card);
        ListView_SetTextColor(g_listView, g_theme->text);

        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    }

    void CreateControls(HWND hwnd)
    {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        LOGFONTW logFont = metrics.lfMessageFont;
        wcscpy_s(logFont.lfFaceName, L"Segoe UI");
        logFont.lfHeight = -S(15);
        // Both fonts live as long as the (only) window, so they are never freed.
        const HFONT uiFont = CreateFontIndirectW(&logFont);
        logFont.lfHeight = -S(20);
        logFont.lfWeight = FW_SEMIBOLD;
        const HFONT titleFont = CreateFontIndirectW(&logFont);

        // Positions are set by LayoutControls; creation order is the tab order.
        const auto make = [&](const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD exStyle = 0)
        {
            const HWND control = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd,
                                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), TRUE);
            return control;
        };

        g_stateLabel = make(L"STATIC", L"", SS_CENTERIMAGE, IDC_STATIC_STATE);
        SendMessageW(g_stateLabel, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
        g_toggleButton = make(L"BUTTON", L"", WS_TABSTOP, IDC_BTN_TOGGLE);

        g_editApp = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_EDIT_APP, WS_EX_CLIENTEDGE);
        SendMessageW(g_editApp, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"Process name, e.g. notepad.exe"));
        make(L"BUTTON", L"Add", WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_BTN_ADD);

        g_listView = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
                          IDC_LIST_APPS);
        ListView_SetExtendedListViewStyle(g_listView,
                                          LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        // A 1px-wide image list is the standard way to get taller, roomier rows.
        ListView_SetImageList(g_listView, ImageList_Create(1, S(30), ILC_COLOR, 1, 0), LVSIL_SMALL);
        LVCOLUMNW column{};
        ListView_InsertColumn(g_listView, 0, &column); // width is set by LayoutControls

        make(L"BUTTON", L"Remove selected", WS_TABSTOP, IDC_BTN_REMOVE);
        g_startupCheck = make(L"BUTTON", L"Run on startup", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_CHK_STARTUP);
        Button_SetCheck(g_startupCheck, IsStartupEnabled() ? BST_CHECKED : BST_UNCHECKED);
        // Themed checkboxes always draw their own text black, which is unreadable in dark mode. So the
        // checkbox is laid out box-only (its text stays as the accessible name) with this label beside it.
        make(L"STATIC", L"Run on startup", SS_NOTIFY | SS_CENTERIMAGE, IDC_LBL_STARTUP);

        g_statusLabel = make(L"STATIC", L"", 0, IDC_STATIC_STATUS);
        ApplyTheme(hwnd);
    }

    void LayoutControls(HWND hwnd)
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = client.right;
        const int height = client.bottom;
        const int margin = S(20);
        const int row = S(30);
        const int gap = S(12);
        const int inner = width - margin * 2;
        const int statusHeight = S(20);

        // Header: state on the left, pause/resume on the right.
        int y = margin;
        MoveWindow(g_stateLabel, margin, y, inner - S(112), row, TRUE);
        MoveWindow(g_toggleButton, width - margin - S(100), y, S(100), row, TRUE);

        // Add row.
        y += row + gap;
        MoveWindow(g_editApp, margin, y + S(1), inner - S(92), row - S(2), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_BTN_ADD), width - margin - S(80), y, S(80), row, TRUE);

        // List fills whatever is left above the footer (action row + status line).
        y += row + gap;
        const int footerTop = height - margin - statusHeight - gap - row;
        MoveWindow(g_listView, margin, y, inner, footerTop - gap - y, TRUE);
        ListView_SetColumnWidth(g_listView, 0, LVSCW_AUTOSIZE_USEHEADER); // fill the list's width

        MoveWindow(GetDlgItem(hwnd, IDC_BTN_REMOVE), margin, footerTop, S(130), row, TRUE);
        MoveWindow(g_startupCheck, margin + S(146), footerTop, S(15), row, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_LBL_STARTUP), margin + S(168), footerTop, S(140), row, TRUE);
        MoveWindow(g_statusLabel, margin, height - margin - statusHeight, inner, statusHeight, TRUE);
    }

    void HandleListItemChanged(const NMLISTVIEW *info)
    {
        if (g_suppressListNotifications || info->iItem < 0)
        {
            return;
        }
        // Detect state-image (checkbox) changes.
        if ((info->uChanged & LVIF_STATE) == 0 ||
            ((info->uNewState ^ info->uOldState) & LVIS_STATEIMAGEMASK) == 0 ||
            (info->uOldState & LVIS_STATEIMAGEMASK) == 0)
        {
            return;
        }

        const bool checked = ListView_GetCheckState(g_listView, info->iItem) != FALSE;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            if (static_cast<size_t>(info->iItem) < g_state.apps.size())
            {
                g_state.apps[static_cast<size_t>(info->iItem)].enabled = checked;
            }
        }
        PersistState();
        UpdateStatus();
    }

    void ShowMainWindow(HWND hwnd)
    {
        ShowWindow(hwnd, SW_SHOW);
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            CreateControls(hwnd);
            AddTrayIcon(hwnd);
            RefreshListView();
            UpdateStatus();
            return 0;

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED)
            {
                ShowWindow(hwnd, SW_HIDE);
                ShowTrayBalloonOnce();
            }
            else
            {
                LayoutControls(hwnd);
            }
            return 0;

        case WM_GETMINMAXINFO:
        {
            MINMAXINFO *info = reinterpret_cast<MINMAXINFO *>(lParam);
            info->ptMinTrackSize.x = S(380);
            info->ptMinTrackSize.y = S(420);
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        {
            HDC dc = reinterpret_cast<HDC>(wParam);
            COLORREF color = g_theme->text;
            if (reinterpret_cast<HWND>(lParam) == g_stateLabel)
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                color = g_state.active ? g_theme->active : g_theme->paused;
            }
            else if (reinterpret_cast<HWND>(lParam) == g_statusLabel)
            {
                color = g_theme->subtle;
            }
            SetTextColor(dc, color);
            SetBkColor(dc, g_theme->window);
            return reinterpret_cast<LRESULT>(g_backgroundBrush);
        }

        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wParam), g_theme->text);
            SetBkColor(reinterpret_cast<HDC>(wParam), g_theme->card);
            return reinterpret_cast<LRESULT>(g_cardBrush);

        case WM_CTLCOLORBTN:
            return reinterpret_cast<LRESULT>(g_backgroundBrush);

        case WM_ERASEBKGND:
        {
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, g_backgroundBrush);
            return 1;
        }

        case WM_SETTINGCHANGE: // the user switched Windows between light and dark
            if (lParam != 0 && wcscmp(reinterpret_cast<const wchar_t *>(lParam), L"ImmersiveColorSet") == 0)
            {
                ApplyTheme(hwnd);
            }
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDC_BTN_ADD:
            case IDOK: // Enter key via IsDialogMessage
                AddAppFromEdit(hwnd);
                return 0;
            case IDC_BTN_REMOVE:
                RemoveSelectedApps();
                return 0;
            case IDC_BTN_TOGGLE:
            case IDM_TRAY_TOGGLE:
                ToggleActive();
                return 0;
            case IDC_CHK_STARTUP:
                SetStartupEnabled(Button_GetCheck(g_startupCheck) == BST_CHECKED);
                return 0;
            case IDC_LBL_STARTUP:
                if (HIWORD(wParam) == STN_CLICKED)
                {
                    SendMessageW(g_startupCheck, BM_CLICK, 0, 0);
                }
                return 0;
            case IDM_TRAY_STARTUP:
                SetStartupEnabled(!IsStartupEnabled());
                Button_SetCheck(g_startupCheck, IsStartupEnabled() ? BST_CHECKED : BST_UNCHECKED);
                return 0;
            case IDM_TRAY_OPEN:
                ShowMainWindow(hwnd);
                return 0;
            case IDM_TRAY_EXIT:
                DestroyWindow(hwnd);
                return 0;
            default:
                break;
            }
            break;

        case WM_NOTIFY:
        {
            const NMHDR *header = reinterpret_cast<const NMHDR *>(lParam);
            if (header->idFrom == IDC_LIST_APPS && header->code == LVN_ITEMCHANGED)
            {
                HandleListItemChanged(reinterpret_cast<const NMLISTVIEW *>(lParam));
            }
            else if (header->idFrom == IDC_LIST_APPS && header->code == LVN_GETEMPTYMARKUP)
            {
                NMLVEMPTYMARKUP *markup = reinterpret_cast<NMLVEMPTYMARKUP *>(lParam);
                markup->dwFlags = EMF_CENTERED;
                wcscpy_s(markup->szMarkup, L"No apps yet. Add a process name above.");
                return TRUE;
            }
            else if (header->idFrom == IDC_LIST_APPS && header->code == LVN_KEYDOWN)
            {
                const NMLVKEYDOWN *key = reinterpret_cast<const NMLVKEYDOWN *>(lParam);
                if (key->wVKey == VK_DELETE)
                {
                    RemoveSelectedApps();
                }
            }
            break;
        }

        case WM_TRAYICON:
            switch (LOWORD(lParam))
            {
            case WM_LBUTTONDBLCLK:
                SendMessageW(hwnd, WM_COMMAND, IDM_TRAY_OPEN, 0);
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                ShowTrayMenu(hwnd);
                break;
            default:
                break;
            }
            return 0;

        case WM_KILLCOUNT:
            UpdateStatus();
            return 0;

        case WM_CLOSE: // close button hides to tray; exit via tray menu
            ShowWindow(hwnd, SW_HIDE);
            ShowTrayBalloonOnce();
            return 0;

        case WM_DESTROY:
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR commandLine, int)
{
    g_instance = hInstance;
    g_dpi = static_cast<int>(GetDpiForSystem());

    // Single instance: a second launch just surfaces the first. The mutex is held until exit.
    CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (const HWND existing = FindWindowW(kWindowClass, nullptr))
        {
            ShowMainWindow(existing);
        }
        return 0;
    }

    const bool startMinimized = commandLine != nullptr && wcsstr(commandLine, L"--minimized") != nullptr;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    LoadSettings(g_state);

    // Opt the process into dark menus and scrollbars. uxtheme ordinal 135 (SetPreferredAppMode,
    // 1 = AllowDark) is undocumented but unchanged since Windows 10 1903; without it the app still works.
    if (const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
    {
        if (const FARPROC setPreferredAppMode = GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)))
        {
            reinterpret_cast<int(WINAPI *)(int)>(setPreferredAppMode)(1);
        }
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = hInstance;
    windowClass.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    g_mainWindow = CreateWindowExW(0, kWindowClass, kWindowTitle,
                                   WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, S(440), S(540),
                                   nullptr, nullptr, hInstance, nullptr);
    if (g_mainWindow == nullptr)
    {
        return 1;
    }

    if (!startMinimized)
    {
        ShowWindow(g_mainWindow, SW_SHOW);
        UpdateWindow(g_mainWindow);
    }

    g_workerStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker(WorkerLoop);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (IsDialogMessageW(g_mainWindow, &msg))
        {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Settings are saved on every change, and the OS reclaims handles and GDI objects on exit.
    SetEvent(g_workerStopEvent);
    worker.join();
    return static_cast<int>(msg.wParam);
}
