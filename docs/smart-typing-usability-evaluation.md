# 智慧中英文：文章、混輸與打錯鍵的實際流程測量

## 結論

目前智慧中英文的 **raw-key 撤銷與重新輸入穩定，但中英切分仍不夠可靠**。
普通中文文章在輸入標點、等待現有模型回應後，可以得到很好的最終文字；
混入英文、數字、email、帳號時，部分中文按鍵被保留成 ASCII，模型沒有
收到這些位置的注音，因而救不回來。許氏的混輸錯誤較明顯。

本次沒有修改 core、服務或正式輸入行為。使用目前的 Engine、已安裝的
250M Q4_K_M 本機模型與服務，**沒有套用上一個實驗的文字重排工具**。

## 模擬方式與範圍

新增：

- `engine/tests/rawkey/typing_usability_cases.json`：四篇文章與 15 個編輯流程。
- `engine/tests/rawkey/typing_usability_probe.cpp`：獨立 raw-key 測量 runner。
- `engine/tools/model_mixed_probe/CMakeLists.txt`：新增 `typing_usability_probe` target。

四篇文章分別是週末日記、學習心得、工作更新、技術討論；每種鍵盤的目標
文件合計 **386 字元**，含 26 個標點，共兩種鍵盤 772 字元。中英文不刻意
全部加空白，例如「我用python寫了一個小程式」；保留 `GitHub`、
`README.md`、`WebSocket`、URL、email、`user_name42`、`v2.1.0`。

中文字與讀音由 fixture 明確指定，不用待測模型替答案標注；每個讀音都
驗證能從 canonical table 找到目標字，並回放實際 keymap 驗證鍵盤編碼。
一聲使用 Space，中文逗號／句號用 `Ctrl+,`／`Ctrl+.`。

模擬客戶端會接收 Engine 的真正提交，讓游標移動與 Backspace/Delete
作用於文件；未被輸入法吃掉的英文字母也交給客戶端。客戶端文件不包含
preedit，每次事件後提供實際文件與游標位置作為 surrounding context。

測量條件：

1. **normal-sentence**：每鍵 60 ms，標點後再停 120 ms，句號後 Enter 確認。
   這是固定節奏的偏快模擬，不是從真人收集的按鍵時間分布。
2. **normal-with-typos**：相同節奏，每篇插入 3–4 個錯鍵，晚六個 raw keys
   才發現，Backspace 刪回錯誤位置、重打，再繼續全文。
3. **burst-paragraph**：不等模型回應，整段快速灌入並提交；屬於壓力測試，
   不可當成一般人的實際速度。
4. **offline-sentence**：關閉服務自動啟動且使用無服務 socket，快速輸入；
   觀察 fallback 路徑，節奏與 normal 不同，不是單一變因的模型 A/B。
5. **traditional**：關閉 SmartEnglish，對純中文文章跑相同 60 ms 節奏，
   作為共享引擎流程的參考；結果受到下述非同步時序問題影響。

另外測：52 個獨立子句的首頁候選修復、527 個注入錯鍵樣本、30 個編輯
流程、8 個長組字長度／鍵盤測點、8 個標點時序診斷。

## 文章結果：目前的正式流程

CER 是編輯距離除以目標字元數，包含替換、插入與刪除；不是單純錯字
比例，也不是語言切分準確率。沒有人工選字的 normal-sentence 結果：

| 文章 | 標準 CER | 許氏 CER |
| --- | ---: | ---: |
| 週末日記 | 0.0% | 0.0% |
| 學習心得 | 4.3% | 0.0% |
| 工作更新 | 26.9% | 39.8% |
| 技術討論 | 5.6% | 25.4% |

按字元數加權，兩篇純中文合計為標準 **2.2%**、許氏 **0.0%**；兩篇混輸
合計為標準 **14.8%**、許氏 **31.6%**。四篇乘兩種鍵盤，全文完全一致為
**3/8**。這是小型固定語料結果，不能當作一般使用者準確率。

代表性錯誤：

```text
目標：剛開始的時候
標準：e; 開始的時候

目標：把json資料存進postgresql
標準：183jsony xul4hjp6rup4postgresql
許氏：byfjson資料存進postgresql

目標：目前還有3個bug需要修正
許氏：mxjvemdhideof3ggjbugcu 要修正

目標：測試報告請寄到dev@example.com
許氏：agjcjbwjgwjvelfjejdwjdev@example.com
```

`mixed_input_probe standard 'e; '` 顯示字面路徑 -1.040、「剛」-1.252；
這個常用的一聲中文就會輸給字面解讀。email 的 ASCII grammar 允許很長
的 alphanumeric local part，因此也可能把前面的注音鍵當成 email 的一部分。
這些觀察支持先處理候選切分與排名，而不是只擴大中文詞頻表。

### 模型其實會在標點後接手

本次正式 Engine 在智慧模式輸入 `Ctrl+標點` 時，會先把 pending 路徑
轉入 buffer，經 `apply_mixed_path()` 請求既有模型。因此：

```text
標點前 pending：今天早上我和朋有意起去工原散不
    ↓ 輸入逗號，等待模型
標點後 buffer：今天早上我和朋友一起去公園散步，
```

所以「智慧模式 pending 不走模型」不等於「整篇文章完全沒有模型」。
但標點前會長時間看到離線排名的錯字；若按鍵已被解讀成英文字面內容，
模型也無法重新找回其注音。

技術討論文章的大小寫英文還會在每種鍵盤觸發 **5 次普通字母輸入期間的
提交**，符合預設 `ShiftLetterKeys=directly_output_uppercase` 的既有行為。
例如進入大寫英文前的中文可能已提交，不能繼續靠後面的預測修正。

## 打錯鍵與修復

### 527 個注入樣本

從文章子句與英文對照詞取 10 個 seed，在字首、字中、字尾與部分聲調／
空白邊界注入：鄰鍵替換、重複按鍵、漏鍵、兩鍵交換、額外空白。
標準 264、許氏 263，共 527；不同錯誤共用 seed，並不是 527 個獨立
自然語言句子，也沒有模擬真實錯鍵頻率。

| 修復方式 | 回到無錯鍵輸出 |
| --- | ---: |
| 立刻發現，刪回錯誤位置重打 | 527/527 |
| 再打六鍵才發現，刪回錯誤位置重打 | 527/527 |

延後發現的修復平均增加約 **12.5 次按鍵**，最多 **16 次**。不修復時，
478/527 的輸出與無錯鍵參考不同；其餘可能是被捨棄的鍵、同一解讀或
碰巧相同，不應算成有效自動糾錯。

**回到無錯鍵輸出不等於得到正確中文。** 這組證明 raw-key 撤銷可恢復，
不能掩蓋原本的切分／選字錯誤。此組使用離線短子句，與真實服務時序分開。

### 全文裡晚發現錯字

另以 real service、60 ms 節奏輸入四篇全文，在每篇注入 3–4 次錯鍵，
等六個 raw keys 後才 Backspace 重打。兩種鍵盤合計 **28 次修復**，
pending raw keys 全部恢復；**8/8 篇的最終文字與各自無錯鍵全文完全一致**。
每篇因此增加 42 或 58 次按鍵。原本混輸切錯的內容依然切錯。

### 30 個實際編輯流程

15 種流程各跑兩種鍵盤，**30/30 通過**：英文漏字／交換／中段刪字、
錯聲調立即修改、中文中段 Delete 重打、提交後刪除重打、候選取消後修改、
手動選字後混輸、Escape 取消重打、多空白修復、聲調旁多按一鍵、
CapsLock 誤觸、大寫英文、一聲空白刪除重加、組字中焦點切換。

## 候選修復不夠容易

以正確的前文分別輸入 26 個子句，按 Down 查看首頁九項候選；若目標在
首頁就實際選字並 Enter，驗證提交。這是一個樂觀的前文條件，沒有延續
前句的錯誤，也沒有等到標點才修正，更沒有遍歷每個字的同音字候選。

| 鍵盤 | 原本正確 | 首頁選字新增修復 | 修復後合計 |
| --- | ---: | ---: | ---: |
| 標準 | 6/26 | 6 | 12/26 |
| 許氏 | 5/26 | 8 | 13/26 |

合計 41 個原本錯的子句，只能以首頁整段／目前目標候選救回 **14 個**；
另外 27 個需要更複雜的逐字選字、移動或重打。不能把這個數字解讀為
完整候選召回率，但它反映目前「按 Down 快速修好」的能力有限。

## 長組字效能

將兩篇純中文的真實注音串接，保留一聲 Space，移除標點與確認，循環至
指定長度，避免只重複「你好我」固定短串。用同一個組字逐步增加長度，量測每個區間的同步
raw-key event 處理時間；不是螢幕呈現延遲，也不含模型服務 round-trip。

| 累積 raw keys | 標準區間 p95 | 許氏區間 p95 |
| --- | ---: | ---: |
| 128 | 1.9 ms | 3.3 ms |
| 256 | 4.7 ms | 8.6 ms |
| 512 | 12.7 ms | 23.5 ms |
| 768 | 23.9 ms | 33.0 ms |

許氏 768 鍵時最大約 **40.5 ms**，全程沒有提前提交。長 pending 的整段
重算已值得處理；短句模型速度良好，並不能代表長文章每鍵仍然便宜。

## 另外找到的非同步問題

SmartEnglish=False 的參考流程偶爾在模型請求未完成時輸入標點，會使
原本已改善的字退回 fallback。 focused 診斷中，許氏的一次結果：

```text
標點前：今天早上我和朋友一起去公園散不   （1 個字的差距）
標點後：今天早上我和朋有意起去工原散不， （5 個字的差距）
```

先等 prediction 完成再輸入標點則正確。這是時序相關觀察，不是每次都
發生；本次智慧模式的同一診斷沒有重現這個退回現象。
`engine/src/host/engine.cpp` 的 `handle_prediction_response()` 在回應
correlation 正確、但 composition 已改變時，會對該請求的所有位置套用
fallback，與觀察相符。仍需要以 scripted 延遲回應做決定性回歸，再處理
過期回應應保留哪些已有候選。此處未修改正式行為。

## 建議優先順序

1. **中英切分**：數字、email、URL／識別字的字面保護不要吞掉相鄰中文。
   保留可逆 raw keys 與真實候選，針對完整文章案例改善排名。
2. **模型更新時機**：讓連續組字中完整注音也能取得模型候選，而非主要靠
   標點／選字結束 pending；需要組字版本與手選保護。
3. **候選操作**：改善早期字位及切分候選的可取得性，減少整段重打。
4. **長組字增量運算**：避免每鍵從頭重算整段 lattice。
5. **過期回應**：補決定性的回歸，避免無效回應讓已有的正確預覽退回。

## 重現與驗證

```sh
cmake -S engine/tools/model_mixed_probe -B build/model-mixed-probe \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DLLAVON_IME_WARNINGS_AS_ERRORS=ON
cmake --build build/model-mixed-probe --target typing_usability_probe --parallel 6
build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/typing_usability_cases.json \
  build/model-mixed-probe/typing-report.json 60
```

加上 `focused` 可只跑候選與標點時序；`repaired-only` 可只跑全文錯鍵修復。
此次在 Apple M3／Metal／macOS 26.5.2 執行，沿用已安裝的 service binary。
原始結果分三次執行保存於 `typing-usability-report.json`、
`typing-focused-report.json`、`typing-repaired-report.json`（皆在 build 目錄）。

工具以 C++23、完整 warning flags 與 `-Werror` 建置通過。既有 unit 與 raw-key
測試 target 2/2 通過。所有測量執行成功，服務 log 沒有 `[ERR]`；core 的
working tree 仍為空。語言品質不合目標會記錄為觀察，不是 runner 執行錯誤。

這是共用引擎的確定按鍵／有限時序模擬，不是桌面真人可用性研究；沒有
測實體鍵盤漏事件、作業系統選字窗、螢幕呈現、應用程式快捷鍵衝突或所有
錯鍵修復策略。完整 JSON 保留目標、實際提交、修復按鍵數、候選與操作軌跡。
