# 建置指南(含「如何取得 OBS 原始碼 / Qt6」)

## 先澄清:沒有「申請」這回事

OBS 原始碼與 Qt6 都是**免費、開源、可直接下載**的,不需要任何申請或授權。
而且——**建置 OBS 外掛時,你通常根本不需要自己安裝 Qt6 或編譯 OBS**。
官方的 [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) 內建一個 `buildspec.json`,會在編譯時**自動下載對應版本的 OBS 開發檔案 + Qt6 + 相依套件**(目前釘選 OBS 31.1.1 + Qt6,2025-07-11 版)。

所以下面給你三條路,**從最省事到最完整**。推薦先試 **路線 A**。

---

## 路線 A:GitHub 雲端建置(零本機安裝,最推薦先試)

完全不用在自己電腦裝 Visual Studio / Qt / OBS。GitHub 的免費 CI 會幫你編出 Windows DLL。

1. 註冊一個免費 [GitHub](https://github.com) 帳號。
2. 在本機安裝 [Git](https://git-scm.com/download/win)(只需要這個)。
3. 在本專案資料夾把模板建置系統拉進來(會自動下載 OBS/Qt 的設定,真正下載發生在 CI 上):
   ```powershell
   pwsh ./scripts/Integrate-Template.ps1
   ```
   這會把模板的 `cmake/`、`.github/`(CI 設定)、`CMakePresets.json`、`buildspec.json` 等複製進來,並自動改好我們的外掛名稱與原始碼清單。
4. 建立一個 GitHub repo 並推上去:
   ```powershell
   git init
   git add .
   git commit -m "Initial commit: obs-auto-resize-output"
   git branch -M main
   git remote add origin https://github.com/<你的帳號>/obs-auto-resize-output.git
   git push -u origin main
   ```
5. 打開 GitHub repo 的 **Actions** 分頁 → 等綠色勾完成 → 進入該次 run → 在 **Artifacts** 下載 Windows 的 zip,裡面就是編好的 `obs-auto-resize-output.dll`(+ `data/`)。
6. 跳到本文件最後的「**安裝到 OBS**」。

> 之後每次改程式碼 `git push`,CI 會自動重編,你只要下載新 artifact。

---

## 路線 B:本機建置(模板自動下載 OBS/Qt)

想在自己電腦編、方便除錯時用這條。**仍然不需要手動裝 Qt6 或編 OBS**——模板會自動抓。

### 取得工具(一次性)

| 工具 | 下載 | 備註 |
|---|---|---|
| Git | https://git-scm.com/download/win | |
| Visual Studio 2022(Community 免費) | https://visualstudio.microsoft.com/ | 安裝時勾選 **「使用 C++ 的桌面開發」** workload |
| CMake ≥ 3.30 | https://cmake.org/download/ | VS2022 通常已內建;否則裝獨立版並加入 PATH |

> OBS 原始碼/開發檔案與 Qt6 **不必另外裝**——下一步的 `buildspec.json` 會自動下載到 build 目錄。

### 步驟

1. 拉入模板建置系統:
   ```powershell
   pwsh ./scripts/Integrate-Template.ps1
   ```
2. 設定 + 建置(第一次會自動下載 OBS 31.1.1 + Qt6,需幾分鐘):
   ```powershell
   cmake --preset windows-x64
   cmake --build --preset windows-x64 --config RelWithDebInfo
   ```
3. 產物在 `build_x64/RelWithDebInfo/`(或 `release/`)下,檔名 `obs-auto-resize-output.dll`。
4. 跳到「安裝到 OBS」。

---

## 路線 C:本機建置(自備相依,使用我們的 standalone CMake)

只有在你**已經有** OBS 開發檔案與 Qt6(例如你已從原始碼編過 OBS)時才需要。

1. 取得 OBS 開發檔案的兩種方式(任選):
   - **從原始碼編 OBS**:`git clone --recursive https://github.com/obsproject/obs-studio.git`,依其 README 編譯;它會用自己的 buildspec 下載 Qt6 + deps。編好後其 build 樹會匯出 `libobs` / `obs-frontend-api` 的 CMake package。
   - **用預編好的 obs-deps**:從 [obs-deps releases](https://github.com/obsproject/obs-deps/releases) 下載含 Qt6 的 bundle(但 `libobs` 開發檔仍需來自 OBS build 樹)。
2. 用我們保留的 standalone CMake(`CMakeLists.standalone.cmake`)建置,並把 `CMAKE_PREFIX_PATH` 指到上面那些套件:
   ```powershell
   cmake -B build -S . -C CMakeLists.standalone.cmake `
     -DCMAKE_PREFIX_PATH="C:/path/to/obs-build;C:/path/to/Qt6"
   # 或把 CMakeLists.standalone.cmake 覆蓋回 CMakeLists.txt 再 configure
   cmake --build build --config RelWithDebInfo
   ```

> 路線 C 步驟最多、最容易卡在找不到套件;除非你已經是 OBS 開發環境,否則建議用 A 或 B。

---

## 安裝到 OBS(三條路線通用)

把 DLL 與 `data/` 放到 OBS 的外掛目錄:

```
%ProgramData%\obs-studio\plugins\obs-auto-resize-output\bin\64bit\obs-auto-resize-output.dll
%ProgramData%\obs-studio\plugins\obs-auto-resize-output\data\locale\en-US.ini
```

(`%ProgramData%` 通常是 `C:\ProgramData`。)

啟動 OBS → 上方選單 **Docks** → 勾選「**Auto Resize Output**」即可看到面板。

---

## 我的建議

先走 **路線 A**:十分鐘內就能拿到一顆能裝進 OBS 的 DLL,完全不必在本機裝任何重量級工具,也能立刻驗證外掛是否正確編譯與載入。等確認可用、想開始本機改程式碼除錯時,再加裝路線 B 的 VS2022 即可。
