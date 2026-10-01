# 智慧組字即時模型選字：正式 Engine 整合

## 本次改進

上一輪 raw-key adapter 驗證過的單路預覽，現在接入 Linux／macOS 共用的
`Engine`。開啟「智慧型中英文」即自動使用即時模型選字，設定畫面不再
另外顯示「智慧組字即時模型選字」。舊 JSON／INI 的
`smart_model_preview`／`SmartModelPreview` 已忽略，重新儲存也不會輸出。
程式內保留預設 true 的控制供 raw-key 測試／量測離線基準使用，
不是使用者設定。Linux／macOS 共用 schema，不修改 `ime-core` 或服務 API。

`engine/src/engine/pending_model_preview.{hpp,cpp}` 負責：

- **非同步開啟 session 與推論**：按鍵不等待模型；完整注音出現才請求。
- **只處理目前顯示的切分**：沒有每鍵四路預取的額外成本，也不假設
  同音字內的順位能比較中英切分。
- **保留 raw keys**：結果寫入正式 `MixedPath` 候選與顯示文字，Enter
  使用同一份結果；Backspace 刪目前顯示的一個字，Shift+Backspace
  撤銷未確認的原始按鍵。詳見[編輯行為](smart-mixed-input-editing.md)。
- **共同前綴沿用**：新韻母尚未完成時沿用模型已選好的前字；相同完整
  讀音與前文不重複請求。
- **最新輸入優先**：一個 job 在途，後續輸入合併為最新 snapshot；
  不將每個中間按鍵排進模型工作佇列。
- **過期回應捨棄**：檢查 raw revision、buffer revision、游標、layout、
  前文、敏感狀態、設定與預覽切分；候選窗開啟後不重新排列它。
- **手選與英文前文**：buffer 中的確切字／字母作為 context，不把
  手選字當成可重新選擇的目標。
- **模型失敗保留目前預覽**：拒絕非同音字與不符 correlation 的回應；
  缺服務、缺模型時沿用原本 fallback，Enter 可以正常提交。
- **重啟恢復**：服務回傳 `UnknownSession` 時，同一個 job 最多重新
  開啟一次，不需使用者多打一個音節才恢復，也不無限重試。
- **session 清理**：detach／焦點、設定、transport 變更時關閉協調器；
  shared lease 處理開 session 回應已到 worker、但尚未交回 UI 的競態。

## 另外修正的兩個實際問題

### 已改善的字不能因為過期回應退回 fallback

原 buffer prediction 的回應雖然 request correlation 正確，但若輸入
標點令 buffer revision 改變，舊邏輯會對原請求位置重新套 fallback。
現在直接捨棄這種過期答案，保留既有候選；需要新推論的編輯仍由原有
dirty 排程處理。

新增 raw-key 回歸先讓「你」被模型選成「擬」，再於下一個請求在途時
輸入「好，」，刻意釋放過期答案。顯示與提交都必須維持 **擬好，**，
不能退回「你好，」。

### Transport 停止時不能重新連線

新整合的 teardown 回歸重現了 shutdown 時仍有提交／close 在佇列的
情況。原 worker 在 socket 已 shutdown 後繼續處理佇列，可能重新連線
並卡在 service epoch handshake，導致 Engine 析構等待不結束。

現在看到 stopping 就離開 worker 主迴圈，把剩餘 callback 以停止錯誤
完成，不在停止期間重連。既有 transport／reconfigure 測試與本次
焦點、重設、detach、設定切換案例一起驗證這條路徑。

## 真正使用正式引擎的測量

工具新增三種模式，**沒有建立上一輪的 `LiveMixedModelPreview` adapter**：

- `production-comparison`：即時模型設定關／開，同一篇文章與操作比較。
- `production-timing`：192 個快速輸入／Enter 等待組合。
- `production-typos`：1,452 個注入錯鍵樣本，各測立即／晚六鍵修復。

沿用 12 篇文章、55 個子句、37 個流程與標準／許氏兩種鍵盤。一般文章
每鍵 60 ms、標點後停 120 ms、句號後 Enter；子句診斷則等待正在進行的
模型工作完成。這兩個條件不能當成同一個速度下的準確率。

`production_pending_requests` 是目前正式 pending 協調器的請求數；
`model_requests` 仍只計原有 buffer prediction，兩者不能混為一欄。
測量工具以訓練 callback 接收模擬提交，不把 fixture 當成使用者訓練資料。

### 正式 Engine 的實測結果

本輪共 48 次全文輸入、220 個獨立子句診斷、148 次操作流程。比較組
只有新設定的關／開，沒有測試 adapter 在旁邊修改正式引擎的候選。

| 指標（兩種鍵盤合併） | 設定關閉 | 設定開啟 |
| --- | ---: | ---: |
| 已輸入前綴完全符合目標 | 303/960 | 641/960 |
| 子句等待模型後直接提交正確 | 16/110 | 79/110 |
| 包含首頁選字修復的子句正確 | 37/110 | 83/110 |
| 操作流程符合目標 | 58/74 | 68/74 |
| 整篇文章完全符合目標 | 9/24 | 9/24 |

前綴測點是「整個已輸入前綴」完全一致，不能當成逐字準確率。全文
標準 CER 為 15.76% → 15.40%，許氏為 21.09% → 20.73%；整段完全
正確的篇數沒有增加。原因是原流程在標點後本來就會走模型，本次主要
改善輸入途中與沒有標點的提交，而不是解決所有中英切分。

例如兩種鍵盤都將大寫英文前的 `在卻任WebSocket` 改成
`再確認WebSocket`。前文案例的「信箱」、「架構」、「雞湯」、「測試」
也直接改善；「公式」仍可能成為「工事」，未修的 `helo` 仍是 `helo`。
這些是語言品質觀察，沒有調權重使 fixture 強行過關。

時序測量 **192/192 提交與 Enter 前顯示相同**；每鍵 60 ms 且立即
Enter 的八個 seed／兩種鍵盤，正確數從 7/16 → 13/16。整段瞬間輸入
立即 Enter 時模型通常來不及改善，不能因此讓 Enter 阻塞等推論。

正式 Engine 的 1,452 個錯鍵樣本中：

- 立即發現刪回重打：**1,452/1,452** 恢復無錯鍵版本的預覽與提交。
- 晚六鍵發現再重打：**1,452/1,452** 恢復。
- 延後修復平均增加約 12.7 次按鍵，最多 16 次。

這些為固定 seed／位置／錯誤型態組合，不是獨立真人語料；恢復無錯鍵
參考不代表原有切分或同音字一定正確。三次真實服務執行皆成功，log
沒有 `[ERR]` 或 `invalid bpmf`。

原始結果位於 `build/model-mixed-probe/production-preview-{comparison,timing,typos}.json`。
全文比較先於 session lease／UnknownSession 恢復的最後健全性修正執行；
後兩組使用最後版協調器，正常推論路徑一致。首次全文比較仍沿用當時
工具的既有提交記錄路徑，後兩組已使用 fixture 訓練 sink。

## 驗證與重現

```sh
cmake -S engine -B build/engine-tests \
  -DLLAVON_IME_ENGINE_BUILD_TESTS=ON \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DLLAVON_IME_WARNINGS_AS_ERRORS=ON
cmake --build build/engine-tests --target llavon_ime_tests llavon_ime_rawkey_tests --parallel 6
ctest --test-dir build/engine-tests --output-on-failure

cmake --build build/model-mixed-probe --target typing_usability_probe --parallel 6
build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/model_workflow_cases.json \
  build/model-mixed-probe/production-preview-comparison.json 60 production-comparison
```

新增 production raw-key 情境直接經 Engine 呼叫服務，不由測試工具
覆寫候選。包括預覽／Enter／training sample 一致、不完整尾音快取、
改聲調、候選窗、提交／取消／焦點／重設／detach、手選保護、前文變更、
敏感欄位、服務／protocol 錯誤、許氏英文續打、不支援讀音、設定關閉、
無服務 fallback 與 UnknownSession 恢復。

一般離線 raw-key harness 預設關閉新設定，以保留其原有的確定性離線
測量；production suite 與工具 production 模式明確開啟它。正式設定的
預設值為 true，與測試 fixture 的基準條件分開。

新 production suite 含 21 個情境，加上獨立的過期 buffer 回歸；上一輪
17 個 adapter 情境仍保留。完整 unit／raw-key target **2/2 通過**，新增
模型情境均實際執行。既有 Linux memscan 情境在 macOS 依條件略過。
建置使用 C++23、完整 warning flags 與 `-Werror`；`git diff --check` 通過。

## 範圍

這次正式實作的是**既有切分內的即時模型選字**。若 decoder 已把中文
按鍵解讀為 email、數字或識別字的一部分，模型不會直接重寫該切分。
仍使用有限手寫語料與固定時序，未涵蓋桌面真人研究、完整前端呈現與
跨中英切分的模型校準。此次未安裝或重啟使用者的 macOS 輸入法 App。
