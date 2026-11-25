#include "pch.h"
#include "Settings.h"

#include <shlobj.h>
#include <cstdlib>
#include <cwctype>

namespace
{
    constexpr const wchar_t* SECTION = L"FindMyMouse";

    constexpr const wchar_t* DEFAULT_SETTINGS_TEXT =
        L"; Find My Mouse (standalone) 設定檔\r\n"
        L"; 修改後請在系統匣圖示的選單中按「重新載入設定」，或重新啟動程式。\r\n"
        L"\r\n"
        L"[FindMyMouse]\r\n"
        L"; 啟動方式：\r\n"
        L";   DoubleLeftControl  = 連按兩下左 Ctrl（預設）\r\n"
        L";   DoubleRightControl = 連按兩下右 Ctrl\r\n"
        L";   ShakeMouse         = 搖晃滑鼠\r\n"
        L";   Shortcut           = 使用 ActivationShortcut 指定的快捷鍵\r\n"
        L"ActivationMethod=DoubleLeftControl\r\n"
        L"\r\n"
        L"; ActivationMethod=Shortcut 時使用的快捷鍵，例如 Win+Shift+F、Ctrl+Alt+M、Ctrl+Shift+F12\r\n"
        L"ActivationShortcut=Win+Shift+F\r\n"
        L"\r\n"
        L"; 連按 Ctrl 時，是否需要同時按住 Windows 鍵（1=是, 0=否）\r\n"
        L"IncludeWinKey=0\r\n"
        L"\r\n"
        L"; 全螢幕遊戲（遊戲模式）時不啟動（1=是, 0=否）\r\n"
        L"DoNotActivateOnGameMode=1\r\n"
        L"\r\n"
        L"; 顏色格式 #AARRGGBB（AA = 不透明度）或 #RRGGBB\r\n"
        L"BackgroundColor=#80000000\r\n"
        L"SpotlightColor=#80FFFFFF\r\n"
        L"\r\n"
        L"; 聚光燈半徑（像素）\r\n"
        L"SpotlightRadius=100\r\n"
        L"\r\n"
        L"; 動畫時間（毫秒）\r\n"
        L"AnimationDurationMs=500\r\n"
        L"\r\n"
        L"; 聚光燈初始縮放倍率\r\n"
        L"SpotlightInitialZoom=9\r\n"
        L"\r\n"
        L"; 搖晃偵測：最小移動距離（像素）、時間窗（毫秒）、倍率（百分比）\r\n"
        L"ShakeMinimumDistance=1000\r\n"
        L"ShakeIntervalMs=1000\r\n"
        L"ShakeFactor=400\r\n"
        L"\r\n"
        L"; 排除的應用程式（前景視窗為這些程式時不啟動），以分號分隔，可填執行檔名或視窗標題片段\r\n"
        L"; 例如：ExcludedApps=notepad.exe;vlc.exe\r\n"
        L"ExcludedApps=\r\n";

    std::wstring ReadString(const std::wstring& path, const wchar_t* key, const wchar_t* def)
    {
        std::wstring buffer(4096, L'\0');
        DWORD len = GetPrivateProfileStringW(SECTION, key, def, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
        buffer.resize(len);
        return buffer;
    }

    std::wstring Trim(std::wstring s)
    {
        const wchar_t* ws = L" \t\r\n";
        auto start = s.find_first_not_of(ws);
        if (start == std::wstring::npos)
        {
            return {};
        }
        auto end = s.find_last_not_of(ws);
        return s.substr(start, end - start + 1);
    }

    std::wstring Upper(std::wstring s)
    {
        if (!s.empty())
        {
            CharUpperBuffW(s.data(), static_cast<DWORD>(s.length()));
        }
        return s;
    }

    int ReadInt(const std::wstring& path, const wchar_t* key, int def, int minValue, int maxValue)
    {
        auto text = Trim(ReadString(path, key, L""));
        if (text.empty())
        {
            return def;
        }
        wchar_t* end = nullptr;
        long value = wcstol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end != L'\0' || value < minValue || value > maxValue)
        {
            return def;
        }
        return static_cast<int>(value);
    }

    bool ReadBool(const std::wstring& path, const wchar_t* key, bool def)
    {
        auto text = Upper(Trim(ReadString(path, key, L"")));
        if (text == L"1" || text == L"TRUE" || text == L"YES")
            return true;
        if (text == L"0" || text == L"FALSE" || text == L"NO")
            return false;
        return def;
    }

    bool ParseHexByte(const std::wstring& s, size_t pos, uint8_t& out)
    {
        unsigned value = 0;
        for (size_t i = pos; i < pos + 2; ++i)
        {
            wchar_t c = s[i];
            value <<= 4;
            if (c >= L'0' && c <= L'9')
                value |= c - L'0';
            else if (c >= L'A' && c <= L'F')
                value |= c - L'A' + 10;
            else if (c >= L'a' && c <= L'f')
                value |= c - L'a' + 10;
            else
                return false;
        }
        out = static_cast<uint8_t>(value);
        return true;
    }

    winrt::Windows::UI::Color ReadColor(const std::wstring& path, const wchar_t* key, winrt::Windows::UI::Color def)
    {
        auto text = Trim(ReadString(path, key, L""));
        if (!text.empty() && text[0] == L'#')
        {
            text.erase(0, 1);
        }
        uint8_t a = 255, r = 0, g = 0, b = 0;
        if (text.length() == 8 &&
            ParseHexByte(text, 0, a) && ParseHexByte(text, 2, r) && ParseHexByte(text, 4, g) && ParseHexByte(text, 6, b))
        {
            return { a, r, g, b };
        }
        if (text.length() == 6 &&
            ParseHexByte(text, 0, r) && ParseHexByte(text, 2, g) && ParseHexByte(text, 4, b))
        {
            return { 255, r, g, b };
        }
        return def;
    }

    struct NamedKey
    {
        const wchar_t* name;
        UINT vk;
    };

    constexpr NamedKey NAMED_KEYS[] = {
        { L"SPACE", VK_SPACE },
        { L"ENTER", VK_RETURN },
        { L"TAB", VK_TAB },
        { L"ESC", VK_ESCAPE },
        { L"BACKSPACE", VK_BACK },
        { L"INSERT", VK_INSERT },
        { L"DELETE", VK_DELETE },
        { L"HOME", VK_HOME },
        { L"END", VK_END },
        { L"PAGEUP", VK_PRIOR },
        { L"PAGEDOWN", VK_NEXT },
        { L"LEFT", VK_LEFT },
        { L"RIGHT", VK_RIGHT },
        { L"UP", VK_UP },
        { L"DOWN", VK_DOWN },
        { L"PAUSE", VK_PAUSE },
        { L"SCROLLLOCK", VK_SCROLL },
        { L"PRINTSCREEN", VK_SNAPSHOT },
    };

    bool ParseHotkey(const std::wstring& text, UINT& modifiers, UINT& vk)
    {
        UINT mods = 0;
        UINT key = 0;
        std::wstring upper = Upper(text);
        size_t start = 0;
        while (start <= upper.length())
        {
            size_t plus = upper.find(L'+', start);
            std::wstring token = Trim(upper.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start));
            start = (plus == std::wstring::npos) ? upper.length() + 1 : plus + 1;
            if (token.empty())
            {
                continue;
            }

            if (token == L"WIN" || token == L"WINDOWS")
                mods |= MOD_WIN;
            else if (token == L"CTRL" || token == L"CONTROL")
                mods |= MOD_CONTROL;
            else if (token == L"SHIFT")
                mods |= MOD_SHIFT;
            else if (token == L"ALT")
                mods |= MOD_ALT;
            else if (token.length() == 1 && ((token[0] >= L'A' && token[0] <= L'Z') || (token[0] >= L'0' && token[0] <= L'9')))
                key = token[0];
            else if (token.length() >= 2 && token[0] == L'F' && iswdigit(token[1]))
            {
                int n = _wtoi(token.c_str() + 1);
                if (n >= 1 && n <= 24)
                    key = VK_F1 + n - 1;
                else
                    return false;
            }
            else
            {
                bool found = false;
                for (const auto& named : NAMED_KEYS)
                {
                    if (token == named.name)
                    {
                        key = named.vk;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    return false;
            }
        }

        if (key == 0 || mods == 0)
        {
            return false;
        }
        modifiers = mods;
        vk = key;
        return true;
    }
}

std::wstring GetSettingsPath()
{
    PWSTR appData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
    {
        path = appData;
        CoTaskMemFree(appData);
    }
    else
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        path = exe;
        path = path.substr(0, path.find_last_of(L'\\'));
    }
    path += L"\\FindMyMouse";
    CreateDirectoryW(path.c_str(), nullptr);
    return path + L"\\settings.ini";
}

void EnsureSettingsFile(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        return;
    }

    // UTF-16 LE with BOM so that the *PrivateProfile* APIs read non-ASCII text correctly.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    WriteFile(file, DEFAULT_SETTINGS_TEXT, static_cast<DWORD>(wcslen(DEFAULT_SETTINGS_TEXT) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

FindMyMouseSettings LoadSettings(const std::wstring& path)
{
    FindMyMouseSettings s;

    auto method = Upper(Trim(ReadString(path, L"ActivationMethod", L"")));
    if (method == L"DOUBLELEFTCONTROL" || method == L"0")
        s.activationMethod = FindMyMouseActivationMethod::DoubleLeftControlKey;
    else if (method == L"DOUBLERIGHTCONTROL" || method == L"1")
        s.activationMethod = FindMyMouseActivationMethod::DoubleRightControlKey;
    else if (method == L"SHAKEMOUSE" || method == L"2")
        s.activationMethod = FindMyMouseActivationMethod::ShakeMouse;
    else if (method == L"SHORTCUT" || method == L"3")
        s.activationMethod = FindMyMouseActivationMethod::Shortcut;

    UINT mods = 0, vk = 0;
    if (ParseHotkey(ReadString(path, L"ActivationShortcut", L""), mods, vk))
    {
        s.hotkeyModifiers = mods;
        s.hotkeyVk = vk;
    }

    s.includeWinKey = ReadBool(path, L"IncludeWinKey", s.includeWinKey);
    s.doNotActivateOnGameMode = ReadBool(path, L"DoNotActivateOnGameMode", s.doNotActivateOnGameMode);
    s.backgroundColor = ReadColor(path, L"BackgroundColor", s.backgroundColor);
    s.spotlightColor = ReadColor(path, L"SpotlightColor", s.spotlightColor);
    s.spotlightRadius = ReadInt(path, L"SpotlightRadius", s.spotlightRadius, 1, 10000);
    s.animationDurationMs = ReadInt(path, L"AnimationDurationMs", s.animationDurationMs, 0, 60000);
    s.spotlightInitialZoom = ReadInt(path, L"SpotlightInitialZoom", s.spotlightInitialZoom, 1, 1000);
    s.shakeMinimumDistance = ReadInt(path, L"ShakeMinimumDistance", s.shakeMinimumDistance, 0, 1000000);
    s.shakeIntervalMs = ReadInt(path, L"ShakeIntervalMs", s.shakeIntervalMs, 0, 60000);
    s.shakeFactor = ReadInt(path, L"ShakeFactor", s.shakeFactor, 0, 100000);

    auto apps = Upper(ReadString(path, L"ExcludedApps", L""));
    size_t start = 0;
    while (start <= apps.length())
    {
        size_t sep = apps.find_first_of(L";,", start);
        auto item = Trim(apps.substr(start, sep == std::wstring::npos ? std::wstring::npos : sep - start));
        if (!item.empty())
        {
            s.excludedApps.push_back(item);
        }
        if (sep == std::wstring::npos)
            break;
        start = sep + 1;
    }

    return s;
}

std::wstring HotkeyToString(UINT modifiers, UINT vk)
{
    std::wstring text;
    if (modifiers & MOD_WIN)
        text += L"Win+";
    if (modifiers & MOD_CONTROL)
        text += L"Ctrl+";
    if (modifiers & MOD_ALT)
        text += L"Alt+";
    if (modifiers & MOD_SHIFT)
        text += L"Shift+";

    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
    {
        text += static_cast<wchar_t>(vk);
    }
    else if (vk >= VK_F1 && vk <= VK_F24)
    {
        text += L"F" + std::to_wstring(vk - VK_F1 + 1);
    }
    else
    {
        bool found = false;
        for (const auto& named : NAMED_KEYS)
        {
            if (named.vk == vk)
            {
                text += named.name;
                found = true;
                break;
            }
        }
        if (!found)
        {
            text += L"VK" + std::to_wstring(vk);
        }
    }
    return text;
}
