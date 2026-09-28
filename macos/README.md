# macOS 原生前端

macOS 原生輸入法是以 Swift 撰寫的 InputMethodKit app，透過引擎的 C ABI
（`engine/include/llavon_ime/llavon_ime.h`）驅動與宿主無關的引擎。macOS 上已取代
fcitx5-macos 前端；Linux 仍繼續使用 fcitx5 附加元件。

## 目錄結構

- `App/` — 輸入法 app（Swift）：IMK server、輸入控制器、引擎橋接、候選視窗、
  按鍵對應、`Info.plist`。
- `Tests/` — `CoreTests.swift`，平台無關 Swift 核心的純函式測試
  （`scripts/verify-core.sh`）。輸入行為不在這裡測試：它屬於共用的 raw-key
  測試套件（見下方「測試」）。
- `Smoke/` — C ABI 的無介面 Swift 冒煙測試（不使用 InputMethodKit）。
- `scripts/build-native-app.sh` — 建置引擎、編譯 app bundle、ad-hoc 簽章，可選擇
  安裝。

## 建置與安裝

```sh
scripts/build-macos.sh                       # 與 Linux 對稱的入口：建置、測試並安裝全部
scripts/build-macos.sh --user                # 裝到 ~/Library（不需要 sudo）
scripts/build-macos.sh --no-service          # 只建置與安裝 app

macos/scripts/build-native-app.sh            # 只建置 dist/macos/LlavonIME.app
macos/scripts/build-native-app.sh --install  # 安裝 app 與服務（與上方共用同一套流程）
```

`scripts/build-macos.sh` 是 `scripts/build-linux.sh` 的 macOS 對應入口，內部呼叫
`macos/scripts/build-native-app.sh --install`。

建置需要 `pkg-config`（vcpkg 用）與 CMake；腳本會自己 bootstrap vcpkg。第一次
建置時 vcpkg 會從原始碼編譯 libsodium，需要 autotools：`brew install autoconf
autoconf-archive automake libtool`。

`--install` 也會透過 `scripts/build-macos-service.sh` 建置、測試並安裝 AI 預測
服務：系統安裝裝到 `/Library/Application Support/llavon-ime/payload`（套件使用、
app 優先讀取的路徑），`--user` 時裝到 `~/Library/fcitx5`。位於
`/Library/Application Support/llavon-ime/models` 的模型已存在就沿用，否則下載。

服務安裝也會下載固定版本的 LoRA Trainer 到套件相同的位置：系統安裝是
`/Library/Application Support/llavon-ime/tools/lora`，`--user` 是
`~/Library/fcitx5/lib/llavon-ime/tools/lora`（`LLAVON_IME_SKIP_LORA_TRAINER`
可跳過）。安裝完成後會停掉仍在執行舊版、無法自我接手的 LoRA 管理器
（`LLAVON_IME_SKIP_LORA_GUI_RESTART` 可跳過；訓練中的管理器不受影響）。
`--no-service` 會跳過服務與 LoRA Trainer，只安裝 app。

安裝步驟會把 app 複製到 `/Library/Input Methods/`，也就是套件使用的位置，因此
套件安裝與開發安裝不會互相 shadow。它會先移除 `~/Library/Input Methods/` 的
舊 copy，因為同 bundle ID 的使用者層 copy 會蓋掉系統層的，也會讓套件安裝時把
bundle relocate 到家目錄。沒有 sudo 的機器可用 `--install --user` 裝到家目錄。

搬動 bundle 會讓文字輸入系統丟掉輸入來源，這就是安裝後輸入選單裡沒有
「拉風輸入法」的原因。因此這個步驟會用套件建置的同一個 helper
（`packaging/macos/tools/tis.c`）重新註冊輸入來源，並在 macOS 15 及更早的版本
把 bundle 加入使用者的已啟用輸入來源（`com.apple.HIToolbox` 的
`AppleEnabledInputSources`，也就是登入時重建輸入選單的依據）。它不會用
`TISEnableInputSource` 做這件事：該呼叫會回傳 noErr，但對第三方輸入法不會寫入
任何東西。

macOS 26（Tahoe）把已啟用的第三方輸入來源存在受保護的 store
（`com.apple.inputsources`），只有「系統設定」能寫入；把舊的 HIToolbox 清單寫
進去會讓輸入選單在下次登入前失去來源清單，所以 helper 不碰它：macOS 26 上必須
在「系統設定 › 鍵盤 › 輸入方式」加入一次（或在安裝後登入一次）。套件的
postinstall 也做一樣的事，並在 payload 安裝後啟動 app 一次，因此升級在任何版本
上都能不必登出就繼續運作。macOS 只會註冊 bundle ID 含 `.inputmethod.` 的輸入法
（預設 bundle ID 是 `com.llavon.inputmethod.LlavonIME`）。

## Caps Lock 切換

要用 Caps Lock 在「拉風輸入法」與英文之間切換，請在「系統設定 › 鍵盤 › 輸入方式」
啟用「使用大寫鎖定鍵切換至／從…」。輸入來源必須保持非拉丁：
`tsInputMethodCharacterRepertoireKey` 與 `tsInputModeCharacterRepertoireKey`
不能列出 `Latn`。列出 `Latn` 會把輸入來源標記為可輸入 ASCII，macOS 就會將它
歸類為拉丁來源，Caps Lock 切換也不再把它當作中文那一邊。

## 設定

輸入選單在「拉風輸入法」下會列出「設定…」。設定會存到
`~/.config/llavon-ime/config.json`（支援 XDG），強制替代詞彙則位於
`~/.config/llavon-ime/phrase_overrides.txt`，與 fcitx5 附加元件共用；設定視窗中的
「編輯替代詞彙…」會用預設編輯器開啟該檔案。在第一次儲存設定之前，fcitx5 前端
留下的 `~/Library/Application Support/fcitx5/conf/llavon-ime.conf` 仍會繼續生效。
設定視窗左下角會顯示版本：開發建置帶有 `r<commit 數>.g<hash>` 後綴（與
`scripts/build-linux.sh` 提供給 fcitx5 的資訊一致），發行套件只有版本號。

設定視窗是從引擎的設定 schema（`engine/src/config/config_schema.cpp`）產生的，
該 schema 同時驅動 fcitx5 附加元件的設定：只要在 schema 新增選項就夠了，因為
JSON/INI 的（反）序列化、C ABI 的 schema 匯出與兩邊的設定介面都由此衍生。新增
選項不需要改任何 Swift 或附加元件程式碼。

**使用我的輸入改進模型**設定按鈕（也可從輸入選單的**管理個人化訓練…**開啟）會
啟動已安裝服務 payload 中獨立的 `llavon-ime-lora-gui` 執行檔。它會開啟與 Linux
相同的臨時本機瀏覽器介面；原生設定視窗仍負責靜態設定欄位。套件會把固定版本的
LoRA Trainer 發行版放在 `/Library/Application Support/llavon-ime/tools/lora` 下；
如果該目錄不存在（例如手動清理過），postinstall 腳本會下載它，下載失敗不會中斷
安裝，因為設定頁面之後仍可安裝。開發安裝（`scripts/build-macos.sh` 或
`macos/scripts/build-native-app.sh --install`）同樣會下載這份 trainer。

模型路徑的規則與 fcitx5 附加元件相同：以設定檔儲存的值為準，否則使用
`/Library/Application Support/llavon-ime/models` 下已安裝的模型。
`LLAVON_IME_MODEL_PATH` 只會覆寫預測服務啟動時使用的路徑，不會寫回設定。

app 依以下順序尋找資源：

1. `LLAVON_IME_*` 環境變數覆寫（`TABLE_PATH`、`TABLES_DIR`、`MODEL_PATH`、
   `UNIX_SERVICE_PATH`、`PHRASE_OVERRIDES_PATH`）。
2. 套件 payload：`/Library/Application Support/llavon-ime/payload`（服務 + 表）與
   `/Library/Application Support/llavon-ime/models`。
3. 開發安裝：`~/Library/fcitx5`（由 `scripts/build-macos-service.sh` 產生）。

沒有服務或模型時，引擎仍可用表候選字運作；預測是額外加分項。

## 手動測試清單（TextEdit）

- 注音輸入、候選清單、翻頁、選字鍵、提交。
- 智慧英文與詞彙標記提示（Shift+←、Enter、Esc）。
- 切換 app／焦點（失焦時提交組字）。
- 密碼欄位（不得讀取或送出上下文）。
- 游標前後文字提供給預測。

## 測試

一套標準行為測試套件，所有前端共用，外加按鍵到不了的部分的單元測試：

- `engine/tests/rawkey/`（`llavon_ime_rawkey_tests`）— 標準套件：餵入原始按鍵，
  檢查面板狀態與提交結果，透過兩個前端共用的無宿主引擎 API 驅動。它在 Linux 與
  macOS 上執行，情境可以讓傳輸指向腳本化的或真實的預測服務。所有輸入行為
  （注音、智慧英文、候選導覽、符號選單、強制替代詞彙、數字鍵盤、生命週期、預測）
  都在這裡。
- `engine/tests/`（`llavon_ime_tests`）— 引擎內部、無法用按鍵表達的單元測試：
  協定框架、設定解析、UTF 處理、服務傳輸與 C ABI 契約。
- `macos/scripts/verify-core.sh` — 純函式 Swift 核心：按鍵轉換、設定 schema/值
  模型與候選翻頁計算。

```sh
scripts/verify-engine-tests.sh   # 單元 + raw-key 套件（Linux 與 macOS）
macos/scripts/verify-core.sh     # Swift 核心
```

## 冒煙測試（僅引擎）

```sh
cd fcitx5
cmake --preset macos
cmake --build --preset macos --parallel
ctest --test-dir ../build/macos --output-on-failure   # 4/4

cd ..
swiftc -O -parse-as-library \
  -I engine/include \
  macos/Smoke/Smoke.swift \
  build/macos/engine/libllavon_ime_engine.a \
  -lc++ -framework Foundation \
  -o /tmp/llavon-ime-smoke

LLAVON_IME_TABLE_PATH=ime-core/table/bopomofo_char.json \
  /tmp/llavon-ime-smoke
```

冒煙測試使用 `auto_start_service = 0`，所以永遠不會啟動模型服務；預測會快速
失敗，引擎改回表候選字。

## C ABI 契約

- 所有引擎呼叫都發生在宿主的主執行緒。
- `lv_host.post` 由背景執行緒呼叫（預測回應），必須把 `body(body_user)` 交回
  主執行緒（marshal）。`lv_engine_destroy` 會清空這些回呼；它回傳之後再呼叫
  `body` 是 no-op。
- `lv_host.commit`、`lv_host.update_ui` 與 `lv_host.surrounding_text` 在主執行緒
  執行。
- `lv_engine_render` 會存下一份快照供文字存取子讀取；`update_ui` 回呼已在其
  上下文中更新它。
- `lv_surrounding_text` 的位移是 UTF-16 碼元。
- 按鍵欄位對應引擎的 `InputKey`：`sym` 是 X11 keysym，`states` 存放修飾鍵位元，
  `release` 標記放開按鍵（key-up）事件（會收到，但不消費）。

## 備註／已知缺口

- 套件仍打包 fcitx5-macos 設定；等原生 app 在 macOS 驗證完成後，才會把
  `scripts/package-macos.sh`、Homebrew tap 與 release workflow 遷移過來。
- app 未簽章（ad-hoc）；要發佈需要 Developer ID 與 notarization。
- 候選視窗以 `attributes(forCharacterIndex:lineHeightRectangle:)` 定位；可能需要
  依各客戶端 app 調整座標。
