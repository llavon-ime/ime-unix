# Linux App 管理更新

## 操作與更新範圍

在「拉風設定與個人化」視窗選擇「軟體更新」，或執行
`llavon-ime-lora-gui --page updates`。

- 「檢查更新」：確認原生套件已安裝、刷新套件來源，再查詢可用更新。
- 「立即更新」：等待個人化工作結束，透過 PackageKit 更新
  `llavon-ime-fcitx5` 及套件管理器判定必要的依賴。
- 「套用新版並重啟輸入法」：等待訓練結束，由 Fcitx5 自己檢查所有組字後重啟。
- 可選擇在設定視窗開啟時每日自動檢查。沒有額外常駐更新 daemon，關閉視窗後不會
  繼續排程；這是自動檢查＋使用者發起安裝，不是無人值守安裝。

PackageKit 使用系統 bus，APT／DNF backend 管理版本比較、下載、依賴、套件鎖與
簽章；polkit 處理必要的管理員授權。GUI 不執行 sudo、root shell 或自行覆蓋 `/usr`。
安裝旗標要求可信任套件，不忽略簽章錯誤。套件可以更新必要的依賴，並非整套系統升級。

需要發行版的 PackageKit 與桌面授權 agent。deb／RPM 將 PackageKit 列為建議依賴；
刻意省略建議依賴的最小安裝仍可使用輸入法，但需自行安裝 PackageKit 才能在介面更新。
無 PackageKit、來源檢查失敗、取消授權與多架構歧義都會顯示錯誤，不觸發重啟。

AUR `-git`／`-preview-git` 不會被替換成正式 deb／RPM。仍使用 yay／paru（VCS
更新可能需要 `--devel`），原始碼安裝沿用建置腳本。App 只接受套件管理器所報告的
精確產品套件 ID，不用 GitHub latest 比較發行版版本。

## 重啟與組字

Fcitx addon 提供 session D-Bus 介面：

```
service:   org.fcitx.Fcitx5
path:      /llavon/update
interface: org.llavon.IME.Update1
Version() -> s
RestartIfIdle() -> b
```

`RestartIfIdle` 在 Fcitx event loop 中同步檢查共用 engine 的所有 context，以及其他
輸入法留下的 input-panel/client preedit。就緒檢查不提交、清除或重設任何文字；
確認無組字且 Fcitx 能自行重啟才呼叫 `Instance::restart()`。因此沒有「GUI 查詢
就緒 → 新按鍵到達 → GUI 強制 kill」的競態。engine 結束時沿用 transport 的服務
停止流程，重啟後載入新版 addon 與預測服務。

更新磁碟檔案不代表正在執行的 addon 已換版。第一次從舊版更新時，舊程序尚無此
D-Bus 介面，GUI 會要求完成輸入後登出／登入；不能用強制終止繞過保護。設定視窗與
backend 也要重新開啟以載入新版。重啟會影響本次登入的整個 Fcitx5，不只拉風。

安裝與重啟前檢查本機管理器工作狀態，並以 `/proc/*/comm` 觀察 CLI／Trainer
程序。等待與安裝期間禁止從同一管理器開始新的訓練／下載工作。這不是跨使用者、
跨任意外部訓練器的交易鎖；系統套件更新仍是 system-wide，其他登入工作階段需自行
載入新版。設定視窗更新處理中不能直接關閉；尚在等待時可取消等待。

## 官方 repository 產生與部署

初始套件目標為 **Debian 13 amd64、Fedora 43 x86_64**。Ubuntu 24.04／Debian 12
不能直接使用目前需 glibc 2.41 的 deb；不能藉由新增來源解決 ABI 相容性。
其他 Ubuntu 版本與發行版需另行驗證，登錄腳本不猜測相容性。

GitHub repository 設定：

- Secret `LINUX_REPOSITORY_PRIVATE_KEY`：專用 OpenPGP 簽章金鑰的 ASCII-armored
  私鑰。CI 使用不需互動解鎖的專用 signing key；妥善保存離線備份與撤銷憑證。
- Variable `LINUX_REPOSITORY_KEY_ID`：完整 40 位 hex 的主金鑰 fingerprint。

`.github/workflows/linux-repository.yml` 在 Linux release workflow 完成且成功後，
下載該次驗證的正式版套件；也可手動指定已發布版本。工具程式來自 default branch，
不執行下載套件中的程式碼或 triggering branch 腳本。

`scripts/build-linux-repository.sh` 產生：

```
repository/
├── llavon-ime-repository.asc
├── debian13/amd64/
│   ├── Packages、Packages.gz
│   ├── Release、InRelease、Release.gpg
│   ├── snapshot.json
│   └── packages/*.deb
└── fedora43/x86_64/
    ├── repodata/repomd.xml、repomd.xml.asc、各索引
    ├── snapshot.json
    └── packages/*.rpm
```

APT 驗證 signed Release 及 Packages 中的 SHA-256；RPM 同時簽署套件與 repomd，
登錄設定啟用 `gpgcheck=1`、`repo_gpgcheck=1`。公開金鑰與 repository 部署 tarball
會附加到對應版本 GitHub Release。

**產生部署包不等於已建立線上更新來源。** 必須將解壓後的完整目錄放到穩定 HTTPS
服務。GitHub Releases 的附件目錄不是可以直接使用的 DNF repository。
部署應在獨立版本目錄完整上傳與驗證後，以 atomic directory/symlink promotion
更新公開入口；保留舊套件供已取得舊索引的客戶端下載。不要把不同版本的索引／套件
混合，或讓舊版重跑覆蓋較新的公開入口。`snapshot.json` 供部署端核對版本與摘要，
實際信任依據仍是 OpenPGP 簽章與原生套件管理器。

目前 workflow 負責 signed repository snapshot，**沒有假設或建立任何雲端帳號、
bucket、domain 或公網服務**。正式啟用需配置簽章金鑰、選定 HTTPS hosting，並接上
該主機的部署方式。

## 首次加入來源

在獨立管道核對官方 fingerprint，再下載公開金鑰。來源正式部署後：

```sh
sudo bash scripts/configure-linux-repository.sh \
  https://YOUR-REPOSITORY-HOST/linux \
  ./llavon-ime-repository.asc \
  YOUR_40_DIGIT_TRUSTED_FINGERPRINT
```

腳本只接受支援的 distro／架構與 HTTPS URL，核對單一主金鑰 fingerprint。Debian
使用 `Signed-By` 限定 keyring，Fedora 開啟 package／metadata 簽章檢查。首次登錄
需要管理員權限，後續可用 App 或原生套件工具更新。

加入 repository 不會自行讓 Ubuntu/Debian 的 unattended-upgrades 或 Fedora 的
自動更新服務安裝它。無人值守安裝依使用者的系統更新設定；App 不更改其全域策略。

## 驗證

```sh
cmake -S scripts/linux-update-tests -B build/linux-update-tests
cmake --build build/linux-update-tests --parallel
ctest --test-dir build/linux-update-tests --output-on-failure
bash scripts/tests/verify-linux-repository.sh
```

PackageKit tests 在隔離 D-Bus daemon 上驗證原生介面：installed resolve → refresh
→ updates → trusted targeted install，涵蓋重複點擊、取消授權、來源錯誤、AUR 排除與
多架構歧義。它不是對真實 desktop polkit agent 的替代測試。

Repository tests 以臨時金鑰與最小 fixture 產生 signed apt／RPM repository，使用
隔離 APT state、DNF installroot 下載套件，核對 RPM 簽章，並確認竄改的套件或 metadata
遭拒絕；不改寫開發機的套件來源或安裝產品。共用 raw-key tests 驗證多輸入 context
就緒檢查不影響 preedit／提交。`linux-updates-ci.yml` 在 Debian 13 執行這些檢查。

正式上線還需桌面上的 N−1 → N 整合驗證：系統授權對話框、取消安裝、Fcitx 所有
組字的延後重啟、訓練延後、service 的新版本載入，以及手動／系統更新後的同一版本
判斷。沒有此驗證前，不宣稱提供完整無人值守升級或自動 rollback。
