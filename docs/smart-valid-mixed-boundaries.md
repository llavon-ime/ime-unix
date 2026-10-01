# 合法中英邊界與單韻母語氣詞

## 1. 使用者回報與重現

標準布局、智慧中英文模式，連續輸入：

```text
xu/4j94vu04y94appao6u.3wj61ul
另  外  現  在 app 沒  有 圖 標（一聲 Space）
```

注意最後要有一聲 Space。整串 raw 為
`xu/4j94vu04y94appao6u.3wj61ul `。

- 原先預覽：`另外現在appao6有圖標`。
- 接著在同一組字輸入 `o4`，原先預覽：`另外現在appao6有圖標o4`。
- 分別單獨輸入 `appao6` 或 `o4`，已能得到 `app沒`／`欸`。
  所以必須用完整、不中斷的輸入重現，不能只測片段就宣稱沒有問題。

修正後預覽為 `另外現在app沒有圖標`；同一組字再輸入 `o4` 為
`另外現在app沒有圖標欸`。提交後重新輸入 `o4` 也能得到 `欸`。
智慧模式原本的字面括號契約保留，`o4]` 提交為 `欸]`。

## 2. 原因與一般性修正

### 合法讀音不能與「錯鍵狀態」競爭

前一輪局部錯鍵保留新增 `BopomofoUnresolved`。建立尾端未完成
狀態時，看到聲調完成便判定不是 unfinished，卻又增加 raw recovery
路徑。即使 `ㄟˋ` 是表內的完整讀音，也可能被當成 recovery 的
`o4`；弱詞頻的「欸」因此輸給較便宜的 raw 錯鍵狀態。

現在 recovery builder 明確排除：

- 已有完整合法讀音與候選的按鍵段。
- 既有 lexicon 認識的完整英文單字。
- 既有 lexicon 認識的英文前綴，後接完整合法注音的分段。

例如 `app` + `ao6` 是正常中英邊界，不是一個六鍵的錯誤注音。
沒有新增 `app`／「沒」／「欸」字串特例，也沒有修改字典或詞頻。
未列入表內的 `ㄟˊ`／`ㄟˇ` 等，不能因字形相近就虛構候選。

### 中文內的英文單字段落，避免雙重切換懲罰

僅排除錯鍵狀態後，完整回報句子的正確 path 仍以微小差距輸給
整段 raw。原先中文 → `app` → 中文支付兩次語言切換成本。

現在把「由完整中文進入、lexicon 已認識的英文單字」標記為一段
literal island：進入時支付既有切換成本，回到中文時不再重複支付。
未知 stems／不透明 identifier 仍使用原本成本。

這是一般的「中文—已知英文單字—中文」政策，不依句子答案增加
獎勵，也没有調整既有的 `kSwitchPenalty` 数值。這個狀態加入 beam
等價檢查，避免同樣 rendered text 卻有不同後續成本的 paths 被誤合併。
單字證據在建 edge 時查一次，不在每個 beam extension 重複查字典。

## 3. 驗證與限制

- 新增兩個 raw-key suites：完整回報句子、同一組字／提交後的「欸」、
  括號、刪字、逐鍵聲調撤銷、原文手選，以及九個不同英文詞後接
  四種中文讀音。另測表內單韻母讀音，確認不是錯鍵狀態。
- 新增正式非同步模型 suite：舊 job 在途時接續英文單字、`ㄇㄟˊ`
  與 `ㄟˋ`，確認讀音沒有退回 raw、English island 沒有被改写。
- macOS Swift adapter 使用與本機相同的 `directly_put_to_buffer`
  Shift 字母設定，驗證整句、同組字「欸」、提交與下一次 `欸]`。
- 完整 resource／unit／raw-key targets **3/3 通過**；C++23、共同
  warnings 與 `-Werror` 建置通過。單次長輸入 timing test 曾在同時
  跑多個資料比較 process 時超過 100 ms 門檻；edge 證據預算改為
  一次查詢後，無並行實驗負載的完整測試通過，没有放寬門檻。
- 已安裝 250M Q4_K_M 模型服務：五個回報／編輯／原文流程，各跑
  模型關閉／開啟，共 **10/10** 中間預覽及最終提交檢查通過。

先凍結使用者案例及同類 development 檢查，共 43 組：完全符合
預期由 **18/43 → 39/43**。剩餘四組是 lexicon 未認識的 `cli`
後接中文，没有為這個結果加入縮寫白名單。這些是已知需求及機械
檢查，不是獨立自然語句盲測。

既有 structured corpus：**618/618** 純 ASCII 保留原文，混輸
沒有新增自動錯改或首頁候選損失；該批混輸原本偏好整段 raw，
不能把 1854 個候選可達案例算成自動輸入正確。

前輪的 112 組重複鍵 development fixture 也重新比較：保留
`你好` 前綴為 **101 → 100**，整段 raw 為 **9 → 10**。差異是
精業的 `d-avladd`：`dd` 同時是既有 lexicon 的英文命令，不應再
把它聲稱為必然的注音錯鍵；「你好dd」仍在首頁。没有為保住單一
統計結果重加非法 recovery。這仍是有相反意圖的 raw，不保證都
能自動判對；147 個 ASCII 對照沒有新增誤改。

這輪修改共用 `engine/`；`ime-core` 沒有修改，也没有安裝或重啟
目前的桌面輸入法。

## 4. 證據與重現

證據位於 `build/model-mixed-probe/valid-mixed-boundaries-20261001/`：
凍結 fixture／baseline hashes、僅排除錯鍵狀態的中間結果、完整
literal-island 政策結果、原文／錯鍵回歸、真實服務 manifest 及每鍵 trace。

```sh
cmake --build build/engine-tests \
  --target llavon_ime_tests llavon_ime_rawkey_tests --parallel
ctest --test-dir build/engine-tests --output-on-failure
bash macos/scripts/verify-core.sh

cmake --build build/model-mixed-probe --target typing_usability_probe --parallel
build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/smart_valid_boundary_cases.json \
  build/model-mixed-probe/valid-boundary-repeat.json 0 production-layouts
```
