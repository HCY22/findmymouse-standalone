// Standalone host for Find My Mouse: tray icon, settings file, hotkey and single-instance handling.
#include "pch.h"
#include "FindMyMouse.h"
#include "Settings.h"
#include "resource.h"

namespace
{
    constexpr const wchar_t* TRAY_CLASS = L"FindMyMouseStandaloneTray";
    constexpr const wchar_t* MUTEX_NAME = L"Local\\FindMyMouseStandalone-3C9D2E0A";
    constexpr const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr const wchar_t* RUN_VALUE = L"FindMyMouse";

    constexpr UINT WM_TRAY = WM_APP + 10;
    constexpr UINT TRAY_ID = 1;
    constexpr int HOTKEY_ID = 1;

    enum MenuId : UINT
    {
        IDM_ENABLED = 100,
        IDM_TRIGGER,
        IDM_EDIT_SETTINGS,
        IDM_RELOAD,
        IDM_AUTOSTART,
        IDM_EXIT,
    };

    HINSTANCE g_hinst{};
    HWND g_trayHwnd{};
    UINT g_wmTaskbarCreated = 0;
    UINT g_wmCommand = 0; // registered message for --trigger / --exit from another process
    std::wstring g_settingsPath;
    FindMyMouseSettings g_settings;
    bool g_enabled = true;
    bool g_hotkeyRegistered = false;

    constexpr WPARAM CMD_TRIGGER = 1;
    constexpr WPARAM CMD_EXIT = 2;
    constexpr WPARAM CMD_RELOAD = 3;

    std::wstring ExePath()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return path;
    }

    bool IsAutostartEnabled()
    {
        wchar_t value[MAX_PATH * 2];
        DWORD size = sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
    }

    void SetAutostart(bool enable)
    {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        {
            return;
        }
        if (enable)
        {
            std::wstring command = L"\"" + ExePath() + L"\"";
            RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.length() + 1) * sizeof(wchar_t)));
        }
        else
        {
            RegDeleteValueW(key, RUN_VALUE);
        }
        RegCloseKey(key);
    }

    std::wstring MethodDescription()
    {
        switch (g_settings.activationMethod)
        {
        case FindMyMouseActivationMethod::DoubleLeftControlKey:
            return L"連按兩下左 Ctrl";
        case FindMyMouseActivationMethod::DoubleRightControlKey:
            return L"連按兩下右 Ctrl";
        case FindMyMouseActivationMethod::ShakeMouse:
            return L"搖晃滑鼠";
        case FindMyMouseActivationMethod::Shortcut:
            return HotkeyToString(g_settings.hotkeyModifiers, g_settings.hotkeyVk);
        default:
            return L"";
        }
    }

    void UpdateTrayIcon(DWORD action)
    {
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = g_trayHwnd;
        nid.uID = TRAY_ID;
        nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = static_cast<HICON>(LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        std::wstring tip = L"Find My Mouse - ";
        tip += g_enabled ? MethodDescription() : L"已暫停";
        StringCchCopyW(nid.szTip, ARRAYSIZE(nid.szTip), tip.c_str());
        Shell_NotifyIconW(action, &nid);
        if (nid.hIcon)
        {
            DestroyIcon(nid.hIcon);
        }
    }

    void ShowBalloon(const wchar_t* title, const std::wstring& text)
    {
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = g_trayHwnd;
        nid.uID = TRAY_ID;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_WARNING;
        StringCchCopyW(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
        StringCchCopyW(nid.szInfo, ARRAYSIZE(nid.szInfo), text.c_str());
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void UpdateHotkey()
    {
        if (g_hotkeyRegistered)
        {
            UnregisterHotKey(g_trayHwnd, HOTKEY_ID);
            g_hotkeyRegistered = false;
        }
        if (g_enabled && g_settings.activationMethod == FindMyMouseActivationMethod::Shortcut)
        {
            g_hotkeyRegistered = RegisterHotKey(g_trayHwnd, HOTKEY_ID, g_settings.hotkeyModifiers | MOD_NOREPEAT, g_settings.hotkeyVk) != FALSE;
            if (!g_hotkeyRegistered)
            {
                ShowBalloon(L"Find My Mouse",
                            L"無法註冊快捷鍵 " + HotkeyToString(g_settings.hotkeyModifiers, g_settings.hotkeyVk) +
                                L"，可能已被其他程式占用。請在設定檔中更換 ActivationShortcut。");
            }
        }
    }

    void ReloadSettings()
    {
        g_settings = LoadSettings(g_settingsPath);
        FindMyMouseApplySettings(g_settings);
        UpdateHotkey();
        UpdateTrayIcon(NIM_MODIFY);
    }

    void SetEnabled(bool enabled)
    {
        g_enabled = enabled;
        FindMyMouseSetPaused(!enabled);
        UpdateHotkey();
        UpdateTrayIcon(NIM_MODIFY);
    }

    void ExitApp()
    {
        if (g_hotkeyRegistered)
        {
            UnregisterHotKey(g_trayHwnd, HOTKEY_ID);
            g_hotkeyRegistered = false;
        }
        UpdateTrayIcon(NIM_DELETE);
        DestroyWindow(g_trayHwnd);
        // Destroys the sonar window, which posts WM_QUIT.
        FindMyMouseTerminate();
    }

    void ShowTrayMenu()
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (g_enabled ? MF_CHECKED : MF_UNCHECKED), IDM_ENABLED, L"啟用(&E)");
        AppendMenuW(menu, MF_STRING, IDM_TRIGGER, L"立即顯示聚光燈(&S)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_EDIT_SETTINGS, L"編輯設定檔(&O)...");
        AppendMenuW(menu, MF_STRING, IDM_RELOAD, L"重新載入設定(&R)");
        AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED), IDM_AUTOSTART, L"開機時自動啟動(&A)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_EXIT, L"結束(&X)");
        SetMenuDefaultItem(menu, IDM_EDIT_SETTINGS, FALSE);

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(g_trayHwnd);
        UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_trayHwnd, nullptr);
        DestroyMenu(menu);
        PostMessageW(g_trayHwnd, WM_NULL, 0, 0);

        switch (cmd)
        {
        case IDM_ENABLED:
            SetEnabled(!g_enabled);
            break;
        case IDM_TRIGGER:
            FindMyMouseTrigger();
            break;
        case IDM_EDIT_SETTINGS:
            EnsureSettingsFile(g_settingsPath);
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + g_settingsPath + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            break;
        case IDM_RELOAD:
            ReloadSettings();
            break;
        case IDM_AUTOSTART:
            SetAutostart(!IsAutostartEnabled());
            break;
        case IDM_EXIT:
            ExitApp();
            break;
        }
    }

    LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_TRAY)
        {
            switch (LOWORD(lParam))
            {
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
            case WM_LBUTTONUP:
                ShowTrayMenu();
                break;
            }
            return 0;
        }
        if (message == WM_HOTKEY)
        {
            if (wParam == HOTKEY_ID)
            {
                FindMyMouseTrigger();
            }
            return 0;
        }
        if (g_wmTaskbarCreated != 0 && message == g_wmTaskbarCreated)
        {
            // Explorer restarted: re-add the icon.
            UpdateTrayIcon(NIM_ADD);
            return 0;
        }
        if (g_wmCommand != 0 && message == g_wmCommand)
        {
            switch (wParam)
            {
            case CMD_TRIGGER:
                if (g_enabled)
                    FindMyMouseTrigger();
                break;
            case CMD_EXIT:
                ExitApp();
                break;
            case CMD_RELOAD:
                ReloadSettings();
                break;
            }
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // Returns true if a command was forwarded to the already running instance.
    bool ForwardToRunningInstance(WPARAM cmd)
    {
        HWND existing = FindWindowW(TRAY_CLASS, nullptr);
        if (!existing)
        {
            return false;
        }
        if (cmd != 0)
        {
            PostMessageW(existing, g_wmCommand, cmd, 0);
        }
        return true;
    }
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    g_hinst = hInstance;
    g_wmCommand = RegisterWindowMessageW(L"FindMyMouseStandalone.Command");
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // Command line: --trigger | --exit | --reload
    WPARAM cmd = 0;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if (arg == L"--trigger" || arg == L"/trigger")
            cmd = CMD_TRIGGER;
        else if (arg == L"--exit" || arg == L"/exit")
            cmd = CMD_EXIT;
        else if (arg == L"--reload" || arg == L"/reload")
            cmd = CMD_RELOAD;
    }
    if (argv)
    {
        LocalFree(argv);
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // Another instance is running: forward the command (if any) and quit.
        ForwardToRunningInstance(cmd);
        if (mutex)
            CloseHandle(mutex);
        return 0;
    }
    if (cmd == CMD_EXIT || cmd == CMD_RELOAD)
    {
        // Nothing running to talk to.
        if (mutex)
            CloseHandle(mutex);
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_settingsPath = GetSettingsPath();
    EnsureSettingsFile(g_settingsPath);
    g_settings = LoadSettings(g_settingsPath);

    WNDCLASSW wc{};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = TRAY_CLASS;
    RegisterClassW(&wc);
    // A top-level (not message-only) hidden window, so it receives the TaskbarCreated broadcast.
    g_trayHwnd = CreateWindowExW(WS_EX_TOOLWINDOW, TRAY_CLASS, L"Find My Mouse", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!g_trayHwnd)
    {
        MessageBoxW(nullptr, L"無法建立視窗。", L"Find My Mouse", MB_ICONERROR);
        return 1;
    }

    if (!FindMyMouseInitialize(hInstance, g_settings))
    {
        MessageBoxW(nullptr, L"無法初始化 Find My Mouse。\n需要 Windows 10 1903 (build 18362) 或更新版本。", L"Find My Mouse", MB_ICONERROR);
        DestroyWindow(g_trayHwnd);
        return 1;
    }

    UpdateTrayIcon(NIM_ADD);
    UpdateHotkey();

    if (cmd == CMD_TRIGGER)
    {
        FindMyMouseTrigger();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex)
    {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return static_cast<int>(msg.wParam);
}
