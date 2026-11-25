#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <strsafe.h>
#include <hidusage.h>
#include <Unknwn.h>
#include <DispatcherQueue.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

// Minimal logging: visible in DebugView / Visual Studio output window.
template<typename... Args>
inline void LogError(const wchar_t* fmt, Args... args)
{
    wchar_t buffer[512];
    if (SUCCEEDED(StringCchPrintfW(buffer, ARRAYSIZE(buffer), fmt, args...)))
    {
        OutputDebugStringW(L"[FindMyMouse] ");
        OutputDebugStringW(buffer);
        OutputDebugStringW(L"\n");
    }
}
