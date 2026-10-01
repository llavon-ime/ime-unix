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
