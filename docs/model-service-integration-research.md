# 現有模型／服務整合研究

## 結論

**適合借鑑的是「可逆 lattice ＋受約束模型 ＋明確手選 ＋有限推論
預算」；現有模型不適合被當成任意英文／中文切分的自動裁判。**

已完成四個開源輸入法的原始碼／作者文件比較、真實服務能力觀測，
以及停頓時補候選的離線 raw-key 原型。後者只在標準鍵盤看到有限
收益；許氏沒有收益，額外請求不便宜，尚不足以進入正式預設。

本輪加入的是研究工具與報告；`ime-core` 與正式 runtime 沒有本輪
改動。各層可用訊號與候選政策分別見以下章節。

## 2026-09-30：觀測前固定的研究條件

本輪先研究輸入法的可借鑑架構，再查清現有服務可以提供的訊號。
不重新啟用已否決的 structured stem 降分；既有否決紀錄見
`structured-scoring-validation.md`。不修改 `ime-core`。

### 能力實驗，並非語言準確率驗證

`engine/tools/audit_model_service.py` 先產生並 hash 26 個查詢、13 組成對
假說、token trace、模型／服務／probe／core 原始檔摘要，才啟動服務。
使用現有 `ServiceTransport` 與正式 protocol；不寫入訓練紀錄。

固定驗收條件：

1. 相同 prompt token 與 chosen decode 行為的查詢，其未手選位置的
   **完整候選列**應相同，不能只比較第一候選。
2. 手選位置必須原樣回傳 singleton；其他候選必須符合輸入讀音。
3. 正序重用 session、倒序重用 session、新 session 三種歷史下，
   相同查詢的候選列應一致，以排除 accidental KV cache 汙染。
4. 不同 token 的控制組不要求第一候選不同：token 有差異只代表
   訊號可見，不代表模型理解它或提升準確率。
5. 不將有限能力查詢稱為自然語料 holdout，也不從這批資料調參。

成對組包括不透明識別字、email、數字、大小寫、未知符號、開頭未知
符號、連字號名稱、已知英文、中文 context、chosen ASCII／數字／
中文／標點。三種歷史共 78 次觀測，cold warmup 另列。

Token trace 是對當前 core 原始碼的**離線鏡像**，只供解釋實驗，
不可作正式 tokenizer 或正式 cache key。現有 core 原始碼與實際安裝
binary 的相容性要以服務觀測驗證，不能只從原始碼推定。

## 第二個實驗：停頓時補候選的固定政策（觀測前）

使用既有 14 篇文章／63 子句的 development／回歸資料，兩種鍵盤。
這批資料已看過，不稱盲測。先讓正式單路預覽 settle，再在獨立 shadow
raw-key harness 的相同輸入快照上，最多研究四個不同讀音／字母切分。
四是沿用上一輪的工作預算，沒有為本輪資料搜尋最漂亮的設定。

候選插入政策不讀目標文字：保留 raw、原預覽及**所有原 paths**；在
每個原 path 後面附加結構相同但同音字被模型改善的 path（按 rendered
去重）。不使用模型順位重排中英切分，也不變更主預覽。實驗中的
shadow 推論完成後才產生面板；這是停頓後的樂觀可修復性上界，並非
正式 timer、實際畫面反應或真人選字研究。

預先門檻：raw 候選仍在首頁、原預覽完全不變、原本在首頁可修復的
目標不能消失、明確選擇後提交與選中的文字一致。只報每個快照額外
請求、等待、首頁可修復率及損失，不把候選增益當自動準確率。
若正確舊候選被擠出首頁，否決這個插入政策，而非另調排序或白名單。

## 1. 別人怎麼做，以及能借鑑到哪裡

引用的是實作或作者原文，不將其他輸入法的日文／中文評測數字當作
本專案可達到的品質。以下 source snapshot 均固定 commit。

### Rime：辨識、切分、翻譯與 grammar 是不同層

- [`recognizer.cc`](https://github.com/rime/librime/blob/388911c517155eb09f7922db90771e31eaa71e54/src/rime/gear/recognizer.cc)
  只在 active input、既有／已確認 segment 邊界上匹配設定的 pattern。
- [`matcher.cc`](https://github.com/rime/librime/blob/388911c517155eb09f7922db90771e31eaa71e54/src/rime/gear/matcher.cc)
  將匹配範圍標成 segment tag；不能從某個 pattern 自動推出使用者
  一定想打英文或一定想打中文。
- [`poet.cc`](https://github.com/rime/librime/blob/388911c517155eb09f7922db90771e31eaa71e54/src/rime/gear/poet.cc)
  在合法 word graph 上用 grammar 評分，依設定選 DP 或 beam search。

對我們：structured shape 有助保護字串與提供候選，但不要再將
不熟悉的名字視為中文證據。raw 與合法中英解釋應共存。

### Mozc：保留未知文字路徑，約束、錯鍵與後處理可分離

[`immutable_converter.cc`](https://github.com/google/mozc/blob/a069a88d4cb5c011de0f9aebb6c149a1c808d904/src/converter/immutable_converter.cc)
的 `AddCharacterTypeBasedNodes` 插入原 key/value 的未知字元／同類
字串節點；`ViterbiInternal` 計算詞成本與 transition cost；固定
segment 另有約束。`InsertCorrectedNodes` 對候選加錯鍵成本，且使用者
調整邊界後不再套同一個 key corrector。

對我們：原字串是候選搜尋的一部分；若以後研究 typo correction，
應為可選、可回復的額外解釋，不能把未知識別字直接改掉。這不是
照搬 Mozc 的成本常數；它們依賴該專案詞庫與日文輸入設定。

### McBopomofo：reading grid 與個人手選紀錄

- [`Gramambular2 README`](https://github.com/openvanilla/McBopomofo/blob/be6564acad6c4d3265c34a2e1a872d80f9db6068/Source/Engine/gramambular2/README.md)
  說明用 unigram model 進行 reading segmentation 的基本架構。
- [`reading_grid.cpp`](https://github.com/openvanilla/McBopomofo/blob/be6564acad6c4d3265c34a2e1a872d80f9db6068/Source/Engine/gramambular2/reading_grid.cpp)
  有插入、刪除、游標與 override；這些是可編輯組字操作。
- [`UserOverrideModel.cpp`](https://github.com/openvanilla/McBopomofo/blob/be6564acad6c4d3265c34a2e1a872d80f9db6068/Source/Engine/UserOverrideModel.cpp)
  觀察手選前後 walk，以局部 reading/value context、次數與時間衰退
  提供建議，並使用有限容量 LRU。

對我們：確切手選比猜測「停頓代表中文」更有意圖資訊；可研究本地
候選記憶或明確詞組 override。對方的衰退常數／三字限制不直接移植。
我們現有 `CommitEntry` 已區分 `manually_selected` 與 `literal`，
服務也有 staged commit／即時 correction discard，可沿用資料語意。
**普通 Enter 接受模型答案不等於使用者已校正該答案**，不能把兩種
事件當同樣強的個人偏好監督。

### azooKey／Zenzai：古典候選作 draft，神經模型給約束，限制推論

- [作者 2024 年技術說明](https://zenn.dev/azookey/articles/ea15bacf81521e)
  描述以古典候選作 draft，模型找第一個不接受的位置，回傳 prefix
  constraint 後再搜尋；也討論每次输入推論上限及沿用前次 prefix。
- [當前 Zenzai API 文件](https://github.com/azooKey/AzooKeyKanaKanjiConverter/blob/d59a28e4c7ca049aef04f29a91eae9677a7753f2/Docs/zenzai.md)
  有 `inferenceLimit`、模型版本相依的左右 context。這些格式是其
  模型訓練支援的功能，不是任意模型都會理解的 prompt magic。

對我們：保留傳統候選／閱讀約束、限制模型工作、不要一律重算多個
候選是有用的原則。但**現在的 service API 沒有 draft verification
或第一個 mismatch/logits**；已有的 prefix cache 不等於 speculative
decoding，也不能只送更多 `Predict` 就宣稱得到相同加速。

## 2. 現有模型／服務到底提供了什麼

### 同音字概率不能判斷這串 raw 是否應該轉中文

Read-only core 的 `masked_predict`（`llama_engine.hpp:809`）只對該
讀音允許的字取 logits，再做候選內 softmax：

```text
q(c | allowed reading) = exp(logit(c)) / sum(exp(logit(h)), h in homophones)
```

分母不包含 ASCII、其他讀音或其他切分。即使把 core 的 float 穿過
服務，它也只是候選內條件分布，不是 `P(這個 raw 是中文)`。兩組
整體 likelihood 極不同的 homophone logits，normalize 後仍可以有
完全相同的 `0.9 / 0.1`；候選只有一字時更會得到 1。

另外 tokenizer 先將**整份給定 padding**放入 input，decoder 再逐字
greedy 選字。後面字的列是 conditioned on 前面 greedy/chosen 字，
不是每個位置相互獨立的機率。不能把候選順位相加，或任取第二候選
後還假設後面的第一候選仍是相同條件。

服務 `session_engine.cpp:45` 確實丟掉 probability，現有
`protocol::Prediction` 只保留候選列。這裡有**兩層限制**：
傳輸未提供分數，以及 core 分數本身沒有跨 raw／中英切分的語意。
只修改服務傳輸第一層，無法解決第二層。

### 英文 context 與 chosen ASCII 走不同 token 路徑

Canonical table 有 14,141 個 char tokens、1,744 個 Latin tokens。
`tokenizer.hpp:63` 將 context 中連續 ASCII 字母／數字／`-_+` 合為
一個 lower-case Latin run：已知詞取 latin token，未知整段用 `<LATIN>`。
所以不同不透明帳號、數字與版本內容可能對模型完全相同。

`chosen_char` 則只查 chars table，不查 Latin run。在当前 table，
ASCII 字母、數字與空格都没有 char token；許多 ASCII 標點有。
字母／數字 chosen 會進 input `<UNK>`，且 greedy output 前綴不會
append 不支援的 char token；服務仍會原樣 echo singleton。

```text
context="請檢查quasar_cache_73"   -> 請 檢 查 <LATIN>
context="請檢查mistral_cache_91"  -> 請 檢 查 <LATIN>

context="請使用Python"          -> 請 使 用 latin:python
context="請使用pYtHoN"          -> 請 使 用 latin:python

chosen='a' / chosen='z'           -> input <UNK>, output token omitted
service returned 'a' / 'z'        -> echo preserved, not evidence of model understanding
```

因此現在以 Latin string 作後一個中文 run 的 **context**，比把字母
逐字假裝成支援的 `chosen_char` 更符合現有 token schema；原樣保留
ASCII 仍由 engine 負責。也不能任意把 `identifier_42` 切成模型熟悉
的英文詞来冒充更好的 context；那会改变 token 分布，需要另一個
獨立語言品質實驗。

### 真實服務能力結果

`service-capability-20260930/capability-report.json`：

- 26 個查詢／13 組成對假說／三種 request-history，共 **78 次觀測**。
- 9 組相同 token／chosen decode 的對照，**27/27 組完整未手選
  候選列完全相同**，不是只看 top-1。
- 26 個查詢在正序、倒序與 fresh session 的候選列皆一致。
- 所有 chosen singleton／同音字／correlation 驗收通過。
- 四組不同-token 控制的候選列均不同，但其中三組的 top-1 相同：
  能看到訊號不代表模型會用它得到不同答案。

fixture SHA-256：
`1269da7bbc348824756c413dda8679d4d3911702f316b601fb1d4cfed07c7bee`。
服務 binary、模型與 source/table hashes 詳見 `manifest.json`。
觀測支持當前 tokenizer 的能力限制，不是估計自然分布錯誤率。

## 3. 停頓時補候選：有小幅收益，但尚不足以正式啟用

`pause-candidates-20260930-r3/decision.json`：

| 結果 | 標準 | 許氏 | 合計 |
| --- | ---: | ---: | ---: |
| 子句快照 | 63 | 63 | 126 |
| 有至少兩列候選，可做補候選 | 62 | 63 | 125 |
| 原預覽已正確或首頁可修復 | 45 | 42 | 87 |
| 補候選後預覽已正確或首頁可修復 | 51 | 42 | 93 |
| 新增可修復／丟失原可修復 | 6 / 0 | 0 / 0 | 6 / 0 |
| 額外 Predict 請求 | 165 | 197 | 362 |
| 新增不同顯示 path | 30 | 43 | 73 |
| 原預覽改變／raw 消失／提交不一致 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |

125 次有候選面板的 raw-key 明確選擇與立即 Enter，全部與選中的
文字一致。未找到標籤時選 raw；因此不是 125 次全部語言正確。
沒有服務／模型失敗。單一 `.md` 尾巴只有 raw 一列，正式引擎不開
面板，不將它冒充一次選字驗證。

具體新增首頁選項包括「剛開始的時候」、「請寄到dev@example.com」、
「這個功能是end-to-end測試」與「我的帳號是report_2026」。六個
收益來自五篇既有文章，全在標準鍵盤；不概括為一般雙鍵盤品質提升。

**成本也需正視**：每個可補候選快照平均額外 2.896 個 Predict，最大
10 個；四個 path 不等於四次服務請求，英文間隔會產生多個中文 run。
shadow session 開啟到完成的時間，中位 53.3 ms、p95 116.6 ms、最大
161.0 ms。這是本機離線快照工作時間，不是桌面畫面延遲，也沒有
量測它與快速續打的前景工作競爭時的尾延遲。

這份結果只讓固定政策取得「可再做獨立驗證」資格。尚未收集新盲測
語料或真人候選閱讀成功率；也沒有實作正式停頓 timer／面板更新。

### 測量工具修正紀錄

保留 r1／diagnostic／r2 的產物與 log，避免偷偷刪除不漂亮的執行：

- r1 對唯一 raw path 誤要求打開面板，測量因此停止；依既有引擎
  `entries.size() < 2` 的規則修正 observer，沒有改候選政策。
- r2 把 shifted capital/symbol 引發的已提交子句前綴誤當作尚未提交
  context，報了 7 個「提交不一致」。診斷顯示選中 raw 的提交實際
  正確。r3 改用**實際 document prefix ＋ buffer prefix ＋選中字串**
  比較整篇文稿，而非假設整句都還在 pending。
- 三輪資料內容與插入／推論政策不變；r2 的提交判定不應當作候選
  政策失敗證據。r3 沒有因目標而調分或換排序。

固定展開 fixture SHA-256：
`f3f2a040594f1874b7b2a9c403296f65c161e4a8d7b208446e381cb137615673`。

## 4. 與我們的模型／服務結合：研究順序

### 使用者追加約束：避免過度修正與 overfitting

後續研究以最小、可否決的政策為單位，以下作為硬性驗收規則：

1. **區分不確定與錯誤**：低詞頻、不透明名稱、停頓或模型偏好都不
   足以證明原輸入錯了。缺少明確意圖時保留原解釋，額外建議可不產生。
2. **限制修正範圍**：先研究使用者指定的一個候選結構；不得順便
   改其他邊界、拼寫、手選字或主預覽。模型輸出仍須符合原讀音，
   不把額外候選學成未知名稱的全域替換規則。
3. **先固定，再觀測**：每輪執行前固定政策、實際工作預算、資料
   分組與驗收條件。任何依失敗結果修改過的資料都轉成 development；
   下一版另取未看資料驗證，不以反覆重跑同一批資料當泛化證據。
4. **保留對立意圖與負控制**：相同 raw 的完整 ASCII／混輸意圖都
   保留；按根名稱／文章／来源分組，衍生 suffix、typo 與不同鍵盤
   版本不能跨開發／驗證集。熟悉詞、不透明名稱、數字／版本內容
   分開報，不能只挑模型擅長的輸入。
5. **收益不能抵銷破壞**：原字串被錯改、手選被覆寫、原可修復目標
   消失或提交與選中內容不一致，任何一项都否決本輪政策。額外
   按鍵、候選擁擠、等待与前景延遲也要報，不只看新增正确數。
6. **允許沒有改進**：沒有可重現的收益就保留原行為；不用名字
   白名單、fixture 特判或為消掉單一失敗而逐項加 heuristic。
   分組驗證通過只表示那批資料未發現退步，不代表普遍無風險。

前述六個新增可修復案例只作研究線索，不作調參目標。正式採用前
仍需新資料的分組驗證與時序／互動測量；維持不修改 core 的約束。

本次追加的單路／單請求原型結果見
[`bounded-explicit-repair-experiment.md`](bounded-explicit-repair-experiment.md)。
行為門檻通過，但既有文章只多一個可修復子句；新 challenge 原本
就已全可在首頁修復。保留為研究原型，沒有因這些結果再調排序。

```text
raw keys ──> 共用 decoder／可逆 paths ──> 立即主預覽
                      │                         │
                      │              完整讀音／前文／revision
                      │                         ▼
                      │                  單路非同步 Predict
                      │                         │
                      │                  同音字候選更新
                      ▼
       停頓／明確想修復時的低優先工作（待驗證）
       ├─ 保留原 paths／raw／主預覽
       ├─ 額外同音字解釋，有限 work budget
       └─ 面板開啟前固定 snapshot；面板中不換號碼
                      │
                      ▼
                明確手選並固定
                      │
                      ▼
            提交／校正紀錄（區分監督強度）
```

### 優先：一個明確修復目標，而非每個停頓固定四路

現有結果說明補候選可修復某些 structured 邊界，但許氏與平均成本
不支持直接正式啟用四路政策。下一輪先凍結**按使用者明確指定的
候選結構補字**、或只有準備修復時一次有限工作的政策；預算以
服務請求／耗時計，不能只數 path。對立 ASCII 意圖仍保留。

面板一旦打開，不應讓晚回應改變候選號碼。可研究開面板前 cache
或獨立「刷新候選」操作；不能把 async 等待包裝成阻塞 Enter。
停頓只提供運算時機，沒有因此提供中英語言意圖。

### 服務：需要排程語意，而不只是更多 concurrency

`ServiceTransport::run` 在 worker 中送一個 request，讀完 response
才取下一個；同一 transport 上背景候選不能用 service 的
`max_concurrent_predictions=2` 自動繞過前景。在另一 transport
做背景工作，也仍共享 service worker／GPU／concurrency 預算。

`SessionManager` 每 session 最多一個 predict 在途，有 global
prediction budget；預測時持有 session mutex。CloseSession 不是
可靠的即時取消已執行 GPU inference。現在 Engine latest-job 合併
與丟棄 stale response 是正確基礎，但不代表已執行的工作被取消。

下一輪若量測發現前景被背景壅塞，再研究 engine／transport 的
priority queue、未送工作丟棄、service 的 cancel-before-start 或
工作種類／budget；這些可在 engine／service 層研究，仍不修改 core。
先測停頓 0/短/長、立即續打、換 focus、面板開啟與服务重啟，分開
報 queue wait、model work、stale work、前景 p95/p99 與選字收益。

### 明確手選的本地記憶

借鑑 McBopomofo 的局部 correction memory，可與現有 phrase override
和 staged commit/discard 結合：區分手選、預設接受與立即更正。
讀音與鍵盤／context 邊界要作為 key；不要把單次選中文解釋擴大為
「這個未知 stem 永遠是中文」的全域規則。此項還未實作或量測。

### 跨中英自動排名仍需不同訊號

只公開 masked probabilities 可研究同音字不確定性，無法完成 raw／
中文／英文 competing interpretation 的 likelihood 比較。Zenzai
的 draft verifier 需要模型 logits／verification API；当前 core
公開 API 沒有此能力。因此在不改 core 的約束下，本輪選擇候選
修復與排程，不虛構一個已有的跨語言 confidence。

## 5. 重現與驗證

```sh
cmake -S engine/tools/model_mixed_probe -B build/model-mixed-probe \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DLLAVON_IME_WARNINGS_AS_ERRORS=ON
cmake --build build/model-mixed-probe \
  --target service_capability_probe typing_usability_probe --parallel 6

python3 engine/tools/audit_model_service.py \
  build/model-mixed-probe/service_capability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  build/model-mixed-probe/service-capability-new-run

python3 engine/tools/evaluate_pause_candidates.py \
  build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/structured_boundary_cases.json \
  build/model-mixed-probe/pause-candidates-new-run
```

output directory 必須是新的，不能覆寫已觀測 fixture／證據。能力
invariant 失敗會報錯；語言品質政策未過則保留 decision 否決紀錄。
所有 C++ 工具用 C++23、完整 warnings 與 `-Werror`。

本輪兩個 probe 建置與 Python syntax 檢查通過；完整 engine unit／
raw-key targets **2/2 通過**（ctest，約 65.5 秒）。本輪檔案的
`git diff --check` 通過，`git -C ime-core status --porcelain=v1` 為空。
