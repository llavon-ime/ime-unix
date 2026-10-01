# 智慧混輸：刪除顯示字與注音順序

## 1. 編輯契約

先前把「可逆」等同於「Backspace 永遠刪一個原始按鍵」，造成畫面
已經是 `你好`，按一下卻變成 `你cl`。現在把使用者要刪的文字與
要修的按鍵分開，七種布局共用相同行為：

| 畫面／操作 | 結果 |
| --- | --- |
| `你好` + Backspace | `你`，移除「好」整個 raw 音節及其隱含邊界 |
| `你` + Backspace | 空組字，不提交 |
| `你好hello` + Backspace | `你好hell` |
| `你cl` + Backspace | `你c`，未完成注音仍逐鍵編輯 |
| 未確認 `你好` + Shift+Backspace | 標準布局為 `你cl`，只撤銷最後的 `3` |
| 游標移到 `你\|好` + Delete | `你` |
| 游標移到 `你\|好` + Backspace | `好` |
| 已提交、組字為空 + Backspace | 交回客戶端，由文件編輯器刪除 |

刪除依照目前顯示 path 的 raw 範圍，不反推中文字的鍵位。它保留
剩下的顯示前綴、候選與原文，不因刪掉 `_` 等字元就重新猜前面的
文字是中文；後續新輸入仍可以重新解碼。這不是手選或隱藏提交。
一聲 Space 屬於其中文字；被解碼消耗、没有顯示的後續 Space 也不
需要額外按一下才刪掉該字。真正顯示的英文空白則正常刪除一個空白。

Shift+Backspace 的逐鍵撤銷只針對未確認 pending raw。已手選／游標
移動後落入 composition buffer 的中文字仍按字編輯。既有面板與
過期回應保護繼續生效；刪掉的中文字不能被晚到模型回應補回。

局部注音錯鍵導致整段退回英文的問題，另由
[錯鍵局部保留](smart-local-phonetic-errors.md) 處理：未解析區段保留
raw，沒有自動猜讀音，並允許後續有效音節恢復中文解析。

## 2. 先介音／韻母，再聲母

嚴格音節接受的是「填入空的聲母、介音、韻母槽」，不是要求按鍵
必須依聲母 → 介音 → 韻母出現。聲調仍是音節完成邊界，重複覆寫
同一個槽不算這種亂序輸入。

```text
標準布局：u       s          3
          ㄧ  →  ㄋㄧ  →  ㄋㄧˇ → 你
raw 保留：u      us         us3
```

智慧模式中的未完成輸入仍以 raw 顯示；上述實際預覽是
`u` → `us` → `你`。不是針對「你」加特例：也測試了 `中` 的全部
六種聲／介／韻排列，及 `表`、`寫`、`點` 的各六種排列。

許氏、倚天26鍵等 compact 布局有一鍵多義，因此在已有介音／韻母
且缺聲母時，增加該鍵 standalone 聲母的合法解讀，保留原本的
contextual 讀音。聲調鍵不跨越完成邊界重新當聲母。CP26 的既有
連按循環與覆寫拒絕規則共用於 buffer 和 lattice replay。

選中的 canonical 注音會再次用相同 raw replay 驗證，再寫進 buffer，
避免候選上能選「你」，實際提交卻套用另一個 compact 讀音。

| 布局 | 先 ㄧ 再 ㄋ，三聲的 raw |
| --- | --- |
| 標準 | `us3` |
| 許氏 | `enf` |
| IBM | `a7,` |
| 倚天 | `en3` |
| 精業 | `-da` |
| 倚天26鍵 | `enj` |
| 大千26鍵 | `usr` |

七種布局都能選到「你」與原文。精業的這個 raw 在觀測中仍預覽
原文，需要 Down 選字；compact 布局也可能同時有 ㄧㄣˇ 等讀音。
增加合法解讀沒有改排序權重，也没有保證任何歧義 raw 都自動選對。

## 3. 驗證與證據

- 新增 **5 個 raw-key suites**：七布局按字刪除、逐鍵撤銷、未完成
  注音、中段 Delete／Backspace、手選原文、英文負例與音節排列。
- 新增正式模型時序 suite：刪除後晚到回應、已模型選字前綴的刪除、
  標準／許氏／倚天26鍵乱序候選與提交；既有逐鍵修聲調的回歸明確
  改用 Shift+Backspace。
- 完整 resource／engine unit／raw-key targets **3/3 通過**；C++23、
  共同 warnings 與 `-Werror` 建置通過。
- macOS Swift adapter 通過七布局 Backspace／raw Shift 傳遞檢查，
  以及標準布局 `us3` 預覽與提交。
- 真實服務／250M Q4_K_M 模型：五個共用編輯流程及每布局一個亂序
  流程，七布局 × 模型關閉／開啟，共 **84/84 通過**。檢查實際
  preedit checkpoint 與文件提交；部分流程明確手選。
- 既有已看過的 structured corpus 回歸：**618/618** 純 ASCII 預覽
  保留原文，**1854/1854** 混輸正確解讀仍在九列首頁，沒有新增
  自動錯改或首頁候選損失。這批混輸並沒有自動正確，不能把可修復
  當作自動準確率。
- 舊聲調修復流程改用明確 raw undo 後，兩布局 × 模型關閉／開啟
  的 **16/16** 檢查通過；以 `你好` 與兩組英文為種子的錯鍵測試，
  立即／延遲修復各 **122/122** 恢復乾淨輸入的引擎結果。輸出明確
  標記 Shift+Backspace，這不是普通按字刪除的自動修復能力。

真實服務 fixture 在執行前凍結，SHA-256 為
`99fcd822081cb42afb2de8e4ab04835af98aec20d892513de4f9efa2d4fa821d`。
產物位於 `build/model-mixed-probe/smart-editing-20260930/`，包含
manifest、每鍵 trace、summary 與 structured 回歸比較。

這些是手寫機械編輯檢查與既有資料回歸，不是新的獨立真人可用性
盲測。它們證明特定編輯契約，而不是整體智慧混輸已經好用。
實作位於本 repo 的 `engine/`，`ime-core` 沒有修改；這輪没有安裝
或重啟桌面輸入法。

## 4. 重現

```sh
cmake -S engine -B build/engine-tests \
  -DLLAVON_IME_ENGINE_BUILD_TESTS=ON \
  -DLLAVON_IME_WARNINGS_AS_ERRORS=ON \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build/engine-tests \
  --target llavon_ime_resource_tests llavon_ime_tests llavon_ime_rawkey_tests --parallel
ctest --test-dir build/engine-tests --output-on-failure
bash macos/scripts/verify-core.sh

cmake --build build/model-mixed-probe --target typing_usability_probe --parallel
build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/smart_editing_cases.json \
  build/model-mixed-probe/smart-editing-repeat.json 0 production-layouts
```

Linux 設定 CMake 可省略 macOS 的 vcpkg toolchain 參數。量測 runner
支援每個 workflow 的布局限制與 `expect_preedit` checkpoint，以免
只檢查最終提交而漏掉中間的編輯錯誤。
舊量測中的「刪回某個 raw offset」與聲調修復流程已明確改用
Shift+Backspace，輸出也標記 `repair_key`，不能再把它解釋為普通
Backspace 按字刪除的可用性結果。
