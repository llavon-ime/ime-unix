# 「巫師三」：中英切分與同音字搜尋的架構瓶頸

## 1. 結論與重現

後續已完成[有界完整詞讀音修復](smart-lexical-reading-spans.md)，
七布局現在可自動輸入「巫師三」。以下保留修正前的架構調查與觀察。

這不是缺字、沒有支援單鍵注音，或模型不認識「巫師三」。主要問題
是 **用少數同音字的分數先決定中英切分，之後模型只能修那個已決定
的切分**。正確讀音可以存在於 beam，仍完全沒有機會交給正式模型。

標準布局連續輸入 `j g n0 `（三個一聲 Space 都包含在 raw）：

```text
實體鍵       j␠         g␠         n0␠
讀音         ㄨ         ㄕ         ㄙㄢ
目標         巫         師         三
目前預覽     j␠         g␠         三
```

- 目前顯示／提交為 `j g 三`，不是「巫師三」。
- 正確的 `ㄨ | ㄕ | ㄙㄢ` 邊界在 decoder beam 中，但不是 best path。
- 把完整正確讀音交給已安裝的 250M Q4_K_M 模型，得到 **巫師三**。
  沒有提供固定中文字或新增字典項目；提供的是正確音節與邊界。
- 即使加上已確認前文「我最近在玩」，目前正式預覽仍是 `j g 三`。
- 前輪局部錯鍵修正以前的 binary、app 邊界修正以前的 binary，
  也都顯示 `j g 三`。這是既有瓶頸，不是最近錯鍵保護新增的回歸。

## 2. 三個相互放大的架構問題

### 2.1 字頻與同音字順位，過早決定了語言

`mixed_input_decoder.cpp` 在延伸注音 edge 時，直接加上
`chinese_score(history, candidate)`，也按候選順位扣分。這些分數
同時決定音節切分與最終中文字，與英文 token／symbol paths 競爭。

單鍵一聲的 `j `、`g ` 很像英文變數加空白，不能無條件視為中文。
但目前勝負取決於「屋／失」等少數同音字的離線分數，而不是整串
`ㄨ | ㄕ | ㄙㄢ` 作為中文讀音序列是否合理。

這個樣本的分數：

```text
j g 三       -3.408  ← best
j 失三       -4.094
屋失三       -4.768  ← 完整中文讀音已有代表
```

模型後來只能收到 best path 中的 `ㄙㄢ`，前面的 `j g ` 已經是 literal。
所以等更久、重試相同 request，都不能讓模型改出「巫師」。

### 2.2 同音字展開吃掉搜尋預算，較後順位的早期字無法出現

目前每個注音 edge 只展開前 **4** 個候選，每個 vertex 的 beam 共
**16** 條。這些槽同時容納切分差異與同音字差異。

「巫」在 `ㄨ ` 表內是第 **8** 個；「師」在 `ㄕ ` 是第 **3** 個。
「巫」存在於整個候選向量，但不參與早期 lattice 的文字展開。
`engine/data/mixed_lexicon.inc` 也已經有「巫」和「巫師」，然而
第一個字沒有展開成「巫」，後面的「巫師」詞頻便沒有機會發揮。
補這個詞的字典不是根本修法。

`expand_candidates()` 優先列完整 paths，剩餘空間只補**最後一個字**
的同音字。於是打完「巫師三」後，不能靠尾字候選補回第一字「巫」；
完整目標不在九列首頁。單獨輸入 `j ` 時，「巫」仍可手選。

### 2.3 模型只有目前一條顯示切分，無法救回已判 literal 的讀音

`PendingModelPreview::capture()` 只擷取目前 preview path；raw path
不啟動模型，mixed path 也只把其中 `Bopomofo` segments 送去推論。
這個設計有保護英文、限制延遲、保留 raw 與手選的好處，但形成
以下閉環：

```text
raw keys
   │
   ▼
中英切分 + 前4同音字離線排名
   │  預覽選 j g 三
   ▼
只把 ㄙㄢ 給模型，j g 已成 literal
   │
   ▼
模型可以修「三」，不能重新判讀「巫師」
```

應把「讀音／邊界是否可達」、「哪條顯示切分獲選」、「選字是否
正確」分開量測。candidate 有原始字表、beam 有正確邊界，都不
代表使用者能自動輸入或在目前候選首頁找到目標。

## 3. 真實模型與七布局觀察

### 17 組結構案例，標準／許氏各一輪

凍結 fixture 包含回報名稱、帶前文名稱、詩人／師父／吃飯／公司／
知道／醫院／污染／烏黑、較後順位字詞及四種 literal 對照。
每個例子用真實 raw-key engine 取得 baseline，再分別比較正式
顯示切分、去中文詞頻的 ablation、去 lexicon 的 ablation，以及
提供正確讀音邊界的 oracle 模型。

- 三個 singleton 名稱樣本 × 兩布局：目前顯示切分加模型 **0/6**
  完全正確，提供正確讀音的模型 **6/6** 完全正確。
- 所有 26 個中文例子：目前顯示切分加模型 **6/26**，正確讀音
  oracle **20/26**。這是這批 development 案例，不是自然文章准确率。
- 還有真實模型本身的誤選：師父 → 師傅、污染 → 汙染、
  鶴立雞群 → 赫利基群。正確切分不保證每個字都正確。
- 去掉中文詞頻，八個 literal 對照有 **5/8** 不再保留原文；
  去掉所有 lexicon，則 **6/8** 不再保留原文。不能直接採用
  ablation 作為正式修法。
- 去 lexicon 的其中一個 counterfactual 產生模型不支援的 `ㄇ `。
  診斷工具現在會記錄那個 arm 的 `*_error`、request 數與耗時，
  不再因這個已知詞表差異丟掉整份報告。正式 preview 的
  token 支援檢查未改動。

### 七布局的實際 engine 流程

三種工作流程各跑七布局，並各跑 `SmartModelPreview` 關／開，
共 42 次：

| 流程 | 關閉 pending model preview | 開啟 pending model preview |
| --- | --- | --- |
| 智慧中英連續輸入「巫師三」 | 0/7 | 0/7 |
| 關閉 SmartEnglish、普通注音輸入 | 7/7 | 7/7 |
| 智慧中英逐字明確手選「巫」「師」，再輸入「三」 | 7/7 | 7/7 |

第二行的普通注音仍使用 buffer 的真實模型服務。
`SmartModelPreview=False` 只停用**智慧 pending 預覽模型**，
不等於普通注音的整個模型服務關閉。

普通注音之所以能成功，是完整讀音保留在 buffer 交給模型，沒有
先用英文 token 的競爭把 `ㄨ`、`ㄕ` 丟掉。這也排除了缺字／
布局 mapping 問題。

## 4. 接下來適合的架構方向

先拆分**讀音切分搜尋**和**同音字選擇**，再驗證模型候選如何進入
明確可選的候選面板：

```text
raw keys ──► 有界讀音／literal lattice
                    │ 保留按鍵範圍、讀音、布局、多義性
                    ▼
             按不同邊界保留代表
                    │
                    ▼
        同音字排序／明確選字／模型受限刷新
                    │
                    ▼
            預覽 + 完整 raw 選項
```

具體順序：

1. 先按 `(raw ranges, kinds, readings, consumed boundaries)` 區分
   真正的切分；不要讓同一讀音的十多個文字變體吃掉全部切分預算。
2. 每個切分保留完整同音字列表與逐字選擇來源；較早字的低順位
   候選也應可達，不只展開尾字。
3. 先驗證**使用者明確選了中文切分**後的一路模型刷新，將選切分
   與「固定每個字」分開。現有 `apply_mixed_path(manual=true)`
   是把每個字也標為手選，因此不能直接把選了「中文」當成同意
   固定「屋失三」。
4. 若再做自動策略，先凍結延遲／請求預算及否決門檻；保留 raw
   與英文對立意圖。不要用 masked probability 跨不同讀音切分
   比大小，也不要為單鍵一聲全部加固定獎勵。
5. 使用按來源／名稱根分組的未看資料，分別報切分召回、候選首頁
   召回、自動 CER、ASCII 誤改與修復成本。

這一輪完成定位、診斷報告與 raw-key recovery 測試，**尚未將上述
新架構或額外模型推論政策接入正式 decoder**。沒有更動分數、
新增名稱白名單、修改 `ime-core`，也没有安裝或重啟輸入法。

目前可驗證的臨時操作是：輸入「巫」的注音後按 Down 手選「巫」，
再輸入「師」的注音並手選「師」，最後輸入「三」；或者切回普通
注音模式。這是可用的修復路徑，不把它計為自動正確。

新增 raw-key suite 獨立列出七布局實體鍵，檢查第八順位的「巫」
仍可明確選擇並提交完整「巫師三」；另有非同步服務 suite 檢查
後續模型更新保留已明確手選的「巫」「師」。C++23 warnings／
`-Werror` 建置通過，完整 raw-key runner **87 suites、0 failures**。
其中六個 Linux memscan helper 分支在本機 macOS 因未設定
`LLAVON_IME_TEST_MEMSCAN` 跳過；不把它們算成已驗證的 Linux
記憶體讀取行為。

## 5. 證據與重現

固定案例：

- `engine/tests/rawkey/smart_reading_architecture_cases.json`
- `engine/tests/rawkey/smart_singleton_workflows.json`

完整 JSON、log、baseline binary hash、七布局逐鍵 trace：
`build/model-mixed-probe/reading-architecture-20261001/`。

```sh
cmake --build build/model-mixed-probe \
  --target model_mixed_probe typing_usability_probe --parallel

build/model-mixed-probe/model_mixed_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/smart_reading_architecture_cases.json \
  build/model-mixed-probe/reading-architecture-repeat.json

build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/smart_singleton_workflows.json \
  build/model-mixed-probe/singleton-workflows-repeat.json 0 production-layouts

build/engine-tests/tests/llavon_ime_rawkey_tests singleton
```
