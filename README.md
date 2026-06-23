# obs-auto-resize-output

為 OBS Studio 提供 **每個 Scene 各自的錄製 / 輸出設定**。切換 scene 時自動套用該 scene 的設定;複製 scene 時設定也會一起被複製。

## 功能

每個 scene 可獨立覆寫以下設定:

| 設定 | 套用時機 | 備註 |
|---|---|---|
| 基底 (canvas) 解析度 | 切換到該 scene 時(限閒置) | 透過 `obs_reset_video()` |
| 輸出 (scaled) 解析度 | 切換到該 scene 時(限閒置) | 同上 |
| FPS(整數) | 切換到該 scene 時(限閒置) | 同上 |
| 錄製資料夾 | 下一次開始錄製時 | 寫入 profile config |
| 錄製格式 (mkv/mp4/mov/ts/flv…) | 下一次開始錄製時 | 寫入 profile config |
| 音軌(bitmask) | 下一次開始錄製時 | 寫入 profile config |
| 錄製 bitrate(kbps) | 下一次開始錄製時 | **僅 Advanced 輸出模式**;寫入 `recordEncoder.json`,設為 CBR |
| 音訊監聽裝置(你聽的裝置) | 切換到該 scene 時(**錄製中也可,不中斷**) | 切換 OBS 全域監聽裝置;見下方〈音訊監聽〉 |

此外還有一個**全域**控制:

- **「Mute to me」一鍵開關**(dock 最上方):一鍵讓**你自己**聽不到被監聽的音訊,但**錄製完全不受影響**(內容、音量都不變),錄製中也能隨時切。詳見〈音訊監聽〉。

> **設定是「進入該 scene 時套用」,不會自動還原。** 若 scene B 沒有覆寫某個欄位,該欄位會維持目前的值(也就是上一個 scene 設過的值),而不是回到 profile 預設。要讓某 scene 用特定值,就在該 scene 明確勾選並設定它。

切換 scene 時,外掛會讀取該 scene 的設定並套用;設定資料存在該 scene source 的 `private_settings` 裡,因此:

- **複製 scene** → 設定自動跟著複製(libobs 在 `obs_scene_duplicate` 時會 `obs_data_apply` 複製 private_settings)。
- **存檔** → 設定隨 scene collection 的 `.json` 一起序列化,不需額外檔案。

## ⚠️ 一個 OBS 的硬限制(務必理解)

**解析度與 FPS 無法在錄製 / 串流 / 虛擬攝影機運行時更改。** 這是 libobs 影片管線的根本限制:`obs_reset_video()` 只有在沒有任何 output 運行時才會成功,否則回傳 `OBS_VIDEO_CURRENTLY_ACTIVE`。

因此本外掛的行為是:

- **閒置時**切到某 scene → 立刻套用解析度/FPS。接著你按下錄製,就會用該 scene 的設定錄製。
- **錄製中**切到某 scene:
  - 錄製資料夾 / 格式 / 音軌仍會寫入,套用到**下一次**錄製。
  - 解析度 / FPS **預設不會**即時改變(會在 dock 顯示提示)。
  - 若你在該 scene 勾選了「restart recording to apply」,且當時**只有錄製**在跑(沒有串流/虛擬攝影機),外掛會**停止錄製 → 套用新解析度 → 重新開始錄製**(會產生一個新檔案)。

> 任何宣稱能「錄製中無縫切換解析度」的方案都做不到這點 —— 這是 OBS 架構決定的,不是本外掛的缺陷。

## 音訊監聽(讓你自己聽不到,但照常錄製)

OBS 的「監聽(monitoring)」是**播放給你聽**的路徑,和錄製/串流用的編碼器路徑**完全獨立**。因此調整監聽**不會中斷錄製、也不會改變錄到的內容或音量**。本外掛用這個特性提供兩個東西:

1. **每個 scene 的監聽裝置**:切到某 scene 時,自動把 OBS 的全域監聽裝置切到該 scene 指定的裝置。錄製中切換也安全、即時。
2. **「Mute to me」一鍵開關**(全域):一鍵把監聽導向一個靜音目標,讓你**瞬間聽不到**所有被監聽的音訊;再按一次還原。錄製全程不受影響。

**前提(務必理解)**:這只對「**經由 OBS 監聽**才聽得到」的音訊有效 —— 也就是在 OBS 混音器裡把該音源設為 **"Monitor and Output"** 的來源。如果某個聲音是**作業系統直接播放**到你喇叭/耳機的(例如一般桌面音訊,OBS 只是 loopback 擷取),那你是在聽系統的聲音,OBS 的監聽開關管不到它;此時要靜音只能靜音裝置,但那會連 loopback 擷取也一起靜音,錄製就跟著沒聲音了。

**為什麼用「監聽裝置」而不是「每個來源的監聽類型」**:`obs_save_source()` 會在**每次存檔**(自動存檔、切換 scene collection、關閉 OBS 前的存檔)把每個來源的 `monitoring_type` 寫進 scene collection。關閉時的存檔發生在外掛能還原之前,沒有任何 hook 趕得及。所以若用「切換監聽類型」來靜音,一旦在靜音狀態存檔/關閉,使用者原本的 "Monitor and Output" 設定就會被永久覆寫。**監聽裝置是單一全域值(不隨來源存檔),因此安全。** 外掛另有開機自癒:若上次被強制關閉時仍處於靜音,下次啟動會把殘留的靜音裝置重設回 Default。

## 已知限制 / 尚未支援

- **錄製 bitrate 僅支援 Advanced 輸出模式**:Advanced 模式下寫入 `recordEncoder.json`(rate_control=CBR + bitrate),OBS 在開始錄製時讀取,故套用到下一次錄製。Simple 模式的錄製位元率與「錄製品質」模式綁定、且與串流共用編碼器,無法乾淨地單獨設定,因此在 Simple 模式下此欄位會被忽略(並於 log 提示)。
- 設為 bitrate 會把錄製編碼器切成 **CBR**(這是 bitrate 生效的前提);若你原本用 CQP/CRF,套用後會變成 CBR。
- 錄製設定的 config key(`RecFormat2`、`RecTracks`、`FilePath`/`RecFilePath`)以 **OBS 30/31+** 為目標。若你的 OBS 較舊,key 名稱可能不同(見 `src/ApplyPreset.cpp` 集中管理處)。

## 設計重點

- **資料存放**:每個 scene 的設定以一個巢狀 `obs_data` 物件(key `auto_resize_output`)存進該 scene source 的 `private_settings`。這是「複製即帶走、存檔即持久化」能成立的關鍵。
- **僅依賴穩定公開 API**:`obs.h`(`obs_reset_video` / `obs_get_video_info` / private_settings / obs_data)、`obs-frontend-api.h`(dock、事件、profile config)。不碰 OBS UI 的私有結構,降低版本升級破壞風險。
- **事件驅動**:監聽 `OBS_FRONTEND_EVENT_SCENE_CHANGED`(套用)、`FINISHED_LOADING`(啟動套用)、`RECORDING_STOPPED`(完成重啟錄製流程)、`SCENE_LIST_CHANGED` / `SCENE_COLLECTION_CHANGED`(刷新 dock 清單)。

程式碼結構:

```
src/
├── plugin-main.cpp   模組進入點、frontend 事件、dock 註冊
├── ScenePreset.*     從/向 scene private_settings 讀寫設定
├── ApplyPreset.*     套用設定:obs_reset_video + profile config + 監聽裝置 / Mute to me + 重啟錄製狀態機
└── PresetDock.*      Qt dock UI
```

## 建置

需要 OBS 開發環境(會匯出 `libobs` 與 `obs-frontend-api` 的 CMake package)以及對應的 Qt6。

```bash
cmake -B build -DCMAKE_PREFIX_PATH="<path-to-obs-dev>;<path-to-Qt6>"
cmake --build build --config RelWithDebInfo
```

取得 OBS 開發檔案最簡單的方式,是直接套用官方 [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) 的依賴下載流程(`buildspec.json` + `.github/scripts`),再把本專案的 `src/`、`data/`、`CMakeLists.txt` 放進去;它會自動抓取對應 OBS 版本的 `libobs`/`obs-frontend-api` 與 Qt,並產生跨平台 CI。

### 本機安裝(Windows 快速測試)

把建置出的 `obs-auto-resize-output.dll` 與 `data/` 放到:

```
%ProgramData%\obs-studio\plugins\obs-auto-resize-output\bin\64bit\obs-auto-resize-output.dll
%ProgramData%\obs-studio\plugins\obs-auto-resize-output\data\locale\en-US.ini
```

啟動 OBS 後,從 **Docks** 選單開啟「Auto Resize Output」。

## 使用

1. 在 dock 選擇要設定的 scene(預設跟隨目前的 program scene;一旦你手動選了別的 scene,切換直播 scene 時就不會再搶走你的選擇,直到你選回直播中的 scene)。
2. 勾選「Enable per-scene overrides for this scene」。
3. 勾選你想覆寫的項目並填值。可按「Copy from current OBS settings」用目前 OBS 設定快速帶入起始值(含錄製 bitrate 與監聽裝置)。
4. 切換到該 scene 時會自動套用(閒置時連解析度都會即時改;監聽裝置即使錄製中也會即時切)。
5. 想讓自己暫時聽不到(但繼續錄製)時,按 dock 最上方的 **「Mute to me」**;再按一次恢復。

## 版本升級耐受性

- 僅用穩定公開 API,原始碼層級相容性佳;OBS 大版本更新通常只需重新編譯。
- 設定存在 source private_settings(OBS 原生序列化路徑),不自訂檔案格式。
- 若未來 OBS 改了錄製 config key 名稱,集中在 `ApplyPreset.cpp` 一處調整即可。

## License

GPL-2.0-or-later(因連結 libobs)。見 [LICENSE](LICENSE)。
