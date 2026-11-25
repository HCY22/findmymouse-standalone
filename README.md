# Find My Mouse（獨立版）

從 [Microsoft PowerToys](https://github.com/microsoft/PowerToys) 的 Mouse Utilities 中抽出的 **Find My Mouse**（尋找我的滑鼠），做成單一 `.exe`，不需要安裝 PowerToys。

觸發後畫面變暗，滑鼠游標周圍出現聚光燈；移動滑鼠時聚光燈會跟著走，滑鼠停住約 1 秒、按下滑鼠鍵或按其他鍵後消失。

## 和 PowerToys 版的差異

| 項目 | PowerToys 版 | 獨立版 |
| --- | --- | --- |
| 繪圖 | WinAppSDK + XAML Islands | 系統內建 `Windows.UI.Composition`（不需安裝額外的 runtime） |
| 設定 | PowerToys 設定 UI | `%APPDATA%\FindMyMouse\settings.ini` |
| 快捷鍵 | PowerToys runner 的鍵盤 hook | `RegisterHotKey` |
| 其他 | 遙測、記錄檔、GPO | 已移除；改為系統匣圖示 |

偵測邏輯（連按 Ctrl、搖晃滑鼠、遊戲模式、排除程式）和動畫參數都直接沿用上游程式碼，預設值也相同。

## 系統需求

- Windows 10 1903（build 18362）以上，或 Windows 11
- 編譯：Visual Studio 2022（含「使用 C++ 的桌面開發」）+ CMake 3.20 以上。Windows SDK 已內含 C++/WinRT 標頭，不需另外安裝 NuGet 套件。

## 編譯

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

產出：`build\Release\FindMyMouse.exe`（靜態連結 CRT，單檔即可執行）。ARM64 請把 `-A x64` 改成 `-A ARM64`。

也可以把這個資料夾推到 GitHub，`.github/workflows/build.yml` 會自動用 GitHub Actions 編譯 x64 / ARM64，在 Actions 頁面下載 artifact。

## 使用方式

1. 執行 `FindMyMouse.exe`，右下角系統匣會出現圖示。
2. 預設**連按兩下左 Ctrl** 會顯示聚光燈。
3. 按系統匣圖示可以：
   - 啟用／暫停
   - 立即顯示聚光燈
   - 編輯設定檔（用記事本開啟）
   - 重新載入設定
   - 開機時自動啟動（寫入 `HKCU\...\Run`）
   - 結束

### 命令列

```bat
FindMyMouse.exe --trigger   :: 顯示／隱藏聚光燈（程式沒在執行時會先啟動它）
FindMyMouse.exe --reload    :: 讓正在執行的程式重新讀取設定
FindMyMouse.exe --exit      :: 結束正在執行的程式
```

`--trigger` 可以綁在滑鼠驅動程式的自訂按鍵、AutoHotkey 或其他工具上。

## 設定檔

位置：`%APPDATA%\FindMyMouse\settings.ini`（第一次執行時自動建立，內有中文說明）

| 鍵 | 預設值 | 說明 |
| --- | --- | --- |
| `ActivationMethod` | `DoubleLeftControl` | `DoubleLeftControl` / `DoubleRightControl` / `ShakeMouse` / `Shortcut` |
| `ActivationShortcut` | `Win+Shift+F` | `ActivationMethod=Shortcut` 時使用。可用 `Win`、`Ctrl`、`Alt`、`Shift` + `A-Z`、`0-9`、`F1-F24`、`Space`、`Home`… |
| `IncludeWinKey` | `0` | 連按 Ctrl 時是否必須同時按住 Win 鍵 |
| `DoNotActivateOnGameMode` | `1` | 全螢幕遊戲時不啟動 |
| `BackgroundColor` | `#80000000` | 背景遮罩顏色（`#AARRGGBB` 或 `#RRGGBB`） |
| `SpotlightColor` | `#80FFFFFF` | 聚光燈顏色 |
| `SpotlightRadius` | `100` | 聚光燈半徑（像素） |
| `AnimationDurationMs` | `500` | 淡入淡出時間（毫秒）。Windows 關閉動畫效果時會自動略過動畫 |
| `SpotlightInitialZoom` | `9` | 聚光燈一開始的放大倍率（會縮小到 `SpotlightRadius`） |
| `ShakeMinimumDistance` | `1000` | 搖晃偵測：最小移動距離 |
| `ShakeIntervalMs` | `1000` | 搖晃偵測：時間窗（毫秒） |
| `ShakeFactor` | `400` | 搖晃偵測：移動距離要大於移動範圍對角線的百分比 |
| `ExcludedApps` | （空） | 以 `;` 分隔的執行檔名或視窗標題片段，例如 `notepad.exe;vlc.exe` |

修改後在系統匣選單按「重新載入設定」即可套用，不用重新啟動。

## 檔案結構

```
src/FindMyMouse.cpp   聚光燈與偵測邏輯（移植自 PowerToys 的 FindMyMouse.cpp）
src/FindMyMouse.h     設定結構與預設值
src/Settings.cpp      ini 設定檔讀取
src/main.cpp          系統匣、快捷鍵、單一執行個體、命令列
```

## 授權

MIT License，原始程式碼著作權屬於 Microsoft Corporation（見 `LICENSE`）。移植自 PowerToys commit `f144bfa`。
