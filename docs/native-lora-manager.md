# 原生設定與個人化管理器

## 架構

```text
Qt Widgets 視窗 (C++23)
  ├── 輸入法設定：共用 engine schema，鍵盤／候選字／輸入行為／模型與執行
  ├── 替代詞彙：沿用原有格式與驗證，儲存後通知輸入法
  ├── 訓練資料：加密收集、密碼、檢視、篩選、分頁、排除、刪除
  ├── 模型與訓練：checkpoint、訓練器、強度、參數、進度、取消
  └── 訓練歷程：可自由拖曳的節點圖、基底、共同祖先、套用與回復
           │ 私有匿名管線 / JSON lines
           ▼
llavon-ime-lora-backend (單執行緒，無 HTTP)
           │ 資料庫 / fork + exec / 密碼 FD
           ▼
llavon-ime-lora CLI → 固定版本的 LoRA Trainer
```

Qt app 不載入 Torch 或輸入法推論模型。訓練器只在背景工作需要時啟動。
GUI 與後端分開，使既有的單執行緒 fork/exec 邏輯不會在 Qt 的多執行緒行程中執行。
關閉時回收工作群組；取消會先送 SIGTERM，五秒後送 SIGKILL。

歷程使用原生 `QGraphicsView`／`QGraphicsScene`：Bezier 曲線連接父子節點，
拖曳節點會同步更新連線；空白區可拖曳平移、滾輪可捲動、Ctrl／Command＋滾輪縮放。
縮放範圍為 60%–250%，提供縮放、重設視圖與重新整理節點佈局按鈕。
背景輪詢不會重設使用者的位置、縮放或選取；重開視窗時使用自動佈局。
鍵盤支援 `+`／`-` 縮放、`0` 重設、`[`／`]` 選取版本、方向鍵微調節點位置。

節點顯示訓練日期、強度、新增／累計筆數、步數與套用／最新狀態；點選節點後，
底部顯示父版本與主要參數，並提供設為基底、套用與共同祖先操作。

## 建置

使用 Qt **6.4 以上**，依賴 Widgets、Network、Linux 的 DBus 與測試時的 Test 模組。
沒有 Python、QML、Qt WebEngine 或瀏覽器 runtime。

- macOS：`brew install cmake pkg-config qtbase`
- Debian / Ubuntu：`sudo apt install qt6-base-dev qt6-qpa-plugins qt6-wayland`
- Fedora：`sudo dnf install qt6-qtbase-devel qt6-qtwayland`
- Arch：`sudo pacman -S qt6-base qt6-wayland`

服務的正常 CMake 建置會包含 GUI、後端與安裝項目。macOS 設定時仍要使用 repo 的
vcpkg toolchain，並先初始化子模組、bootstrap vcpkg。

也可以只建置管理器與 CLI，避免重建 llama.cpp：

```sh
cmake -S ime-unix-service/src/training/native -B build/lora-native \
  -DCMAKE_BUILD_TYPE=Release \
  -DIME_UNIX_SERVICE_BUILD_TESTS=ON
cmake --build build/lora-native --parallel
ctest --test-dir build/lora-native --output-on-failure
```

上述設定在 macOS 需另外加上
`-DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake"`，並指定既有
vcpkg 安裝前綴或 manifest。Linux 獨立建置也支援系統 SQLite、libsodium、utf8cpp 4 以上
與 nlohmann-json 開發套件。

## 看見實際 UI 與驗證

### 原有設定入口

- macOS 輸入法選單「設定…」開啟同一個 app 的設定頁，「管理個人化訓練…」開啟訓練資料頁。
- Linux Fcitx5 狀態選單提供「拉風設定與個人化…」；既有設定工具內的「開啟拉風設定與個人化」、
  「管理強制替代詞彙」及訓練按鈕會開啟對應頁面。齒輪只顯示啟動按鈕，原有欄位／詞彙編輯器已移除。
- `llavon-ime-settings`、`llavon-ime-phrases` 是同一個 app 的啟動入口；
  `llavon-ime-lora-gui --page settings|phrases|records|training|history` 可指定頁面。
  已執行時透過本機 socket 切換既有視窗的頁面。
- macOS 沿用 `~/.config/llavon-ime/config.json`，Linux 沿用
  `~/.config/fcitx5/conf/llavon-ime.conf`；兩者都遵循 `XDG_CONFIG_HOME`。
  替代詞彙共用 `~/.config/llavon-ime/phrase_overrides.txt`。
- 儲存會驗證 schema、保留未知設定、原子替換檔案；若其他程式改過檔案，要求重新載入，
  避免覆蓋外部修改。切換頁面保留尚未儲存的內容，關閉視窗會確認。
- macOS 透過 Darwin notification 重新載入；Linux 透過 Fcitx5 D-Bus
  `ReloadAddonConfig` 重新載入。修改推論參數時要求背景服務重新啟動。

### 替代詞彙查表

詞彙直接呈現為可編輯列表，每列包含詞彙文字、逐字注音下拉選單與刪除按鈕。
按「新增詞彙」加入空白列，輸入 2–8 個字後自動使用目前 `bopomofo_char.json` 查音。
只有一種讀音時自動填入，多音字必須明確選取；修改既有詞彙時保留位置與字元未變的讀音。
直接在各列修改或刪除，再按「儲存並套用」，不需要手寫注音格式。

原有檔案會轉成相同的列表控制項；不合法的既有讀音仍會顯示供修正，不會默默換掉。
無法解析的舊資料也會保留為待修正列，原有註解與空白行會在儲存時保留。

編輯時有問題的列即時顯示原因，全部有效時以底部摘要顯示；另有「檢查全部」按鈕，儲存前再次查表：

- 字數與注音數須相同，範圍為 2–8 個 Unicode 字元。
- 每個字與所選讀音必須在目前表內；不相符時顯示列號、字的位置及可用讀音。
- 同一組注音不可重複指定替代詞，避免引擎最後一行覆蓋前面的設定。
- 表內一聲結尾空白對應編輯器中的無聲調注音；空白與連字號分隔的同音組合視為重複。
- 找不到／無法解析指定的注音表時，不會跳過檢查或覆寫既有詞彙檔案。

`--tables-dir` 優先指定這個 app 的注音表；未指定時遵循 `LLAVON_IME_TABLE_PATH`、
`LLAVON_IME_TABLES_DIR`，接著尋找安裝位置與開發用原始表。滑鼠停在字音來源標籤可見完整路徑。

### 測試

`ime-unix-service/tests/lora_native_tests.cpp` 直接建立真正的 Manager 視窗，使用
Qt Test 點擊按鈕、切換篩選、輸入密碼，透過真正的後端與 CLI 操作獨立測試資料庫。
涵蓋設定密碼、錯誤密碼、解鎖、鎖定、分頁、手動選字篩選、排除、刪除、暫停／恢復
收集、清除、工作取消與失敗回報。另涵蓋完整設定 schema 往返、無效值拒絕、外部修改衝突、
未知設定保留、替代詞彙格式驗證、設定頁切換與儲存。

設定 `LLAVON_UI_CAPTURE_DIR`，測試會以 `QWidget::grab()` 保存實際畫面的 PNG。
macOS 用 `QT_QPA_PLATFORM=cocoa` 驗證實際桌面平台；Linux 可用 Xvfb 的 xcb 平台。
CI 用 `offscreen`，不需要登入桌面，也不需要瀏覽器。

```sh
QT_QPA_PLATFORM=cocoa \
LLAVON_UI_CAPTURE_DIR="$PWD/build/lora-ui-captures/macos" \
ime-unix-service/build/macos/src/training/native/llavon_lora_native_tests
```

另外可選擇跑真正的 CPU 一步 LoRA 訓練、GGUF 匯出、接續基底選擇、套用與回復：

```sh
LLAVON_UI_REAL_ASSETS="/path/to/existing/checkpoint/assets" \
LLAVON_UI_REAL_TRAINER="/path/to/llavon-lora" \
QT_QPA_PLATFORM=cocoa \
LLAVON_UI_CAPTURE_DIR="$PWD/build/lora-ui-captures/macos" \
ime-unix-service/build/macos/src/training/native/llavon_lora_native_tests realTraining
```

測試只讀取既有 checkpoint；所有測試文字、密碼、adapter、匯出模型與套用設定寫入
臨時目錄，結束後清除。預設測試不下載模型，也不執行耗資源的真實訓練。

## 已完成的驗證（2026-09-30）

- **macOS 26.5.2 / Apple Silicon / Qt 6.11.1**：在 Cocoa 平台操作真正的視窗，
  加密資料檢視與背景工作測試通過；另外完成一次真實 CPU LoRA 訓練（1 step）、
  GGUF 匯出、歷史版本選擇、模型套用與回復 Base model。
- **Ubuntu 24.04 / ARM64 / GCC 13 / Qt 6.4.2**：在獨立 QEMU VM 的 Xvfb / xcb
  平台編譯與操作真正的 Widgets，資料管理與背景工作測試通過，獨立安裝成功。
  Linux 的可選真實訓練測試未啟用。
- macOS 已測試完整 CMake 安裝與 Qt runtime 佈署；原生 GUI 執行檔約 **0.3 MiB**，
  使用此開發環境的 Homebrew Qt runtime 打包後 app 約 **72 MiB**。此大小不包含
  訓練器、Torch 或模型權重；Linux 使用系統共用 Qt，不在 app 中複製 Qt runtime。
- 安裝後的單一視窗啟動測試通過：再次開啟會喚回既有視窗。閒置 GUI 的單次 RSS
  量測約為 macOS **130 MiB**、Linux **58 MiB**（只量測 GUI 行程，不含後端、訓練器
  與模型；不同 Qt 版本、字型與桌面環境會影響數值）。
- 使用服務的完整 CTest，原有服務測試與新增原生管理器測試均通過。

上述大小與 RSS 是初版訓練介面的量測，尚未針對加入設定頁後重新量測。

統一設定頁後的追加驗證：

- macOS：Swift 前端型別檢查通過；實際執行 `EngineBridge` 驗證設定、詞彙、訓練頁的
  啟動參數，並驗證跨行程 Darwin 通知可更新運作中的 engine 設定。
- Linux：編譯並載入 Ubuntu 24.04 的 Fcitx5 5.1.7 addon；從原有 Fcitx 設定工具點擊
  「開啟拉風設定與個人化」成功啟動 app。在 app 修改執行緒數並儲存後，透過 D-Bus
  讀回運作中 addon 的新值，確認不需手動重新載入。engine、raw-key 與 metadata 三組 CTest 通過。
- 兩平台：真正的 GUI 子行程測試五種 `--page` 的單一視窗訊息；安裝後的設定／詞彙
  啟動器可切換同一個 app。macOS bundle 的完整簽章檢查通過。
- Linux 桌面驗證使用 Xvfb/xcb；Wayland 尚未實測。

新增的多分支 UI 測試以獨立 SQLite 測試資料建立 5 個版本，經真正的後端載入後，
驗證父子連線、節點拖曳與連線更新、畫布平移、按鈕／滾輪縮放、縮放界限、共同祖先、
設定訓練基底、鍵盤微調與背景更新保留視角。多分支截圖使用這組測試資料，並非五次
實際訓練結果；真實一步訓練另由 `realTraining` 測試驗證。

實際畫面輸出在 `build/lora-ui-captures/macos/` 與 `build/lora-ui-captures/linux/`；
這些 PNG 由正在執行的 Widgets 擷取，不是網頁模擬或設計稿。

## Apple Design 介面更新

設計參考 [emilkowalski/skills 的 apple-design](https://skills.sh/emilkowalski/skills/apple-design)：
採用系統字體、明確的側欄導覽、灰白／深色中性表面、藍色主要操作與即時按下回饋。
將原本佔據內容區的常駐狀態橫幅移至側欄，設定以標籤與控制項左右對齊呈現，
詞彙仍可逐列直接編輯，注音選擇器依寬度排列成 2–4 欄；800×620 視窗不需橫向捲動。
節點圖保留 1:1 拖曳與即時連線更新，不加入會延遲操作或改變節點落點的動畫。

`appearance.cpp` 集中管理外觀、配色及自行繪製的導覽圖示；使用內嵌 XPM 箭頭，
不增加 SVG/Web runtime。跟隨 Qt 提供的系統明暗 palette，切換外觀時保留未儲存的編輯。

更新後的 Cocoa 與 Ubuntu xcb 原生 UI suite 各為 **9 passed / 0 failed / 1 skipped**；
skip 是選用的真實訓練測試。另測試即時明暗切換、注音鍵盤操作、箭頭資源載入、
緊湊視窗無橫向溢出。實際新畫面位於 `build/lora-ui-captures/apple-macos/`、
`build/lora-ui-captures/apple-linux/`；`30`/`31` 為明暗設定頁，`32`/`33` 為明暗詞彙頁。

### 原入口追加端到端驗證

- macOS：呼叫真實 `LlavonInputController.menu()` 的 NSMenu target，驗證原「設定…」與
  「管理個人化訓練…」入口，以及舊詞彙入口與含空白路徑。安裝版 app 完成冷啟動、
  隱藏後喚回、同一行程切頁及終止後重開。測試時發現 Qt 單獨 `activateWindow()` 不會解除
  macOS 的 application Hide，已加入 AppKit `unhide`/`activate`。
  這是選單 action 與真實 app 的整合測試，不是在使用者的系統選單列中點擊輸入來源。
- Linux：點擊真實 Fcitx5「Input Method → 拉風輸入法 → Configure」齒輪，接著驗證三個
  原入口按鈕；從 app 儲存執行緒數後用 D-Bus 讀回運作中 addon 的新值。另驗證真實
  X11 tray action、縮小後恢復、終止後重開與 `.desktop` 入口；均維持單一管理器行程。

```sh
# 先完成 macOS app 的 CMake install；只操作該 staged app 與臨時資料。
LLAVON_TEST_REAL_GUI="$PWD/build/lora-native-install/bin/llavon-ime-lora-gui" \
  bash macos/scripts/verify-settings-entry.sh

# Ubuntu fixture；先把 addon 與 app 安裝到相同的獨立 prefix。
# 需要 fcitx5-config-qt、xdotool、ImageMagick、openbox、trayer、xterm、x11-utils、libatspi2.0-dev。
xvfb-run -a -s '-screen 0 1280x1024x24' dbus-run-session \
  bash scripts/verify-native-settings-linux.sh /path/to/test-install /path/to/empty-artifacts
```

Linux 整合腳本中的原始齒輪與 tray 位置固定於上述 Xvfb fixture；其他控制項透過
AT-SPI 名稱操作。macOS 終止重開測試使用 SIGTERM，未將其宣稱為關閉按鈕／Quit 測試。

## 設定遷移與舊介面移除

逐項比對舊 macOS/Fcitx 設定與共用 engine schema 後，所有鍵盤、選字、輸入行為、模型與
執行參數均由共用「輸入法設定」頁提供，UI 測試逐欄確認 schema 中每個 key 都有控制項。
此次補上原本仍留在平台介面的內容：

- macOS 每日自動檢查、背景下載與手動檢查更新，移至「軟體更新」頁。
- 輸入法版本及 Linux AT-SPI／上下文來源狀態，移至「版本與狀態」頁。
- 舊 macOS「重新載入替代詞彙」功能，移至共用詞彙頁的「重新載入」，同時通知 host。

刪除 `macos/App/SettingsWindow.swift`、`macos/App/UpdateWindow.swift`、
`macos/Core/EngineConfig.swift`、`fcitx5/fcitx5/ime_config.cpp` 及 `ime_config.hpp`。
移除 Swift 的 ConfigSchema/ConfigField/ConfigValue/EngineConfig 與 Fcitx SchemaOptions、
choice annotation、shared-config 雙向轉換、舊詞彙表單。共用 engine schema 繼續作為唯一
欄位定義；host 只載入執行期設定，平台入口保留啟動管理器的功能。

macOS 的 `SettingsHost` 使用同一使用者的 CFMessagePort，讓 GUI 背景 helper 讀取
運作中 IMK host 的版本／狀態，並操作同一個 Sparkle updater 的偏好。Linux 的
`org.llavon.IME.Update1.Status` 在 Fcitx event loop 回傳即時版本、上下文及生效設定。
新版 Fcitx 的 `save()` 不再寫設定檔，舊表單 `SetConfig` 只重新載入；舊詞彙表單提交
也不會覆寫檔案。JSON、INI 與詞彙文字檔仍在既有路徑，保留未知設定與原子儲存。

遷移追加驗證：

- Cocoa/xcb 原生 UI suite 各 **10 passed / 0 failed / 1 skipped**；skip 為選用真實訓練。
- macOS 完整 IMK app 編譯、Swift runtime/core、原選單入口及 settings-host 通訊通過。
  更新 checkbox 點擊、偏好重新讀取、檢查要求及 host 斷線測試使用隔離 host fixture；
  沒有下載或安裝正式更新套件。
- Linux addon／engine CTest **3/3**（含 raw-key）；真實齒輪、tray、切頁、最小化喚回、
  終止重開及 desktop 入口通過。另驗證舊 INI 值載入、新值即時生效、未知 key 保留、
  舊平台表單提交及 Fcitx 結束均不改寫 app-owned 設定檔。
- repo 根目錄 raw-key suite 與 staged macOS 管理器驗證通過。

```sh
LLAVON_TEST_HOST_HELPER="$PWD/ime-unix-service/build/macos/src/training/native/llavon-ime-lora-gui.app/Contents/MacOS/llavon-ime-lora-gui" \
  bash macos/scripts/verify-settings-host.sh
# Qt 更新互動測試可設定 LLAVON_SETTINGS_HOST_FIXTURE 指向上述腳本建立的 settings-host-tests。
```

遷移畫面位於 `build/lora-ui-captures/migration-macos/`（`34` 更新、`35` 狀態），
Linux 真實 addon 狀態畫面為隔離入口 fixture 的 `migrated-host-status.png`。

## 控制項細節修整

`controls.cpp` 統一設定、詞彙與訓練頁的下拉選單：明暗 popup 使用相同的列表間距、
圓角、選取底色及目前值勾選標記。長名稱會省略並可從 tooltip 讀取完整值；選單最多
顯示八列，較長列表可捲動，捲軸同樣跟隨明暗外觀。關閉選單時滾輪不會修改選項，鍵盤選擇與 Escape 取消仍沿用 Qt。

按鈕統一尺寸、留白與主要／次要／安靜／刪除操作的層次，補齊 hover、pressed、focus 及
disabled 狀態。焦點框預留相同尺寸，不造成版面位移；數字欄位的上下按鈕也有 hover／pressed
回饋。設定列表右側欄位採固定對齊寬度，訓練表單避免無限制拉長，checkbox 在兩平台及
更新頁使用同一個可縮放繪製。未設定資料密碼時，訓練頁會同時隱藏密碼欄位與標籤。

`controlInteractions` 以真實 Cocoa/xcb 視窗驗證下拉選單滑鼠選擇、鍵盤確認／取消、
滾輪不誤改、長名稱捲動選擇、數字步進按鈕、停用操作、焦點尺寸穩定及 800×620 版面。
另覆蓋 Qt 6.4 popup 曾取得焦點後的結束流程，先清除 popup 焦點與自有 proxy style，
避免 teardown 讀取已刪除的 style。原生 UI suite 各 **11 passed / 0 failed / 1 skipped**；
skip 為選用真實訓練。

實際控制項與打開的選單截圖位於 `build/lora-ui-captures/controls-macos/` 及隔離 Linux
fixture 的 `/home/lora/controls-captures/`。Qt popup 行為參考：
[QComboBox implementation](https://github.com/qt/qtbase/blob/v6.11.1/src/widgets/widgets/qcombobox.cpp)。
