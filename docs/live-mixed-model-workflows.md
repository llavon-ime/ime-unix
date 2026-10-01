# 將本機模型接到更多打字流程：raw-key 實驗

## 範圍與實作

沿用現有 250M Q4_K_M 模型與已安裝服務，不改 `ime-core` 或服務 API。
這次將模型接到 **真實 raw-key harness 的 pending 路徑**，不是只在提交
後替報告中的字串重排。新增 `live_mixed_model_preview.hpp` 作為實驗 adapter；
正式 Engine 的呼叫政策尚未切換到此 adapter。

比較三個流程：

1. **baseline**：目前 Engine 的正常行為。
2. **live-best**：完整注音出現時，非同步更新顯示路徑的同音字候選。
3. **live-candidates**：再預測至多四個不同注音／英文邊界的候選路徑，
   留給使用者選字；不使用模型機率比較不同中英切分。

模型返回的候選寫回 `MixedPath` 的 `candidates` 與 `rendered`；同一份
狀態提供預覽、候選清單與 Enter 提交。原始按鍵保持不變。此輪歷史
實驗的 Backspace 刪 raw keys；後續正式編輯行為改為 Backspace 刪
顯示字、Shift+Backspace 撤銷未確認 raw key，對應回歸也已更新。
詳見[編輯行為](smart-mixed-input-editing.md)。字母、email 等已被
decoder 決定為字面內容時保持原文。

```text
raw keys ──→ 現有 decoder ──→ 離線預覽
                    │
                    ├─ 完整注音＋文件前文＋已選字＋英文前綴
                    │                    ↓
                    │              既有服務／模型
                    │                    ↓
                    └─ 新鮮回應 ──→ 同音字候選／預覽 ──→ Enter
                         │
                         └─ 過期、已取消、已手選、已開候選窗：捨棄
```

實驗處理了：

- 只有完整讀音／有效模型前文變更才需要新 query；不完整尾音沿用已取得
  的共同前綴候選，避免每個聲母、韻母都重跑整段模型。
- 單個 job 在途，更新欲處理的最新 snapshot；回應版本不符就捨棄並處理
  最新 snapshot，不並行對同一模型 session 發請求。
- 比對 raw revision／內容、buffer revision、游標、layout、敏感欄位、
  文件前文與 preview path；候選面板打開後不接受重新排序。
- 驗證 session／request ID／revision；僅接受 canonical table 內的
  同音字，保留模型順序，再補齊其他同音字。
- 模型結果不能覆寫 buffer 中的手選字；buffer 的已選內容成為後續
  pending 預測的前文。
- 英文前綴以確切原文傳入 context；敏感欄位排除文件前文，context
  依現有 context-length 預算裁切。
- 在四路候選模式中，**顯示路徑先發布**，再背景處理其他路徑，避免
  候選預取讓正常打字的預覽變慢。
- 使用服務的 `tokens/bpmf.json` 檢查可用讀音；fallback 可處理但模型
  沒有 token 的罕見音節保留原候選，作為下一段預測的文字前文。

## 擴充案例

`model_workflow_cases.json` 繼承上一輪的四篇文章與 15 個編輯流程，另加入：

- 八篇文章：家人聯絡、購物、研究筆記、旅遊行程、團隊聊天、部署指令、
  資料分析、英文縮寫與標點。
- 22 個流程：文件前文消歧義、低順位同音字、手選字保護、候選窗等待、
  模型在途時改聲調／取消／切焦點、許氏中文前綴變英文、錯聲調未修復、
  英文拼錯未修復、縮寫、識別字、email、URL、敏感前文與鍵盤切換。
- 八個提交時序 seed：常用中文、低順位字、中文英文中文、英文中文、
  一聲歧義、前文、數字邊界、純英文。每個跑 0／20／60 ms 每鍵，以及
  Enter 前等待 0／120 ms。

總計 **12 篇文章、55 個子句、37 個流程、8 個時序 seed**。每種鍵盤的
全文目標合計 844 字元（含標點）；兩種鍵盤合計 1,688 字元。每種鍵盤都
完整回放；目標字／讀音由 fixture 明確標注並經 canonical table 與實際
keymap 驗證。沒有以目標答案調模型輸出或強制選字，首頁候選修復測量
  僅在選字模擬階段查找目標，與模型推論分開。

## 完整注音出現時就選字：改善主要在預覽與短句 Enter

文章每鍵 60 ms、標點後等待 120 ms、句號後 Enter；使用者不額外等候
實驗 adapter 完成，也不自動選字。分別測三組，共 72 次全文輸入。

| 指標 | 現行 baseline | live-best |
| --- | ---: | ---: |
| 標準：整個已輸入前綴完全符合目標 | 148/480 | 311/480 |
| 許氏：整個已輸入前綴完全符合目標 | 155/480 | 332/480 |
| 標準：全文 CER | 15.76% | 15.40% |
| 許氏：全文 CER | 21.09% | 20.73% |
| 全文完全正確 | 9/24 | 9/24 |

前綴測點包含每個中文字與完整英文片段，不是逐字準確率；前面一個字
錯會讓後續整個前綴都不合目標。改善從 303/960 到 643/960 反映預覽
較早接近目標，不能把它直接稱為一般打字準確率。

**最終文章改善小，是因為 baseline 在中文標點後本來就會走模型。**
最明顯的額外收益出現在預覽，以及模型來得及在大寫英文提前提交前
修正中文：兩種鍵盤都將 `在卻任WebSocket` 改成 `再確認WebSocket`。
email、數字、識別字吞掉的中文依然不能靠同音字重排復原。

更實際的收益出現在 **不等標點、直接 Enter 的子句**。以正確前文分別
測 55 個子句，沒有先選字時：

| 鍵盤 | baseline 正確 | live-best 正確 |
| --- | ---: | ---: |
| 標準 | 9/55 | 41/55 |
| 許氏 | 7/55 | 38/55 |

此組為 burst 輸入後等當前模型完成再檢查預覽／候選，隔離「模型能不能
選對」與「打字時來不來得及」；下一節另外測不等待的實際提交。

## Enter 的時機與「螢幕看見什麼就提交什麼」

八個時序 seed 各跑兩種鍵盤、兩種流程、三種每鍵間隔、兩種 Enter 前
等待，共 **192 個組合**。全數 **192/192 提交與按 Enter 前顯示一致**，
晚到的模型回應不會改已提交文字，也沒有拿 fixture 目標替提交內容。

| 每鍵間隔／Enter 前等待 | 標準 baseline | 標準 live-best | 許氏 baseline | 許氏 live-best |
| --- | ---: | ---: | ---: | ---: |
| 0 ms／0 ms | 3/8 | 3/8 | 4/8 | 4/8 |
| 20 ms／0 ms | 3/8 | 4/8 | 4/8 | 5/8 |
| 60 ms／0 ms | 3/8 | 6/8 | 4/8 | 7/8 |
| 0 ms／120 ms | 3/8 | 6/8 | 4/8 | 7/8 |

每鍵 20 或 60 ms 後再等 120 ms，結果也分別為標準 6/8、許氏 7/8。
標準的一聲「剛開始」與數字邊界、許氏的數字邊界依然有錯誤。
因此正式整合不應強行等模型阻塞 Enter；應優先讓結果更早到達預覽。

live-best 各篇暖機後 job 中位數約 12–32 ms、p95 約 22–58 ms；包含
host 排程與多段中文 query，不含 OS 顯示。不是首次載入延遲，也不是
不同模型的公平速度 benchmark。

## 前文、手選字、錯鍵修復等流程

37 個流程各跑兩種鍵盤與三個比較組，共 222 次。baseline 為每種鍵盤
29/37，live-best 為每種鍵盤 **34/37**。

直接改善的例子：

```text
前文：請把資料寄到我的電子    信鄉 → 信箱
前文：這個專案需要重新設計軟體  價夠 → 架構
前文：奶奶今天煮了一鍋       機湯 → 雞湯
前文：我們用新的輸入法進行模型  冊市 → 測試
```

模型也有失敗：前文提到物理學仍把「公式」選成「工事」，「鮮香」選成
「先香」。小模型加前文並不保證所有同音字都能正確消歧義。

未修的「你號」在模型組輸出「你好」，但這 **不是修改 raw 聲調**：
canonical table 的 `ㄏㄠˋ` 本來也包含「好」（如喜好），模型仍在同音字
集合內選字。不能由此宣稱能修正所有聲調或漏鍵。未修的英文 `helo`
在所有組別仍為 `helo`，此 API 沒有做英文拼字修正。

原有 15 個編輯流程與擴充的手選、面板穩定、在途修改／取消／焦點切換、
識別字與 URL 保存都沒有被這個 adapter 破壞；質量不合目標的流程
保存為結果，並非 runner 執行失敗。

另外以突發 raw keys 注入 **1,452 個錯鍵樣本**（標準 728、許氏 724），
停下後等 live-best 完成，再與同一模型流程的無錯鍵版本比較：

- 立即發現刪回重打：**1,452/1,452** 回到無錯鍵預覽與提交。
- 再打六鍵才發現、刪回重打：**1,452/1,452** 恢復。
- 延後修復平均增加約 12.7 次按鍵，最多 16 次。
- 兩種修復流程合計丟棄 **3,299 個過期 job**，沒有模型回應失敗。

這些不是 1,452 個獨立自然句子，而是固定 seed／位置／錯誤類型組合。
沒有手動修復時，1,333 個樣本仍不同於無錯鍵輸出。恢復到無錯鍵參考
不代表原本的中英切分也正確。

## 四路候選預取的實驗演進

第一版在四路都完成後才發布，導致主預覽被其他路徑拖慢。改成先發布
主路徑後，腳本服務測試證明即使第二個 query 暫停，主預覽也先顯示，
而 Enter、手選與候選窗優先權仍保持。

進一步發現 fallback 字表與模型讀音 vocabulary 不完全相同。例如
`ㄍ `、`ㄊ `、`ㄝˋ`、`ㄡˊ` 存在於候選路徑，服務卻會拒絕。不是傳輸
或讀音標注錯誤；原四路文章測量有 14 個這類拒絕，live-best 沒有遇到。
最後版依服務的 token table 分段處理，保留這些音節的既有候選，把其
文字當作後段 context，避免一個罕見音節讓整段 query 失敗。

最後版重跑全文、子句與流程，文章模型回應拒絕數 **14 → 0**。主預覽
與 live-best 的全文輸出相同；首頁候選修復能力如下：

| 鍵盤 | baseline：原本＋首頁修復 | live-best：原本＋首頁修復 | 四路預取：原本＋首頁修復 |
| --- | ---: | ---: | ---: |
| 標準 | 19/55 | 42/55 | 46/55 |
| 許氏 | 18/55 | 41/55 | 41/55 |

四路預取額外救回標準的「剛開始的時候」、「測試報告請寄到 email」、
「我會用pandas計算平均值」、「這個功能是end-to-end測試」四個子句。
這是讓較好的模型候選進入首頁，**不是自動選對新的中英切分**。

代價是文章部分的額外請求從 live-best 合計 **946** 增為四路的 **4,396**
（約 4.6 倍）。主預覽正確前綴為 640/960，並未優於單路的 643/960；
相同全文結果下，全面每鍵預取沒有足夠收益支持直接當預設。

## 優先整合哪些流程

1. **一般連續組字與直接 Enter**：最有證據的收益；完整讀音更新模型，
   不完整尾音沿用結果，必須保留可逆 raw keys。
2. **大寫英文前的中文**：在既有 Shift 提前提交之前改善中文預覽，
   無需改模型或把已提交文字偷偷重寫。
3. **有前文／手選字的後續選字**：將確切文件、手選字和英文前綴送入
   模型，保留使用者選字優先權，提供低順位候選供快速修正。
4. **按 Down 才做更多候選預取**：四路處理增加請求成本，應優先主路徑，
   把更多路徑留給停頓或選字時，而非每鍵都全面推論。
5. **切分錯誤與錯讀音**：需要另建可比較的切分證據／修復候選，現有
   同音字內候選順位不足以直接做跨語言或拼字修復。

數字、email、URL、`C++23` 中的符號與字面鍵保護需要 decoder／按鍵規則
配合；把它們全部交給現在的模型重排不會自動變好。

## 重現

```sh
cmake -S engine/tools/model_mixed_probe -B build/model-mixed-probe \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DLLAVON_IME_WARNINGS_AS_ERRORS=ON
cmake --build build/model-mixed-probe --target typing_usability_probe --parallel 6
build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/model_workflow_cases.json \
  build/model-mixed-probe/live-model-expanded.json 60 model-comparison
```

其他最後一個參數：

- `model-candidates`：僅跑顯示路徑優先的四路預取，方便比較發布政策。
- `model-timing`：僅跑 192 個提交時序組合。
- `model-typos`：以突發 raw keys 注入錯鍵，等待 live-best 完成後比較
  無錯鍵參考與立刻／晚六鍵修復結果。這是停止輸入後的模型結果，不能
  當成真人每鍵延遲。

腳本服務回歸 `live_mixed_model_tests.cpp` 編入標準 raw-key target，不需要
安裝模型即可檢查過期回應、候選窗凍結、Enter／Escape／重設／焦點優先、
手選保護、context、非同音字拒絕、錯誤 correlation、服務錯誤與候選預取
不得阻塞主預覽。

此回歸含 17 個決定性情境；完整 unit 與 raw-key target **2/2 通過**。
工具以 C++23、既有完整 warning flags、`-Werror` 建置，core working tree
仍為空；沒有提交或推送。

此次測量分多次啟動服務，依序執行，沒有同時爭用 Metal 來比較延遲：

- `live-model-expanded.json`：baseline、live-best、原始四路策略；文章
  baseline／live-best 與該組子句、流程數字取自此檔。
- `live-model-best-first.json`：主預覽優先，仍有未支援讀音拒絕。
- `live-model-prefetch-diagnostic.json`：縮小至三篇文章，辨識拒絕原因。
- `live-model-token-aware.json`：最終四路策略，全文與操作測量重跑。
- `live-model-timing.json`：192 個提交時序組合。
- `live-model-typos.json`：1,452 個錯鍵及兩種修復，使用主路徑優先版本。

這些檔案位於 `build/model-mixed-probe/`；後兩組與 baseline／live-best
沒有遭遇未支援 token，token-aware 修正主要針對四路的額外候選。

```sh
build/engine-tests/tests/llavon_ime_rawkey_tests 'live mixed model preview'
ctest --test-dir build/engine-tests --output-on-failure
```

## 限制

模型 API 的候選順位只在指定注音內成立，沒有跨中英切分的可比較機率。
因此本次不讓「某路徑的模型很有自信」直接覆寫 decoder 的中英判斷。
錯聲調、漏鍵、交換鍵需要候選讀音修復或讓使用者確認；不能把選同音字
當成自動更正所有錯鍵。

這是小型、手寫語料與固定時間的共用引擎模擬，未涵蓋桌面前端、真人
速度分布與長期個人化。候選修復只查首頁九項，也不代表完整候選召回率。
原始 JSON 提供各篇全文、每個子句、提交時序、修復軌跡與請求數供檢查。
