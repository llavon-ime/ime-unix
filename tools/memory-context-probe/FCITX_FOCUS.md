# Fcitx 聚焦記憶體上下文：第一版

分支：`feat/linux-focus-memory-context`。

已接入 Linux Fcitx addon：context 啟用時，可要求一次「插入標記 → 確認 → 刪除 → 確認 → 掃描殘留」。讀出的前文接入 engine 的預測上下文來源。

**預設關閉，程式白名單為空。** 本版依選定範圍，要求 client 提供 surrounding text 以驗證選區及插入／刪除。完全不提供回報的 app 會跳過。Undo、編輯事件及異常中斷時的清理問題尚未解決，不能宣稱無副作用。

## 建置與設定

在 Linux repo 根目錄執行，需要 C++23、Fcitx5 開發套件及 nlohmann-json：

```sh
cmake -S fcitx5 -B build/memory-fcitx -DLLAVON_IME_BUILD_TESTS=ON
cmake --build build/memory-fcitx --target llavon-ime-addon --parallel
```

設定檔為 Fcitx 的 `conf/llavon-ime.conf`。只在隔離測試 client 驗證回報行為後加入程式：

```ini
MemoryContextEnabled=True

[MemoryContextPrograms]
0=my-tested-editor
```

名稱必須同時等於 Fcitx `InputContext::program()` 與 `/proc/PID/exe` 的檔名。範例不是現成支援名單。本次未安裝 addon 到系統，也未啟用任何日常程式。

設定頁 `MemoryContextStatus` 顯示最近狀態，如 `waiting-fresh-surrounding`、`awaiting-insert-echo`、`awaiting-delete-echo`、`scanning-residue`、`ready`。無法讀取為 `process-unavailable`；未找到完整相符副本時保留原有上下文來源。

## 實際流程

1. `Engine::activate()` 要求一次探測。必須在白名單、有焦點、非敏感欄位、支援 surrounding text，且尚未組字。
2. 等待**此次啟用之後的新 surrounding 通知**。不採用舊快取作插入許可。拒絕非空選區、空文字、超過 4096 UTF-16 units、無效游標及 surrogate 中間的游標。
3. 背景尋找同使用者、執行檔名稱相符的程序，確認 `/proc` 與 `process_vm_readv` 可讀，記錄 PID 及 process start time。權限不足就跳過，不提升權限或修改 ptrace 設定。
4. 回主執行緒重查焦點世代、文字及游標。將 128-bit 隨機 nonce 編成 128 個 U+2063／U+2064，直接 `commitString()`，不經 engine 的一般提交／訓練路徑。
5. client 回報完整「原文字＋標記」及正確游標後，才呼叫 `deleteSurroundingText(-128, 128)`。不經中間選取、不送 Ctrl+Z。標記全為 BMP scalar，刪除計數不受原文 emoji 影響。
6. 另一個新回報必須精確還原文字、游標與 anchor，才啟動掃描。插入／刪除合計 150 ms 軟性逾時；掃描不延長標記留在欄位的時間。
7. 在匿名、私有、可讀寫的駐留頁面搜尋 UTF-8／UTF-16LE nonce；每次合計限制 64 MiB attempted bytes、150 ms 軟性預算。系統呼叫可能超過軟性預算。
8. 每個候選重新讀取並比對**完整已知文字窗口＋標記**，再查 PID start time，才回傳實際讀出的前文。多個相同副本得到相同內容，不以命中順序推論目前欄位。
9. 主執行緒再確認焦點世代與 client 內容。有效結果優先用於 `Engine::resync_context()`；否則沿用 native／accessibility 來源。

**目前只取得可被 client 已知窗口驗證的前文，尚不會從窗口外的任意 bytes 推導更長上下文。** 這版接通記憶體讀取流程，尚未消除 surrounding text 依賴。

單一背景 worker 可取消舊工作。插入前收到按鍵就取消；交易期間最多暫存 64 個原始按鍵，確認還原後繼續交給 engine。失敗時向原 context 轉送按鍵；context 已銷毀時無法轉送。掃描期間不暫存按鍵。

## 失效與限制

- 焦點離開、reset、detach、敏感欄位、client 文字／游標改變、未處理按鍵及一般提交會使結果失效。背景晚到結果不能套用到下一次聚焦。
- 插入／刪除回報缺少或不符時停止；可能仍有標記時顯示 `cleanup-unconfirmed`，本次 addon 生命週期停用該程式。**此時不能保證標記已移除。** 不會對已變更欄位盲目刪除或 Undo。
- 回報與 app 執行刪除之間不是原子操作，滑鼠、腳本及其他輸入來源仍可能介入；白名單必須逐一驗證實際 client。
- context 粒度由 frontend 決定，有些內部欄位切換沒有新 `activate()`，不能承諾涵蓋所有 DOM／widget 聚焦。
- [Chromium 實驗](MARKER_UNDO_RESULTS.md) 已確認提交／刪除改變 Undo 並產生編輯事件。本版移除中間選取步驟，尚未在實際 Fcitx + Wayland／X11 app 測得零閃爍或 Undo 完全不變。
- 沙箱、讀取權限及名稱不符均可能讓探測不可用。

## 實作

- [Fcitx 接線](../../fcitx5/fcitx5/ime_engine.cpp)、[設定](../../fcitx5/fcitx5/ime_config.hpp)。
- [聚焦與 worker](../../engine/src/memory/focus_probe.cpp)、[交易確認](../../engine/src/context/marker_transaction.cpp)。
- [程序查找／擷取](../../engine/src/memory/process_memory.cpp)、[逐頁 scanner](../../engine/src/memory/memory_scan.cpp)。
- [raw-key 測試](../../engine/tests/rawkey/memory_context_tests.cpp)、[實際子程序測試](../../engine/tests/memory_process_tests.cpp)。

## 驗證

2026-09-28，WSL2 Ubuntu 24.04、GCC 13.3.0、Fcitx 5.1.7：

- addon 編譯成功，共用 engine 測試通過。
- raw-key：42 組、0 失敗，包含四組新增的標記／聚焦／輸入情境。
- 實際子程序：UTF-8、UTF-16LE 都能在可見文字還原後讀出保留副本；拒絕錯誤 PID start time、不同文字窗口、取消與無權限目標。
- Fcitx ↔ GTK／Qt／Chromium ↔ Wayland／X11 桌面端到端測試尚未完成。raw-key 的 client 回報使用測試替身，跨程序讀取另用真正的子程序測試。

```sh
cmake -S engine -B build/engine-tests -DLLAVON_IME_ENGINE_BUILD_TESTS=ON
cmake --build build/engine-tests --target llavon_ime_rawkey_tests llavon_ime_memory_tests --parallel
build/engine-tests/tests/llavon_ime_rawkey_tests
build/engine-tests/tests/llavon_ime_memory_tests
```

Ubuntu 5.1.7 相容調整：addon 自己持有 `EventDispatcher`；候選頁內 cursor 轉成 global index，使用舊版已有介面。[上游介面](https://github.com/fcitx/fcitx5/blob/master/src/lib/fcitx/candidatelist.h) 將頁內 `setCursorIndex` 標為 5.1.9 起提供。
