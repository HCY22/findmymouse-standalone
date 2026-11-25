#pragma once
#include "FindMyMouse.h"

// Path of the settings file: %APPDATA%\FindMyMouse\settings.ini
std::wstring GetSettingsPath();

// Creates the settings file with default values (and comments) if it does not exist yet.
void EnsureSettingsFile(const std::wstring& path);

// Loads settings from the ini file. Invalid / missing values fall back to defaults.
FindMyMouseSettings LoadSettings(const std::wstring& path);

// "Win+Shift+F" style text for the configured hotkey.
std::wstring HotkeyToString(UINT modifiers, UINT vk);
