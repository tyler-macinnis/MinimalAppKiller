// MinimalAppKiller - lightweight tray-based GUI app that automatically
// terminates user-selected processes.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
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
    HFONT g_uiFont = nullptr;
    HFONT g_titleFont = nullptr;
    HBRUSH g_backgroundBrush = nullptr;
    NOTIFYICONDATAW g_trayIcon{};
    bool g_trayBalloonShown = false;
    bool g_suppressListNotifications = false;

    AppState g_state;
    std::mutex g_stateMutex;
    std::atomic<bool> g_workerRunning{true};
    std::atomic<unsigned long long> g_killCount{0};
    std::thread g_workerThread;
    HANDLE g_workerStopEvent = nullptr;

    // Processes that must never be added as kill targets.
    const std::unordered_set<std::wstring> kProtectedProcesses = {
        L"minimalappkiller.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe",
        L"services.exe", L"lsass.exe", L"smss.exe", L"svchost.exe",
        L"system", L"system idle process", L"dwm.exe"};

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

    std::wstring SettingsDirectory()
    {
        wchar_t appData[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData)))
        {
            return std::wstring();
        }
        std::wstring dir = std::wstring(appData) + L"\\MinimalAppKiller";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }

    std::wstring SettingsPath()
    {
        const std::wstring dir = SettingsDirectory();
        if (dir.empty())
        {
            return std::wstring();
        }
        return dir + L"\\settings.ini";
    }

    void LoadSettings(AppState &state)
    {
        const std::wstring path = SettingsPath();
        if (path.empty())
        {
            return;
        }

        std::ifstream file(WideToUtf8(path).c_str());
        if (!file)
        {
            return;
        }

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
        const std::wstring path = SettingsPath();
        if (path.empty())
        {
            return;
        }

        std::ofstream file(WideToUtf8(path).c_str(), std::ios::trunc);
        if (!file)
        {
            return;
        }

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
        while (g_workerRunning)
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

            if (!targets.empty())
            {
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
                    if (g_mainWindow != nullptr)
                    {
                        PostMessageW(g_mainWindow, WM_APP + 2, 0, 0);
                    }
                }
            }

            // Sleep until the next scan, waking immediately if shutdown is requested.
            if (g_workerStopEvent == nullptr)
            {
                Sleep(kScanIntervalMs);
            }
            else if (WaitForSingleObject(g_workerStopEvent, kScanIntervalMs) != WAIT_TIMEOUT)
            {
                break;
            }
        }
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

    void CreateControls(HWND hwnd)
    {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        LOGFONTW logFont = metrics.lfMessageFont;
        wcscpy_s(logFont.lfFaceName, L"Segoe UI");
        logFont.lfHeight = -15;
        g_uiFont = CreateFontIndirectW(&logFont);
        logFont.lfHeight = -17;
        logFont.lfWeight = FW_SEMIBOLD;
        g_titleFont = CreateFontIndirectW(&logFont);

        g_stateLabel = CreateWindowExW(0, L"STATIC", L"\u25CF Active", WS_CHILD | WS_VISIBLE,
                                       0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_STATIC_STATE), g_instance, nullptr);
        g_toggleButton = CreateWindowExW(0, L"BUTTON", L"Pause", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                         0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_BTN_TOGGLE), g_instance, nullptr);

        g_editApp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_APP), g_instance, nullptr);
        SendMessageW(g_editApp, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"Process name, e.g. notepad.exe"));

        CreateWindowExW(0, L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_BTN_ADD), g_instance, nullptr);

        g_listView = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
                                     0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LIST_APPS), g_instance, nullptr);
        ListView_SetExtendedListViewStyle(g_listView,
                                          LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SetWindowTheme(g_listView, L"Explorer", nullptr);

        LVCOLUMNW column{};
        column.mask = LVCF_WIDTH;
        column.cx = 400;
        ListView_InsertColumn(g_listView, 0, &column);

        CreateWindowExW(0, L"BUTTON", L"Remove selected", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_BTN_REMOVE), g_instance, nullptr);
        g_startupCheck = CreateWindowExW(0, L"BUTTON", L"Run on startup",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                         0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_CHK_STARTUP), g_instance, nullptr);
        Button_SetCheck(g_startupCheck, IsStartupEnabled() ? BST_CHECKED : BST_UNCHECKED);

        g_statusLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_STATIC_STATUS), g_instance, nullptr);

        EnumChildWindows(hwnd, [](HWND child, LPARAM font) -> BOOL
                         {
                             SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
                             return TRUE;
                         },
                         reinterpret_cast<LPARAM>(g_uiFont));
        SendMessageW(g_stateLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_titleFont), TRUE);
    }

    void LayoutControls(HWND hwnd)
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        const int margin = 16;
        const int rowHeight = 30;

        MoveWindow(g_stateLabel, margin, margin, width - margin * 2 - 110, rowHeight, TRUE);
        MoveWindow(g_toggleButton, width - margin - 100, margin - 2, 100, rowHeight, TRUE);

        const int editTop = margin + rowHeight + 10;
        MoveWindow(g_editApp, margin, editTop, width - margin * 2 - 90, rowHeight - 4, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_BTN_ADD), width - margin - 80, editTop - 1, 80, rowHeight - 2, TRUE);

        const int listTop = editTop + rowHeight + 8;
        const int bottomArea = 76;
        MoveWindow(g_listView, margin, listTop, width - margin * 2,
                   height - listTop - bottomArea, TRUE);
        ListView_SetColumnWidth(g_listView, 0, width - margin * 2 - GetSystemMetrics(SM_CXVSCROLL) - 6);

        const int buttonsTop = height - bottomArea + 8;
        MoveWindow(GetDlgItem(hwnd, IDC_BTN_REMOVE), margin, buttonsTop, 130, rowHeight - 2, TRUE);
        MoveWindow(g_startupCheck, margin + 144, buttonsTop + 3, 160, rowHeight - 6, TRUE);
        MoveWindow(g_statusLabel, margin, height - 28, width - margin * 2, 20, TRUE);
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

    void ExitApplication(HWND hwnd)
    {
        RemoveTrayIcon();
        DestroyWindow(hwnd);
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
            info->ptMinTrackSize.x = 380;
            info->ptMinTrackSize.y = 420;
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, RGB(255, 255, 255));
            if (reinterpret_cast<HWND>(lParam) == g_stateLabel)
            {
                bool active = false;
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    active = g_state.active;
                }
                SetTextColor(dc, active ? RGB(22, 138, 61) : RGB(196, 119, 0));
            }
            else if (reinterpret_cast<HWND>(lParam) == g_statusLabel)
            {
                SetTextColor(dc, RGB(110, 110, 110));
            }
            return reinterpret_cast<LRESULT>(g_backgroundBrush);
        }

        case WM_CTLCOLORBTN:
            return reinterpret_cast<LRESULT>(g_backgroundBrush);

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
            case IDM_TRAY_STARTUP:
                SetStartupEnabled(!IsStartupEnabled());
                Button_SetCheck(g_startupCheck, IsStartupEnabled() ? BST_CHECKED : BST_UNCHECKED);
                return 0;
            case IDM_TRAY_OPEN:
                ShowWindow(hwnd, SW_SHOW);
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
                return 0;
            case IDM_TRAY_EXIT:
                ExitApplication(hwnd);
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

        case WM_APP + 2: // kill-count update from worker thread
            UpdateStatus();
            return 0;

        case WM_CLOSE: // close button hides to tray; exit via tray menu
            ShowWindow(hwnd, SW_HIDE);
            ShowTrayBalloonOnce();
            return 0;

        case WM_DESTROY:
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

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND existing = FindWindowW(kWindowClass, nullptr);
        if (existing != nullptr)
        {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(mutex);
        return 0;
    }

    const bool startMinimized = commandLine != nullptr && wcsstr(commandLine, L"--minimized") != nullptr;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    LoadSettings(g_state);

    g_backgroundBrush = CreateSolidBrush(RGB(255, 255, 255));

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = hInstance;
    windowClass.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = g_backgroundBrush;
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    g_mainWindow = CreateWindowExW(0, kWindowClass, kWindowTitle,
                                   WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 440, 520,
                                   nullptr, nullptr, hInstance, nullptr);
    if (g_mainWindow == nullptr)
    {
        if (mutex != nullptr)
        {
            CloseHandle(mutex);
        }
        return 1;
    }

    ShowWindow(g_mainWindow, startMinimized ? SW_HIDE : SW_SHOW);
    if (!startMinimized)
    {
        UpdateWindow(g_mainWindow);
    }

    g_workerStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_workerThread = std::thread(WorkerLoop);

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

    g_workerRunning = false;
    if (g_workerStopEvent != nullptr)
    {
        SetEvent(g_workerStopEvent);
    }
    if (g_workerThread.joinable())
    {
        g_workerThread.join();
    }
    if (g_workerStopEvent != nullptr)
    {
        CloseHandle(g_workerStopEvent);
        g_workerStopEvent = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        SaveSettings(g_state);
    }

    if (g_uiFont != nullptr)
    {
        DeleteObject(g_uiFont);
    }
    if (g_titleFont != nullptr)
    {
        DeleteObject(g_titleFont);
    }
    if (g_backgroundBrush != nullptr)
    {
        DeleteObject(g_backgroundBrush);
    }
    if (mutex != nullptr)
    {
        CloseHandle(mutex);
    }

    return static_cast<int>(msg.wParam);
}
