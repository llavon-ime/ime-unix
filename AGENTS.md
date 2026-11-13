Linux Fcitx5 與 macOS 前端共用 `engine/` 的輸入行為；架構見 `README.md`，發布與 tag 遵循 `RELEASING.md`。回覆與 commit message 使用繁體中文（既有英文 commit message 維持原樣）；產品名、API、檔名與必要技術名詞保留原文。

## 驗證
- 全部 engine 測試：`scripts/verify-engine-tests.sh`；單一 suite：`build/engine-tests/tests/llavon_ime_rawkey_tests "名稱片段"`。其他元件的指令見 `.github/workflows/`。
- 改按鍵、組字、選字、提交、焦點切換或預測行為，要在 `engine/tests/rawkey/` 加或改情境。
- 這些 CI 不涵蓋，改到相關程式要在本機跑：`macos/scripts/verify-core.sh`（C ABI、`macos/Core/`，只在 release 跑）、`macos/scripts/verify-settings-*.sh`、`macos/scripts/verify-input-source-lifecycle.sh`。
- `[skip]` 和沒跑的項目要在回報中明說。

## 不要
- 在 `fcitx5/`、`macos/` 寫輸入邏輯，或在主執行緒等模型、IPC。
- 為了通過案例加字串特例、白名單或調權重，或放寬、刪除既有斷言。
- 壓警告（新 CMake target 要設 C++23 並呼叫 `llavon_ime_enable_warnings`）；使用 GCC 13 沒有的 `<print>`、`std::ranges::to`、`<flat_map>`，或 AppleClang 沒有的 `std::views::enumerate`。
- 讓使用者輸入、前文或訓練資料離開本機或進 release log。
- 做任務外的重排或重構；repo 沒有 clang-format 設定，不要跑。
- 未經同意就跑會安裝輸入法的腳本（`scripts/build-linux.sh`、`scripts/build-macos*.sh`、`macos/scripts/build-native-app.sh --install`）、新增相依、改使用者資料或 IPC 格式、改 `ime-core/` 或 submodule 指標。

## Git
- 只有使用者要求時才 commit、push、建 tag、amend 已推送的提交或 force push；push 到 `main` 會自動發布 AUR。
- 只提交指定範圍內的正式程式碼、建置／CI 設定、必要資源與回歸測試。`docs/`、臨時研究、一次性腳本、實驗資料、測試輸出、截圖、日誌與本機設定不提交；依用途判斷，不看檔名。
- 保留其他 session 的修改；用明確路徑暫存，不用 `git add .`、`-A`、`commit -a`。commit 前看 `git diff --cached` 與 `--check`。
- 驗證實際要提交的版本；工作區有範圍外修改（含 `M ime-core`）時用乾淨 worktree。
- 改相依或建置流程要同步 CI。push 後用 `gh run list --commit <sha>` 確認結果。
