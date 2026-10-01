# 單一明確修復請求：固定政策與驗證

## 結論

**工作／修正範圍約束通過，但效益證據不足，保留研究原型，不進入
正式預設。** 沒有為保住上一輪六個成功案例而增加例外、擴大推論
預算或重新排列候選。

新凍結 synthetic challenge 驗證了成對意圖、零額外自動修正與一個
請求上限；它原本就已能在首頁選到所有目標，不能拿零增益來判定
語言能力無效，也不能拿零新增破壞當成普遍安全證明。既有文章的
消融只增加一個可修復子句，仍需更有代表性的互動資料。

## 觀測前固定的政策（2026-09-30）

依使用者要求避免過度修正，本輪只研究一個獨立「補候選」請求的
離線原型。這個請求與既有數字鍵手選不同；**普通手選仍代表固定
確切文字**，不能偷偷重新解釋為允許模型重寫。未接入正式前端。

固定模擬操作：在已 settle 的九列首頁中，指定第一個 `char_index=0`
且含模型支援完整讀音的非 raw path。指定的是候選結構，不是標籤
中的正確字；政策完全不讀 expected。這是固定 actor 的能力研究，
不聲稱真人一定會挑這列或理解這種操作。

每次請求最多 **一個 Predict、零重試、一個候選 path**。只處理該
path 的第一個連續支援注音 run，後面 run 不處理；前面確切文字作
context。沒有合適列、沒有新結果或服務失敗時不增加建議。這個
上限是運算／修正範圍約束，不是從六個既有成功案例調出的門檻。

新增結果仍是原結構內的同音字解釋：保留 raw、原預覽與全部原 paths，
僅在來源 path 後插入一個不同 rendered 的新 path。不比較跨切分
probability；不做拼寫／錯鍵自動修正。正常打字、停頓或 ASCII 意圖
沒有這個明確請求時，額外工作為零。

## 驗收與資料角色

門檻於執行前固定：

- 每個明確事件額外 Predict ≤ 1；未請求的控制組額外 Predict = 0。
- 主預覽完全不變、raw 可選、原本首頁可修復的完整目標不能消失。
- 明確選擇後，提交必須與被選中的完整文稿相同。
- 推論回應須符合 correlation／讀音；失敗報告且保留原行為。
- 分開報收益、無新增建議、丟失目標、負控制既有錯誤與新增錯改；
  不以收益抵銷破壞。未通過就否決，不能在原驗證集上再調政策。

先凍結程式、政策與工作預算，再產生新 synthetic boundary challenge：
按新根名稱成組，三種形狀（identifier、email、domain）、三種中文
前綴。每一組保存相同 raw／context 的混輸意圖與完整 ASCII 意圖。
兩種鍵盤與 suffix／意圖是同組衍生資料，不能當獨立真人樣本。

新 synthetic 集只驗證結構邊界與負控制，不代表真人文章分布。
來源／generator 與歷史實驗有相似性，所以稱「新凍結 challenge」，
不稱獨立自然語料盲測。另跑既有 14 篇文章作 development 消融，
不根據結果增加特例。

原始輸入、policy/source/binary/model hashes 與門檻先寫入新目錄，
才啟動模型。看過之後該 challenge 即轉為回歸資料。

## 1. 新凍結 challenge：成對意圖與負控制

先固定 `explicit-first-visible-one-run-v1`、binary／source／policy
摘要，才用 seed `2026093003` 生成案例並寫入 fixture SHA-256：

```text
f2db2dc6d077bf68eb51ffa93e0af144926b40892980cd9bd5d61a170136bda3
```

三個名稱族，各八個根：technical、隨機小寫字母、hex-like 名稱。
每根三種結構、三種中文前綴、兩種相反意圖、兩種鍵盤，共 **864
個組合、24 個根名稱組**；不能當成 864 篇獨立文章。

前綴為「設定」、「請看」、「今天」。完整 ASCII 意圖指同一串 raw
keys 原樣作文字輸入；標準鍵盤可能包含聲調鍵、斜線或空格，所以
並非每個完整 raw 都是合法 identifier。shape 標籤描述後接的名稱
部分，不能把這批資料包裝成全是合法帳號的母體評測。

每對意圖的 raw、context、正式 baseline 預覽均相同，pair mismatch
為零。模型政策不讀語言答案；有明確請求的混輸 actor 與沒有請求
的 ASCII actor 分開觀測。

| 指標 | 明確請求 | 未請求 ASCII 控制 | 合計 |
| --- | ---: | ---: | ---: |
| 組合數 | 432 | 432 | 864 |
| 原預覽正確或首頁可修復 | 432 | 432 | 864 |
| 原始自動預覽符合意圖 | 216 | 144 | 360 |
| 額外 Predict | 432 | **0** | 432 |
| 每事件最大額外 Predict | **1** | **0** | 1 |
| 新增不同顯示候選 | 0 | 0 | 0 |
| 原目標消失／主預覽改變／raw 消失 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| 明確選擇與提交不一致 | 0 | 0 | 0 |

**既有自動預覽並非完全正確**：432 個 ASCII 對立意圖只有 144 個
自動預覽原樣符合，另外 288 個原本就不符合。這不是本輪新增
修正造成，也不能用 raw 候選可選把它稱為「零自動錯改」。對立
意圖的區分仍需要額外訊號；本輪沒有調 baseline 來消掉这些結果。

432 次模型工作都沒有新增不同顯示 path，因為結果與既有 paths
重複。這說明有工作预算上限也可能做了無用工作；「未新增建議」
不是模型已校準地判斷不該修正。原首頁目標已全可達，這批資料
主要檢查候選保護與成本，沒有未修復樣本來量測額外修復能力。

有請求的工作時間中位 16.4 ms、p95 17.7 ms，包含本次額外 transport
與 session／Predict／close；不包含原本主預覽 settle，也不是 OS
畫面延遲。這是單機離線觀測，未測與前景推論競爭的 tail latency。

## 2. 既有文章的固定政策消融

同一個凍結 binary 與政策再跑既有 14 篇文章、63 子句、兩種鍵盤。
資料內容與先前的展開 fixture 相同，SHA-256：
`f3f2a040594f1874b7b2a9c403296f65c161e4a8d7b208446e381cb137615673`。

| 指標 | 不補候選 | 上一輪停頓四路 | 本輪明確單路／單請求 |
| --- | ---: | ---: | ---: |
| 預覽正確或首頁可修復 | 87/126 | 93/126 | **88/126** |
| 新增可修復 | — | 6 | **1** |
| 額外 Predict | 0 | 362 | **125** |
| 每快照最大額外 Predict | 0 | 10 | **1** |
| 原可修復目標消失 | — | 0 | 0 |

本輪唯一收益是標準鍵盤的 `structured-roundtrip:3`：可選
「我會寄到admin@example.net」。許氏仍沒有新增收益。
125/126 個快照沒有新增不同顯示候選；額外工作中位 30.6 ms、
p95 58.2 ms。不同實驗工作量／actor 不同，不把這些時間視為
嚴格隔離其他因素的效能因果實驗。

沒有為其他五個上一輪收益增加第二次請求、另選更好的 source row，
也沒有使用 expected 來選要預測的切分。一般數字鍵手選仍是固定
確切字，不是此原型的「補字」事件。

## 3. 研究決定

兩份 `decision.json` 的 `acceptance_passed=true` 指**既定行為門檻**
通過，不是產品效益或普遍泛化已證明。此次結論為：

1. 一個事件／一個 Predict 的範圍可以限制住，並保留原解釋與提交。
2. **機械地重算第一個可見中文解釋，收益不足**；只減少請求不代表
   得到值得加入正式 UI 的功能。
3. 下一步先量測真正的候選選擇／修復需求，以及既有結果的 cache
   可重用性；本輪不根據這批失敗／零增益案例再調 source 排名。
4. 864 組 challenge 現在轉成回歸資料；下個政策另用未看的資料
   驗證，並補真人或獨立來源文章。沒有宣稱已完成真人驗證。

本輪只修改研究 runner／orchestrator 與文件，沒有變更正式 engine
按鍵行為、設定預設或 `ime-core`。

## 重現

```sh
cmake --build build/model-mixed-probe --target typing_usability_probe --parallel 6

python3 engine/tools/evaluate_bounded_repair.py \
  build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  build/model-mixed-probe/bounded-challenge-new-run

python3 engine/tools/evaluate_bounded_repair.py \
  build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  build/model-mixed-probe/bounded-development-new-run \
  --development engine/tests/rawkey/structured_boundary_cases.json
```

目錄只能新建。`frozen-policy.json` 記錄觀測前的 source／binary／model
摘要；`preregistered-protocol.md` 是內容與該摘要相符的原始政策文件，
不含後來追加的結果章節。這些摘要不是伺服器簽章或獨立時間戳證明。

## 驗證紀錄

- Research probe 用 C++23／完整 warnings／`-Werror` 建置通過。
- 兩輪真實服務觀測完成；864 個新組合與 125 個有面板的既有子句，
  共 989 次 raw-key 選擇／提交檢查通過；另有一個單列尾巴不開面板。
- 完整 engine unit／raw-key targets **2/2 通過**（ctest，約 65.1 秒）。
- Python syntax 與本輪 `git diff --check` 通過；core submodule 為乾淨。
