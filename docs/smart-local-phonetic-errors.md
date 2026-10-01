# 智慧混輸：錯鍵局部保留

## 1. 問題與操作

局部音節無法解析時，先前的 graph 沒有對應的注音錯鍵狀態，必須
把該區段當 Latin／數字／符號來計分。中英切換及整段 ASCII 的
分數可能因此勝出，連已經顯示的中文也一起退回 raw。

已重現的倚天例子：

```text
raw：ne3hz3 + nnn
原本：你好 → ne3hz3nnn
現在：你好 → 你好nnn
                   ↓ Backspace 三次
                  你好
```

新增 `BopomofoUnresolved` 表示局部未解析的按鍵。它保留原始 raw，
沒有 guessed reading 或候選字，不偷偷把錯鍵修成另一個中文字。
聲調形成邊界後，後面的有效音節可以繼續解析，例如標準布局：

```text
su3cl3 + sss3 + su3cl3
  你好   錯鍵    你好
顯示：你好sss3你好
```

尚未完成的錯鍵區逐字刪除；錯鍵後又打了幾個字才發現，也能用
Shift+Backspace 逐 raw key 刪回，再重打。普通 Backspace 的按字
刪除契約沿用[編輯行為](smart-mixed-input-editing.md)。Enter 會保留
錯鍵區的 exact raw，不會在提交時自動替換它。

## 2. 限制與反過度修正

- 錯鍵區只可接在真正的完整注音或已有錯鍵區後面，不能憑空把
  字首的任意英文當成注音錯鍵。
- 後續[合法中英邊界修正](smart-valid-mixed-boundaries.md) 另外排除
  已有合法候選的完整讀音，以及已知英文單字／單字加完整注音，
  避免正常混輸或「欸」被錯鍵狀態吞掉。
- 每個區段最多沿用 `kMaxSyllableKeys` 的六鍵預算；遇到第一個
  實際完成邊界就結束，不跨過有效音節去取得較便宜的分數。
- 只接受該布局的注音鍵／一聲 Space；URL、email、identifier
  等字面語法仍走 ASCII graph。
- 沿用未完成注音的 `-2.0` 分數，不根據布局名稱、單字或觀測答案
  新增獎勵。正常英文仍參與原本排名；不是把已顯示中文永久鎖死。
- 新狀態沒有讀音／候選，模型不能把它當成已知注音。正式模型
  仍只處理目前顯示 path 中真正的讀音；沒有新增候選預取政策。
- 全部原文仍可選，手選固定、候選面板及過期回應保護沿用現有契約。

此處處理的是解析失敗造成的擴散。錯鍵碰巧形成另一個有效音節、
未知英文與注音同 raw 的歧義、没有完整中文前綴的錯鍵等，仍不能
僅靠這個機制保證修復，也不保證所有布局都不再退回 raw。

## 3. 驗證

### 功能檢查

- 新增兩個 raw-key suites：七布局多按三個聲母、錯鍵區刪除、
  錯鍵後再打兩字、延後撤銷修復、raw 手選／提交，以及正常英文。
- 新增正式模型 suite：模型在途時加入錯鍵區及後面的有效讀音，
  確認晚到回應没有改寫未解析 raw，也沒有為它填入假讀音。
- macOS Swift adapter 驗證七布局的錯鍵區預覽、刪回中文及提交。
- 完整 resource／engine unit／raw-key targets **3/3 通過**；C++23、
  共同 warnings 與 `-Werror` 建置通過。
- 已安裝服務／250M Q4_K_M 模型的七布局編輯流程，各跑模型關閉／
  開啟，共 **14/14** preedit checkpoints／最終提交檢查通過。

### 固定機械案例的前後比較

先保存 baseline binary 與 fixture，再實作新狀態。七布局各 16
組重複聲母、聲調邊界與後續輸入，共 **112 組**，都使用同一個
`你好` 前綴，不能當成 112 個獨立自然句子。

| 量測 | 原本 | 邊界限制後 |
| --- | ---: | ---: |
| 預覽整段退回 exact raw | 26/112 | 9/112 |
| 預覽仍以 `你好` 開頭 | 69/112 | 101/112 |

九個仍整段 raw 的案例為精業兩個、CP26 七個；没有為追這些案例
再調權重或加入鍵串特例。147 個 ASCII 對照沒有新增自動錯改，
其中原本就有一個相反意圖的 `su3cl3_backup` 被預覽為混輸。

初版錯鍵區允許跨完成邊界，導致合法中文音節被廉價的 raw 路徑
吞掉，並破壞一個布局編輯 roundtrip。保留初版比較結果；改成
第一個完成邊界即截止後，兩個既有功能回歸均通過。這是修正
區段的結構邊界，沒有依失敗句子的內容調分。

既有已看過的 structured corpus 回歸：純 ASCII **618/618** 保留
原文，混輸 **1854/1854** 正確目標仍在九列首頁，没有新增自動
錯改或首頁候選損失。這批混輸的自動正確仍是 0/1854，不能把
候選可達當作自動輸入準確率。

### 證據與重現

資料位於 `build/model-mixed-probe/local-phonetic-error-20260930/`：

- `manifest.json`／`frozen-cases.json`／baseline decoder hash。
- `initial-comparison.json`：保留未限制邊界的初版觀測。
- `boundary-restricted-comparison.json`：固定 112 組與 ASCII 對照。
- `structured-initial-comparison.json`／`structured-final-comparison.json`。
- `service-manifest.json`／`frozen-service-cases.json`／實際每鍵 trace。

```sh
cmake --build build/engine-tests \
  --target llavon_ime_tests llavon_ime_rawkey_tests --parallel
ctest --test-dir build/engine-tests --output-on-failure
bash macos/scripts/verify-core.sh

cmake --build build/model-mixed-probe --target typing_usability_probe --parallel
build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/smart_local_error_cases.json \
  build/model-mixed-probe/local-error-repeat.json 0 production-layouts
```

这些是機械驗證及已看資料回歸，不是新的真人可用性盲測。
`ime-core` 没有修改，這輪沒有安裝或重啟桌面輸入法。
