// FindMyMouse.cpp : Based on Raymond Chen's SuperSonar.cpp
//
// Standalone port of the PowerToys "Find My Mouse" module
// (https://github.com/microsoft/PowerToys/tree/main/src/modules/MouseUtils/FindMyMouse).
// Changes from upstream:
//   * Uses system Windows.UI.Composition + DesktopWindowTarget instead of WinAppSDK / XAML islands,
//     so no extra runtime has to be installed.
//   * PowerToys logger / telemetry / settings dependencies removed.
#include "pch.h"
#include "FindMyMouse.h"

namespace winrt
{
    using namespace winrt::Windows::System;
    using namespace winrt::Windows::UI::Composition;
    using namespace winrt::Windows::UI::Composition::Desktop;
}

namespace ABI
{
    using namespace ABI::Windows::System;
    using namespace ABI::Windows::UI::Composition::Desktop;
}

namespace
{
    // Private messages handled by the sonar window.
    constexpr UINT WM_PRIV_SHORTCUT = WM_APP + 1;

    inline bool detect_game_mode()
    {
        QUERY_USER_NOTIFICATION_STATE notification_state;
        if (SHQueryUserNotificationState(&notification_state) != S_OK)
        {
            return false;
        }
        return (notification_state == QUNS_RUNNING_D3D_FULL_SCREEN);
    }

    inline bool GetAnimationsEnabled()
    {
        BOOL enabled = TRUE;
        if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
        {
            return true;
        }
        return enabled != FALSE;
    }

    inline std::wstring get_process_path(DWORD pid) noexcept
    {
        std::wstring name;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process != nullptr)
        {
            name.resize(MAX_PATH);
            DWORD name_length = static_cast<DWORD>(name.length());
            if (QueryFullProcessImageNameW(process, 0, name.data(), &name_length) == 0)
            {
                name_length = 0;
            }
            name.resize(name_length);
            CloseHandle(process);
        }
        return name;
    }

    // Get the executable path, or the real module for UWP apps hosted in ApplicationFrameHost.
    inline std::wstring get_process_path(HWND window) noexcept
    {
        const static std::wstring app_frame_host = L"ApplicationFrameHost.exe";

        DWORD pid{};
        GetWindowThreadProcessId(window, &pid);
        auto name = get_process_path(pid);

        if (name.length() >= app_frame_host.length() &&
            name.compare(name.length() - app_frame_host.length(), app_frame_host.length(), app_frame_host) == 0)
        {
            DWORD new_pid = pid;
            EnumChildWindows(
                window, [](HWND hwnd, LPARAM param) -> BOOL {
                    auto new_pid_ptr = reinterpret_cast<DWORD*>(param);
                    DWORD childPid;
                    GetWindowThreadProcessId(hwnd, &childPid);
                    if (childPid != *new_pid_ptr)
                    {
                        *new_pid_ptr = childPid;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&new_pid));

            if (new_pid != pid)
            {
                return get_process_path(new_pid);
            }
        }
        return name;
    }

    // Checks if a process path is included in a list of strings.
    inline bool find_app_name_in_path(const std::wstring& where, const std::vector<std::wstring>& what)
    {
        for (const auto& row : what)
        {
            const auto pos = where.rfind(row);
            const auto last_slash = where.rfind('\\');
            if (pos != std::wstring::npos && pos <= last_slash + 1 && pos + row.length() > last_slash)
            {
                return true;
            }
        }
        return false;
    }

    inline bool check_excluded_app_with_title(HWND hwnd, const std::vector<std::wstring>& excludedApps)
    {
        WCHAR title[256];
        int len = GetWindowTextW(hwnd, title, ARRAYSIZE(title));
        if (len <= 0)
        {
            return false;
        }
        std::wstring titleStr(title, len);
        CharUpperBuffW(titleStr.data(), static_cast<DWORD>(titleStr.length()));
        for (const auto& app : excludedApps)
        {
            if (titleStr.find(app) != std::wstring::npos)
            {
                return true;
            }
        }
        return false;
    }

    inline bool check_excluded_app(HWND hwnd, const std::wstring& processPath, const std::vector<std::wstring>& excludedApps)
    {
        return find_app_name_in_path(processPath, excludedApps) || check_excluded_app_with_title(hwnd, excludedApps);
    }
}

#pragma region Super_Sonar_Base_Code

template<typename D>
struct SuperSonar
{
    bool Initialize(HINSTANCE hinst);
    void Terminate();

protected:
    // You are expected to override these, as appropriate.

    DWORD GetExtendedStyle()
    {
        return 0;
    }

    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        return BaseWndProc(message, wParam, lParam);
    }

    void BeforeMoveSonar() {}
    void AfterMoveSonar() {}
    void SetSonarVisibility(bool visible) = delete;
    void UpdateMouseSnooping();
    bool IsForegroundAppExcluded();

protected:
    // Base class members you can access.
    D* Shim() { return static_cast<D*>(this); }
    LRESULT BaseWndProc(UINT message, WPARAM wParam, LPARAM lParam) noexcept;

    HWND m_hwnd{};
    POINT m_sonarPos = ptNowhere;

    // Only consider double left control click if at least 100ms passed between the clicks, to avoid keyboards that might be sending rapid clicks.
    // At actual check, time a fifth of the current double click setting might be used instead to take into account users who might have low values.
    static const int MIN_DOUBLE_CLICK_TIME = 100;

    bool m_destroyed = false;
public:
    bool m_paused = false;
protected:
    FindMyMouseActivationMethod m_activationMethod = FIND_MY_MOUSE_DEFAULT_ACTIVATION_METHOD;
    bool m_includeWinKey = FIND_MY_MOUSE_DEFAULT_INCLUDE_WIN_KEY;
    bool m_doNotActivateOnGameMode = FIND_MY_MOUSE_DEFAULT_DO_NOT_ACTIVATE_ON_GAME_MODE;
    int m_sonarRadius = FIND_MY_MOUSE_DEFAULT_SPOTLIGHT_RADIUS;
    int m_sonarZoomFactor = FIND_MY_MOUSE_DEFAULT_SPOTLIGHT_INITIAL_ZOOM;
    DWORD m_fadeDuration = FIND_MY_MOUSE_DEFAULT_ANIMATION_DURATION_MS;
    std::vector<std::wstring> m_excludedApps;
    int m_shakeMinimumDistance = FIND_MY_MOUSE_DEFAULT_SHAKE_MINIMUM_DISTANCE;
    UINT m_hotkeyVk = 'F';

    // Don't consider movements started past these milliseconds to detect shaking.
    int m_shakeIntervalMs = FIND_MY_MOUSE_DEFAULT_SHAKE_INTERVAL_MS;
    // By which factor must travelled distance be than the diagonal of the rectangle containing the movements. (value in percent)
    int m_shakeFactor = FIND_MY_MOUSE_DEFAULT_SHAKE_FACTOR;

private:
    // Save the mouse movement that occurred in any direction.
    struct PointerRecentMovement
    {
        POINT diff;
        ULONGLONG tick;
    };
    std::vector<PointerRecentMovement> m_movementHistory;
    // Raw Input may give relative or absolute values. Need to take each case into account.
    bool m_seenAnAbsoluteMousePosition = false;
    POINT m_lastAbsolutePosition = { 0, 0 };

    static inline int GetSign(LONG const& num)
    {
        if (num > 0)
            return 1;
        if (num < 0)
            return -1;
        return 0;
    }

    static bool IsEqual(POINT const& p1, POINT const& p2)
    {
        return p1.x == p2.x && p1.y == p2.y;
    }

    static constexpr POINT ptNowhere = { LONG_MIN, LONG_MIN };
    static constexpr UINT_PTR TIMER_ID_TRACK = 100;
    static constexpr DWORD IdlePeriod = 1000;

    // Activate sonar: Hit LeftControl twice.
    enum class SonarState
    {
        Idle,
        ControlDown1,
        ControlUp1,
        ControlDown2,
        ControlUp2,
    };

    HWND m_hwndOwner{};
    SonarState m_sonarState = SonarState::Idle;
    POINT m_lastKeyPos{};
    ULONGLONG m_lastKeyTime{};

    static constexpr DWORD NoSonar = 0;
    static constexpr DWORD SonarWaitingForMouseMove = 1;
    ULONGLONG m_sonarStart = NoSonar;
    bool m_isSnoopingMouse = false;

private:
    static constexpr auto className = L"FindMyMouseStandaloneSonar";

    static constexpr auto windowTitle = L"Find My Mouse";

    static LRESULT CALLBACK s_WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    BOOL OnSonarCreate();
    void OnSonarDestroy();
    void OnSonarInput(WPARAM flags, HRAWINPUT hInput);
    void OnSonarKeyboardInput(RAWINPUT const& input);
    void OnSonarMouseInput(RAWINPUT const& input);
    void OnMouseTimer();

    void DetectShake();
    bool KeyboardInputCanActivate();

    void StartSonar(FindMyMouseActivationMethod activationMethod);
    void StopSonar();
};

template<typename D>
bool SuperSonar<D>::Initialize(HINSTANCE hinst)
{
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc{};
    if (!GetClassInfoW(hinst, className, &wc))
    {
        wc.lpfnWndProc = s_WndProc;
        wc.hInstance = hinst;
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
        wc.lpszClassName = className;

        if (!RegisterClassW(&wc))
        {
            LogError(L"RegisterClassW failed. GetLastError=%lu", GetLastError());
            return false;
        }
    }

    m_hwndOwner = CreateWindowW(L"static", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hinst, nullptr);
    if (!m_hwndOwner)
    {
        LogError(L"Failed to create owner window. GetLastError=%lu", GetLastError());
        return false;
    }

    DWORD exStyle = WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | Shim()->GetExtendedStyle();
    HWND created = CreateWindowExW(exStyle, className, windowTitle, WS_POPUP, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, m_hwndOwner, nullptr, hinst, this);
    if (!created)
    {
        LogError(L"CreateWindowExW failed. GetLastError=%lu", GetLastError());
        DestroyWindow(m_hwndOwner);
        m_hwndOwner = nullptr;
        return false;
    }

    return true;
}

template<typename D>
void SuperSonar<D>::Terminate()
{
    if (!m_destroyed)
    {
        m_destroyed = true;
        // Destroying the owner also destroys the sonar window, which posts WM_QUIT.
        DestroyWindow(m_hwndOwner);
    }
}

template<typename D>
LRESULT SuperSonar<D>::s_WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    SuperSonar* self;
    if (message == WM_NCCREATE)
    {
        auto info = reinterpret_cast<LPCREATESTRUCT>(lParam);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(info->lpCreateParams));
        self = static_cast<SuperSonar*>(info->lpCreateParams);
        self->m_hwnd = hwnd;
    }
    else
    {
        self = reinterpret_cast<SuperSonar*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (self)
    {
        return self->Shim()->WndProc(message, wParam, lParam);
    }
    else
    {
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
}

template<typename D>
LRESULT SuperSonar<D>::BaseWndProc(UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
    switch (message)
    {
    case WM_CREATE:
        if (!OnSonarCreate())
            return -1;
        UpdateMouseSnooping();
        return 0;

    case WM_DESTROY:
        OnSonarDestroy();
        break;

    case WM_INPUT:
        OnSonarInput(wParam, reinterpret_cast<HRAWINPUT>(lParam));
        break;

    case WM_TIMER:
        switch (wParam)
        {
        case TIMER_ID_TRACK:
            OnMouseTimer();
            break;
        }
        break;

    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_PRIV_SHORTCUT:
        if (m_sonarStart == NoSonar)
        {
            StartSonar(FindMyMouseActivationMethod::Shortcut);
        }
        else
        {
            StopSonar();
        }
        return 0;
    }

    return DefWindowProc(m_hwnd, message, wParam, lParam);
}

template<typename D>
BOOL SuperSonar<D>::OnSonarCreate()
{
    RAWINPUTDEVICE keyboard{};
    keyboard.usUsagePage = HID_USAGE_PAGE_GENERIC;
    keyboard.usUsage = HID_USAGE_GENERIC_KEYBOARD;
    keyboard.dwFlags = RIDEV_INPUTSINK;
    keyboard.hwndTarget = m_hwnd;
    return RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard));
}

template<typename D>
void SuperSonar<D>::OnSonarDestroy()
{
    PostQuitMessage(0);
}

template<typename D>
void SuperSonar<D>::OnSonarInput(WPARAM /*flags*/, HRAWINPUT hInput)
{
    RAWINPUT input;
    UINT size = sizeof(input);
    auto result = GetRawInputData(hInput, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
    if (result == static_cast<UINT>(-1) || result < sizeof(RAWINPUTHEADER))
    {
        return;
    }

    switch (input.header.dwType)
    {
    case RIM_TYPEKEYBOARD:
        OnSonarKeyboardInput(input);
        break;
    case RIM_TYPEMOUSE:
        OnSonarMouseInput(input);
        break;
    }
}

template<typename D>
void SuperSonar<D>::OnSonarKeyboardInput(RAWINPUT const& input)
{
    // Don't stop the sonar when the shortcut is released
    if (m_activationMethod == FindMyMouseActivationMethod::Shortcut && (input.data.keyboard.Flags & RI_KEY_BREAK) != 0)
    {
        return;
    }

    // The keys of the shortcut itself also arrive here (RegisterHotKey does not hide them from raw input).
    // Ignore them, otherwise they could close the spotlight the shortcut just opened.
    if (m_activationMethod == FindMyMouseActivationMethod::Shortcut)
    {
        switch (input.data.keyboard.VKey)
        {
        case VK_LWIN:
        case VK_RWIN:
        case VK_SHIFT:
        case VK_CONTROL:
        case VK_MENU:
            return;
        default:
            if (input.data.keyboard.VKey == m_hotkeyVk)
            {
                return;
            }
        }
    }

    if ((m_activationMethod != FindMyMouseActivationMethod::DoubleRightControlKey && m_activationMethod != FindMyMouseActivationMethod::DoubleLeftControlKey) || input.data.keyboard.VKey != VK_CONTROL)
    {
        StopSonar();
        return;
    }

    bool pressed = (input.data.keyboard.Flags & RI_KEY_BREAK) == 0;

    bool leftCtrlPressed = (input.data.keyboard.Flags & RI_KEY_E0) == 0;
    bool rightCtrlPressed = (input.data.keyboard.Flags & RI_KEY_E0) != 0;

    if ((m_activationMethod == FindMyMouseActivationMethod::DoubleRightControlKey && !rightCtrlPressed) || (m_activationMethod == FindMyMouseActivationMethod::DoubleLeftControlKey && !leftCtrlPressed))
    {
        StopSonar();
        return;
    }

    switch (m_sonarState)
    {
    case SonarState::Idle:
        if (pressed)
        {
            m_sonarState = SonarState::ControlDown1;
            m_lastKeyTime = GetTickCount64();
            m_lastKeyPos = {};
            GetCursorPos(&m_lastKeyPos);
            UpdateMouseSnooping();
        }
        break;

    case SonarState::ControlDown1:
        if (!pressed)
        {
            m_sonarState = SonarState::ControlUp1;
        }
        break;

    case SonarState::ControlUp1:
        if (pressed && KeyboardInputCanActivate())
        {
            auto now = GetTickCount64();
            auto doubleClickInterval = now - m_lastKeyTime;
            POINT ptCursor{};
            auto doubleClickTimeSetting = GetDoubleClickTime();
            if (GetCursorPos(&ptCursor) &&
                doubleClickInterval >= (std::min)(static_cast<UINT>(MIN_DOUBLE_CLICK_TIME), doubleClickTimeSetting / 5) &&
                doubleClickInterval <= doubleClickTimeSetting &&
                IsEqual(m_lastKeyPos, ptCursor))
            {
                m_sonarState = SonarState::ControlDown2;
                StartSonar(m_activationMethod);
            }
            else
            {
                m_sonarState = SonarState::ControlDown1;
                m_lastKeyTime = GetTickCount64();
                m_lastKeyPos = {};
                GetCursorPos(&m_lastKeyPos);
                UpdateMouseSnooping();
            }
            m_lastKeyTime = now;
            m_lastKeyPos = ptCursor;
        }
        break;
    case SonarState::ControlUp2:
        // Also deactivate sonar with left control.
        if (pressed)
        {
            StopSonar();
        }
        break;
    case SonarState::ControlDown2:
        if (!pressed)
        {
            m_sonarState = SonarState::ControlUp2;
        }
        break;
    }
}

// Shaking detection algorithm is: Has distance travelled been much greater than the diagonal of the rectangle containing the movement?
template<typename D>
void SuperSonar<D>::DetectShake()
{
    ULONGLONG shakeStartTick = GetTickCount64() - m_shakeIntervalMs;

    // Prune the story of movements for those movements that started too long ago.
    std::erase_if(m_movementHistory, [shakeStartTick](const PointerRecentMovement& movement) { return movement.tick < shakeStartTick; });

    double distanceTravelled = 0;
    LONGLONG currentX = 0, minX = 0, maxX = 0;
    LONGLONG currentY = 0, minY = 0, maxY = 0;

    for (const PointerRecentMovement& movement : m_movementHistory)
    {
        currentX += movement.diff.x;
        currentY += movement.diff.y;
        distanceTravelled += sqrt(static_cast<double>(movement.diff.x) * movement.diff.x + static_cast<double>(movement.diff.y) * movement.diff.y); // Pythagorean theorem
        minX = (std::min)(currentX, minX);
        maxX = (std::max)(currentX, maxX);
        minY = (std::min)(currentY, minY);
        maxY = (std::max)(currentY, maxY);
    }

    if (distanceTravelled < m_shakeMinimumDistance)
    {
        return;
    }

    // Size of the rectangle that the pointer moved in.
    double rectangleWidth = static_cast<double>(maxX) - minX;
    double rectangleHeight = static_cast<double>(maxY) - minY;

    double diagonal = sqrt(rectangleWidth * rectangleWidth + rectangleHeight * rectangleHeight);
    if (diagonal > 0 && distanceTravelled / diagonal > (m_shakeFactor / 100.f))
    {
        m_movementHistory.clear();
        StartSonar(m_activationMethod);
    }
}

template<typename D>
bool SuperSonar<D>::KeyboardInputCanActivate()
{
    return !m_includeWinKey || (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
}

template<typename D>
void SuperSonar<D>::OnSonarMouseInput(RAWINPUT const& input)
{
    if (m_activationMethod == FindMyMouseActivationMethod::ShakeMouse)
    {
        LONG relativeX = 0;
        LONG relativeY = 0;
        if ((input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == MOUSE_MOVE_ABSOLUTE && (input.data.mouse.lLastX != 0 || input.data.mouse.lLastY != 0))
        {
            // Getting absolute mouse coordinates. Likely inside a VM / RDP session.
            if (m_seenAnAbsoluteMousePosition)
            {
                relativeX = input.data.mouse.lLastX - m_lastAbsolutePosition.x;
                relativeY = input.data.mouse.lLastY - m_lastAbsolutePosition.y;
            }
            m_lastAbsolutePosition.x = input.data.mouse.lLastX;
            m_lastAbsolutePosition.y = input.data.mouse.lLastY;
            m_seenAnAbsoluteMousePosition = true;
        }
        else
        {
            relativeX = input.data.mouse.lLastX;
            relativeY = input.data.mouse.lLastY;
        }
        if (m_movementHistory.size() > 0)
        {
            PointerRecentMovement& lastMovement = m_movementHistory.back();
            // If the pointer is still moving in the same direction, just add to that movement instead of adding a new movement.
            // This helps in keeping the list of movements smaller even in cases where a high number of messages is sent.
            if (GetSign(lastMovement.diff.x) == GetSign(relativeX) && GetSign(lastMovement.diff.y) == GetSign(relativeY))
            {
                lastMovement.diff.x += relativeX;
                lastMovement.diff.y += relativeY;
            }
            else
            {
                m_movementHistory.push_back({ .diff = { .x = relativeX, .y = relativeY }, .tick = GetTickCount64() });
                // Mouse movement changed directions. Take the opportunity do detect shake.
                DetectShake();
            }
        }
        else
        {
            m_movementHistory.push_back({ .diff = { .x = relativeX, .y = relativeY }, .tick = GetTickCount64() });
        }
    }

    if (input.data.mouse.usButtonFlags)
    {
        StopSonar();
    }
    else if (m_sonarStart != NoSonar)
    {
        OnMouseTimer();
    }
}

template<typename D>
void SuperSonar<D>::StartSonar(FindMyMouseActivationMethod /*activationMethod*/)
{
    if (m_paused)
    {
        return;
    }

    // Don't activate if game mode is on.
    if (m_doNotActivateOnGameMode && detect_game_mode())
    {
        return;
    }

    if (IsForegroundAppExcluded())
    {
        return;
    }

    // Cover the entire virtual screen.
    // HACK: Draw with 1 pixel off. Otherwise, Windows glitches the task bar transparency when a transparent window fill the whole screen.
    SetWindowPos(m_hwnd, HWND_TOPMOST, GetSystemMetrics(SM_XVIRTUALSCREEN) + 1, GetSystemMetrics(SM_YVIRTUALSCREEN) + 1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 2, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 2, 0);
    m_sonarPos = ptNowhere;
    OnMouseTimer();
    UpdateMouseSnooping();
    Shim()->SetSonarVisibility(true);
}

template<typename D>
void SuperSonar<D>::StopSonar()
{
    if (m_sonarStart != NoSonar)
    {
        m_sonarStart = NoSonar;
        Shim()->SetSonarVisibility(false);
        KillTimer(m_hwnd, TIMER_ID_TRACK);
    }
    m_sonarState = SonarState::Idle;
    UpdateMouseSnooping();
}

template<typename D>
void SuperSonar<D>::OnMouseTimer()
{
    auto now = GetTickCount64();

    // If mouse has moved, then reset the sonar timer.
    POINT ptCursor{};
    if (!GetCursorPos(&ptCursor))
    {
        // We are no longer the active desktop - done.
        StopSonar();
        return;
    }
    ScreenToClient(m_hwnd, &ptCursor);

    if (IsEqual(m_sonarPos, ptCursor))
    {
        // Mouse is stationary.
        if (m_sonarStart != SonarWaitingForMouseMove && now - m_sonarStart >= IdlePeriod)
        {
            StopSonar();
            return;
        }
    }
    else
    {
        // Mouse has moved.
        if (IsEqual(m_sonarPos, ptNowhere))
        {
            // Initial call, mark sonar as active but waiting for first mouse-move.
            now = SonarWaitingForMouseMove;
        }
        SetTimer(m_hwnd, TIMER_ID_TRACK, IdlePeriod, nullptr);
        Shim()->BeforeMoveSonar();
        m_sonarPos = ptCursor;
        m_sonarStart = now;
        Shim()->AfterMoveSonar();
    }
}

template<typename D>
void SuperSonar<D>::UpdateMouseSnooping()
{
    bool wantSnoopingMouse = m_sonarStart != NoSonar || m_sonarState != SonarState::Idle || m_activationMethod == FindMyMouseActivationMethod::ShakeMouse;
    if (m_isSnoopingMouse != wantSnoopingMouse)
    {
        m_isSnoopingMouse = wantSnoopingMouse;
        RAWINPUTDEVICE mouse{};
        mouse.usUsagePage = HID_USAGE_PAGE_GENERIC;
        mouse.usUsage = HID_USAGE_GENERIC_MOUSE;
        if (wantSnoopingMouse)
        {
            mouse.dwFlags = RIDEV_INPUTSINK;
            mouse.hwndTarget = m_hwnd;
        }
        else
        {
            mouse.dwFlags = RIDEV_REMOVE;
            mouse.hwndTarget = nullptr;
        }
        RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    }
}

template<typename D>
bool SuperSonar<D>::IsForegroundAppExcluded()
{
    if (m_excludedApps.size() < 1)
    {
        return false;
    }
    if (HWND foregroundApp{ GetForegroundWindow() })
    {
        auto processPath = get_process_path(foregroundApp);
        CharUpperBuffW(processPath.data(), static_cast<DWORD>(processPath.length()));

        return check_excluded_app(foregroundApp, processPath, m_excludedApps);
    }
    return false;
}

struct CompositionSpotlight : SuperSonar<CompositionSpotlight>
{
    static constexpr UINT WM_OPACITY_ANIMATION_COMPLETED = WM_APP;
    float m_sonarRadiusFloat = static_cast<float>(m_sonarRadius);

    DWORD GetExtendedStyle()
    {
        // DesktopWindowTarget draws directly through DWM; no GDI redirection surface needed.
        return WS_EX_NOREDIRECTIONBITMAP;
    }

    void AfterMoveSonar()
    {
        const winrt::Windows::Foundation::Numerics::float2 pos{ static_cast<float>(m_sonarPos.x), static_cast<float>(m_sonarPos.y) };
        // Move gradient center
        if (m_spotlightMaskGradient)
        {
            m_spotlightMaskGradient.EllipseCenter(pos);
        }
        // Move spotlight visual (color fill) below masked backdrop
        if (m_spotlight)
        {
            m_spotlight.Offset({ pos.x, pos.y, 0.0f });
        }
    }

    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        switch (message)
        {
        case WM_CREATE:
            if (!OnCompositionCreate())
                return -1;
            return BaseWndProc(message, wParam, lParam);

        case WM_OPACITY_ANIMATION_COMPLETED:
            OnOpacityAnimationCompleted();
            break;
        }
        return BaseWndProc(message, wParam, lParam);
    }

    void SetSonarVisibility(bool visible)
    {
        m_batch = m_compositor.GetCommitBatch(winrt::CompositionBatchTypes::Animation);
        bool isEnabledAnimations = GetAnimationsEnabled();
        m_animation.Duration(std::chrono::milliseconds{ isEnabledAnimations ? m_fadeDuration : 1 });
        m_batch.Completed([hwnd = m_hwnd](auto&&, auto&&) {
            PostMessage(hwnd, WM_OPACITY_ANIMATION_COMPLETED, 0, 0);
        });
        m_root.Opacity(visible ? 1.0f : 0.0f);
        if (visible)
        {
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        }
    }

    HWND GetHwnd() noexcept
    {
        return m_hwnd;
    }

private:
    bool OnCompositionCreate()
    try
    {
        // A layered window must have its attributes set to be shown at all.
        SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);

        // 1) Windows.UI.Composition needs a DispatcherQueue on this thread.
        if (!m_dispatcherQueueController)
        {
            DispatcherQueueOptions options{
                sizeof(DispatcherQueueOptions),
                DQTYPE_THREAD_CURRENT,
                DQTAT_COM_NONE,
            };
            ABI::IDispatcherQueueController* controller = nullptr;
            winrt::check_hresult(CreateDispatcherQueueController(options, &controller));
            *winrt::put_abi(m_dispatcherQueueController) = controller;
        }

        // 2) Compositor + desktop window target for our HWND.
        m_compositor = winrt::Compositor();
        ABI::IDesktopWindowTarget* target = nullptr;
        winrt::check_hresult(m_compositor.as<ABI::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(m_hwnd, false, &target));
        *winrt::put_abi(m_target) = target;

        // 3) Build the composition tree
        //
        // [root] ContainerVisual (fills host)
        //  \ LayerVisual
        //     \ [spotlight circle]
        //     \ [backdrop dim * radial gradient mask (hole)]
        m_root = m_compositor.CreateContainerVisual();
        m_root.RelativeSizeAdjustment({ 1.0f, 1.0f });
        m_root.Opacity(0.0f);
        m_target.Root(m_root);

        auto layer = m_compositor.CreateLayerVisual();
        layer.RelativeSizeAdjustment({ 1.0f, 1.0f });
        m_root.Children().InsertAtTop(layer);

        const float r = m_sonarRadiusFloat;
        const float zoom = static_cast<float>(m_sonarZoomFactor);

        // Spotlight shape (below backdrop, visible through hole)
        m_circleGeometry = m_compositor.CreateEllipseGeometry();
        m_circleShape = m_compositor.CreateSpriteShape(m_circleGeometry);
        m_circleShape.FillBrush(m_compositor.CreateColorBrush(m_spotlightColor));
        m_circleShape.Offset({ r * zoom, r * zoom });
        m_spotlight = m_compositor.CreateShapeVisual();
        m_spotlight.Size({ r * 2 * zoom, r * 2 * zoom });
        m_spotlight.AnchorPoint({ 0.5f, 0.5f });
        m_spotlight.Shapes().Append(m_circleShape);
        layer.Children().InsertAtTop(m_spotlight);

        // Dim color (source)
        m_dimColorBrush = m_compositor.CreateColorBrush(m_backgroundColor);
        // Radial gradient mask (center transparent, outer opaque)
        // Fixed feather width: 1px for radius < 300, 2px for radius >= 300
        const float featherPixels = (m_sonarRadius >= 300) ? 2.0f : 1.0f;
        const float featherOffset = 1.0f - featherPixels / (std::max)(r * zoom, 1.0f);
        m_spotlightMaskGradient = m_compositor.CreateRadialGradientBrush();
        m_spotlightMaskGradient.MappingMode(winrt::CompositionMappingMode::Absolute);
        m_maskStopCenter = m_compositor.CreateColorGradientStop();
        m_maskStopCenter.Offset(0.0f);
        m_maskStopCenter.Color(winrt::Windows::UI::Color{ 0, 0, 0, 0 });
        m_maskStopInner = m_compositor.CreateColorGradientStop();
        m_maskStopInner.Offset(featherOffset);
        m_maskStopInner.Color(winrt::Windows::UI::Color{ 0, 0, 0, 0 });
        m_maskStopOuter = m_compositor.CreateColorGradientStop();
        m_maskStopOuter.Offset(1.0f);
        m_maskStopOuter.Color(winrt::Windows::UI::Color{ 255, 255, 255, 255 });
        m_spotlightMaskGradient.ColorStops().Append(m_maskStopCenter);
        m_spotlightMaskGradient.ColorStops().Append(m_maskStopInner);
        m_spotlightMaskGradient.ColorStops().Append(m_maskStopOuter);
        m_spotlightMaskGradient.EllipseCenter({ r * zoom, r * zoom });
        m_spotlightMaskGradient.EllipseRadius({ r * zoom, r * zoom });

        m_maskBrush = m_compositor.CreateMaskBrush();
        m_maskBrush.Source(m_dimColorBrush);
        m_maskBrush.Mask(m_spotlightMaskGradient);

        m_backdrop = m_compositor.CreateSpriteVisual();
        m_backdrop.RelativeSizeAdjustment({ 1.0f, 1.0f });
        m_backdrop.Brush(m_maskBrush);
        layer.Children().InsertAtTop(m_backdrop);

        // 4) Implicit opacity animation on the root
        m_animation = m_compositor.CreateScalarKeyFrameAnimation();
        m_animation.Target(L"Opacity");
        m_animation.InsertExpressionKeyFrame(1.0f, L"this.FinalValue");
        m_animation.Duration(std::chrono::milliseconds{ m_fadeDuration });
        auto collection = m_compositor.CreateImplicitAnimationCollection();
        collection.Insert(L"Opacity", m_animation);
        m_root.ImplicitAnimations(collection);

        // 5) Spotlight radius shrinks as opacity increases (expression animation)
        SetupRadiusAnimations(r * zoom, r, featherPixels);

        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        LogError(L"Failed to create FindMyMouse visual: 0x%08X %s", static_cast<unsigned>(e.code()), e.message().c_str());
        return false;
    }

    void OnOpacityAnimationCompleted()
    {
        if (m_root.Opacity() < 0.01f)
        {
            ShowWindow(m_hwnd, SW_HIDE);
        }
    }

    // Helper to setup radius and feather expression animations
    void SetupRadiusAnimations(float startRadius, float endRadius, float featherPixels)
    {
        // Radius expression: shrinks from startRadius to endRadius as opacity goes 0->1
        auto radiusExpression = m_compositor.CreateExpressionAnimation();
        radiusExpression.SetReferenceParameter(L"Root", m_root);
        wchar_t expressionText[256];
        winrt::check_hresult(StringCchPrintfW(
            expressionText, ARRAYSIZE(expressionText),
            L"Lerp(Vector2(%.1f, %.1f), Vector2(%.1f, %.1f), Root.Opacity)",
            startRadius, startRadius, endRadius, endRadius));
        radiusExpression.Expression(expressionText);
        m_spotlightMaskGradient.StartAnimation(L"EllipseRadius", radiusExpression);

        // Feather expression: maintains fixed pixel width as radius changes
        auto featherExpression = m_compositor.CreateExpressionAnimation();
        featherExpression.SetReferenceParameter(L"Root", m_root);
        wchar_t featherExpressionText[256];
        winrt::check_hresult(StringCchPrintfW(
            featherExpressionText, ARRAYSIZE(featherExpressionText),
            L"1.0f - %.1ff / Lerp(%.1ff, %.1ff, Root.Opacity)",
            featherPixels, startRadius, endRadius));
        featherExpression.Expression(featherExpressionText);
        m_maskStopInner.StartAnimation(L"Offset", featherExpression);

        // Circle geometry radius for visual consistency
        if (m_circleGeometry)
        {
            auto radiusExpression2 = m_compositor.CreateExpressionAnimation();
            radiusExpression2.SetReferenceParameter(L"Root", m_root);
            radiusExpression2.Expression(expressionText);
            m_circleGeometry.StartAnimation(L"Radius", radiusExpression2);
        }
    }

public:
    void ApplySettings(const FindMyMouseSettings& settings, bool applyToRuntimeObjects)
    {
        m_sonarRadius = settings.spotlightRadius;
        m_sonarRadiusFloat = static_cast<float>(m_sonarRadius);
        m_backgroundColor = settings.backgroundColor;
        m_spotlightColor = settings.spotlightColor;
        m_activationMethod = settings.activationMethod;
        m_includeWinKey = settings.includeWinKey;
        m_doNotActivateOnGameMode = settings.doNotActivateOnGameMode;
        m_fadeDuration = settings.animationDurationMs > 0 ? settings.animationDurationMs : 1;
        m_sonarZoomFactor = settings.spotlightInitialZoom;
        m_excludedApps = settings.excludedApps;
        m_shakeMinimumDistance = settings.shakeMinimumDistance;
        m_shakeIntervalMs = settings.shakeIntervalMs;
        m_shakeFactor = settings.shakeFactor;
        m_hotkeyVk = settings.hotkeyVk;

        if (!applyToRuntimeObjects || m_destroyed || !m_compositor)
        {
            return;
        }

        UpdateMouseSnooping(); // For the shake mouse activation method

        // Apply new settings to runtime composition objects.
        if (m_dimColorBrush)
        {
            m_dimColorBrush.Color(m_backgroundColor);
        }
        if (m_circleShape)
        {
            if (auto brush = m_circleShape.FillBrush().try_as<winrt::CompositionColorBrush>())
            {
                brush.Color(m_spotlightColor);
            }
        }
        const float r = m_sonarRadiusFloat;
        const float zoom = static_cast<float>(m_sonarZoomFactor);
        const float featherPixels = (m_sonarRadius >= 300) ? 2.0f : 1.0f;
        const float startRadius = r * zoom;
        m_spotlightMaskGradient.StopAnimation(L"EllipseRadius");
        m_maskStopInner.StopAnimation(L"Offset");
        if (m_circleGeometry)
        {
            m_circleGeometry.StopAnimation(L"Radius");
        }
        m_spotlightMaskGradient.EllipseCenter({ startRadius, startRadius });
        if (m_spotlight)
        {
            m_spotlight.Size({ r * 2 * zoom, r * 2 * zoom });
            m_circleShape.Offset({ startRadius, startRadius });
        }
        SetupRadiusAnimations(startRadius, r, featherPixels);
    }

private:
    winrt::DispatcherQueueController m_dispatcherQueueController{ nullptr };
    winrt::Compositor m_compositor{ nullptr };
    winrt::DesktopWindowTarget m_target{ nullptr };

    winrt::ContainerVisual m_root{ nullptr };
    winrt::CompositionCommitBatch m_batch{ nullptr };
    winrt::SpriteVisual m_backdrop{ nullptr };
    // Spotlight shape visuals
    winrt::CompositionEllipseGeometry m_circleGeometry{ nullptr };
    winrt::ShapeVisual m_spotlight{ nullptr };
    winrt::CompositionSpriteShape m_circleShape{ nullptr };
    // Radial gradient mask components
    winrt::CompositionMaskBrush m_maskBrush{ nullptr };
    winrt::CompositionColorBrush m_dimColorBrush{ nullptr };
    winrt::CompositionRadialGradientBrush m_spotlightMaskGradient{ nullptr };
    winrt::CompositionColorGradientStop m_maskStopCenter{ nullptr };
    winrt::CompositionColorGradientStop m_maskStopInner{ nullptr };
    winrt::CompositionColorGradientStop m_maskStopOuter{ nullptr };
    winrt::Windows::UI::Color m_backgroundColor = FIND_MY_MOUSE_DEFAULT_BACKGROUND_COLOR;
    winrt::Windows::UI::Color m_spotlightColor = FIND_MY_MOUSE_DEFAULT_SPOTLIGHT_COLOR;
    winrt::ScalarKeyFrameAnimation m_animation{ nullptr };
};

#pragma endregion Super_Sonar_Base_Code

#pragma region Super_Sonar_API

namespace
{
    CompositionSpotlight* g_sonar = nullptr;
}

bool FindMyMouseInitialize(HINSTANCE hinst, const FindMyMouseSettings& settings)
{
    if (g_sonar != nullptr)
    {
        return true;
    }

    auto sonar = new CompositionSpotlight();
    sonar->ApplySettings(settings, false);
    if (!sonar->Initialize(hinst))
    {
        LogError(L"Couldn't initialize a sonar instance.");
        delete sonar;
        return false;
    }
    // Intentionally kept alive until process exit: the window proc references it.
    g_sonar = sonar;
    return true;
}

void FindMyMouseTerminate()
{
    if (g_sonar != nullptr)
    {
        g_sonar->Terminate();
    }
}

void FindMyMouseApplySettings(const FindMyMouseSettings& settings)
{
    if (g_sonar != nullptr)
    {
        g_sonar->ApplySettings(settings, true);
    }
}

void FindMyMouseTrigger()
{
    if (g_sonar != nullptr && g_sonar->GetHwnd() != nullptr)
    {
        PostMessageW(g_sonar->GetHwnd(), WM_PRIV_SHORTCUT, 0, 0);
    }
}

void FindMyMouseSetPaused(bool paused)
{
    if (g_sonar != nullptr)
    {
        g_sonar->m_paused = paused;
    }
}

#pragma endregion Super_Sonar_API
