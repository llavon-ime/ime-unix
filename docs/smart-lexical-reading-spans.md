# 以完整詞的讀音路徑修復落成英文的單鍵注音

## 1. 本輪成果

標準布局輸入 `j g n0 `，現在可直接預覽及提交 **巫師三**。
七種布局的實體鍵都驗證成功；不需要先逐字手選，也不必等待模型
才出現中文。真實模型開啟後仍得到完整「巫師三」。

這是[前輪架構調查](smart-reading-architecture.md)後的一個有界修復
步驟：利用既有中文詞庫的完整詞路徑，繞過「前四同音字」的早期
展開限制。不是把整個 decoder 改成無限制的中文字 Cartesian product。

## 2. 方法與界線

```text
原本的 raw / 中英搜尋
        │
        ├─ 原本勝出是完整 raw ─────────► 原本結果
        │
        └─ 已有完整中文 + 單鍵一聲落成 literal
                          │
                          ▼
             既有中文詞的 prefix index
             對齊 2–4 個合法完成音節
                          │
                          ▼
             完整詞路徑參與受限讀音搜尋
                          │
                          ▼
             預覽 + scored exact raw 候選
                          │
                          ▼
             原本的一路非同步模型選字
```

### 完整詞路徑

`MixedLexicon` 從原本的詞頻資料建立 immutable 中文 prefix index：
只索引 2–4 個 BMP 漢字的既有詞，不加入新詞、名稱或特定按鍵串。
decoder 將 index 的字元延伸與相鄰完整注音 edges 的**全部**候選
交集匹配，而不是先截掉較後順位的字。

「巫」虽然是 `ㄨ ` 的第八候選，但既有「巫師」能對齊 `ㄨ | ㄕ`，
因此整個詞可作為一個搜尋提案進入 DAG，不能在第一字就被剪掉。
每個詞仍保存獨立 segment 的 raw 範圍、body keys、tone、canonical
reading 與完整候選向量，原有 replay、刪字、提交及模型協定都沿用。

完整詞使用原本 `chinese_score` 的字頻／詞頻；不另外套用逐字字表
**ordinal** 懲罰。字表第八個不是這個詞只有第八名機率，更不是
判定整個讀音是英文的證據。原本一般單字 paths 的順位規則仍保留。

### 保守的修復範圍

修復必須同時滿足：

1. 原本勝出的 path 已含至少一個完成中文音節，且不是完整 raw。
2. 它將一個合法單鍵一聲讀音拆成 literal key 加 Space。
3. 既有完整中文詞能以 2–4 个音節**精確**對齊該位置。
4. 詞路徑不跨原本的完整英文 token、不透明 token、未完成注音
   或 `BopomofoUnresolved` 錯鍵島。

Latin 或符號鍵均須經當前布局的 replay 驗證。這讓倚天 `/ `、
精業 `[ ` 與標準 `g ` 等都走相同規則，沒有按布局或名稱加特判。

修復搜尋在有完整詞解釋的 singleton 位置比較讀音，而不是繼續
讓某布局的英文字母詞頻決定勝負；每次按鍵都從原本圖重新判定，
沒有永久鎖定中文。**原本整段 raw 勝出的輸入不啟動這個修復**。
例如 `j g `、`a i u `、`gcc -g ` 仍保持字面輸入。

完整 raw 在受限搜尋之前保留原本 scored entry，之後再放回結果，
所以仍能明確手選原文。已選字、候選面板、過期回應與 Enter 不等待
模型的契約沒有改動；没有增加多路模型推論或比較 masked probability。

### 預算

- 詞路徑最多四個音節，prefix index 避免無限制同音字組合。
- 每個 raw vertex 最多保留原本 `kTopK=16` 個詞路徑提案。
- 只有存在修復條件時才進行第二次有界搜尋。
- 沒有新增模型 API、背景候選工作或重試政策。

## 3. 被否決或修正的中間版本

- 第一版對所有中文詞都開放完整詞路徑，讓既有長句
  `你好我會上的下` 變成 `你好我會上地下`，`smart long` 回歸失敗。
  正式版本收窄為**修復目前落成 literal 的 singleton**；完整中文
  路徑不觸發新搜尋，原回歸恢復通過。沒有改測試答案或放寬門檻。
- 接著只增加 scored 詞路徑，標準等四種布局可成功，但 IBM 的
  `s y 三` 仍因英文字母詞頻高出 0.239 而勝出，倚天／精業符號鍵
  也未被涵蓋。保留這版真實服務觀察（36/42 流程通過）。
- 最終改為以 canonical replay 判定 singleton，並在有詞庫證據、
  已有中文的局部位置做讀音修復。沿用既有分數常數，不針對差距
  調權重。最後 42/42 流程通過。

中間結果與最後政策 manifest 留在證據目录，不把 development
迭代當成獨立盲測。

## 4. 驗證

### 回報、編輯、原文與非同步模型

- 七布局、模型 pending 預覽關／開，各跑連續自動輸入、普通注音、
  原本逐字手選，共 **42/42** 真實服務流程通過。
- 新 raw-key suite 檢查七布局自動預覽、提交、Shift+Backspace
  逐鍵撤銷、Backspace 按字刪除、重打及完整 raw 手選。
- 同一 suite 另測「詩人」「吃飯」「醫院」「資金」與 ASCII 負例。
- 受控 decoder 圖另測 literal／讀音成本相同時保留完整 raw，
  不依 beam 同分排序偶然性啟動修復。
- 有真實 transport 的受控服務 suite 檢查模型晚到、立即提交、
  原文手選、刪字及已開面板，確認舊回應不改寫目前狀態。
- macOS Swift adapter 驗證「巫師三」及原文 `j g ` 的預覽與提交。

### 新句子與舊負例

在收窄政策後、量測前凍結 12 個另外撰寫的句子根，包含醫生、
詩人、吃飯、資金、蜘蛛、烏龜、獅子、知識、老師、公司、師兄、
醫院。各跑七布局：

- 不使用 pending 模型，完整句子正確由 baseline **4/84 → 26/84**。
- 開啟正式 pending 模型，candidate **72/84** 完整正確。
- 無新增原本正確句子的離線誤改。

七布局共享同一批 12 個句子根，**不是 84 個獨立意圖**。這些是
人工撰寫的新結構檢查，也不是外部來源盲測。剩餘 12 个模型開啟
的失敗包含 IBM／精業等布局的中英切分問題，不能歸因成只有同音字
模型錯字；本輪没有再按這些句子調整政策。

舊固定資料：

- **618/618** 純 ASCII 保留原文，沒有新增自動錯改。
- **1854/1854** structured 混輸目標仍在九列首頁，無新增候選損失。
  這批自動正確仍為 0，不能把候選可達算成自動準確率。
- 112 個局部錯鍵樣本的 preview 全部與本輪 baseline 相同：保留
  「你好」前綴 100/112，整段 raw 10/112。147 個 ASCII 對照沒有
  新增自動錯改。

### 建置與時間

完整 resource／unit／raw-key targets **3/3 通過**（約 100 秒）；
raw-key runner **89 suites、0 failures**。六個 Linux memscan helper
分支仍因本機 macOS 未設定 helper 而跳過。C++23、warnings／
`-Werror`、Swift adapter 及 `git diff --check` 通過。

沒有模型負載時，重複 singleton 詞的 direct decode 各五次：

| raw keys | median | max |
| --- | --- | --- |
| 7 | 0.200 ms | 0.210 ms |
| 28 | 0.849 ms | 0.897 ms |
| 56 | 1.717 ms | 1.851 ms |
| 112 | 3.623 ms | 3.804 ms |
| 224 | 8.288 ms | 8.836 ms |

既有 raw-key 長行 benchmark p95 為 2.231 ms、max 2.566 ms。
這些不包含桌面繪製、啟動詞庫、服務競爭與模型延遲。

## 5. 限制與重現

沒有額外意圖訊號，不能同時保證 `j g ` 自動是英文變數和「巫師」。
本輪保留原本完整 raw 的勝出，所以單獨兩音節仍可能維持 raw；
接續「三」提供完成中文後，才能修復成完整「巫師三」。同理，沒有
詞庫依據或沒有中文 anchor 的 raw 不會靠這個機制猜出名字。

`ime-core` 沒有修改；桌面輸入法尚未安裝或重啟，這些修改仍未提交。

證據位於 `build/model-mixed-probe/lexical-reading-spans-20261001/`，
包含 baseline、初版服務觀察、最後政策／source hash、舊負例比較、
42 個回報流程、168 個新句子流程與 direct decode timing。

```sh
cmake --build build/engine-tests \
  --target llavon_ime_rawkey_tests llavon_ime_tests mixed_input_probe --parallel
ctest --test-dir build/engine-tests --output-on-failure
bash macos/scripts/verify-core.sh

cmake --build build/model-mixed-probe --target typing_usability_probe --parallel
build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/smart_singleton_workflows.json \
  build/model-mixed-probe/singleton-repair-repeat.json 0 production-layouts
```
