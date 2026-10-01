# macOS App 管理更新

## 使用方式

含更新器的正式版不論由 Homebrew 或 Finder 安裝，皆由輸入法 App 透過
Sparkle 2 更新。輸入來源選單提供「檢查更新…」與「軟體更新設定…」；找到新版後，
選單改顯示「更新至 <版本>…」。更新設定可停用每日背景檢查或背景下載。

「軟體更新設定…」現在開啟兩平台共用原生管理器的「軟體更新」頁，舊 AppKit 更新設定
視窗已移除。管理器透過同一使用者的 CFMessagePort 向運作中的 IMK host 讀取狀態，
並操作同一個 `SPUUpdater` 的偏好；原有 UserDefaults domain 與值不變。
未啟用輸入法或開發版不支援正式更新時，頁面會顯示原因並停用不可用的操作。

背景提示不搶走目前輸入程式的焦點。安裝需由使用者發起並取得 macOS 管理員授權，
不是免授權的靜默更新。Sparkle package updates 不支援差分更新，所以第一版仍
下載完整 `.pkg`，一起更新 App、service、注音表、模型與 Trainer。

不含更新器的舊版需手動升級一次。開發版與 `~/Library/Input Methods` 安裝不啟用
正式更新，避免把使用者的開發程式替換成系統安裝。最低支援 macOS 13、Apple Silicon。

## 組字與工作協調

`UpdateInstallationGate` 在 Sparkle 進入安裝／重啟前，觀察所有附掛的輸入 context，
不提交、清除或重設組字。使用者按 Enter 或取消組字後才繼續。程序探測在背景執行，
回到主執行緒時再次檢查組字，避免探測期間的新輸入被忽略。

`llavon-ime-lora` CLI 與 `llavon-lora` Trainer 還在執行時，延後安裝；管理視窗本身
不阻擋更新。這是程序層級的保守判斷，涵蓋訓練、匯出與 CLI 下載工作，並非跨所有
外部訓練器的工作排程或交易鎖。正常離開 App 會關閉 engine 與其自動啟動的服務。

Sparkle helper 存在時，pkg postinstall 把 relaunch 留給 Sparkle；一般手動安裝與
Homebrew 安裝繼續使用既有的註冊／啟動流程。使用者設定、替代詞彙與個人化模型仍
存於原本的使用者路徑，不是更新包的 payload。

第一版沿用 macOS Installer 的整份 pkg 安裝；不宣稱提供自動回復舊 App／資料庫
的功能。安裝失敗時顯示 Sparkle 錯誤，必要時可重新安裝完整正式包。多使用者登入
時，安裝仍是 system-wide；其他使用者的前端需於下一次重新啟動／登入載入新版。

## 發行設定

正式 workflow 啟用 `LLAVON_IME_ENABLE_UPDATES=1`，必須具備以下 GitHub 設定：

- Repository variable：`SPARKLE_PUBLIC_ED_KEY`，Base64 編碼的 32-byte 公鑰。
- Repository secret：`SPARKLE_PRIVATE_ED_KEY`，Sparkle 匯出的 Base64 私鑰檔案內容。
- 既有 Apple secrets：`MACOS_CERTIFICATE_P12`、`MACOS_CERTIFICATE_PASSWORD`、
  `KEYCHAIN_PASSWORD`、`DEVELOPER_ID_APPLICATION`、`DEVELOPER_ID_INSTALLER`、
  `APPLE_ID`、`APPLE_TEAM_ID`、`APPLE_APP_SPECIFIC_PASSWORD`。
- 既有 Tap secret：`HOMEBREW_TAP_TOKEN`。

從 `macos/scripts/sparkle-config.sh` 固定的 Sparkle distribution 使用官方
`bin/generate_keys` 產生金鑰，依工具說明以 `-x <private-key-file>` 匯出。
將工具印出的公鑰設為 variable、匯出檔內容設為 secret。保留離線備份；不要提交
私鑰到 Git。Package updates 不具備一般 app bundle updates 的金鑰輪替 fallback。

App 啟用 `SUPublicEDKey`、`SUVerifyUpdateBeforeExtraction` 與 `SURequireSignedFeed`。
CI 在簽章／公證後簽署 pkg，用 CryptoKit 依 App 公鑰獨立驗證，接著產生並簽署 XML feed。
Sparkle framework 的 Autoupdate、Updater 與 XPC helpers 由內而外使用同一個
Developer ID 簽章，以符合 hardened runtime 的 library validation。

## Feed 發布

固定 URL：

https://github.com/llavon-ime/ime-unix/releases/download/macos-updates/appcast.xml

`macos-updates` 是專用 prerelease，不參與 GitHub 穩定版的 latest 選擇。每次正式
發行的順序為：

1. 建置 App 與完整 pkg，簽章、公證、staple。
2. 簽署 pkg、驗證公私鑰匹配，驗證並合併先前的 signed feed。
3. 發布版本 tag 的 pkg 與 feed，確認 pkg URL 可下載。
4. 更新 `macos-updates` 的 `appcast.xml`，並下載核對發布內容。
5. 更新 Homebrew Cask 的版本、checksum 與 `auto_updates true`。

Feed 保留最近 20 個版本，以數值排序避免重跑舊 workflow 降低最新版；同一版本
不能換成不同 pkg。不同內容的重新打包必須使用新版本。每版皆標記 package installation
與最低 macOS 13。Linux 的發布不會被當作可用的 macOS 更新。

Homebrew 繼續負責首次安裝與解除安裝。App 更新後 Caskroom 可能仍紀錄舊版本，
因此輸入法使用自己的 `CFBundleVersion` 判斷版本。一般 `brew upgrade` 預設略過
`auto_updates true` Cask；明確使用 `--greedy` 仍可經由 Homebrew 更新。若要固定
App 版本，請在更新設定停用自動檢查，而非只依賴 Homebrew 的 pin。

## 建置與驗證

一般開發建置會嵌入固定版本 Sparkle，但不連線檢查正式更新：

```sh
macos/scripts/build-native-app.sh
bash macos/scripts/verify-updates.sh
macos/scripts/verify-core.sh
```

可用 `LLAVON_IME_SPARKLE_DIR` 指定下載／快取目錄。首次下載會核對固定 SHA-256。
更新啟用的建置還需指定 `LLAVON_IME_VERSION` 與 `SPARKLE_PUBLIC_ED_KEY`。
`LLAVON_IME_RELEASE_REPOSITORY` 改變預設 feed 的 repo；`LLAVON_IME_UPDATE_FEED_URL`
可覆寫 HTTPS feed，供隔離測試使用。

自動測試涵蓋 feed 版本排序、重跑一致性、已發布版本不可替換、簽章格式、實際 pkg／feed
簽章與竄改拒絕，以及組字／訓練延後、探測競態與取消後過期回呼。Swift core 與
共用 raw-key 測試驗證讀取更新就緒狀態不會改變組字或提交。

正式啟用前，使用獨立測試帳號與簽章、公證過的 N−1／N pkg 完成整合驗證：

- Homebrew 安裝 N−1 → App 更新 N → Homebrew 解除安裝。
- Finder 安裝 N−1 → App 更新 N，確認收據、App、service 皆為新版。
- 組字中按更新、訓練中按更新，以及使用者取消管理員授權。
- 更新前後輸入來源仍可選，macOS 26 已啟用的來源維持可用。
- 斷線／錯誤簽章不安裝；重新啟動後可再次檢查。

參考：[Sparkle Package Updates](https://sparkle-project.org/documentation/package-updates/)、
[Sparkle Setup](https://sparkle-project.org/documentation/)。
