# 注音鍵盤配置與智慧中英文

## 1. 支援配置

設定頁的「注音鍵盤配置」現在提供七種選項；`keyboard_layout` 使用
下表的 canonical 值，JSON、Fcitx5 INI、macOS／Linux 原生設定 UI
都由共同 config schema 產生。

| 配置 | JSON 值 | 二聲／三聲／四聲／輕聲 | 「你好」實體按鍵 |
| --- | --- | --- | --- |
| 標準（大千） | `standard` | `6 / 3 / 4 / 7` | `su3cl3` |
| 許氏 | `hsu` | `d / f / j / s` | `nefhwf` |
| IBM | `ibm` | `m / , / . / /` | `7a,-;,` |
| 倚天（41鍵） | `et` | `2 / 3 / 4 / 1` | `ne3hz3` |
| 精業 | `ginyieh` | `q / a / z / 1` | `d-avla` |
| 倚天26鍵 | `et26` | `f / j / k / d` | `nejhzj` |
| 大千26鍵 | `dachen_cp26` | `e / r / d / y` | `surclr` |

一聲都用 Space。IBM 輕聲鍵是斜線 `/`；表格中的最後一組也寫作
「斜線」，避免把分隔符誤當成另一個聲調鍵。

聲調使用字母的配置，字母在音節開頭也可能是聲母。倚天26键的
聲韻轉換、大千26鍵的連按循環都由音節狀態決定，不是字母替換：

- 倚天26鍵：`ve ` → ㄑㄧ；`vx ` → ㄍㄨ；`p ` 主讀音是 ㄡ，保留 ㄆ 的替代讀音。
- 大千26鍵：`q`／`qq` → ㄅ／ㄆ；`l`／`ll` → ㄠ／ㄤ。
- 大千26鍵：`u`／`uu`／`uuu` → ㄧ／ㄚ／ㄧㄚ；`ju` → ㄨㄚ；`um` → ㄧㄡ。

直接映射配置中，分號、逗號、句點、引號、括號等若是注音鍵，
會先送入注音組字。Shift 標點及 Ctrl 標點保留既有操作。
許氏、倚天26鍵、大千26鍵的未 Shift 標點是半形；未分配給注音
的數字作字面數字，未完成音節時不會塞進該音節。

## 2. 共用智慧中英文

```text
使用者的原始按鍵 + 所選布局
              │
              ├── 字面英文／數字／識別字 paths
              └── 該布局的嚴格音節編輯器 → canonical 注音 paths
                                   │
                       共同 decoder／可逆 raw 候選
                                   │
                     當前顯示 path 的非同步同音字模型
                                   │
                         預覽 → 手選 → Enter 提交
```

「智慧型中英文」仍需明確開啟，預設關閉。七種配置都保留：

- 原始 raw 可選；Backspace 刪除目前顯示的一個字，Shift+Backspace
  撤銷一個未確認的原始按鍵。詳見[刪除與注音順序](smart-mixed-input-editing.md)。
- Down 開啟混輸候選；手選的完整文字固定，後續不重解。
- Enter 不等待模型；面板打開後晚到回應不能改變列號或預覽。
- 模型收到共同注音，沒有每種配置各自的 tokenizer 或英文白名單。
- 切換鍵盤設定時，未確認的 raw 依既有契約落為字面文字，避免被
  新布局重新解讀。

排序沿用既有的證據類型：字母聲調與英文競爭，數字／標點聲調
是較強的音節邊界訊號。没有按配置名稱、文章答案或個別單字調權重。
CP26 合法連按循環可進入嚴格 decoding，但任意覆寫仍不算音節
延伸。模型仍只改善目前切分內的同音字。

## 3. 參考來源

固定參考新酷音 libchewing snapshot
`3c4a93aa03d574c7f011ff84e8a2437c2f79b2cf` 的公開鍵位及狀態行為：

- [IBM](https://github.com/chewing/libchewing/blob/3c4a93aa03d574c7f011ff84e8a2437c2f79b2cf/src/editor/zhuyin_layout/ibm.rs)
- [倚天](https://github.com/chewing/libchewing/blob/3c4a93aa03d574c7f011ff84e8a2437c2f79b2cf/src/editor/zhuyin_layout/et.rs)
- [精業](https://github.com/chewing/libchewing/blob/3c4a93aa03d574c7f011ff84e8a2437c2f79b2cf/src/editor/zhuyin_layout/ginyieh.rs)
- [倚天26鍵](https://github.com/chewing/libchewing/blob/3c4a93aa03d574c7f011ff84e8a2437c2f79b2cf/src/editor/zhuyin_layout/et26.rs)
- [大千26鍵](https://github.com/chewing/libchewing/blob/3c4a93aa03d574c7f011ff84e8a2437c2f79b2cf/src/editor/zhuyin_layout/dc26.rs)

另對照小麥注音 `be6564acad6c4d3265c34a2e1a872d80f9db6068` 的
[倚天／倚天26鍵映射](https://github.com/openvanilla/McBopomofo/blob/be6564acad6c4d3265c34a2e1a872d80f9db6068/Source/Engine/Mandarin/Mandarin.cpp)。
本實作使用 `engine/` 自己的 Syllable、buffer、completion、rollback
契約；不宣稱與其他輸入法所有亂序按鍵及 UI 操作逐項相同。

## 4. 驗證順序與結果

先實作普通注音，五個新 raw-key suites 通過後，才接入各布局的
智慧中英文。最後用同一套 raw-key harness、共用 engine、Swift
C ABI 及真實模型服務做驗證。

功能回歸包括：

- 三種直接布局全部 37 個非聲調鍵：**111 個鍵位**。
- 五種新布局各五個聲調、刪除與 Shift 英文。
- 倚天26鍵的單字轉換／替代讀音，CP26 連按與已有介音的循環。
- 七种配置的中文／英文候選、原文、email／URL／識別字、錯鍵刪除、
  Escape、焦點提交與變更設定。
- 七种配置的正式非同步模型更新，以及晚到回應的面板／原文保護。
- macOS Swift adapter 對七种配置的普通／智慧混輸：**14 組提交**。

完整 engine unit／raw-key targets **2/2 通過**；新增布局相關
**9 個 suites 全通過**；Swift core 驗證通過。C++23、全部 warnings
與 `-Werror` 建置通過。`ime-core` 只有 read-only 參考，工作樹乾淨。

### 真實模型服務觀測

新手寫 synthetic fixture：3 篇／12 子句／6 個編輯流程，七种配置
各跑關閉／開啟模型，共 **42 篇文章輸入、168 個子句修復觀測、84
個編輯流程**。先保存 fixture、source／binary／model hashes 再執行；
這不是獨立真人盲測，結果没有用來修改排序。

在正確前文、等待當前預覽模型 settle 後，完整子句的數量：

| 配置 | 不開模型：自動正確 | 模型 settle 後：自動正確 | 首頁可修復（含已正確），前 → 後 |
| --- | ---: | ---: | ---: |
| 標準 | 4/12 | 10/12 | 7/12 → 11/12 |
| 許氏 | 3/12 | 9/12 | 6/12 → 10/12 |
| IBM | 2/12 | 6/12 | 6/12 → 9/12 |
| 倚天 | 3/12 | 7/12 | 7/12 → 10/12 |
| 精業 | 1/12 | 3/12 | 5/12 → 6/12 |
| 倚天26鍵 | 3/12 | 9/12 | 6/12 → 10/12 |
| 大千26鍵 | 3/12 | 8/12 | 6/12 → 9/12 |

編輯流程包含英文錯鍵修復、識別字、email、URL、混輸候選修復與
手選固定，**84/84 符合流程的提交目標**。這不代表文章逐句全正確。

無停頓、立即提交的文章測量仍有較多切分錯誤，且來不及採用模型
回應；其字元錯誤率沒有因開模型改善。精業在本批資料的無停頓
字元錯誤率約 75.9%，IBM 約 41.4%，因此**功能支援已接通，不等於
各配置的自動混輸品質已達到相同水準**。沒有為補這些失敗增加
布局特判；SmartEnglish 預設仍關閉，保留明確候選與原文操作。

第一個 observer 執行在啟動服務前發現 CP26 的 `ㄧㄡ` reverse encoder
有誤；修正量測用的鍵位產生器，補 `um`／`jm`／`ju` raw-key 測試。
保留 r1 preflight log／manifest，r2 使用完全相同 fixture，正式鍵位
編輯器與排序政策没有因這個失敗修改。

觀測檔位於 `build/model-mixed-probe/keyboard-layouts-20260930-r2/`；
附件保存結果與前次失敗。這次没有安裝、重啟已安裝的桌面輸入法。

## 5. 重現

```sh
cmake -S engine -B build/engine-tests \
  -DLLAVON_IME_ENGINE_BUILD_TESTS=ON \
  -DLLAVON_IME_WARNINGS_AS_ERRORS=ON \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build/engine-tests \
  --target llavon_ime_tests llavon_ime_rawkey_tests --parallel
ctest --test-dir build/engine-tests --output-on-failure
bash macos/scripts/verify-core.sh

cmake --build build/model-mixed-probe --target typing_usability_probe --parallel
build/model-mixed-probe/typing_usability_probe \
  "/Library/Application Support/llavon-ime/payload/bin/llavon-ime-unix-service" \
  "/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf" \
  engine/tests/rawkey/keyboard_layout_cases.json \
  build/model-mixed-probe/keyboard-layouts-new-results.json \
  0 production-layouts
```

Linux 設定 CMake 可省略 macOS 的 vcpkg toolchain 參數。`production-layouts`
使用七種配置，原有 research modes 仍預設只測標準／許氏。
