# Linux 記憶體上下文掃描原型

本工具是 `feat/linux-focus-memory-context` 的實驗元件。分支已接上預設關閉的 Fcitx 聚焦流程，限制在能驗證插入／刪除回報的 client；設定、實作與限制見 [FCITX_FOCUS.md](FCITX_FOCUS.md)。

目前以 **magic bytes 插入後立即刪除，再掃殘留資料** 為研究方向。已在真正的 Linux Chromium 驗證；結果、undo 副作用與畫面比較見 [MARKER_UNDO_RESULTS.md](MARKER_UNDO_RESULTS.md)。

## 使用方式

在 Linux／WSL 的 repo 根目錄執行，需要 C++23 編譯器與 CMake：

```sh
cmake -S tools/memory-context-probe -B build/memory-context-probe -DCMAKE_BUILD_TYPE=Release
cmake --build build/memory-context-probe --parallel 2
ctest --test-dir build/memory-context-probe --output-on-failure -V
build/memory-context-probe/context_memory_scan --benchmark
```

`context_memory_scan` 只建立、讀取及回收自己的測試子程序。拒絕存取的測試需以一般使用者執行，不能具有可繞過子程序 dump 限制的權限。

`memory_scan.hpp` 提供 `scan(pid, region, pattern, limits)`；使用 `process_vm_readv` 逐頁讀取，預設每次 256 KiB、2 ms 軟性時間預算、16 個命中上限。時間預算只能在系統呼叫之間檢查，不能中斷正在執行的讀取。回傳候選位址、覆蓋狀態與成本，不回傳已確認的文件前文。

### Chromium 插入、刪除、undo 與畫面實驗

另需 Node 18+ 及可正常啟動 sandbox 的 Linux Chromium，沒有 npm 套件依賴：

```sh
node tools/memory-context-probe/chromium_undo_probe.mjs \
  /absolute/path/to/chrome \
  "$PWD/build/memory-context-probe/memory_residue_probe" \
  "$PWD/build/memory-context-probe/chromium-undo-results"
```

腳本建立獨立暫存 profile，透過私人 DevTools pipe 操作自建的 `input`、`textarea` 與 `contenteditable`。
它只呼叫 scanner 讀取該測試 browser 的 renderer 後代程序，結束後回收 browser 與暫存 profile。不使用現有瀏覽器或桌面按鍵注入，也不關閉 sandbox。

腳本輸出 `results.jsonl` 與各階段 PNG。若環境已有 Python PyGObject／GdkPixbuf，可另外列出原始像素差異及其範圍：

```sh
python3 tools/memory-context-probe/compare_frames.py build/memory-context-probe/chromium-undo-results
```

新增的 C++23 `memory_residue_probe PID PATTERNS_FILE` 是診斷 helper；pattern 檔每行為 `label hex_bytes`，最多八個、每個最多 1024 bytes。
它讀取 `/proc/PID/maps` 與 `pagemap`，只選匿名、私有、可讀寫且當時駐留的頁面，再用既有 scanner 搜尋。
每次程序檢查合計限制 256 MiB、5 秒軟性預算、每個 pattern 最多 256 個命中，輸出數量與完成狀態，不輸出捕獲文字。
頁面狀態可在讀取時改變，因此駐留篩選並不保證沒有 page fault。

此 helper 可用明確 PID 手動執行；實驗 harness 另外驗證 PID 屬於自己的測試 browser。它不提供權限繞過。

## 副本問題

**Magic bytes 能排除原本不含標記的歷史副本，卻無法排除這次插入新產生的副本。**

原型新增一個受控情境：

1. fork 後產生一個 128-bit 隨機值，編為 32 個可列印的十六進位字元。
2. 確認目標 mapping 沒有這個字串。
3. 子程序將相同前文與標記寫到兩個位置，模擬目前欄位及其即時鏡像。
4. 掃描保留兩個命中；搜尋「前文＋標記」也得到兩個命中。
5. 換新標記再做一次，兩個候選都跟著更新，仍不能從更新行為辨識欄位。

這是字串搜尋存在歧義的反例，並非量測 Chromium 實際產生幾份副本。測試的父程序刻意不利用子程序的邏輯游標挑選結果。

其他測試包括：讀取前後 bytes／游標不變、UTF-8 跨頁、UTF-16LE、穩定文字但游標移動、非連續文件、不可讀頁面、預算限制、重新驗證以及權限不足。

## 每次聚焦取得上下文的缺口

Fcitx5 前端已有 `ImeEngine::activate()`，共用引擎在 `Engine::activate()` 會要求現有上下文來源刷新。接入觸發點不是字串掃描最困難的部分；仍須驗證桌面與 app 實際的欄位切換事件是否足夠。

可靠的純記憶體方案需要完成：

- 找出焦點所屬程序與真正儲存文字的程序，確認讀取權限。
- 辨識目前焦點欄位、文字物件、編碼／長度、選區或游標；物件位置及格式可能依框架與版本而變。
- 在程序退出、位址重用、內容變動與焦點切換時使舊結果失效。
- 用每次新 nonce 建立本次探測關聯；處理原本已有選區、文字被 app 改寫、掃描途中焦點切換等情境。
- 在背景限制總工作量及頻率，並量測真實 app 的輸入延遲與上下文正確率。

只找到最新的字串、第一個命中，甚至只有一個命中，都不等於已完成上述辨識。掃描本身也不提供一致的全程序快照。

Chromium 實驗已驗證基本欄位的文字／游標還原、input 事件、undo 與渲染差異；已發現 undo 與選取游標副作用。尚未驗證自動儲存、既有選區、網站自訂編輯器及 Fcitx5／Wayland／X11 端到端行為。

## Benchmark 的範圍

比較已知 64 MiB mapping 與定位後的 4 KiB 頁面，並測試約 1 ms RAM 標記的捕獲率。頁面預先觸碰，標記在尾端，子程序通知 scanner 後才等待刪除。這不代表全程序探索、Chromium 或任意位置的捕獲率；實際存活時間另外輸出。

## 參考

- [process_vm_readv](https://www.man7.org/linux/man-pages/man2/process_vm_readv.2.html)：讀取權限、部分讀取、非原子性。
- [Blink InputMethodController](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/core/editing/ime/input_method_controller.cc)：內部文字輸入狀態涉及焦點元素、選區及編輯範圍；不是靠找最新字串來確定欄位。這是後續定位研究的入口，尚未實作外部記憶體解析器。
