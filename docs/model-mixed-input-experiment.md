# 現有模型與智慧混輸：不修改 ime-core 的實驗

## 結論

現有本機模型能取代相當一部分新增的中文詞頻／詞組選字功能；但是現有
`Predict` API 不提供中英文解讀的共同分數，因此它無法直接替 decoder
決定中英邊界。本次結果支持先將模型接回中文選字，再改善分段。

本次只新增獨立實驗工具、fixture 與本文；`ime-core`、服務原始碼及正式輸入
流程均未修改。實驗呼叫已安裝的服務 binary，並非重新編譯目前 core checkout。

## 實驗方式

工具：`engine/tools/model_mixed_probe/`。
案例：`engine/tests/rawkey/model_mixed_cases.json`。

34 個手寫案例，各執行標準、許氏兩種鍵盤，共 68 個樣本：

- 中文詞句：28 個。
- 不同前文的中文選字：12 個。
- 中英混合，包括雙向切換與未知英文：16 個。
- 純英文／ASCII 控制組：12 個。

按鍵由明確的注音 fixture 產生，經實際 keymap 回放驗證；預期中文字必須
存在於該讀音的對照表。現行組字透過 `raw_key_harness.hpp` 逐鍵輸入，檢查
無提前提交、Enter 的提交文字等於預覽，並檢查與直接 decoder 輸出一致。

六個比較組：

1. **現行**：現有離線 decoder 的 raw-key 預覽。
2. **無中文詞頻**：ChineseScoreFn 改為固定 -5.0，不使用 wordfreq 的中文
   單字／詞組加分；保留英文模型、候選表原始順序、候選順位與結構懲罰。
3. **現行＋模型**：保留現行最佳分段，用本機模型重選中文。
4. **無中文詞頻＋模型**：用第 2 組的最佳分段，再由模型重選中文。
5. **中英文詞頻均停用＋模型**：英文分數固定 -6.0，英文 frequency 回傳 0，
   中文分數固定 -5.0；這也會失去英文詞庫提供的內部切分邊界。這是未調參的
   消融組，不能據此宣稱所有無詞庫方案都無效。
6. **已知正確分段＋模型**：直接使用 fixture 的注音與英文字面範圍，排除
   decoder 的邊界錯誤。此組只用於診斷，不是可部署的輸入流程。

模型串接方式：將每段連續完整注音透過既有 `Predict` 請求送出，取回每個
位置的第一候選；英文／符號按原文保留，並放入後續注音請求的 context。
已消耗的一聲空白不輸出。遇到英文字面範圍就分開請求，因此前面的中文
無法在同一請求裡看到後面的英文。沒有用候選機率比較不同分段。

## 實測結果

Apple M3，macOS 26.5.2，Metal，250M Q4_K_M 模型。整段文字完全一致計為正確。

| 組別 | 中文 28 | 前文 12 | 混輸 16 | 英文 12 | 合計 68 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 現行 | 16 | 7 | 6 | 12 | 41 |
| 無中文詞頻 | 8 | 3 | 3 | 12 | 26 |
| 現行＋模型 | 27 | 10 | 14 | 12 | 63 |
| 無中文詞頻＋模型 | 27 | 10 | 13 | 12 | 62 |
| 中英文詞頻均停用＋模型 | 16 | 11 | 4 | 11 | 42 |
| 已知正確分段＋模型 | 28 | 12 | 16 | 12 | 68 |

「已知正確分段」組的 12 個英文案例僅原文通過，不是模型辨識成功；真正
包含中文、需要模型選字的診斷案例為 **56/56**。兩種鍵盤對同一 fixture
使用相同注音，這不是 56 個獨立語言樣本。

具體改善：

| 目標 | 現行典型輸出 | 模型改善 |
| --- | --- | --- |
| 智慧 | 至會 | 智慧 |
| 模型 | 模行 | 模型 |
| 測試 | 冊市 | 測試 |
| 會議 | 會意 | 會議 |
| 權限 | 全縣 | 權限 |
| 我用python寫程式 | 我用python寫成市 | 我用python寫程式 |

模型能救回被 decoder 前四個同音字限制排除的字，例如對照表的「智」第 8、
「慧」第 5、「試」第 10、「式」第 12。原因是模型服務自行查詢完整同音字表，
不受已產生文字路徑中的前四字限制。

## 剩下的問題在中英邊界

「無中文詞頻＋模型」剩餘 6 個錯誤：

| 鍵盤 | 目標 | 輸出 |
| --- | --- | --- |
| 標準 | 程式 | t/是 |
| 標準 | 模型llama測試 | ai6vu/6llamahk4g4 |
| 標準 | 程式json | t/6g4json |
| 許氏 | 級（前文「他今年讀三年」） | jed |
| 許氏 | 及（前文「請填寫姓名以」） | jed |
| 許氏 | hello你好 | hell扭好 |

這 6 個案例的正確注音邊界都還在該組的候選 beam 裡，但最佳路徑選錯。
若傳入正確分段，模型皆能輸出目標文字。現行＋模型只剩 5 個錯誤，說明
直接拿掉中文詞頻有時仍會損失原本提供的語言／分段證據。

沒有中文詞頻不代表完全沒有先驗：本實驗仍有固定中文分數、同音字表的
原始順位、聲調證據與切換懲罰。英文詞頻停用組亦保留 ASCII grammar。

## 延遲

第一次預測（包含延遲載入模型）約 **265 ms**。只有實際發出模型請求的樣本
列入下表；每個樣本可能包含一個或多個連續中文 run。

| 組別 | 樣本數 | 暖機後 p50 | 暖機後 p95 |
| --- | ---: | ---: | ---: |
| 現行＋模型 | 53 | 11.9 ms | 25.3 ms |
| 無中文詞頻＋模型 | 52 | 5.7 ms | 20.4 ms |
| 已知正確分段＋模型 | 56 | 5.7 ms | 24.5 ms |

這些是整段的同步服務 round-trip，**不是每鍵桌面端到端延遲**。比較組依序
共用一個推論 session，後面組別可能受益於相同 prompt 的 KV cache，不能用
這些數字宣稱無中文詞頻組更快。未測快速打字、過期回應、手動選字保護或
UI 非同步套用；完整產品串接還需要這些 raw-key 回歸。

## 重現

在 repository 根目錄執行：

```sh
cmake -S engine/tools/model_mixed_probe -B build/model-mixed-probe \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DLLAVON_IME_WARNINGS_AS_ERRORS=ON
cmake --build build/model-mixed-probe --target model_mixed_probe --parallel 6
build/model-mixed-probe/model_mixed_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/model_mixed_cases.json \
  build/model-mixed-probe/report.json
```

Linux 可替換 service/model 路徑及 toolchain。工具使用自己建立的 socket
目錄、服務與 session；結束時關閉自己的服務，不使用輸入法目前的 socket，
也不記錄訓練提交。品質不一致是測量結果；fixture、協定或 raw-key 不變量
失敗會以非零狀態結束。機器可讀結果留在 `build/model-mixed-probe/report.json`。

實驗版本識別：

- core checkout：`0e825f44305777f9a35ea084ce288ecdb56d1cdd`（未修改）。
- 已安裝 service SHA-256：`b742ccd4f43cfe82a9b5da5e84ed4150c31ba57c7001b397ad08dbc532e33c02`。
- 模型 SHA-256：`4e0aa7fe4f080d4d1de1dff80cb2ca03a2952fb64170d10a177ad2a366a68b96`。
- 注音表 SHA-256：`18d9e1c337fbca39ccbfd1132cb1464405af6cdb5ab402d03475eb94300003a6`。

以上是少量、手寫、熟悉詞句的可行性實驗，沒有獨立大樣本或盲測；不能
當作一般使用準確率。沒有針對 fixture 調整模型或 decoder 權重。

驗證：兩次各自啟動服務的完整實驗，68 個樣本的六組文字輸出全部一致。
工具以 C++23、專案的完整 warning flags、`-Werror` 建置成功；既有
`ctest --test-dir build/engine-tests --output-on-failure` 的兩個測試 target
（unit 與 raw-key）均通過。實驗後 core 的 `git status --porcelain` 仍為空。

## 建議的下一步

先保留現有中英切分，在 `engine/` 以非同步方式接回模型中文選字，可避免
立即失去既有分段證據。再獨立處理候選邊界與預覽穩定性，評估移除中文
詞頻的效果。此路線的第一步可沿用既有服務 API，無須修改 ime-core。
