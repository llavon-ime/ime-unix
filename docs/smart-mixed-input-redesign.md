# 智慧中英文混輸：調查與重設計

調查日期：2026-09-30。以下記錄研究依據、設計與第一版落地範圍。

## 第一版落地狀態

- 混輸語言決策集中在 `MixedInputDecoder`；移除事件處理器的最長後綴、完整英文強制覆蓋、字母長度與前次中文保留等決策分支。
- `engine/data/mixed_lexicon.inc` 內建 wordfreq 的 28,948 個英文／字母數字詞與 64,617 個傳統中文單字／詞組。token ID 不再參與頻率計算，舊的詞尾推導規則也已移除。
- `MixedLexicon` 提供真實 unigram prior、英文前綴索引、平滑字元 bigram 的未知詞拼寫分數，以及最多四字的中文 backoff phrase prior。前文使用新讀取的 client／既有上下文來源及已確認組字。
- lattice 同時保留完整與未完成中文、英文與字面路徑。中文在前四個同音字中搜尋上下文解讀，候選可呈現較早字位的變化與不同切分；原文容易取回。
- 空白是 raw event，中文一聲與英文詞界共同解碼；不再自動提交英文或整段混輸。Backspace 刪顯示字，Shift+Backspace 可撤銷未確認的空白鍵。Enter 提交，手動選過的範圍形成已確認組字，後續不會重新解析。
- 原有 44 個固定按鍵基準：重設計前 43/44，第一版 44/44。另有 28 個技術詞輸入檢查、兩種布局各 108-key 連續中文、多詞英文／中文混輸、空白撤銷、手動選字及「年＋ru6→級」上下文檢查。這些是小型回歸與效能基準，不是整體準確率，也不是大型獨立語料評估。
- 本機 macOS AppleClang 測量：固定基準按鍵 p50 7µs／p95 45µs；108-key 長段中文 p95 2.088ms／max 2.347ms。這是 host-free 同步引擎路徑，並非實際桌面端到端延遲。AppleClang 與 GCC 15 的 warning-as-error 建置及兩個測試 runner 均通過，raw-key 共 51 suites、0 failures；macOS native app 也已重建。Linux 記憶體 helper 情境沒有在本機執行。
- `mixed_input_probe` 可輸出某串按鍵的所有保留解讀及總分：例如 `build/engine-tests/tests/mixed_input_probe standard ru6 年`。它不連預測服務，可用來檢查離線排名。

目前使用的是可解釋的輕量詞典／拼寫／詞組排名；分數包含工程上設定的鍵盤與切換先驗，尚未由大型開發集校準成機率。每次按鍵對未確認範圍重算有界 beam，沒有宣稱已實作跨事件的 lattice cache。既有 core AI 仍不處理 raw-key 語言判斷；本版未新增神經混輸模型或 service 協議。短英文與許氏重碼、一般詞頻和台灣實際用語的差異、詞組短名單之外的同音字，仍需更大的雙向語料量測。

## 結論

改成「中文、英文、字面內容共同競爭的增量解碼器」，使用真實語言資料與上下文排名，並把排名、顯示、確認分開。不要再以 `InputProcessor` 中不斷增加的條件分支決定語言。

主要目標是保留正常注音的打字節奏，允許自然插入英文，並降低改選與反覆刪字的成本。許氏與標準注音共用解碼框架，按鍵到注音的轉換與歧義先驗則分開。

## 查到的實作依據

### 1. 真正的免切換混輸：並行假設與語言模型

Chen 與 Lee 的 ACL 2000 論文 *A New Statistical Approach to Chinese Pinyin Input*，第 4 節 Modeless Input，直接處理「英文也是合法拼音」與「中英文之間沒有分隔符」的問題：

- 中文輸入模型與英文拼寫模型同時運行。
- 英文由實際混輸語料的詞頻，以及能處理未知詞的拼寫模型評分。
- 歧義時兩個解讀都留在搜尋中，等待更多上下文。
- 由句子語言模型與 Viterbi beam search 決定最可能的混合輸出。
- 未知專有名詞與縮寫仍可作為英文保留。

這是拼音研究，不能直接把其準確率移用到許氏注音；但「並行解讀、未知詞模型、上下文解歧義」是可移植的架構。

來源：https://aclanthology.org/P00-1031/ （PDF 第 5–6 頁）

### 2. 開源工程參考：libime 的圖、詞典與 LM 狀態

libime 的 decoder 將輸入 segment graph 經詞典匹配建立 lattice，然後以 language-model state 做 forward search，最後產生 N-best。它不是只看某段按鍵是否有效，也不是只在最後挑一個字。

可以參考其增量 lattice、上下文狀態、beam pruning 與 N-best 分工。這份程式本身並不證明已提供我們需要的許氏中英免切換模式。

來源：

- https://github.com/fcitx/libime/blob/master/src/libime/core/decoder.cpp
- https://github.com/fcitx/libime/blob/master/src/libime/core/languagemodel.h

### 3. Rime 混輸：多翻譯器合併候選，仍有重碼限制

Rime Ice 同時掛中文與英文 translator，以 quality 合併候選，並另有降低部分短英文順位的 filter。其作者特別說明：這種英文掛載不會自動把任意 `applediannao` 組成 `Apple電腦`，常用中英混合詞需要另設詞典。

可參考它的英文詞典、前綴補全與候選呈現；不能把多 translator 合併當成已解決任意中英邊界辨識。固定「英文詞命中就必定第一」也會傷害中文。

來源：

- https://github.com/iDvel/rime-ice/blob/main/rime_ice.schema.yaml
- https://dvel.me/posts/make-rime-en-better/

### 4. 產品互動參考

ASUS 官方說明有注音／英文免 Shift 混合模式、替代文字、自動完成與使用習慣記憶；SwiftKey 官方說明會持續估計使用中的語言並调整預測。但公開說明不足以還原其演算法，也不能證明支援許氏的相同情境。

來源：

- https://www.asus.com/tw/content/smartinput/
- https://www.asus.com/tw/support/FAQ/1048621/
- https://support.microsoft.com/en-us/swiftkey-keyboard/how-to-use-microsoft-swiftkey-keyboard-with-more-than-one-language

## 調查時基線已確認的根本問題

1. **把 token ID 當詞頻。** `ime-core/table/tokens/latin.json` 目前有 1,744 個詞與唯一 ID，ID 範圍為 15,553–17,545。`ime-core/src/engine/tokenizer.hpp` 直接把這些值放入 token 序列；它們不是頻率。`engine/src/engine/fallback_engine.cpp` 卻把 ID 線性正規化成 `english_frequencies_`，所以目前的 frequency boost 沒有詞頻依據。
2. **token vocabulary 不是完整英文詞典。** 它有常見詞與縮寫，但缺少很多詞形與未知詞；靠詞表命中與少量詞尾規則不足以做語言辨識。
3. **目前的圖搜尋沒有真正的句子 LM。** `MixedInputDecoder` 使用固定 segment 加分、切換懲罰與碎片懲罰，以每個讀音的第一個候選字渲染路徑；沒有用詞組或句子合理性比較中英文。
4. **排名與事件處理互相覆蓋。** decoder 給分後，`rerun_pending_decision()` 又以完整英文、已保留中文、長度、最長後綴等分支改選結果；沒有單一可解釋的決策依據。
5. **可見候選不足。** `expand_candidates()` 固定原文第一，再只展開一個中文結尾路徑的末字候選，其他切分與較早字位的差異很難改選。
6. **既有 AI 不參與未決語言判斷。** `Engine::build_predict_request()` 只從已完成的 `session.buffer` 產生 padding；`pending_token` 的 raw keys 不在請求中。現有協議是每個注音位置的字候選，不是整段混輸路徑排名。
7. **一些測試固定了不佳互動。** 例如標準鍵盤 `283` 優先顯示數字、需要 Down 才能選「打」。回歸測試通過只是符合目前規格，不能證明自然中文輸入仍好用。

## 建議的新架構

```text
原始按鍵事件 + 鍵盤配置 + 前文 + 使用者已確認的範圍
                           ↓
                  增量候選圖（lattice）
          中文完整／未完成音節 | 英文詞／前綴 | 字面內容
                           ↓
               同一套評分、beam search、N-best
                           ↓
                  顯示與確認的互動策略
                           ↓
                 可編輯預覽 / 明確確認 / 提交
```

### 輸入與搜尋狀態

- 保存原始鍵與 modifier；空白也是事件，不只是額外傳入的 bool。
- 中文分支保留完整與未完成的合法音節；標準與許氏各自轉換按鍵。
- 英文 trie 支援完整詞及前綴，另保留未知詞的字面路徑。
- email、URL、路徑、identifier 等以 lexer 辨認；不完整結構也要有延續狀態。
- Space 可以是中文一聲，也可以是英文詞界；先產生兩種有效分支，按上下文評分。
- beam 的狀態至少包含輸入位置、未完成音節／英文前綴、語言與 LM history。不能僅因顯示文字相同就合併掉影響後續判斷的狀態。
- 已明確選過的範圍形成約束，後续輸入不能無故重新解讀它。

### 排名

使用可分解的 log-score：

```text
輸入相容分數
+ 英文詞頻／未知詞拼寫分數
+ 中文詞組／句子語言模型分數
+ 上下文與語言轉移分數
+ 使用者明確偏好
```

- 完整英文詞與詞形推導提供不同強度的證據，沒有「命中就強制英文」分支。
- 未完成英文以前綴可能性評分，不能把每個中途鍵都當完成單字。
- OOV 可先用平滑的字元 n-gram 模型；英文保留原始大小寫，不自動翻譯成中文或擅自改拼字。
- 中文至少需要詞組／上下文分數，不能只累加「每音節第一個同音字」。
- 標準純數字音節與許氏字母聲調都作為歧義特徵；不對整類輸入一律判英文。
- 語言切換與長度係數用開發集調整，留出獨立測試集；分數差距不是未經校準的「準確率」。
- `wordfreq` 是真實詞頻資料的參考候選，與 token ID 有本質差異；其 unigram 資料不能取代上下文 LM。是否採用、格式與資料版本需在資料實作時決定。

詞頻資源參考：https://github.com/rspeer/wordfreq （資料為約 2021 年的語言快照）

### 互動策略

- 預設預覽最佳解讀，不固定原文為第一候選；原文始終容易取回。
- 未完成音節與未決英文可顯示可理解的組字提示，避免所有鍵都只顯示 Latin。
- 歧義時顯示最佳的中文／英文替代；**顯示候選不等於進入按鍵選字狀態**，後續字母與聲調鍵仍繼續組字。
- 把 Space 的一聲／詞界含義與「進入候選後的 Space 選字」明確分開。
- Space 不應因一條英文 heuristic 就不可逆地提交整段中英文字；語言判定、接受詞界、提交是不同動作。
- Enter 明確提交目前預覽；原文確認提供明確操作。這是新設計建議，需與既有快捷鍵及 raw-key 規格一起實作。
- Backspace 刪除目前顯示的一個字；Shift+Backspace 撤銷最近一個未確認
  raw event（包含空白）。刪除保留顯示前綴，候選仍可重新解讀；詳見
  [編輯行為](smart-mixed-input-editing.md)。
- 可對預覽採用經量測的切換差距，減少閃動；不能以打字速度或定時器偷偷決定語言。
- async 排名只更新符合 revision 的未確認內容，不能覆蓋手動選字或已提交內容。

## 不修改 ime-core 的落地範圍

新的 lexer、英文資料、拼寫模型、混輸 ranker 與互動策略都可放在本 repo 的 `engine/`。`ime-core` 的 token 表繼續唯讀用於 tokenizer，不寫入、不改 submodule 指向。

先做可獨立運行的低延遲混輸解碼器。需要 AI 重排時，在本 repo 的 service 增加獨立的混輸評分能力；現有 core API 不能憑空提供未暴露的 raw-key 或任意序列評分。若採用新模型，必須清楚區分它與既有注音選字模型。

不應把「每次按鍵呼叫大型模型判斷語言」作為第一版必要條件。先提供同步可用的本機排名，再評估小模型或 N-best 的非同步重排。

## 實作次序與驗收

1. 建立真實按鍵 benchmark：中文為主、英文為主與無分隔混輸，各自涵蓋標準／許氏。包含同一段歧義按鍵配不同前文、未知英文、短英文、数字、URL、標點及完整編輯流程。
2. 先跑目前版本，紀錄 baseline；從正確候選是否存在（候選生成召回率）區分生成問題與排名問題。
3. 修正 token-ID 假詞頻，建立獨立英文詞典、實際頻率、前綴索引與 OOV 拼寫分數；這一步不是整個功能完成。
4. 重設 mixed lattice 與排名，補上中文詞組／上下文資料；把最長後綴、全字母長度、強制原文第一等分支移出事件處理器。
5. 重做候選與確認策略；依新互動契約更新 raw-key 測試，保留編輯、提交、焦點切換與手動選字的不變條件。
6. 比較中文誤留英文、英文誤轉中文、邊界錯誤、整句正確率、每句額外修正按鍵、預覽翻轉次數、p50/p95 按鍵延遲，以及模型未啟動時的可用性。
7. 把「原本順暢的純中文不退步」與「中英文切換的修正成本明顯下降」列為必須同時通過的驗收条件。實際門檻由 baseline 與獨立測試集決定，不先宣稱改善百分比。

目前新增的小型 raw-key 案例可作回歸項目，不能取代上述品質 benchmark。
