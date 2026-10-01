- Linux Fcitx5 與 macOS 原生前端共用 `engine/` 的輸入行為。新增或修改按鍵、組字、選字、提交、焦點切換或預測行為時，在 `engine/tests/rawkey/` 加入或更新 raw-key 情境；透過 `raw_key_harness.hpp` 餵入按鍵，檢查顯示狀態與提交結果。
- 在 repo 根目錄執行 raw-key 測試：

  ```sh
  cmake -S engine -B build/engine-tests -DLLAVON_IME_ENGINE_BUILD_TESTS=ON
  cmake --build build/engine-tests --target llavon_ime_rawkey_tests --parallel
  build/engine-tests/tests/llavon_ime_rawkey_tests
  ```

- macOS 設定 CMake 時加上 `-DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake"`；先確保 `ime-core`、`vcpkg` submodule 已初始化，且 vcpkg 已 bootstrap。
- 使用c++23
- 編譯時把所有警告都開啟

## Commit 與 push 規則

- 只提交使用者指定範圍內的正式程式碼、建置／CI 設定、必要資源與維護中的回歸測試；不要因為檔案在工作區就一併提交。只有使用者要求時才 commit 或 push。
- `docs/` 文件預設只留在本機，不要提交或推送；只有使用者明確指定要提交文件時才納入。
- 臨時研究、實驗版 adapter、一次性 probe／benchmark／評分腳本、僅供實驗使用的案例資料、研究紀錄與狀態檔、測試輸出、截圖、日誌及機器本地設定，不要提交或推送。判斷依據是用途與引用關係，不是只看檔名或所在目錄。
- 正式功能需要的資料、授權／署名資訊及可重現的資料產生工具屬於必要資源；不要把它們當成臨時檔案移除。
- 提交前先檢查 `git status --short`、工作區 diff 與既有暫存內容，辨識其他工作／session 的修改。保留不在本次範圍內的內容，不要覆寫、刪除或混入提交；不確定用途或範圍時先確認。
- 使用明確檔案路徑或選定的 diff 區塊暫存，不要使用 `git add .`、`git add -A` 或 `git commit -a`。同一檔案混有正式功能與實驗內容時，要拆開提交。
- Commit 前檢查 `git diff --cached --stat`、`git diff --cached` 與 `git diff --cached --check`，確認沒有夾帶文件、臨時檔案、無關修改或未授權的 submodule 指標變動。不要修改 `ime-core/` 或它的 submodule 指標。
- 驗證實際要提交的版本；部分暫存時，不可只測包含其他本機修改的完整工作區。移除臨時檔案時一併檢查建置、測試與連結引用；檔案若仍有本機用途，保留本機副本並加入適當忽略規則。
- 新增相依套件或修改建置流程時，同步檢查相關 CI 的套件安裝與步驟順序。Push 後確認相關 CI 結果，未完成或失敗時不要宣稱全部通過。
- Commit 訊息要清楚描述該批變更。未經使用者明確要求，不要 amend 已推送的提交、重寫遠端歷史或 force push。
