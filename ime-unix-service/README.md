# IME Unix 服務

Llavon IME 在 Linux 與 macOS 上的 Unix socket 服務，為 Linux Fcitx5 附加元件與
macOS 原生 app 提供以 session 為單位的推論。模型載入、斷詞與 llama.cpp 推論由
儲存庫根目錄的 `ime-core` 子模組提供。

## 建置

在儲存庫根目錄初始化子模組，並從建置環境或 CI 傳入 vcpkg 工具鏈：

```bash
git submodule update --init --recursive
cd ime-unix-service
cmake --preset linux \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DIME_UNIX_SERVICE_BUILD_TESTS=ON
cmake --build --preset linux
ctest --test-dir build/linux --output-on-failure
cmake --install build/linux
```

安裝輸出包含服務，以及來自 `ime-core` 的標準表（位於設定的安裝前綴之下）：

```text
bin/llavon-ime-unix-service
bin/llavon-ime-lora
bin/llavon-ime-lora-gui
share/llavon-ime/tables/
```

## 執行

明確傳入必要的模型檔與表目錄：

```bash
dist/bin/llavon-ime-unix-service \
  --model path/to/llavon-ime-llama-250m-Q4_K_M.gguf \
  --tables path/to/tables
```

服務也接受透過 `LLAVON_IME_MODEL_PATH`、`LLAVON_IME_TABLES_DIR` 與
`LLAVON_IME_UNIX_SOCKET_PATH` 環境變數設定。前端會使用已安裝的執行檔，而不是把
服務加為 CMake 子目錄。

## 選用的本機 LoRA 訓練

輸入法的「**收集個人化訓練資料**」選項預設關閉。啟用後，完成且非敏感的注音提交
會送到每位使用者自己的服務，並儲存在 Linux 的
`${XDG_STATE_HOME:-$HOME/.local/state}/llavon-ime/training/commits.sqlite3` 或
macOS 的 `~/Library/Application Support/llavon-ime/training/commits.sqlite3`。
`LLAVON_IME_TRAINING_DATABASE_PATH` 可覆寫該檔案。文字只存在本機，永遠不會送到
模型下載服務。Unix socket 與資料庫僅限目前使用者存取。可用
`llavon-ime-lora list` 檢視待處理紀錄，並用 `llavon-ime-lora exclude --id ID`
或 `delete --id ID` 在訓練前移除紀錄。刪除已訓練過的紀錄會移除其儲存的文字與
讀音，但不會回復已經訓練完成的 adapter。

輸入法選單的「**管理個人化訓練…**」動作會啟動 `llavon-ime-lora-gui`，這是一個
獨立的本機網頁介面。macOS 的設定視窗有「**使用我的輸入改進模型**」按鈕；Linux 的
同一個按鈕位於 Fcitx5 輸入法設定中。與 Windows 相同，頁面會檢查或下載固定的
checkpoint，並提供相同的訓練設定：五種**訓練強度**（極低、低、中、高、進階；預設
極低）。非進階強度會套用共用的 preset（rank/alpha 8/16、dropout 0、batch size 與
gradient accumulation 1、固定 adapter 結構、FP32、`q_proj,v_proj`、device `auto`），
進階則可自行調整 rank、alpha、dropout、batch size、gradient accumulation、epochs、
max steps、learning rate、weight decay、warmup、max gradient norm、save every、
seed、max sequence length、target modules、device 與 dtype。另外有
「**只訓練曾手動選字的句子**」（預設開；下方會即時顯示本次會訓練幾筆，取消勾選
則訓練全部待訓練資料；沒有手動選字或無法轉換的紀錄會跳過，並在訓練資料列表標示
「未手動選字（本次不會訓練）」／「無法轉換（訓練時會跳過）」）、
「**降低模型遺忘（實驗性）**」（預設開；訓練後會保守縮小可能干擾原模型能力的
LoRA 維度，見 arXiv:2410.21228）與「**訓練基底**」選擇：預設接續最新一次完成的
訓練，也可指定任一歷史 run 續訓，或從 Base model 重新開始；選擇基底時會把該
adapter 的 rank、alpha、dropout 與 target modules 帶入進階欄位。
「**僅顯示手動選字過資料**」只篩選檢視畫面。
訓練資料頁可用「**未訓練／已訓練／已排除**」切換檢視（各自顯示筆數）；已訓練與
已排除的紀錄唯讀、不提供刪除，要清掉全部對話紀錄請用密碼區的
「忘記密碼，清除所有對話資料」。
待訓練紀錄全部會加入下一次訓練，不需要的紀錄在頁面上刪除。
按「開始訓練」時會先計算**實際可訓練的筆數**（無法轉換的紀錄會跳過）再確認，
進度與完成訊息也會顯示這個實際筆數。訓練資料摘要會顯示
`共 N 筆 · 手動選字 M 筆 · 可訓練 K 筆`，無法轉換的紀錄在列表上標為
「無法轉換（訓練時會跳過）」；有設定密碼時，精確筆數會在輸入訓練密碼後自動計算。
`預計最多 N steps` 使用樣本數計算（手動選字過的紀錄每個算三個樣本），與 Windows
相同，且在精確筆數已知時直接使用它。
頁面會輪詢資料庫，因此開啟期間輸入的紀錄會自動出現；顯示進度與執行歷史，
並可取消自己的行程群組。訓練歷程以 **Base model 為根的節點圖**呈現每次訓練
（父執行、分支、`新增/累計` 筆數、optimizer 步數、資料範圍、降低模型遺忘、
訓練強度或完整參數、`目前套用`／`最新訓練` 標籤），可以拖曳自由平移、
Ctrl＋滾輪縮放（0.6x–2.5x），也有 `＋`／`－`／`重設視圖` 按鈕；點選節點後可以
「**設為訓練基底**」或「**立即套用**」該次的 GGUF，套用 Base model 會清除自訂
模型路徑、改用安裝的預設模型。最新兩次訓練有共同的祖先 run 時，樹上會出現
「**共同祖先（Tarjan）**」按鈕，與 Windows 的 Tarjan 快捷相同。
無法轉換的紀錄會維持待處理。手動明確選擇候選字會貢獻三個樣本，其他紀錄則貢獻
一個。完成的 GGUF 可以直接從其歷史節點選為推論模型：套用時只保留該模型，其他
run 的 GGUF 會刪除（每個 run 的 adapter 一律保留）；若該 GGUF 已被先前的套用
刪除，會先從它的 adapter 重新匯出再套用（套用 Base model 則刪除所有個人化
GGUF、改用安裝的預設模型）。每筆紀錄會以驗證網頁介面的
風格渲染成附帶注音的預覽；頁面會到已安裝的字表（`bopomofo_char.json`）查讀音，
紀錄中的讀音缺少時改用字表，並標記字表未列出該字讀音的情況。
管理器會呼叫獨立的 `llavon-ime-lora` CLI。管理器只綁定 `127.0.0.1` 的隨機埠，
並使用每次啟動的存取 token。再次開啟時會沿用執行中的管理器；以較新原始碼建置的
管理器會取代閒置中的執行個體，讓瀏覽器不會停留在過舊的介面，但有工作進行中的
執行個體不受影響。管理器也會監看自己的執行檔：重新安裝二進位（例如
`scripts/build-linux.sh` 的安裝步驟）後，執行中的管理器會自動換成新版、保留同一個
頁面網址與 token，頁面偵測到新版 build 會自動重新載入；有訓練工作進行中時會等它
結束再替換。建置腳本另外會停掉仍在執行舊版（沒有自動替換能力）的管理器，並跳過
正在訓練的執行個體（`LLAVON_IME_SKIP_LORA_GUI_RESTART` 可停用）。頁面關閉後管理器會在閒置時結束，有訓練工作進行中則會繼續執行。
Linux 套件用 `xdg-open`、macOS 用 `/usr/bin/open` 開啟預設瀏覽器。想從終端機
啟動時，直接執行 `llavon-ime-lora-gui`（或套件中的私有執行檔路徑）。

`llavon-ime-lora` 是獨立的命令列管理器；不會在輸入法或預測服務裡執行 Torch。
GUI 的「**安裝／更新 LoRA Trainer**」動作會從
[lora-trainer](https://github.com/llavon-ime/lora-trainer) 下載該平台官方、
經 SHA-256 驗證的 CPU 發行版，安裝到使用者的訓練狀態目錄下並自動使用；
「**檢查版本**」按鈕（`llavon-ime-lora check-trainer`）只查詢固定 commit 的發行版
並顯示已安裝版本與是否有更新，不會下載任何東西；頁面開啟時也會自動檢查一次。
頁面以圖示＋標題＋說明列出**基礎模型**、**LoRA 訓練器**（已安裝版本、目前 submodule
對應發行版）與**訓練裝置**（偵測到的 GPU、使用的後端，以及是否還需要下載 libtorch）
的就緒狀態，與 Windows 的設定區相同。安裝的是整個發行目錄，因為
TorchSharp 會載入隨執行檔附帶的原生函式庫；只含執行檔的壓縮檔會被拒絕，缺少那些
函式庫的安裝永遠不會被視為可用。
安裝器是冪等的：已安裝的執行檔仍符合固定的 commit 與其記錄的 SHA-256 時不會
下載任何東西；已驗證的壓縮檔會快取在
`${XDG_CACHE_HOME:-$HOME/.cache}/llavon-ime/lora-trainer`，讓第二個目標或之後
的打包流程重複使用（`LLAVON_IME_LORA_TRAINER_CACHE` 可覆寫該目錄）。
與 Windows 相同，`lora-trainer` 是固定版本的原始碼子模組：其 Git commit 決定
要用的對應發行執行檔，但 app 不會從子模組建置 Torch。安裝器會等 `latest.json`
回報該 commit，然後在發佈執行檔前檢查不可變的版本化發行 manifest 與平台壓縮檔的
SHA-256。永遠不會用其他發行 commit 替代。`scripts/build-linux.sh` 與 macOS 的
`scripts/build-macos.sh` 會在安裝時執行同樣的驗證下載：Linux 放到
`<private library dir>/llavon-ime/tools/lora`，macOS 系統安裝放到
`/Library/Application Support/llavon-ime/tools/lora`、`--user` 安裝放到
`<install prefix>/lib/llavon-ime/tools/lora`；設定
`LLAVON_IME_SKIP_LORA_TRAINER` 可跳過。deb、RPM 與 macOS 套件會在打包時內附該
固定發行版（CI 在 release workflow 下載），因此安裝後就已帶有 trainer；GUI
動作之後會為目前使用者就地更新。開發與測試時，`LLAVON_IME_LORA_CLI_PATH` 可
覆寫執行檔，`LLAVON_IME_LORA_ASSETS_DIR` 可覆寫 checkpoint 與訓練執行根目錄，
與 Windows 服務的變數一致。管理器會檢查執行檔 `--version --json` 的結果是否為
trainer API 2，而不是信任發行 manifest 的 API 欄位。在 Debian/RPM 套件中，
管理器位於 `/usr/lib/llavon-ime/llavon-ime-lora` 或
`/usr/lib64/llavon-ime/llavon-ime-lora`；macOS 則在
`/Library/Application Support/llavon-ime/payload/bin/llavon-ime-lora`。
以下範例假設它的目錄已加入 `PATH`。

例如：

```sh
state="$HOME/.local/state/llavon-ime/training"
assets="$state/assets"
llavon-ime-lora fetch-model --output-dir "$assets"
llavon-ime-lora install-trainer --output-dir "$state/tools/lora"
revision="$(cat "$assets/current.revision")"
model_dir="$assets/$revision"

llavon-ime-lora list
llavon-ime-lora dataset --model-dir "$model_dir" \
  --tables-dir /path/to/installed/share/llavon-ime/tables \
  --output "$assets/review.jsonl"
llavon-ime-lora train --model-dir "$model_dir" \
  --tables-dir /path/to/installed/share/llavon-ime/tables \
  --revision "$revision" \
  --strength low --only-manually-selected 1 --stabilize-intruders 1 \
  --output-dir "$state/runs/first"
# 續訓指定的歷史 run（省略 --base-run-id 則接續最新一次；0 代表 Base model）
llavon-ime-lora train --model-dir "$model_dir" \
  --tables-dir /path/to/installed/share/llavon-ime/tables \
  --revision "$revision" --strength advanced --base-run-id 3 \
  --output-dir "$state/runs/branch"
# 從保留的 adapter 重新匯出某次訓練的 GGUF（被刪除或需要重建時）
llavon-ime-lora export-model --db "$db" --run-id 3 --model-dir "$model_dir"
```

管理器會檢查 checkpoint 版本、模型詞彙與相容的表格式，然後在開始前用 trainer
驗證數值 JSONL。目前的推論表比公開訓練 checkpoint 的詞彙多出兩個 token ID；
管理器會把已知的新增項目投影回與 checkpoint 相容的表，並讓不支援的提交維持
待處理。訓練預設為 auto/float32。`--device` 可選 `auto`、`cpu`、`cuda` 與
`mps`：`auto` 會依序選擇 CUDA（含 ROCm）、Apple Silicon 的 Metal 與 CPU，
`mps` 需要 macOS 的 Metal 版 trainer。「安裝／更新 LoRA Trainer」以及每次訓練
前，管理器會偵測顯示卡：AMD 會下載 PyTorch 官方的 ROCm libtorch、NVIDIA 會下載
CUDA libtorch（分別約 9.4 GB 與 3.9 GB，只下載一次，快取在
`~/.cache/llavon-ime/lora-libtorch`），並以硬連結放進已安裝的 trainer 目錄
（需要 `unzip`），因此不需要另一種 trainer 發行版就能用 GPU；沒有可用的顯示卡時
維持 CPU 訓練，macOS 的 CPU 產物本身已內建 Metal。要用自己建置的 trainer 時，
把 `LLAVON_IME_LORA_CLI_PATH` 指向該執行檔即可。
每次執行會寫出一個 adapter 與一個 Q4_K_M GGUF 模型。與 Windows 相同，預設會從
最新一次完成的 adapter 繼續；`--base-run-id` 可以指定任一歷史 run 作為續訓基底，
或填 0 從 Base model 重新開始。非進階強度會沿用基底的 rank、alpha、dropout 與
target modules（adapter 結構不能改變），進階強度則要求與基底相符。基底記錄的
基礎 checkpoint 版本會被沿用，而不是剛傳入的目錄。啟用「降低模型遺忘」時
（`--stabilize-intruders 1`），訓練先寫到 `adapter-before-stabilization`，再由
trainer 的 `stabilize-adapter --scale 0.9` 產生最終 adapter；找到 intruder 時
rank 會增加 1，並同步調整 alpha 以維持原本的 `alpha / rank` scaling，成功後原始
目錄會刪除。訓練歷史記錄的是最終 adapter 的 rank/alpha，因此下一次續訓會沿用
正確的結構。每次執行也會在 run 目錄寫出 `training_request.json`，並把相同內容存
入資料庫（`training_request_json`，含強度、資料範圍、基底與完整參數）。每個 run
的 adapter 會一直保留；量化 GGUF 只保留目前套用的那一個（與 Windows 相同），
其餘會在套用時刪除，需要時由 GUI 的「立即套用」或 `export-model` 從 adapter
重新匯出。紀錄只會在模型匯出後標記為已訓練；在 GUI 歷史中選用產生的 GGUF 之前，
原本的推論模型會保持使用中。Linux 會重新載入 Fcitx5 設定並為新模型重建推論傳輸；
macOS 會收到本機通知、重新讀取已儲存的設定並重新啟動預測服務。
