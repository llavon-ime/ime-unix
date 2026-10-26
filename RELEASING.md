# 發布規範

本文件是拉風輸入法正式版本的發布依據，供維護者、自動化工具與 AI agent 共同遵循。GitHub Release 與 macOS Sparkle 更新摘要皆取自 annotated tag message，因此 tag message 是每個版本 release notes 的唯一正式來源。

## 發布權限與基本原則

- 只有在維護者明確指定版本並同意發布後，才能建立或推送 tag。
- 正式版本使用 Semantic Versioning，tag 格式固定為 `v<major>.<minor>.<patch>`，例如 `v0.9.10`。
- tag 必須指向 `main` 上已推送的 commit，且工作區、暫存區與 submodule 狀態都必須乾淨。
- 只能使用 annotated tag（`git tag -a`）或 signed tag（`git tag -s`），不得使用 lightweight tag。
- 已推送或已發布的 tag 不得移動、覆寫或 force-push。發布內容有誤時，原則上以新的 patch 版本修正。

## 一、確認發布範圍

先同步主分支與 tags：

```sh
git fetch origin main --tags
git status --short --branch
git switch main
git pull --ff-only origin main
```

找出目前 commit 可追溯到的上一個正式版本：

```sh
previous_tag="$(git describe --tags --abbrev=0 --match 'v[0-9]*' HEAD)"
printf 'Previous release: %s\n' "$previous_tag"
```

不得只閱讀最近一筆 commit。至少檢查以下內容：

```sh
git log --first-parent --oneline "${previous_tag}..HEAD"
git diff --stat "${previous_tag}..HEAD"
git diff --submodule=log "${previous_tag}..HEAD"
```

必要時進一步閱讀完整 diff、合併 PR、測試變更與 submodule commit，確認實際的使用者影響、平台差異、相容性與遷移需求。

## 二、決定版本號

- **Patch**：向下相容的錯誤修正或小幅改善。
- **Minor**：向下相容的新功能或明顯的行為擴充。
- **Major**：不相容變更、資料格式變更或需要使用者遷移的改版。

不要只因 commit 數量決定版本。若無法判斷版本層級，先向維護者確認，不可自行猜測。

## 三、撰寫 release notes

### 語言與風格

- 全文使用繁體中文與臺灣慣用詞；產品名、API、檔名及必要技術名詞可保留原文。
- 第一段用一句話概括該版本最重要的使用者價值。
- 後續依需要使用「新增」「改善」「修正」「相容性」「已知限制」分類；沒有內容的分類不要保留。
- 每個項目使用一致句型，優先以「新增」「改善」「修正」「移除」等動詞開頭，句尾使用全形句號。
- 描述使用者能觀察到的結果及受影響平台，不要只描述修改了哪個函式、檔案或 CI job。
- 不要直接複製 commit subject，也不要列出一次性調查、測試探針、純重構或無使用者影響的 CI 調整。
- Breaking change、手動操作、資料遷移、權限需求與已知限制必須明確寫出。

建議格式：

```markdown
改善 macOS 自動更新與輸入來源管理。

### 改善

- 自動更新視窗現在會顯示每個版本的更新摘要。

### 修正

- 修正部分 macOS 使用者在系統設定中看到重複輸入法名稱的問題。

### 已知限制

- 已受舊版設定影響的 macOS 26 使用者，仍需在系統設定中移除並重新加入輸入法。
```

## 四、發布前檢查

1. 執行與變更範圍相關的完整測試。
2. 確認版本號尚未存在：

   ```sh
   version="0.9.10"
   ! git rev-parse -q --verify "refs/tags/v${version}"
   ```

3. 確認發布 commit 就是遠端 `main`：

   ```sh
   test "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)"
   ```

4. 向維護者展示以下資訊並取得最終確認：
   - 預定版本號。
   - 上一版至本版的比較範圍。
   - 測試結果。
   - 完整 release notes／tag message。

## 五、建立並推送 tag

取得最終確認後，以完整 release notes 建立 annotated tag。建議使用 heredoc，避免 shell quoting 改變換行或標點：

```sh
version="0.9.10"
git tag -a "v${version}" -F - <<'NOTES'
改善 macOS 自動更新與輸入來源管理。

### 改善

- 自動更新視窗現在會顯示每個版本的更新摘要。

### 修正

- 修正部分 macOS 使用者在系統設定中看到重複輸入法名稱的問題。
NOTES
```

推送前再次驗證 tag 類型、目標與摘要：

```sh
test "$(git cat-file -t "refs/tags/v${version}")" = tag
test "$(git rev-parse "refs/tags/v${version}^{commit}")" = "$(git rev-parse HEAD)"
python3 scripts/tag-release-notes.py \
  --tag "v${version}" \
  --output "${TMPDIR:-/tmp}/llavon-release-notes.md"
cat "${TMPDIR:-/tmp}/llavon-release-notes.md"
```

確認無誤後只推送該 tag：

```sh
git push origin "refs/tags/v${version}"
```

不要使用 `git push --tags`，以免一併推送其他本機 tags。

## 六、發布後驗證

tag push 後必須等待相關 workflow 完成，不可在執行中宣稱發布成功：

- `Release macOS package`
- `Release Linux packages`
- 共用 engine 與 raw-key release tests

確認 GitHub Release：

- 版本與 tag 正確。
- Release body 包含完整繁體中文更新摘要。
- macOS `.pkg`、Debian／Ubuntu `.deb` 與 Fedora `.rpm` 等預期資產均已上傳。
- 安裝說明沒有覆蓋或重複 release notes。

確認 macOS Sparkle feed：

- 最新 item 版本與 tag 一致。
- `description` 包含相同的繁體中文更新摘要。
- 套件 URL、Ed25519 signature 與 package installation type 正確。
- 公開 appcast 與 workflow 產生並簽署的檔案一致。

## 七、失敗處理

- **tag 尚未 push**：修正 release notes 或版本後，可刪除本機 tag 並重新建立。
- **tag 已 push、尚未對外發布**：停止操作並先取得維護者明確授權，不得自行刪除或移動遠端 tag。
- **Release 或套件已發布**：保留既有 tag 與產物，調查問題後發布新的 patch 版本；除非維護者明確決定撤回版本，否則不得改寫歷史。
- **部分平台 workflow 失敗**：只重跑相同 tag 的既有 workflow；先確認重跑不會覆蓋不同內容的套件或 appcast。
