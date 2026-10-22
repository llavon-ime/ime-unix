# 拉風輸入法（Linux / macOS）

會看前後文選字的繁體中文注音輸入法。所有推論都在本機完成，不會上傳你打的字。

本儲存庫包含 Linux 的 Fcitx5 前端、macOS 原生輸入法，以及兩個平台共用的本機預測服務。Windows 版在 [ime-windows](https://github.com/llavon-ime/ime-windows)。

> [!IMPORTANT]
> 專案仍在早期開發階段，功能、操作方式與安裝流程都可能變動。

## 支援平台

| 平台 | 架構 | 安裝方式 |
| --- | --- | --- |
| Debian 13 | x86_64 | `.deb`（需要 Fcitx5 ≥ 5.1.12、glibc ≥ 2.41） |
| Fedora 43 | x86_64 | `.rpm` |
| Arch Linux | x86_64 | AUR `llavon-ime-fcitx5-git` |
| macOS 13 以上 | Apple Silicon | `.pkg` |

其他發行版可以自行[從原始碼建置](#從原始碼建置)，但沒有經過測試。

## 安裝

### Debian / Ubuntu

從 [Releases](https://github.com/llavon-ime/ime-unix/releases/latest) 下載 `.deb`：

```bash
sudo apt install ./llavon-ime-fcitx5_<版本>_amd64.deb
```

套件以 Debian 13 建置。其他以 Debian 為基礎的發行版，只要 Fcitx5 與 glibc 版本符合上表也能安裝，但沒有經過測試。

### Fedora

```bash
sudo dnf install ./llavon-ime-fcitx5-<版本>-1.<fedora>.x86_64.rpm
```

### 加入簽章套件來源（選用，建議）

加入官方 APT / DNF 來源後，就能用系統套件管理員，或拉風設定裡的「軟體更新」頁更新。步驟（含公鑰指紋）見 [linux-packages](https://llavon-ime.github.io/linux-packages/)。

### Arch Linux

使用 AUR 套件 [`llavon-ime-fcitx5-git`](https://github.com/llavon-ime/aur-llavon-ime-fcitx5-git)。這個套件會從原始碼建置，不含 LoRA 訓練器；需要時可從拉風設定安裝。

### 啟用（Linux）

開啟 Fcitx5 設定工具，把「拉風輸入法」加入輸入法清單，然後重新載入 Fcitx5：

```bash
fcitx5 -r
```

### macOS

1. 從 [Releases](https://github.com/llavon-ime/ime-unix/releases/latest) 下載 `llavon-ime-<版本>-arm64.pkg`。
2. 開啟安裝包，依照畫面指示完成安裝。
3. 安裝完成後，**登出並重新登入，或重新啟動 Mac**。
4. 前往「系統設定 → 鍵盤 → 文字輸入 → 編輯」，加入「拉風輸入法」。
5. 從選單列的輸入來源選單切換至「拉風輸入法」。

若 macOS 阻擋安裝，請前往「系統設定 → 隱私權與安全性」，選擇「仍要打開」。

> [!NOTE]
> **Homebrew Cask 已停止更新。** 用 Homebrew 裝過的使用者：先備份設定與訓練資料，再直接用新的 `.pkg` 覆蓋安裝，不需要先解除安裝。之後改由輸入法內建的更新功能管理，請不要再對這個 Cask 執行 `upgrade`、`uninstall` 或 `--zap`，以免刪掉輸入法與資料。

## 使用方式

切換到拉風輸入法後，就能用注音輸入。第一次輸入時，輸入法會自動啟動本機預測服務並載入模型，需要稍等一下。服務還沒準備好時，會先用注音表提供候選字。

設定入口：

- **Linux**：輸入法選單或 Fcitx5 齒輪 →「拉風設定與個人化…」
- **macOS**：輸入來源選單 →「設定…」

鍵盤配置、候選字、強制替代詞彙、個人化訓練與軟體更新都在同一個設定程式裡。

### 鍵盤配置

支援標準、許氏、IBM、倚天、精業、倚天 26 鍵與大千 26 鍵，可在設定中切換。

### 智慧型中英文（預設關閉）

開啟後，可以直接混打中英文，不必切換模式。每按一個鍵，輸入法都會重新判斷這串按鍵該當作中文注音、英文單字還是原字，並即時更新預覽。

- 空白鍵不會送出整句，只會完成一聲、確認音節或當作英文字詞間的空白。
- **按 Enter 才會送出目前的預覽。**
- Backspace 一次撤銷一個原始按鍵，所以可以從中文預覽退回英文。
- 按 `Down` 開啟候選：依序是最佳預覽、原始按鍵，再來是其他中英切分與同音字。
- 明確選過的內容會固定，之後的輸入不會再改它。

<details>
<summary>許氏鍵盤範例</summary>

許氏鍵盤用 `d / f / j / s` 當二聲、三聲、四聲、輕聲，空白當一聲。這些字母在音節開頭也可能是聲母，由音節狀態決定。

| 按鍵 | 行為 |
| --- | --- |
| `ne` → `nef` | 先顯示 `ne`，打了三聲後預覽「你」 |
| `nefhwfxhf` | 連續組成「你好我」，按 Enter 送出 |
| `gen` + 空白 | 一聲後預覽「今」 |
| `if`、`hd` | 優先保留英文；按 `Down` 可選「矮」、「哦」等中文 |
| `adds`、`end-to-end` + 空白 | 保留英文與尾端空白，按 Enter 送出 |

標準鍵盤打 `283` 可以直接預覽「打」，`hello283` 預覽 `hello打`。同一串按鍵有時同時是合法英文與合法注音，最佳結果不一定是你要的，可以按 `Down` 改選。詞頻資料來源見 [`engine/data/README.md`](engine/data/README.md)。

</details>

### 強制替代詞彙

模型老是選錯某個人名或專有名詞時，可以把它固定下來：

1. 組字完成後，用 `Shift` + 左右方向鍵（或 `Ctrl + Shift` + 方向鍵）選取範圍。範圍要 2 到 8 個字，且每個字都要有注音。
2. 按 `Enter` 加入。

之後組字時只要出現相同的注音序列，就會優先用你指定的字；你手動改選時，仍以手動選擇為準。也可以在設定程式的「替代詞彙」頁編輯。

詞彙存在 `~/.config/llavon-ime/phrase_overrides.txt`（有設定 `XDG_CONFIG_HOME` 時改在該目錄下），是 UTF-8 純文字，格式與 McBopomofo 的使用者詞彙檔相容：

```text
歐陽芷珩 ㄡ-ㄧㄤˊ-ㄓˇ-ㄏㄥˊ
```

## 軟體更新

- **Linux**：設定程式的「軟體更新」頁透過 PackageKit 檢查並安裝 `llavon-ime-fcitx5`。需要先[加入簽章套件來源](#加入簽章套件來源選用建議)。安裝完成後，會等組字結束再重新載入。AUR 與原始碼安裝請用原本的更新方式。
- **macOS**：從輸入來源選單選「檢查更新…」。更新由內建的 Sparkle 下載並驗證簽章，等組字與個人化訓練結束後才提示安裝；安裝時需要管理員授權。只有安裝在 `/Library/Input Methods` 的正式版支援自動更新。

## 本機個人化（預設關閉）

你可以用自己的輸入紀錄，在本機訓練 LoRA 個人化模型。

要開始收集，必須在設定程式的個人化頁面**設定密碼並啟用收集**。在這之前不會記錄任何內容。啟用後，只有完成且非敏感的注音輸入會加密存進本機資料庫；送出後若立刻按 Backspace 修正（10 秒內、中間沒按其他鍵），那筆紀錄會被撤回。

在個人化頁面可以：

- 檢視、排除或刪除待訓練紀錄
- 下載訓練用的基礎 checkpoint（未量化，與內附的 Q4 推論模型不同）
- 安裝或更新本機 LoRA 訓練器
- 啟動、取消訓練，查看訓練歷程，並切換要用哪一次訓練的結果

訓練資料、模型與管理介面都只存在本機。命令列操作與資料位置見 [`ime-unix-service/README.md`](ime-unix-service/README.md#選用的本機-lora-訓練)。

## 隱私與前文

為了根據前文選字，輸入法會在本機讀取游標前的文字，只拿來做本機推論，不會上傳。

- **Linux**：依序嘗試應用程式提供的 surrounding text、AT-SPI。密碼欄位會被排除。
- **macOS**：透過 InputMethodKit 讀取游標附近文字，不需要「輔助使用」權限。安全輸入模式（例如密碼欄）啟用時不讀取。

Linux 另有實驗性的記憶體前文來源 [`memscan`](memscan/README.md)：給既不提供 surrounding text、也不支援 AT-SPI 的程式（例如部分終端機）使用。它預設關閉，只掃描同一使用者的程序，需要 ptrace 權限，而且目前只有 `scripts/build-linux.sh` 的原始碼安裝會附帶，deb、rpm 與 AUR 套件都不含。

## 模型

預設模型是 [`llavon-ime-llama-250m-Q4_K_M.gguf`](https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF)，約 2.5 億參數，透過 llama.cpp 在本機執行。Linux 套件內含 Vulkan 與 CPU backend，macOS 使用 Metal。

模型權重以 [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/deed.zh-hant) 授權：使用或散布時須註明來源，且僅限非商業用途。這與程式碼的 BSD 2-Clause License 分開適用。

可在設定程式中換成其他 GGUF，或用環境變數 `LLAVON_IME_MODEL_PATH` 指定。

## 解除安裝

- **Debian / Ubuntu**：`sudo apt remove llavon-ime-fcitx5`
- **Fedora**：`sudo dnf remove llavon-ime-fcitx5`
- **macOS**：`sudo "/Library/Application Support/llavon-ime/uninstall.sh"`

解除安裝不會刪除使用者資料。要完全清除，請另外刪除：

- `~/.config/llavon-ime/`：設定、替代詞彙
- 個人化資料：Linux 在 `~/.local/state/llavon-ime/`，macOS 在 `~/Library/Application Support/llavon-ime/training/`

## 開發者資訊

### 架構

```text
應用程式 ⇄ Fcitx5 addon（Linux）     ┐
應用程式 ⇄ LlavonIME.app（macOS）     ├─ 共用 engine：組字、候選、智慧中英、替代詞彙
                                     ┘        │ Unix socket（需要時自動啟動）
                                              ▼
                                   llavon-ime-unix-service
                                   └─ ime-core → llama.cpp（載入一次，各前端共用）

拉風設定與個人化（Qt）→ llavon-ime-lora → llavon-lora 訓練器
```

前端只處理按鍵與畫面，模型由獨立服務載入，所以多個應用程式不會各自佔用一份模型記憶體。服務只接受同一使用者的連線。

### 目錄

| 目錄 | 內容 |
| --- | --- |
| `fcitx5/` | Linux Fcitx5 addon |
| `macos/` | macOS 原生 InputMethodKit 輸入法與 Sparkle 更新（[說明](macos/README.md)） |
| `engine/` | 兩個前端共用的輸入法引擎與 C ABI |
| `common/` | 前端與服務共用的通訊協定 |
| `ime-unix-service/` | 預測服務、個人化資料庫、LoRA 管理與 Qt 設定程式（[說明](ime-unix-service/README.md)） |
| `memscan/` | 實驗性 Linux 記憶體前文 helper（[說明](memscan/README.md)） |
| `ime-core/` | 推論函式庫（submodule，[llavon-ime/ime-core](https://github.com/llavon-ime/ime-core)） |
| `lora-trainer/` | LoRA 訓練器版本來源（submodule，[llavon-ime/lora-trainer](https://github.com/llavon-ime/lora-trainer)） |
| `packaging/` | deb、RPM、AUR 與 macOS 安裝包檔案 |
| `scripts/` | 建置、打包、套件來源與發行腳本 |

### 從原始碼建置

先取得原始碼與所有 submodule：

```bash
git clone --recurse-submodules https://github.com/llavon-ime/ime-unix.git
cd ime-unix
```

#### Linux（x86_64）

```bash
./scripts/build-linux.sh
```

腳本會初始化 vcpkg、建置並測試服務、addon 與 memscan，安裝到 `/usr`，並下載模型與固定版本的 LoRA 訓練器；需要時會用 `sudo`。主要建置相依為 Git、CMake、curl、pkg-config、Fcitx5 開發檔案與 Qt 6.4 以上（Widgets、Network、DBus）；AT-SPI 開發檔案缺少時會關閉 AT-SPI 前文。

| 環境變數 | 用途 |
| --- | --- |
| `LLAVON_IME_MODEL_URL` | 模型下載來源或鏡像站 |
| `LLAVON_IME_MODEL_DIR` | 模型下載目錄（預設 `models/`） |
| `LLAVON_IME_SKIP_LORA_TRAINER` | 非空時不安裝 LoRA 訓練器 |
| `LLAVON_IME_SKIP_MEMSCAN_CAPABILITY` | 非空時不替 memscan 設定 `CAP_SYS_PTRACE` |

#### macOS（Apple Silicon）

```bash
xcode-select --install
brew install cmake pkg-config qtbase autoconf autoconf-archive automake libtool
./scripts/build-macos.sh
```

腳本會建置並測試服務與原生 app，安裝到 `/Library/Input Methods/LlavonIME.app` 與 `/Library/Application Support/llavon-ime/`。

- `--user`：把 app 與服務裝到使用者目錄。**模型仍會裝到 `/Library/Application Support/llavon-ime/models/`，第一次還是需要 `sudo`。**
- `--no-service`：只更新 app，不重建服務與訓練器。

開發版預設不啟用自動更新。第一次安裝後，可能需要登出再登入，macOS 才會重新掃描輸入來源。

### 常用執行期環境變數

| 變數 | 用途 |
| --- | --- |
| `LLAVON_IME_MODEL_PATH` | 指定 GGUF 模型 |
| `LLAVON_IME_UNIX_SOCKET_PATH` | 預測服務 socket（預設 `$XDG_RUNTIME_DIR/llavon-ime/ime.sock`） |
| `LLAVON_IME_DISABLE_SERVICE` | 非空時 Linux 前端不自動啟動服務 |
| `LLAVON_IME_DISABLE_ATSPI` | 非空時停用 AT-SPI 前文 |

舊的 `IME_FCITX5_*` 名稱只在部分 Linux 入口仍相容，例如 Fcitx5 addon 讀取的 `IME_FCITX5_MODEL_PATH`；新設定請一律使用 `LLAVON_IME_*`。

## 授權

程式碼以 [BSD 2-Clause License](LICENSE) 授權。模型權重以 CC BY-NC 4.0 授權。發行套件會一併附上模型與第三方相依套件的授權資訊。
