# 智慧混輸：避免結構化英文吞掉中文前綴

> **最新狀態：下列 stem 拼寫降分已撤回正式 decoder。** 獨立凍結驗證
> 發現合法不透明名稱被誤轉中文，以及正確切分候選被剪掉。下面保留
> 第一輪的歷史方法與數據，不代表目前預設行為。保留底線開頭修正、
> raw 選項與可手選的中文切分；驗證與後續研究見
> [`structured-scoring-validation.md`](structured-scoring-validation.md)。

## 發現與正式修改

上一輪即時模型整合之後，主要瓶頸仍是 decoder 的中英切分。例如：

```text
按鍵：su3cl3user_name42     （標準）
按鍵：nefhwfuser_name42    （許氏）
目標：你好user_name42
```

原本只要整串符合 identifier／email／domain／URL 的語法，整段就拿到
固定的 `-1` 分數。這會把前面的中文注音按鍵當成識別字或 email local
part；許氏的聲調又是字母，更容易被吃進英文裡。

```text
原本：中文按鍵 + 任意字母 + _／@／.／:// → 整段固定語法分數
現在：中文切分證據 ↔ 名稱／scheme 拼寫證據 + 結構化語法證據
                                  ↓
                           所有路徑仍一起排序
```

第一輪的 `engine/src/input/mixed_input_decoder.cpp` 同時評估**第一個名稱或
scheme 的拼寫**，沿用既有 `latin_score_`：

```text
structured_score = -1 + min(0, english_score(stem, false) + 3)
```

`stem` 截在第一個 `_`／`@`／`.`／`:`。`+3` 對應現有拼寫 backoff 的
初始 `-3`，已知、合理的名稱仍然得到結構化優勢；後面出現符號不會
無條件消除前面大量不合理拼寫的證據。Filesystem path 保留現有分數，
因為起始 `/` 本身已是明確結構邊界。

沒有加入新詞庫，也沒有針對網址、帳號或 fixture 的特例；不改 raw
keys、不改模型 API，也不修改 `ime-core`。

## 額外修正：開頭的底線

透過實際 raw keys 發現另一個 decoder probe 看不到的問題：智慧模式
從空組字輸入 `__private_name`，原本會把底線轉成中文破折號：

```text
__private_name → ——private_name
```

`InputProcessor::is_smart_start_char()` 現在讓 `_` 開始可逆 pending input，
符合識別字的輸入方式。智慧模式關閉時仍走既有的標點流程。

## 可重現的獨立 decoder 測量

工具 `engine/tools/evaluate_structured_boundaries.py` 用相同兩個 probe
binary，比較 30 種名稱 × identifier／email／domain，加上 URL、短名稱、
數字識別字與版本等控制組。中文前綴為「你」、「好」、「我」、「你好」。

| 類型 | 樣本數 | 修改前完全吻合 | 修改後完全吻合 |
| --- | ---: | ---: | ---: |
| 標準，單獨英文控制組 | 103 | 103 | 103 |
| 許氏，單獨英文控制組 | 103 | 103 | 103 |
| 標準，中文接結構化英文 | 412 | 9 | 349 |
| 許氏，中文接結構化英文 | 412 | 3 | 341 |

共 **1,030 個固定組合**，原本正確的組合沒有退步。這是合成邊界診斷，
不是 1,030 個獨立自然句子。它也沒有經過 Engine 的逐鍵政策，因此另
加 raw-key 與真實模型測量，不能只憑 decoder 表宣稱真人體驗已完善。

還有未解決的歧義：單字中文接不常見名字、數字前綴、`x86_64`／`mp3_file`
跟注音按鍵衝突。例如標準 `su3cl3x86_64` 仍可能成為「你好剌_64」。
保留原始輸入選項與手選能力仍然必要。

## 第一輪 Raw-key 驗證

`engine/tests/rawkey/structured_boundary_tests.cpp` 共 46 個情境，兩種鍵盤
直接輸入並檢查：

- 中文接 identifier／email／domain／URL／未知名稱，預覽與 Enter 一致。
- 候選窗仍有完整 raw 選項，手選可以提交它。
- 刪除 `_name42` 再重打，中文與英文邊界可以恢復。
- email 尾端打錯字再修正，以及 focus-out 提交。
- 短名字、未知名字、底線開頭、含數字、大小寫名稱與自訂 URL scheme。

既有 Shift-uppercase 可能先提交前段，控制組檢查完整 client 文字，而
不是要求全部內容必須留在同一個 preedit。

本輪也補上 harness 的 `engine()` accessor，讓既有更新準備狀態的
raw-key 測試能使用正式 Engine 的唯讀狀態查詢。

## 文章與模型測量方法

`structured_boundary_cases.json` 延伸上一輪 fixture，再增加兩篇文章／
八個子句與 15 個流程。最後版 runner 支援遞迴載入 `extends`，讓原本
兩層 fixture 的 **14 篇文章、63 個子句、52 個流程** 全部納入；超過
16 層會報錯，避免循環引用。第一輪只有一層繼承的診斷已被最後版
完整報告取代。

一般文章仍是每鍵 60 ms、標點後 120 ms，子句診斷等待模型完成。
正式 `SmartModelPreview` 關／開都測；與上一輪相同 fixture 的舊結果
比較時，也配對相同 layout／mode／id，避免把新語料混進舊指標。

### 正式 Engine、真實模型的最後結果

最後完整執行共 56 次文章、252 個子句診斷、208 次操作流程，全部順利
完成且無操作 exception。以下「修改前」指已接入上一輪即時模型的
正式 Engine，並不是更早的純離線模式。

| 原本 12 篇文章／兩種鍵盤 | 上一輪即時模型 | 本輪切分改善 |
| --- | ---: | ---: |
| 標準全文 CER | 15.40% | **13.27%** |
| 許氏全文 CER | 20.73% | **11.97%** |
| 整篇完全正確 | 9/24 | **11/24** |
| 整個已輸入前綴完全正確 | 641/960 | **678/960** |
| 子句直接提交正確 | 79/110 | **87/110** |
| 包含首頁選字修復的子句正確 | 83/110 | **90/110** |
| 原有操作流程符合目標 | 68/74 | **68/74** |

配對原本文章後，七次全文結果改變，都減少 edit distance；其餘文章
最終文字相同。前綴測點需要整個前綴吻合，不是逐字準確率。

實際改善例如：

```text
標準：我的測試帳cl4g4user_name42 → 我的測試帳號是user_name42
許氏：velfbyfreport_2026.csv     → 請把report_2026.csv
許氏：再把結果輸出vldoutput.json → 再把結果輸出成output.json
```

新增兩篇以結構化邊界為主的文章，使用保存的修改前 runner 另外測量：

| 新語料 | 修改前 | 修改後 |
| --- | ---: | ---: |
| 標準 CER | 31.65% | **6.96%** |
| 許氏 CER | 60.13% | **12.66%** |
| 新操作流程符合目標 | 12/30 | **30/30** |
| 新文章全文完全正確 | 0/4 | **2/4** |

新語料刻意包含多個先前失敗的邊界，不能把較大提升當成一般文章的
平均收益。原本語料與新語料的比例分開呈現。新 baseline 與 final
測量曾有部分執行重疊，使用不同服務 process；本輪沒有用 wall time
或推論延遲比較效能。

新測量還保留了結構化英文後接中文的案例，不把目前仍失敗的部分
拿掉：`snake_case` 後的「命名」、`user_2026_name` 後的「登入」仍可能
被吃進尾端名稱。純數字接中文、許氏版本號前綴也尚未完整解決。

### 時序與錯鍵

- **192/192** 個時序組合：提交與按 Enter 前的顯示一致。
- **1,684** 個錯鍵樣本，包括新增文章的 seed：立即修復與晚六鍵修復
  各 **1,684/1,684** 回到無錯鍵版本，共 3,368 次修復。
- 四次服務測量 log 均無 `[ERR]` 或 `invalid bpmf`。
- 完整 unit／raw-key target **2/2 通過**；Linux memscan 相關條件測試
  在 macOS 略過。C++23、完整警告與 `-Werror` 建置成功。

恢復無錯鍵版本不代表該版本的切分或同音字一定正確；錯鍵樣本為
固定 seed／位置／型態組合。全文同音字仍受模型品質限制，例如物理
前文的「公式」仍可能是「工事」，`helo` 也不會自動修成 `hello`。

最後原始報告為 `structured-production-final.json`、
`structured-new-baseline.json`、`structured-production-timing.json`、
`structured-production-typos.json`；第一輪一層繼承報告不納入上述表格。

## 歷史實驗重現

本輪已回到保守 structured score；以下是第一輪指令記錄，現在直接
執行不會重新得到該輪降分政策的數據。重現被否決的政策需使用當時
保存的 binary；此次 artifacts 在 `build/model-mixed-probe/`：

```sh
python3 engine/tools/evaluate_structured_boundaries.py \
  build/model-mixed-probe/structured-boundary-baseline-probe \
  build/engine-tests/tests/mixed_input_probe \
  build/model-mixed-probe/structured-boundary-sweep.json

cmake --build build/engine-tests --target llavon_ime_tests llavon_ime_rawkey_tests --parallel 6
ctest --test-dir build/engine-tests --output-on-failure

cmake --build build/model-mixed-probe --target typing_usability_probe --parallel 6
build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/structured_boundary_cases.json \
  build/model-mixed-probe/structured-production-final.json 60 production-comparison
```

新模型測量使用正式 Engine，不建立試驗 preview adapter，不把 fixture
提交送成訓練資料。桌面 App 尚未安裝或重啟；實測是共同引擎與已安裝
服務 binary，不能當成完整 macOS 畫面延遲或真人研究。
