# 結構化混輸評分：泛化驗證與否決紀錄

## 結論

**保留多路徑／可逆組字的方向，但撤回「依英文 stem 拼寫降分，自動
改變中英切分」的正式預設。** 第一輪提升是真實的，不過它不足以證明
這條 heuristic 能泛化。沒有針對新失敗名字加白名單或調整 `+3`。

保留上一輪其他獨立改善：即時同音字模型、過期回應保護、底線開頭
識別字、raw-key 編輯與原始文字候選。`ime-core` 未修改。

## 1. 為什麼第一輪結果不夠

- 30 個名稱反覆搭配少數形狀與四個中文前綴，不能當成 1,030 個獨立
  使用者樣本，也沒有足夠代表不透明帳號、雜湊與跨語言人名。
- `english_score` 混合詞頻和拼寫 backoff，並不是「這段是英文」的
  校準機率。既有初始 `-3` 無法證明新增 `+3` 是合適的語言決策門檻。
- 名稱不像自然語句：**不常見、難拼或隨機的 stem 仍可能是完全合法
  的帳號／domain／scheme。** 拼寫不熟悉不等於中文。
- 候選搜尋本身也可能被降分影響。即使平均 top-1 更好，真正需要的
  英文或中英切分被剪掉，仍是退步。

這是泛化失敗證據，不把它稱為已證明的統計學過擬合；本來就沒有獨立
真人分布或可靠母體錯誤率估計。

## 2. 凍結後才執行的獨立驗證

先複製當時的 decoder binary 為 `structured-stem-frozen-probe`，再由
`engine/tools/validate_structured_holdout.py` 產生固定 seed `2026093002`
的完整案例檔與 SHA-256，寫入後才開始查詢兩個 binary。公式沒有調整。

```text
fixture SHA-256:
d858731d07a25631e9185ec91d5c9a6c03932ce0d4216b4aecae1c1a43db782b
```

四個根名稱族各 20 個：跨語言人名、技術名稱、隨機字母名稱、hex ID。
各搭配 identifier、email、domain、URL scheme 形狀；數字開頭 scheme
不合法，因此排除，數字開頭 email／domain 仍保留。兩種鍵盤與三種
中文前綴共 **2,472 個組合**。

獨立指的是不從第一輪 fixture 選名稱或依失敗調參；不是聲稱人名／
技術字沒有出現在既有詞庫或模型訓練資料中。

| 檢查 | 舊的保守 structured score | 被驗證的 stem 降分 |
| --- | ---: | ---: |
| 純英文控制組 top-1 正確 | 618/618 | **614/618** |
| 中文接結構化字串 top-1 正確 | 0/1854 | **638/1854** |
| 正確混輸切分仍存在於 paths | 1854/1854 | **1756/1854** |

四個純英文退步都來自**同一個隨機根名稱**、四種結構，不當成四個
獨立事件。實際例子：

```text
合法原始輸入：hwfmcmemtgjlgqgvwxefgvuy@mail.invalid
被自動改成：  好mcmemtgjlgqgvwxefgvuy@mail.invalid
```

`hwf` 同時可以是許氏的「好」。新的拼寫政策會將這個合法帳號前綴
誤解成中文；相同根的 identifier／domain／scheme 也出現此現象。
另外 **98 個正確混輸候選消失**，不只是排名變低。這些是足以否決
預設啟用的退步，不能用中文組的平均提升抵銷。

分族結果另列在 `holdout-report.json`，例如 hex ID 的許氏 mixed
top-1 是 **0/207**；不能拿熟悉名字上的收益概括為不透明名稱也有效。

驗證檔一旦看過就成為開發／回歸資料；後續 replay 不再稱為 holdout。

## 3. 原則上的同輸入、多意圖歧義

```text
完全一樣的 raw keys：nefhwfnguyen_cache
意圖 A：ASCII 識別字  nefhwfnguyen_cache
意圖 B：中英混輸    你好nguyen_cache
```

在相同前文與按鍵下，兩個目標都合理。只看拼寫分數、停頓或這串 raw
keys，不能保證兩種使用者意圖都猜對。測量時必須保存兩種標籤，不能
把 ASCII 意圖從資料移掉，讓中英自動判斷顯得很準。

需要可觀察的意圖證據或使用者選擇；其中**明確手選**是現有流程已
能使用的訊號。當前模型 API 的 `protocol::Prediction` 只回同音字
候選列，沒有整段 raw／中英解釋的可比較機率。拿同音字的第一順位
當跨切分 confidence，沒有機率上的根據。

## 4. 撤回後繼續研究：可修復性先行

正式 decoder 已恢復結構化字串的保守分數。新 regression 直接經
raw-key harness 驗證合法名稱完整提交；原本中文前綴情境改為檢查
**raw 與中文解釋都在候選窗內，明確選擇後提交一致**，不再以猜中
未觀察的意圖作為唯一通過條件。

`mixed_input_probe --json` 新增診斷輸出，使用正式
`MixedInputDecoder::expand_candidates(..., 9)` 檢查首頁，沒有試驗用
候選排序器。`evaluate_candidate_recovery.py` replay 凍結的驗證檔：

- 純英文 **618/618** 保持原樣，原始文字也在首頁。
- 混輸 **1854/1854** 正確解釋在九列首頁內，兩種鍵盤各 927/927。
- 這個組合上的自動 mixed top-1 是 **0/1854**；候選可修復性不等於
  自動準確率，也不等於使用者實際閱讀並成功選字的比例。

因此本輪選擇保留完整名字與可選混輸，沒有宣稱原來的自動品質提升
仍在預設版本中。當前 raw-key structured 情境為 54 個；完整 unit／
raw-key target **2/2 通過**，新增 opaque name 回歸均實際執行。

## 5. 後續研究的預先約束

下一個自動切分提案必須在執行前記錄：

1. **固定政策與 acceptance gates**：純字串被錯改與正確候選消失
   分開檢查，不以平均分提升相互抵銷。
2. **以根名稱／來源／文章分組切分資料**：同一個 stem 的不同 suffix
   或 typo 不跨 development／validation 集；共享 generator 的大量
   組合不能冒充大量獨立資料。
3. **獨立盲測與真實分布**：新驗證集先凍結，結果只看一次；如果要
   依結果修改政策，它就轉成 development，另取未看的資料再驗證。
4. **校準與消融**：先提出分數的語言意義，再測參數敏感性、去掉詞頻
   或拼寫先驗的影響，不能把單一最漂亮設定當成普遍有效。
5. **保留對立意圖**：同樣 raw 的 ASCII／混輸合法解釋都收錄；報告
   abstention、候選可達性、人工修復代價與自動錯改，不只報 top-1。
6. **模型資訊不足時不推導 confidence**：若研究整段模型 likelihood，
   先確認服務能提供哪些可比較量；本專案不修改 core 的限制仍有效。

以上為後續研究協議，不表示已收集獨立真人語料或完成模型校準。

## 重現

```sh
python3 engine/tools/validate_structured_holdout.py \
  build/model-mixed-probe/structured-boundary-baseline-probe \
  build/model-mixed-probe/structured-stem-frozen-probe \
  build/model-mixed-probe/structured-holdout

cmake --build build/engine-tests --target mixed_input_probe llavon_ime_tests llavon_ime_rawkey_tests --parallel 6
ctest --test-dir build/engine-tests --output-on-failure

python3 engine/tools/evaluate_candidate_recovery.py \
  build/engine-tests/tests/mixed_input_probe \
  build/model-mixed-probe/structured-holdout/frozen-holdout-cases.json \
  build/model-mixed-probe/structured-holdout/candidate-recovery-report.json
```

兩個舊 binary 的 SHA-256 都記在第一個 report；撤回後 binary 的摘要
記在 candidate recovery report。第一輪文檔／artifact 保留作歷史比較。
