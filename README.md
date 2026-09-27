# 拉風輸入法（Linux / macOS）

拉風輸入法（Llavon IME）是一套專為繁體中文注音輸入打造的輸入法。它使用約 2.5 億參數的語言模型，根據前文與注音內容預測更合適的文字，同時確保候選字符合輸入的讀音。

所有輸入內容與模型推論都在電腦本機完成，不需要連線至雲端，也不會為了選字而上傳正在輸入的文字。

本儲存庫提供 Linux 的 Fcitx5 前端、macOS 原生輸入法，以及兩個平台共用的本機預測服務。

> [!IMPORTANT]
> 本專案仍在早期開發階段，功能、操作方式及安裝流程都可能變動。目前 Linux 發行套件僅提供 x86_64，macOS 安裝檔僅提供 Apple Silicon（arm64）。

## 專案特點

- **理解上下文的選字**：依據前後文、注音序列與已選文字預測候選字，不只依靠固定詞頻。
- **專為注音輸入訓練**：採用專用繁體中文注音模型，而非將一般聊天模型直接接到傳統輸入法。
- **完全本機運作**：模型與推論引擎皆在本機執行，輸入內容無須傳送至雲端。
- **熟悉的操作方式**：使用標準注音鍵盤配置，保留傳統注音輸入法熟悉的組字與選字方式。
- **可自訂常用詞彙**：可指定文字及其注音，改善姓名、專有名詞或其他固定詞彙的輸入結果。
- **本機個人化訓練**：可選擇收集自己的輸入紀錄，透過 LoRA 在本機訓練個人化模型；資料收集預設關閉。
- **平台原生整合**：Linux 使用 Fcitx5，macOS 使用原生 InputMethodKit，兩者共用相同的輸入法引擎與預測服務。
- **硬體加速**：Linux 發行套件提供 Vulkan backend，macOS 使用 Metal backend，並支援 CPU 推論。

## 安裝

### Linux

可從 [GitHub Releases](https://github.com/llavon-ime/ime-unix/releases/latest) 下載對應發行套件。

#### Debian / Ubuntu

```bash
sudo apt install ./llavon-ime-fcitx5_<版本>_amd64.deb
```

目前的 deb 套件以 Debian 13 建置，需要 Fcitx5 5.1.12 與 glibc 2.41 以上，例如 Debian 13、Ubuntu 26.04。

#### Fedora

```bash
sudo dnf install ./llavon-ime-fcitx5-<版本>-1.<fedora>.x86_64.rpm
```

安裝完成後，拉風輸入法會自動加入 Fcitx5。若尚未出現，可開啟 Fcitx5 設定工具手動加入 `llavon-ime`，再執行：

```bash
fcitx5 -r
```

目前 Linux 發行套件僅支援 x86_64。

### macOS

建議使用 Homebrew 安裝：

```bash
brew tap llavon-ime/llavon-ime
brew trust --cask llavon-ime/llavon-ime/llavon-ime
brew install --cask llavon-ime
```

安裝過程會要求管理員密碼，並安裝「拉風輸入法」、注音表、模型與本機 AI 預測服務。

安裝完成後，前往「系統設定 → 鍵盤 → 輸入方式」加入「拉風輸入法」。macOS 15 及更早版本通常可由安裝程式自動加入；macOS 26（Tahoe）因第三方輸入來源的啟用狀態由受保護的系統儲存管理，首次安裝時需要手動按「+」加入，或在安裝後登出再登入一次。

也可以從 [GitHub Releases](https://github.com/llavon-ime/ime-unix/releases/latest) 下載 `llavon-ime-<版本>-arm64.pkg`。目前僅提供 Apple Silicon（arm64）安裝檔；套件尚未正式簽章，若被 Gatekeeper 阻擋，可在 Finder 中右鍵選擇「打開」。

解除安裝：

```bash
brew uninstall --cask llavon-ime
```

或使用套件附帶的解除安裝腳本：

```bash
sudo "/Library/Application Support/llavon-ime/uninstall.sh"
```

## 使用方式

切換至拉風輸入法後，即可使用標準注音鍵盤輸入。輸入法會在需要時自動啟動 `llavon-ime-unix-service`，模型載入完成後即可開始使用 AI 選字。

### 預測上下文

為了根據前文改善選字，輸入法會在本機取得游標附近的文字：

- **Linux**：透過 AT-SPI 取得游標前文字。
- **macOS**：透過輸入法的 InputMethodKit client 取得游標附近文字，不需要「輔助使用」權限。

取得的文字只提供本機模型推論使用，不會因選字而上傳至雲端。

## 強制替代詞彙

如果模型經常把特定姓名或專有名詞選錯，可以直接建立「強制替代詞彙」。

組字完成後，用 `Shift` + 左右方向鍵選取要儲存的範圍（也可使用 `Ctrl + Shift` + 方向鍵），再按 `Enter` 加入。選取範圍需為 2 至 8 個字，且每個字都必須有對應注音。

加入後，之後只要組字中出現相同的注音序列，就會優先使用指定文字。替代只作用於相符的範圍，不需要整段輸入完全相同；手動改選其他候選字時，該次輸入仍以使用者的選擇為準。

詞彙以 UTF-8 純文字儲存在：

```text
~/.config/llavon-ime/phrase_overrides.txt
```

若有設定 `XDG_CONFIG_HOME`，則位於 `$XDG_CONFIG_HOME/llavon-ime/phrase_overrides.txt`。

每行格式與 McBopomofo 的使用者詞彙檔相容，例如：

```text
歐陽芷珩 ㄡ-ㄧㄤˊ-ㄓˇ-ㄏㄥˊ
```

Linux 也可從 Fcitx5 的 Llavon IME 設定中選擇「管理強制替代詞彙」進行增刪。

## 本機個人化

拉風輸入法可以使用自己的輸入紀錄訓練 LoRA 個人化模型。**資料收集預設關閉**，只有在設定中主動開啟「收集個人化訓練資料」後，完成的注音輸入才會寫入目前使用者的本機資料庫。

可從輸入法選單選擇「管理個人化訓練…」，或在設定頁面按「使用我的輸入改進模型」，開啟本機管理介面。介面可用來：

- 檢視、排除或刪除待訓練紀錄。
- 下載訓練需要的基礎 checkpoint。
- 安裝或更新本機 `lora-trainer`。
- 啟動、取消與查看訓練工作。
- 選擇完成訓練後產生的 GGUF 模型作為推論模型。

訓練資料與管理介面都只存在本機。套件內附的 Q4 GGUF 模型只供推論使用；LoRA 訓練需要另外下載未量化的基礎 checkpoint。

更完整的命令列操作、資料庫位置與模型相容性說明請參考 [`ime-unix-service/README.md`](ime-unix-service/README.md#optional-local-lora-training)。

## 模型與隱私

本專案預設使用 [`llavon-ime-llama-250m-Q4_K_M.gguf`](https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF/blob/main/llavon-ime-llama-250m-Q4_K_M.gguf)。模型以 GGUF 量化格式透過 llama.cpp 在本機執行；其他版本與相關資訊可在 [`llavon-ime-llama-250m-GGUF`](https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF) 模型頁查看。

模型權重另依 [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/deed.zh-hant) 授權，僅限非商業用途，使用或散布時須註明來源；此授權與本專案程式碼的 BSD 2-Clause License 分開適用。

開發版本可透過 Fcitx5 設定頁面或 `LLAVON_IME_MODEL_PATH` 指定模型。舊版 `IME_FCITX5_*` 環境變數名稱仍保留相容性，例如 `IME_FCITX5_MODEL_PATH`。

## 專案組成

- `fcitx5/`：Linux Fcitx5 輸入法前端與平台整合。
- `macos/`：macOS 原生 InputMethodKit 輸入法。
- `ime-unix-service/`：Linux / macOS 共用的本機預測服務、個人化資料管理與 LoRA 管理工具。
- `ime-core/`：跨平台 C++ 推論函式庫，以 Git submodule 引入，負責模型載入、tokenization 與 llama.cpp 推論。
- `engine/`：兩個 Unix 前端共用的輸入法引擎程式碼。
- `lora-trainer/`：本機 LoRA 訓練器的版本來源 submodule。
- `packaging/`：deb、RPM 與 macOS 套件相關檔案。
- `scripts/`：建置、安裝與發行腳本。

輸入法前端與 `llavon-ime-unix-service` 透過 Unix socket 通訊。模型載入與推論集中在獨立服務中，因此前端不需要各自持有一份模型。

## 從原始碼建置

### Linux

最簡單的方式是使用專案附帶的建置腳本：

```bash
git clone --recurse-submodules https://github.com/llavon-ime/ime-unix.git
cd ime-unix
./scripts/build-linux.sh
```

腳本會初始化子模組與 vcpkg、編譯並測試服務與附加元件，最後進行安裝；需要系統權限時會使用 `sudo`。

需要的主要建置工具包括：

- CMake
- pkg-config
- Fcitx5 開發檔案
- Git

模型若已存在會直接沿用，否則會從 Hugging Face 下載到 `models/`，並安裝至 `/usr/share/llavon-ime/models/`。

可使用以下環境變數調整下載行為：

- `LLAVON_IME_MODEL_URL`：指定模型下載來源或映像站。
- `LLAVON_IME_MODEL_DIR`：指定模型下載目錄。

<details>
<summary>手動建置 Linux 元件</summary>

```bash
git clone --recurse-submodules https://github.com/llavon-ime/ime-unix.git
cd ime-unix
./vcpkg/bootstrap-vcpkg.sh

cd ime-unix-service
cmake --preset linux -DIME_UNIX_SERVICE_BUILD_TESTS=ON
cmake --build --preset linux
ctest --test-dir build/linux --output-on-failure
sudo cmake --install build/linux
cd ..

cd fcitx5
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
cd ..
sudo cmake --install build/fcitx5
```

Linux preset 使用本儲存庫的 vcpkg 工具鏈，並啟用 Vulkan backend（`llama-vulkan`）。

</details>

### macOS

開發者可直接建置原生 InputMethodKit app，不需要 fcitx5-macos：

```bash
macos/scripts/build-native-app.sh --install
```

前置需求：

- Xcode Command Line Tools
- CMake
- pkg-config

可使用 Homebrew 安裝後兩者：

```bash
brew install cmake pkg-config
```

腳本會使用 vcpkg 編譯推論引擎、以 `swiftc` 編譯前端、進行 ad-hoc 簽章，並安裝到：

```text
/Library/Input Methods/LlavonIME.app
```

系統安裝同時會建置、測試並安裝 AI 預測服務至：

```text
/Library/Application Support/llavon-ime/payload
```

模型預設位於：

```text
/Library/Application Support/llavon-ime/models
```

沒有 `sudo` 權限時，可改用：

```bash
macos/scripts/build-native-app.sh --install --user
```

只想更新輸入法 app、不重新建置 service 時，可加上 `--no-service`。

開發版第一次安裝後，可能需要登出再登入，讓 macOS 重新掃描輸入來源。修改程式後可重新執行相同建置指令；需要時可先執行：

```bash
pkill -x LlavonIME
```

<details>
<summary>手動建置舊版 Fcitx5 macOS 元件</summary>

部分舊版開發流程仍保留 Fcitx5 macOS build preset，需要 `fcitx5-macos` 原始碼以取得標頭：

```bash
export FCITX5_MACOS_SOURCE_DIR=/path/to/fcitx5-macos

cd ime-unix-service
cmake --preset macos
cmake --build --preset macos
cmake --install build/macos
cd ..

cd fcitx5
cmake --preset macos
cmake --build --preset macos
ctest --preset macos
cd ..
cmake --install build/macos
```

macOS preset 使用本儲存庫的 vcpkg 工具鏈並啟用 Metal backend（`llama-metal`）。

</details>

## 授權

本專案程式碼依 [BSD 2-Clause License](LICENSE) 授權。模型權重另依 CC BY-NC 4.0 授權；發行套件會一併收錄相關模型與第三方相依套件的授權資訊。

發行套件內含 Q4 GGUF 模型（CC BY-NC 4.0，僅限非商業用途；署名與相依套件授權
隨套件附上）。開發版本需自備模型，透過 fcitx5 設定頁面或
`LLAVON_IME_MODEL_PATH` 指定：

https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF

舊版 `IME_FCITX5_*` 環境變數名稱仍相容（例如 `IME_FCITX5_MODEL_PATH`）。

### 本機 LoRA 個人化（命令列）

注音提交資料的收集預設關閉；在輸入法設定勾選「收集個人化訓練資料」後，
可用服務安裝的 `llavon-ime-lora` 列出、排除或刪除待訓練紀錄，並以選配的
[`lora-trainer`](https://github.com/llavon-ime/lora-trainer) 訓練個人化模型。
也可從輸入法選單的「管理個人化訓練…」或設定頁面的「使用我的輸入改進模型」按鈕開啟共用的本機網頁介面，
在瀏覽器檢視紀錄、下載基礎模型與管理訓練工作。
訓練需要另外下載未量化的基礎 checkpoint；套件內的 Q4 GGUF 只供推論使用。
完整指令及模型相容性說明見 [Unix 服務文件](ime-unix-service/README.md#選用的本機-lora-訓練)。

## 預測上下文

- Linux：透過 AT-SPI 取得游標前文字。
- macOS：透過 Fcitx5.app 的 InputMethodKit client 取得游標附近文字，不使用
  Accessibility API，也不需要「輔助使用」權限（需 Fcitx5.app 0.3.4 以上）。
