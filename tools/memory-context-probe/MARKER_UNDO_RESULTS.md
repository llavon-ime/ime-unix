# 插入後立即刪除，能否從 undo／殘留記憶體取回標記？

## 結果

**可以延後掃描。** 本次 Linux Chromium 的六個情境，在標記刪除並等待一秒後，仍能掃到標記及「前文＋標記」的 UTF-16LE 副本。
**也確認有可復原紀錄：六個情境第一次 Ctrl+Z 都精確恢復含標記的文字，第二次才回到原文。**

但尚未將某個命中位址辨識成特定 undo 物件。掃到的可能包括編輯快取、undo 相關資料或尚未覆寫的配置；不能把所有命中直接稱為 undo buffer。

分支：`feat/linux-focus-memory-context`。包含 C++23 scanner、診斷工具及 Chromium 重現腳本。後續已接上預設關閉的 Fcitx 聚焦實作，見 [FCITX_FOCUS.md](FCITX_FOCUS.md)；本報告數據來自獨立 Chromium 實驗。

## 環境與操作

- 2026-09-28，WSL2 Ubuntu 24.04，GCC 13.3.0，Node 18.19.1。
- 真正的 Linux Chromium `149.0.7827.55`，revision `3188f8a607ae7e067593be8aab7f02d2451fec07`，headless renderer。
- 新建暫存 profile，保留 sandbox，使用私人 DevTools pipe；沒有修改既有 app、系統權限或輸入法設定。
- 欄位原文：`文首：早餐吃蛋餅，現在想喝咖啡。下一句`，游標在 `喝` 與 `咖` 之間，沒有既有選區。
- 每次新建 128-bit 隨機 nonce；ASCII 為 `ctx-` 加 32 個十六進位字元，不可見版本以 U+2063／U+2064 編碼 128 個位元。
- 先掃描確認所選記憶體範圍內沒有本次標記，再用 `Input.insertText` 插入，選取標記，經 Blink 的 Backspace 編輯路徑刪除。
- 刪除後先掃一次，再等待一秒後掃一次，最後送 Ctrl+Z 兩次。完整內容比對在掃描完成後才執行，避免比對操作提前製造含標記的副本。

這是 DevTools 驅動 Blink 編輯，不是 Fcitx5、GTK IM、X11 或 Wayland 輸入協定的端到端測試。

## 等待一秒後的記憶體命中

下表為各受控 renderer 的命中數合計；每組只執行一次，數字會隨配置與 GC 改變。
UTF-8 搜尋在這六組都是零命中，以下為 UTF-16LE。

| 欄位 | 標記 | 標記命中 | 前文＋標記命中 | 第一次 Ctrl+Z | 第二次 Ctrl+Z |
| --- | --- | ---: | ---: | --- | --- |
| input | ASCII | 6 | 4 | 恢復標記 | 恢復原文 |
| input | 不可見 | 4 | 1 | 恢復標記 | 恢復原文 |
| textarea | ASCII | 6 | 4 | 恢復標記 | 恢復原文 |
| textarea | 不可見 | 5 | 3 | 恢復標記 | 恢復原文 |
| contenteditable | ASCII | 6 | 1 | 恢復標記 | 恢復原文 |
| contenteditable | 不可見 | 5 | 1 | 恢復標記 | 恢復原文 |

插入前的掃描全部零命中；本次所選頁面的掃描沒有權限錯誤或預算中斷。
範圍僅包括測試 renderer 的駐留匿名私有可讀寫頁面；不等於整個程序的所有位元組，也不是原子快照。
「前文＋標記」使用已知測試前文作 oracle，證明這些連續副本存在；尚未實作從未知文件推導文字邊界的解析器。

## 閃爍與不可見標記

分開做了一組渲染測試：刻意將標記保留到 renderer 完成畫面，再依序擷取插入、選取、刪除的 PNG。
這能驗證各階段的視覺差異，不表示那些階段在快速操作時都曾呈現在實體螢幕上。

三種欄位的結果一致：

| 階段 | ASCII 標記 | 不可見標記 |
| --- | --- | --- |
| 插入後 | 畫面改變 | PNG 與原畫面完全相同 |
| 選取標記 | 畫面改變 | 僅游標所在的一條 1 × 27 像素線改變 |
| 刪除後 | 回到原畫面 | 回到原畫面 |

同時將 caret 設為透明作對照：不可見標記在插入、選取、刪除各階段的 PNG 都與原畫面相同。
不操作文字、只重設相同游標位置的控制組 PNG 也相同。
因此這組特定文字與樣式下，選取造成的差異可定位到游標，沒有觀察到不可見標記造成的文字重排。

可直接比較 [原畫面](results/input-invisible-auto-baseline.png)、[插入不可見標記](results/input-invisible-auto-inserted.png)、[選取標記](results/input-invisible-auto-selected.png)。

快速插入／選取／刪除的 DevTools 往返時間為 3.30–23.35 ms；六組中三組的 `requestAnimationFrame` 曾觀察到含標記狀態。
這不是標記精確存活時間，也不是實體螢幕閃爍測量，但已不能假設「馬上移除」就一定避開渲染時機。

## 已確認的副作用與下一步

1. **Undo 被改變。** 六組原本沒有可 undo 的操作，探測後都有；Ctrl+Z 會復活標記。只刪掉文件中的標記不代表還原了編輯歷史。
2. **網頁會收到事件。** 六組插入／刪除各產生一次 `input`，合計兩次。自訂編輯器或自動儲存的反應尚未測試。
3. **選取可能造成游標閃動。** 不可見標記解決了本組測試的字形顯示，還要研究不經中間選取畫面的移除路徑。
4. **殘留資料的歸屬尚未驗證。** 需辨識 undo／文字物件、文字邊界與本次焦點，處理重複命中和 GC；不能直接取第一個命中前面的 bytes。

這個方向已證明「刪掉後仍可讀」，可以把掃描移出標記可見的時間窗口。要完成無可感知副作用的每次聚焦擷取，仍須解決 undo、移除路徑及正確上下文的驗證。

## 檔案與驗證

- [完整原始輸出](results/chromium-undo-2026-09-28.jsonl)：環境、六組編輯／記憶體結果及十二組渲染比較。
- [Chromium 重現腳本](chromium_undo_probe.mjs)、[DevTools pipe](cdp_pipe.mjs)。
- [C++23 殘留資料 scanner](memory_residue_probe.cpp)、[逐頁讀取實作](../../engine/src/memory/memory_scan.cpp)。
- C++23 Release 編譯成功，無警告；既有 scanner 的 14 個情境全部通過。
- 本報告的獨立實驗未改動 engine；後續 Fcitx 接入已新增 raw-key 測試，結果記於 [接入說明](FCITX_FOCUS.md)。

重跑指令見 [README](README.md)。
