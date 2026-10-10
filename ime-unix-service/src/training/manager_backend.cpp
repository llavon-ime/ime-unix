#include "commit_store.hpp"
#include "job_process.hpp"
#include "manager_model_policy.hpp"
#include "manager_database.hpp"
#include "gpu_vendor.hpp"
#include "lora_history_lca.hpp"
#include "lora_presets.hpp"
#include "sqlite.hpp"
#include "../../../engine/src/util/parse_number.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <notify.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

volatile sig_atomic_t stop_requested = 0;
void request_stop(int) { stop_requested = 1; }

// Records shown per page; the page script steps its offset by the same value.
constexpr int kRecordsPerPage = 20;

#ifndef LLAVON_NATIVE_GUI
constexpr std::string_view page = R"LLAVON(<!doctype html>
<html lang="zh-Hant"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer"><link rel="icon" type="image/png" href="@@LOGO@@"><title>拉風輸入法・個人化訓練</title>
<style>
:root{
  color:#26221e;background:#f7f4ee;
  font-family:system-ui,-apple-system,BlinkMacSystemFont,"Noto Sans TC","PingFang TC","Microsoft JhengHei",sans-serif;
  font-synthesis:none;text-rendering:optimizeLegibility;line-height:1.5;
  --ink:#26221e;--muted:#665e55;--paper:#fff;--line:#ded7cd;--line-strong:#a99d90;
  --accent:#a34825;--accent-dark:#7b321b;--accent-soft:#f7e7dc;
  --warning:#9b572d;--warning-soft:#fff5eb;--danger:#a6422e;--danger-soft:#fff0ed;
}
*{box-sizing:border-box}
[hidden]{display:none!important}
html{background:#f7f4ee}
body{min-width:320px;min-height:100vh;margin:0;background:#f7f4ee}
button,input,textarea,select{font:inherit}
.site-shell{min-height:100vh}
.topbar{min-height:72px;display:flex;align-items:center;justify-content:space-between;gap:20px;padding:10px max(24px,calc((100vw - 1040px)/2));border-bottom:1px solid var(--line);background:rgba(255,253,249,.94)}
.brand{min-width:0;display:inline-flex;align-items:center;gap:11px}
.brand-logo{width:44px;height:44px;flex:0 0 auto;border:1px solid var(--line-strong);border-radius:11px;background:var(--paper);object-fit:cover}
.brand > span{min-width:0;display:flex;flex-direction:column;line-height:1.2}
.brand strong{font-size:16px;letter-spacing:.02em}
.brand small{margin-top:3px;color:var(--muted);font-size:12px}
.top-note{flex:0 0 auto;padding:6px 9px;border:1px solid var(--line);border-radius:7px;background:var(--paper);color:var(--muted);font-size:12px;font-weight:700}
.topbar-actions{display:flex;align-items:center;gap:9px}
.page-content{width:min(900px,calc(100% - 40px));margin:0 auto;padding:36px 0 64px}
.page-heading{margin-bottom:22px}
.page-heading h1{margin:0;font-size:clamp(28px,4vw,38px);line-height:1.25;letter-spacing:-.035em}
.page-heading p{margin:8px 0 0;color:var(--muted);font-size:15px}
.tabs{display:flex;gap:8px;margin:0 0 18px;padding:5px;border:1px solid var(--line);border-radius:12px;background:#eee9e1}
.tabs button{flex:1;min-width:0;border:0;background:transparent;color:var(--muted);font-size:14px}
.tabs button[aria-selected="true"]{background:var(--paper);color:var(--accent-dark);box-shadow:0 2px 8px rgba(54,42,32,.12)}
.tabs button:focus-visible,button:focus-visible,summary:focus-visible{outline:3px solid var(--accent);outline-offset:2px}
.tab-panel{min-width:0}
.form-card{min-width:0;overflow:hidden;border:1px solid var(--line);border-radius:14px;background:var(--paper);box-shadow:0 10px 28px rgba(54,42,32,.06);margin-bottom:14px}
.field-group{padding:24px 28px;border-bottom:1px solid var(--line)}
.field-group:last-child{border-bottom:0}
.field-label-row{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:16px}
.field-label{display:inline-flex;align-items:center;gap:9px;font-size:17px;font-weight:800}
.field-index{width:23px;height:23px;display:inline-grid;place-items:center;flex:0 0 auto;border-radius:6px;background:var(--accent);color:#fff;font-size:10px;font-weight:800}
.field-label-row small,.field-label-row > span:not(.field-label){color:var(--muted);font-size:13px}
.field-hint{margin:12px 1px 0;color:var(--muted);font-size:13px;line-height:1.6}
.toolbar{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.row{display:flex;align-items:center;gap:9px;flex-wrap:wrap;margin:0}
.notice{display:flex;gap:10px;margin-bottom:18px;padding:13px 15px;border:1px solid var(--line);border-left:4px solid var(--accent);border-radius:10px;background:var(--paper);font-size:14px;white-space:pre-wrap}
.notice.error{border-left-color:var(--danger);background:var(--danger-soft);color:#7b2f21}
button{min-height:40px;padding:0 15px;border:1px solid var(--line-strong);border-radius:8px;background:var(--paper);color:var(--ink);font-size:14px;font-weight:700;cursor:pointer;transition:background 150ms,border-color 150ms}
button:disabled{cursor:not-allowed;opacity:.55}
button.primary{border:0;background:var(--accent);color:#fff}
button.primary:not(:disabled):hover{background:var(--accent-dark)}
button.ghost:not(:disabled):hover{border-color:var(--accent);color:var(--accent-dark)}
button.danger{color:var(--danger)}
button.tiny{min-height:36px;padding:0 11px;font-size:13px;border-radius:7px}
select{min-height:38px;padding:0 9px;border:1px solid var(--line-strong);border-radius:8px;outline:none;background:var(--paper);color:var(--ink);font-size:14px;font-weight:700}
input:not([type=checkbox]){min-height:44px;padding:0 13px;border:1px solid var(--line-strong);border-radius:9px;outline:none;background:var(--paper);color:var(--ink);font-size:15px;font-weight:600;transition:border-color 150ms,box-shadow 150ms}
select:focus-visible,input:not([type=checkbox]):focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(163,72,37,.15)}
.statusline{display:flex;align-items:center;gap:10px;flex-wrap:wrap;font-size:14px;margin-bottom:14px}
.statusline .revision{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:10px;color:var(--muted);overflow-wrap:anywhere}
.setup-status{display:grid;gap:12px;margin:16px 0 4px;padding:15px;border:1px solid var(--line);border-radius:10px;background:#faf8f4}
.setup-row{display:flex;align-items:flex-start;gap:12px;flex-wrap:wrap}
.setup-row + .setup-row{padding-top:12px;border-top:1px solid #f1e8e2}
.setup-icon{flex:0 0 auto;width:26px;height:26px;display:grid;place-items:center;border:1px solid var(--line);border-radius:50%;background:var(--paper);color:var(--muted);font-size:13px;font-weight:800}
.setup-icon.ready{border-color:#20996c;color:#20996c}
.setup-icon.attention{border-color:var(--warning);background:var(--warning-soft);color:var(--warning)}
.setup-icon.busy{border-color:var(--accent);background:var(--accent-soft);color:var(--accent)}
.setup-icon.neutral{color:var(--line-strong)}
.setup-detail-block{flex:1 1 260px;display:grid;gap:2px}
.setup-title{color:var(--ink);font-size:14px;font-weight:800}
.setup-detail{color:var(--muted);font-size:12px;line-height:1.6}
.setup-detail a{color:var(--accent-dark)}
#model-progress{margin-top:12px}
.records{display:grid;gap:10px}
.record-filters{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 12px}
.record-filters button{min-height:32px;padding:0 12px;border:1px solid var(--line);border-radius:999px;background:var(--paper);color:var(--muted);font-size:13px;font-weight:700}
.record-filters button[aria-pressed="true"]{border-color:var(--accent);background:var(--accent-soft);color:var(--accent-dark)}
.record-filters button span{margin-left:2px;font-weight:800}
.record{border:1px solid var(--line);border-radius:12px;background:#fffdfa;padding:17px 18px;display:grid;gap:15px}
.record-head{display:flex;align-items:center;gap:8px;flex-wrap:wrap;color:var(--muted);font-size:12px}
.record-head .time{font-weight:800;color:#55463c}
.record-head .actions{margin-left:auto;display:flex;gap:6px}
.align{padding:3px 8px;border-radius:999px;background:var(--accent-soft);color:var(--accent-dark);font-size:11px;font-weight:800}
.align.partial{background:var(--warning-soft);color:var(--warning)}
.sentence{margin:0;color:var(--ink);font-family:ui-serif,"Noto Serif TC","Songti TC","PMingLiU","Noto Serif CJK TC",serif;font-size:clamp(19px,3vw,24px);line-height:2.25;overflow-wrap:anywhere}
.sentence .context{display:block;max-width:100%;margin-bottom:15px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--muted);font-family:system-ui,sans-serif;font-size:14px;line-height:1.6}
.sentence .context-label{margin-right:10px;color:var(--accent-dark);font-size:12px;font-weight:700}
.answer{display:inline-flex;max-width:100%;flex-wrap:wrap;align-items:flex-end;row-gap:.8em;padding:1em 10px 10px;border-radius:6px;background:var(--accent-soft);color:var(--accent-dark);vertical-align:middle;font-weight:800}
.syllable{position:relative;display:inline-block;min-width:2.1em;text-align:center;vertical-align:bottom;line-height:1.15}
.syllable.literal{min-width:0}
.syllable .reading{position:absolute;left:50%;bottom:100%;transform:translateX(-50%);font-family:inherit;font-size:.55em;font-weight:800;letter-spacing:.01em;color:var(--accent-dark);white-space:nowrap;margin-bottom:2px}
.syllable .character{font-size:1em}
.syllable.manual{background:#eed3c1;border-radius:5px}
.syllable .reading.unresolved{color:var(--warning)}
.record-foot{display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;padding-top:12px;border-top:1px solid #f1e8e2;font-size:13px;color:var(--muted)}
.record-foot .readings{flex:1 1 auto;min-width:0;overflow-wrap:anywhere}
.record-foot .readings strong{margin-left:6px;color:#55463c;font-weight:700}
.revised{color:var(--accent);font-weight:800;letter-spacing:.04em}
.chip{padding:4px 8px;border-radius:999px;background:var(--warning-soft);color:var(--warning);font-size:12px;font-weight:800}
.chip.trained{background:var(--accent-soft);color:var(--accent-dark)}
.chip.excluded{background:#eee8e1;color:var(--muted)}
.tag{padding:4px 8px;border-radius:999px;background:#f2ede7;color:var(--muted);font-size:12px;font-weight:800}
.tag.error{background:var(--danger-soft);color:var(--danger)}
.path{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;color:var(--muted);overflow-wrap:anywhere}
.history-toolbar{display:flex;align-items:center;gap:8px;margin:0 0 8px}
.history-toolbar .hint{margin:0}
.history-toolbar .row{margin-left:auto}
.history-graph{position:relative;overflow:auto;max-height:460px;border:1px solid var(--line);border-radius:12px;background:#fdfcf9;cursor:grab;touch-action:none}
.history-graph.panning{cursor:grabbing}
.history-viewport{position:relative;overflow:hidden}
.history-canvas{position:absolute;left:0;top:0;transform-origin:0 0}
.history-links{position:absolute;left:0;top:0;pointer-events:none}
.history-links line{stroke:var(--line-strong);stroke-width:2}
.history-node{position:absolute;width:76px;height:76px;min-height:76px;padding:4px;border:2px solid var(--line-strong);border-radius:50%;background:var(--paper);display:grid;place-items:center;text-align:center;line-height:1.15;box-shadow:0 2px 6px rgba(54,42,32,.08)}
.history-node:hover{border-color:var(--accent)}
.history-node.selected{border-color:var(--accent);box-shadow:0 0 0 4px rgba(163,72,37,.15)}
.history-node.applied{border-color:#20996c}
.history-node.applied.selected{border-color:var(--accent)}
.history-node .history-face{display:grid;gap:1px;justify-items:center;font-size:10px}
.history-node .history-face strong{font-size:11px;font-weight:800}
.history-details{display:grid;gap:3px;margin-top:12px;padding:13px 15px;border:1px solid var(--line);border-radius:10px;background:#faf8f4;color:var(--muted);font-size:12px}
.history-details .h-title{color:var(--ink);font-size:13px;font-weight:800}
.history-details .mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;overflow-wrap:anywhere}
.tagrow{display:flex;gap:6px;flex-wrap:wrap}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
.options{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:16px;margin-bottom:18px}
.field{display:flex;flex-direction:column;gap:6px}
.field span{color:#554e47;font-size:13px;font-weight:800}
.field input,.field select{min-width:0}
.switch{display:flex;align-items:center;gap:9px;align-self:end;padding-bottom:11px;font-size:14px;font-weight:700}
.switch input{width:16px;height:16px;accent-color:var(--accent)}
.estimate{display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin:0 0 16px;color:var(--muted);font-size:14px}
.advanced{margin:0 0 20px;border:1px solid var(--line);border-radius:10px;padding:0 16px}
.advanced summary{padding:13px 0;cursor:pointer;color:var(--accent-dark);font-size:14px;font-weight:700}
.advanced .options{padding:8px 0 0}
progress{width:100%;height:6px;accent-color:var(--accent);border:none;border-radius:999px}
.hint{margin:9px 1px 0;color:var(--muted);font-size:13px;line-height:1.6}
.log{margin-top:14px;padding:13px 15px;border:1px solid var(--line);border-radius:10px;background:#faf8f4;color:#554e47;font-size:11px;max-height:14rem;overflow:auto;white-space:pre-wrap;overflow-wrap:anywhere}
.empty{margin:0;color:var(--muted);font-size:14px;line-height:1.7}
.protection{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:16px;padding:15px;border:1px solid var(--line);border-radius:10px;background:#faf8f4}
.protection .hint{color:var(--muted);font-size:13px;flex:1 1 230px}
.protection input[type=password]{width:150px;padding:6px 8px;border:1px solid var(--line);border-radius:8px;font:inherit;font-size:12px}
.protection-setup{display:grid;gap:12px;margin-bottom:16px;padding:18px;border:1px solid var(--line);border-radius:10px;background:var(--accent-soft)}
.locked-note{margin:0;color:var(--muted);font-size:14px}
.pagination{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin:16px 0 0;color:var(--muted);font-size:13px}
.pagination .row{margin-left:auto}
@media(max-width:640px){
  .topbar{padding:12px 18px;gap:10px}
  .brand-logo{width:38px;height:38px}
  .top-note{display:none}
  .page-content{width:calc(100% - 32px);padding-top:28px}
  .page-heading{margin-bottom:18px}
  .page-heading p{font-size:14px}
  .field-group{padding:20px 17px}
  .field-label-row{align-items:flex-start;flex-direction:column;gap:8px}
  .pagination .row{margin-left:0}
  .record{padding:15px}
  .record-head .actions{margin-left:auto}
  .tabs button{padding:0 6px}
  .options{grid-template-columns:repeat(auto-fill,minmax(145px,1fr))}
}
</style>
<div class="site-shell">
<header class="topbar">
  <span class="brand">
    <img class="brand-logo" src="@@LOGO@@" alt="">
    <span><strong>拉風輸入法</strong><small>個人化訓練</small></span>
  </span>
  <span class="topbar-actions">
    <button id="reload-records" class="ghost tiny">重新整理</button>
    <span class="top-note">資料只留在本機</span>
  </span>
</header>
<main class="page-content">
  <div class="page-heading">
    <h1>個人化訓練</h1>
    <p>檢視待訓練資料、訓練專屬模型，完成後再套用到輸入法。</p>
  </div>
  <div id="message" class="notice">連線中…</div>
  <nav class="tabs" role="tablist" aria-label="個人化訓練頁面">
    <button id="nav-records" role="tab" type="button" data-panel="tab-records" aria-controls="tab-records" aria-selected="true">訓練資料</button>
    <button id="nav-training" role="tab" type="button" data-panel="tab-training" aria-controls="tab-training" aria-selected="false">模型與訓練</button>
  </nav>
  <div id="tab-records" class="tab-panel" role="tabpanel" aria-labelledby="nav-records">
  <section class="form-card">
    <div class="field-group">
      <div class="field-label-row">
        <span class="field-label">訓練資料</span>
        <span id="selection-summary"></span>
      </div>
      <div class="record-filters" role="tablist" aria-label="訓練資料狀態">
        <button id="filter-pending" type="button" role="tab" aria-pressed="true">未訓練 <span id="count-pending">0</span></button>
        <button id="filter-trained" type="button" role="tab" aria-pressed="false">已訓練 <span id="count-trained">0</span></button>
        <button id="filter-excluded" type="button" role="tab" aria-pressed="false">已排除 <span id="count-excluded">0</span></button>
      </div>
      <label class="switch"><input type="checkbox" id="only-manual-records">僅顯示手動選字過資料</label>
      <div id="protection" class="protection"></div>
      <div id="password-setup" class="protection-setup" hidden>
        <label class="field"><span>密碼</span><input id="setup-password" type="password" autocomplete="new-password"></label>
        <label class="field"><span>再次輸入密碼</span><input id="setup-confirmation" type="password" autocomplete="new-password"></label>
        <p class="field-hint">密碼不會被系統保存；忘記密碼只能清除已保存的對話資料，模型與訓練歷程會保留。</p>
        <div class="row"><button id="setup-cancel" class="ghost tiny">取消</button><button id="setup-confirm" class="primary tiny">設定並開始收集</button></div>
      </div>
      <div id="records" class="records"></div>
      <div class="pagination">
        <span id="page-summary"></span>
        <div class="row"><button id="previous" class="ghost tiny">上一頁</button><button id="next" class="ghost tiny">下一頁</button></div>
      </div>
    </div>
  </section>
  </div>
  <div id="tab-training" class="tab-panel" role="tabpanel" aria-labelledby="nav-training" hidden>
  <section class="form-card">
    <div class="field-group">
      <div class="field-label-row">
        <span class="field-label">基礎模型</span>
        <span id="model-revision" class="revision"></span>
      </div>
      <div class="setup-row">
        <span id="model-icon" class="setup-icon neutral">●</span>
        <span class="setup-detail-block">
          <span id="model-title" class="setup-title">尚未下載</span>
          <span id="model-detail" class="setup-detail">下載模型後才能開始訓練。</span>
        </span>
        <span class="row" style="margin-left:auto">
          <button id="check" class="ghost tiny">檢查更新</button>
          <button id="fetch" class="primary tiny">下載模型</button>
        </span>
      </div>
      <progress id="model-progress" max="100" style="display:none"></progress>
      <p class="field-hint">訓練使用 tony65535/llavon-ime-llama-250m（約 1 GB，CC-BY-NC-4.0）。訓練完成後由你決定何時套用。</p>
    </div>
  </section>
  <section class="form-card">
    <div class="field-group">
      <div class="field-label-row">
        <span class="field-label">訓練設定</span>
      </div>
      <label class="field"><span>訓練強度</span><select id="strength"></select></label>
      <p class="field-hint" id="strength-hint"></p>
      <div id="parameter-fields">
      <div id="training-options" class="options"></div>
      <details class="advanced"><summary>進階訓練設定</summary><div id="advanced-options" class="options"></div></details>
      </div>
      <label class="switch"><input type="checkbox" id="only-manually-selected" checked>只訓練曾手動選字的句子</label>
      <p class="field-hint" id="manual-scope-hint"></p>
      <label class="switch"><input type="checkbox" id="stabilize-intruders" checked>降低模型遺忘（實驗性）</label>
      <p class="field-hint">訓練後會保守縮小可能干擾原模型能力的 LoRA 維度（<a href="https://arxiv.org/html/2410.21228v3" target="_blank" rel="noopener">論文</a>）。</p>
      <label class="field"><span>訓練基底</span><select id="base-run"></select></label>
      <label class="field" id="train-password-field" hidden><span>訓練密碼</span><input id="train-password" type="password" autocomplete="current-password"></label>
      <p class="field-hint" id="train-password-hint" hidden>開始訓練時須重新輸入密碼；檢視資料的解鎖狀態不會共用。</p>
      <div class="estimate"><span id="estimated-steps">預計 steps：0</span><progress id="progress" max="100" style="display:none"></progress></div>
      <div class="row"><button id="train" class="primary">開始訓練</button><button id="cancel" class="ghost">取消目前工作</button></div>
      <div class="setup-status">
        <div class="setup-row">
          <span id="trainer-icon" class="setup-icon neutral">●</span>
          <span class="setup-detail-block">
            <span id="trainer-title" class="setup-title">尚未安裝 LoRA 訓練器</span>
            <span id="trainer-status" class="setup-detail"></span>
            <span id="trainer-release" class="setup-detail"></span>
          </span>
          <span class="row" style="margin-left:auto">
            <button id="check-trainer" class="ghost tiny">檢查版本</button>
            <button id="install-trainer" class="ghost tiny">安裝／更新</button>
          </span>
        </div>
        <div class="setup-row">
          <span id="device-icon" class="setup-icon neutral">●</span>
          <span class="setup-detail-block">
            <span id="device-title" class="setup-title">訓練裝置</span>
            <span id="gpu-status" class="setup-detail"></span>
          </span>
        </div>
      </div>
      <pre id="log" class="log"></pre>
    </div>
  </section>
  <section class="form-card">
    <div class="field-group">
      <div class="field-label-row">
        <span class="field-label">訓練歷程</span>
        <span id="history-summary"></span>
      </div>
      <div class="history-toolbar">
        <span class="hint">拖曳可平移、Ctrl＋滾輪縮放</span>
        <span class="row">
          <button id="history-zoom-out" class="ghost tiny" aria-label="縮小">－</button>
          <button id="history-zoom-in" class="ghost tiny" aria-label="放大">＋</button>
          <button id="history-zoom-reset" class="ghost tiny">重設視圖</button>
        </span>
      </div>
      <div id="runs" class="history-graph" role="tree" aria-label="訓練歷程"></div>
      <div id="history-details" class="history-details" hidden></div>
      <div id="history-actions" class="row" hidden>
        <button id="history-tarjan" class="ghost tiny" hidden>共同祖先（Tarjan）</button>
        <span id="history-selected" class="hint" style="flex:1 1 auto"></span>
        <button id="history-base" class="ghost tiny">設為訓練基底</button>
        <button id="history-apply" class="primary tiny">立即套用</button>
      </div>
    </div>
  </section>
  </div>
</main>
</div>
<script>
const token = location.hash.slice(1) || sessionStorage.getItem('llavon-token');
// Captured before the token is stripped from the address bar below.
const initialTab = new URLSearchParams(location.search).get('tab');
if (location.hash) { sessionStorage.setItem('llavon-token', token); history.replaceState(null, '', '/'); }
const message = document.getElementById('message');
let recordOffset=0;
const PAGE_SIZE=20;
// Feedback for a click must stay readable; the poll only writes the job status
// while no fresh notice is pinned.
let noticeUntil=0;
let lastJobSignature='';
let trainerCheckRequested=false;
let loadedBuild=null;
function showNotice(text, error=false){
  noticeUntil=Date.now()+15000;
  message.hidden=false;message.className='notice'+(error?' error':'');message.textContent=text;
}

let pendingCount=0;
let pendingManual=0;
let trainedCount=0;
let excludedCount=0;
// Which review category the list shows: pending, trained or excluded.
let recordState='pending';
// The exact trainable count (records/samples/skipped ids) for the current
// filter, fetched through the CLI's dataset conversion when it is available.
let exactCount=null;
let exactCountKey='';
let exactCountPending=false;
let exactCountInitialized=false;
let jobRunning=false;
let modelReady=false;
let trainerReady=false;
let recordRows=[];
let readingsTable={};
let activeModelPath='';
let protectionInfo={configured:false,enabled:false,unlocked:false};
function showTab(panel){
  for(const button of document.querySelectorAll('.tabs button')){
    const active=button.dataset.panel===panel;
    button.setAttribute('aria-selected',String(active));
    document.getElementById(button.dataset.panel).hidden=!active;
  }
}
for(const tab of document.querySelectorAll('.tabs button'))
  tab.onclick=()=>showTab(tab.dataset.panel);
// Deep link for the training tab, used by the review pane and screenshots.
if(initialTab==='training')showTab('tab-training');
function manualOnlyTraining(){
  return document.getElementById('only-manually-selected').checked;
}
// The review list shows one category at a time; the counts come from the
// same database states the CLI uses.
function selectRecordState(state){
  recordState=state;recordOffset=0;
  for(const [id,name] of [['filter-pending','pending'],['filter-trained','trained'],['filter-excluded','excluded']])
    document.getElementById(id).setAttribute('aria-pressed',String(name===state));
  updateEstimate();
  refresh();
}
document.getElementById('filter-pending').onclick=()=>selectRecordState('pending');
document.getElementById('filter-trained').onclick=()=>selectRecordState('trained');
document.getElementById('filter-excluded').onclick=()=>selectRecordState('excluded');
function exactCountMatches(){
  const key=manualOnlyTraining()+'/'+document.getElementById('max-seq-length').value;
  return exactCount&&exactCountKey===key?exactCount:null;
}
// The button follows the job, the installed pieces and the exact data count;
// a run with no convertible record cannot start.
function updateTrainButton(){
  const exact=exactCountMatches();
  document.getElementById('train').disabled=
    jobRunning||!modelReady||!trainerReady||(exact!==null&&exact.records===0);
}
// The dataset conversion decides which records really train; asking the CLI
// keeps the displayed numbers identical to the run. It needs the training
// password when the store is encrypted, so it is retried whenever the password
// is entered.
async function refreshExactCount(){
  if(exactCountPending)return;
  const manualOnly=manualOnlyTraining();
  const maxSeq=document.getElementById('max-seq-length').value;
  let password='';
  if(protectionInfo.configured){
    password=document.getElementById('train-password').value;
    if(!password){exactCount=null;exactCountKey='';updateEstimate();return;}
  }
  exactCountPending=true;
  try{
    const result=await api('count-trainable',{only_manually_selected:manualOnly,max_seq_length:maxSeq,password});
    exactCount=result;exactCountKey=manualOnly+'/'+maxSeq;
    updateEstimate();renderRecords(recordRows);
  }catch(error){
    exactCount=null;exactCountKey='';updateEstimate();
  }finally{exactCountPending=false;}
}
function effectivePendingCount(){
  return manualOnlyTraining()?pendingManual:pendingCount;
}
function updateEstimate(){
  // Pending records all take part in the next run; the manual filter mirrors
  // the Windows manager's "only train manually selected sentences" default.
  // A manually selected record contributes three samples, like the trainer.
  // The exact conversion count is used whenever it has been computed.
  const exact=exactCountMatches();
  const samples=exact?exact.samples:(manualOnlyTraining()?pendingManual*3:pendingCount+2*pendingManual);
  let summary;
  if(recordState==='trained')summary='共 '+trainedCount+' 筆（已訓練）';
  else if(recordState==='excluded')summary='共 '+excludedCount+' 筆（已排除）';
  else{
    summary='共 '+pendingCount+' 筆';
    if(manualOnlyTraining())summary+=' · 手動選字 '+pendingManual+' 筆';
    if(exact)summary+=' · 可訓練 '+exact.records+' 筆';
  }
  document.getElementById('selection-summary').textContent=summary;
  // Say plainly how many records the current filter feeds into the run; the
  // pending list can be much larger than what the manual-only default trains.
  const scopeHint=document.getElementById('manual-scope-hint');
  if(manualOnlyTraining()){
    scopeHint.textContent=exact
      ?('本次會訓練 '+exact.records+' 筆（曾手動選字）。')
      :('只訓練曾手動選字的 '+pendingManual+' 筆；無法轉換的會在開始時跳過。');
  }else{
    scopeHint.textContent=exact
      ?('本次會訓練 '+exact.records+' 筆。')
      :('本次會訓練全部 '+pendingCount+' 筆；無法轉換的會在開始時跳過。');
  }
  updateTrainButton();
  const presets={'ultra-low':1,'low':1,'medium':2,'high':5};
  const values=strengthSelect.value==='advanced'
    ? ['batch-size','gradient-accumulation','epochs','max-steps'].map(name=>Number(document.getElementById(name).value))
    : [1,1,presets[strengthSelect.value],-1];
  if(values.every(Number.isInteger)&&values[0]>0&&values[1]>0&&values[2]>0&&(values[3]===-1||values[3]>0)){
    const epochs=Math.ceil(Math.ceil(samples/values[0])/values[1])*values[2];
    document.getElementById('estimated-steps').textContent=
      '預計最多 '+(values[3]>0?Math.min(epochs,values[3]):epochs)+' steps'+(exact?'':'（有效資料可能較少）');
  }
}
const fields=[['rank','LoRA rank','8'],['alpha','LoRA alpha','16'],['dropout','LoRA dropout','0'],
  ['batch-size','Batch size','1'],['gradient-accumulation','Gradient accumulation','1'],
  ['epochs','Epochs','5'],['max-steps','Max steps','-1'],['learning-rate','Learning rate','0.0001'],
  ['weight-decay','Weight decay','0'],['warmup-steps','Warmup steps','0'],
  ['max-grad-norm','Max gradient norm','1'],['save-every','Save every','0'],['seed','Seed','42'],
  ['max-seq-length','Max sequence length','384'],['target-modules','Target modules','q_proj,v_proj']];
const optionsView=document.getElementById('training-options');
const advancedView=document.getElementById('advanced-options');
const basicFields=new Set(['rank','alpha','epochs','learning-rate']);
const fieldLabels={'rank':'LoRA rank','alpha':'LoRA alpha','dropout':'Dropout',
  'batch-size':'Batch size','gradient-accumulation':'梯度累積','epochs':'訓練回合',
  'max-steps':'最多步數','learning-rate':'學習率','weight-decay':'Weight decay',
  'warmup-steps':'Warmup steps','max-grad-norm':'梯度上限','save-every':'儲存間隔',
  'seed':'隨機種子','max-seq-length':'最大序列長度','target-modules':'目標模組'};
for(const [name,label,value] of fields){
  const field=document.createElement('label');field.className='field';
  const caption=document.createElement('span');caption.textContent=fieldLabels[name]||label;
  const input=document.createElement('input');input.id=name;input.value=value;
  field.append(caption,input);(basicFields.has(name)?optionsView:advancedView).append(field);
  input.oninput=updateEstimate;
  if(name==='max-seq-length')input.onchange=refreshExactCount;
}
for(const [name,label,choices] of [['device','運算裝置',['auto','cuda','mps','cpu']],['dtype','數值精度',['float32','bfloat16']]]){
  const field=document.createElement('label');field.className='field';
  const caption=document.createElement('span');caption.textContent=label;
  const select=document.createElement('select');select.id=name;
  for(const choice of choices){const item=document.createElement('option');item.textContent=choice;select.append(item);}
  field.append(caption,select);optionsView.append(field);
}
const shuffleLabel=document.createElement('label');shuffleLabel.className='switch';
const shuffle=document.createElement('input');shuffle.type='checkbox';shuffle.id='shuffle';shuffle.checked=true;
shuffleLabel.append(shuffle,document.createTextNode('打亂訓練資料'));advancedView.append(shuffleLabel);
// Training strengths are shared with the Windows manager; the individual
// fields only apply to the advanced strength.
const strengthLabels=[['ultra-low','極低'],['low','低'],['medium','中'],['high','高'],['advanced','進階']];
const strengthSelect=document.getElementById('strength');
for(const [value,label] of strengthLabels){
  const item=document.createElement('option');item.value=value;item.textContent=label;strengthSelect.append(item);
}
strengthSelect.value='ultra-low';
function applyStrength(){
  const advanced=strengthSelect.value==='advanced';
  document.getElementById('parameter-fields').style.display=advanced?'':'none';
  document.getElementById('strength-hint').textContent=advanced
    ? '可自行調整訓練參數。'
    : '訓練強度越高，個人化效果與原有選字能力的變化都可能增加。';
  updateEstimate();
}
strengthSelect.onchange=applyStrength;
document.getElementById('only-manually-selected').onchange=()=>{updateEstimate();renderRecords(recordRows);refreshExactCount();};
document.getElementById('base-run').onchange=applyBaseParameters;
document.getElementById('train-password').onchange=refreshExactCount;
document.getElementById('train-password').onblur=refreshExactCount;
applyStrength();
async function api(path, body) {
  const options = {headers:{'X-Llavon-Token':token}};
  if (body !== undefined) {options.method='POST';options.headers['Content-Type']='application/json';options.body=JSON.stringify(body);}
  const response = await fetch('/api/'+path, options);
  const result = await response.json();
  if (!response.ok) throw Error(result.error || '請求失敗');
  return result;
}
let protectionSignature='';
function renderProtection(info){
  // Re-rendering on every poll would clear a password the user is typing, so
  // the bar only changes when the protection state itself does.
  const signature=JSON.stringify(info);
  if(signature===protectionSignature)return;
  protectionSignature=signature;
  const bar=document.getElementById('protection');bar.replaceChildren();
  const chip=document.createElement('span');chip.className='chip '+((info.configured&&info.enabled)?'trained':'');
  chip.textContent=!info.configured?'尚未設定密碼':(info.enabled?'加密收集已啟用':'加密收集已停用');
  bar.append(chip);
  const note=document.createElement('span');note.className='hint';
  // An open setup panel keeps what was typed; it closes once a password exists.
  if(info.configured)document.getElementById('password-setup').hidden=true;
  document.getElementById('train-password-field').hidden=!info.configured;
  document.getElementById('train-password-hint').hidden=!info.configured;
  if(!info.configured){
    note.textContent='設定密碼後才會加密記錄你送出的句子；資料只留在本機。';
    const setup=document.createElement('button');setup.className='primary tiny';setup.textContent='設定密碼並開始收集';
    setup.onclick=()=>{const panel=document.getElementById('password-setup');panel.hidden=!panel.hidden;
      if(!panel.hidden)document.getElementById('setup-password').focus();};
    bar.append(note,setup);
    return;
  }
  if(info.unlocked){
    note.textContent='已解鎖，可以檢視待訓練資料。';
    const lock=document.createElement('button');lock.className='ghost tiny';lock.textContent='鎖定';
    lock.onclick=()=>act('lock',{});bar.append(note,lock);
  }else{
    note.textContent='內容已加密，輸入密碼後才能檢視。';
    const password=document.createElement('input');password.type='password';password.placeholder='密碼';
    password.autocomplete='current-password';
    const unlock=document.createElement('button');unlock.className='primary tiny';unlock.textContent='解鎖檢視';
    unlock.onclick=async()=>{
      try{await api('unlock',{password:password.value});password.value='';await refresh();}
      catch(error){showNotice(error.message,true);}
    };
    password.onkeydown=event=>{if(event.key==='Enter')unlock.click();};
    bar.append(note,password,unlock);
  }
  const toggle=document.createElement('button');toggle.className='ghost tiny';
  toggle.textContent=info.enabled?'停用收集':'啟用收集';
  toggle.onclick=()=>act('protection',{action:info.enabled?'disable':'enable'});
  const forget=document.createElement('button');forget.className='ghost tiny danger';
  forget.textContent='忘記密碼，清除所有對話資料';
  forget.onclick=async()=>{
    if(!confirm('將停止收集並清除所有已保存的對話紀錄；模型與訓練歷程會保留。要繼續嗎？'))return;
    if(!confirm('再次確認：清除後無法復原已保存的對話。'))return;
    await act('protection',{action:'forget'});
  };
  bar.append(toggle,forget);
}
function tableReadings(character){
  const known=readingsTable[character];
  return Array.isArray(known)?known:[];
}
function readingSequence(item){
  const characters=Array.from(item.answer||'');
  const readings=item.readings||[];
  const recorded=readings.length>0;
  // Literal positions have no reading; only the composed ones are listed.
  return characters.map((character,index)=>{
    if(recorded&&!readings[index])return null;
    const reading=recorded?readings[index]:tableReadings(character)[0];
    return reading||null;
  }).filter(value=>value).join('　');
}
function composed(item){
  const sentence=document.createElement('p');sentence.className='sentence';
  if(item.context){
    const context=document.createElement('span');context.className='context';
    const label=document.createElement('span');label.className='context-label';label.textContent='前文';
    const text=document.createElement('span');text.textContent=item.context;
    context.title=item.context;context.append(label,text);sentence.append(context);
  }
  const answer=document.createElement('span');answer.className='answer';
  const characters=Array.from(item.answer||'');
  const readings=item.readings||[];
  const manual=item.manual||[];
  const recorded=readings.length>0;
  characters.forEach((character,index)=>{
    const known=tableReadings(character);
    const reading=recorded?readings[index]:known[0];
    // A literal position was typed as-is: it shows the character alone, with
    // no reading above it and no unresolved warning.
    const literal=recorded&&!reading;
    const syllable=document.createElement('span');syllable.className='syllable';
    if(literal)syllable.classList.add('literal');
    if(manual[index])syllable.classList.add('manual');
    const annotation=document.createElement('span');annotation.className='reading';
    annotation.textContent=reading||'';
    // The character table decides whether the recorded reading is plausible;
    // a reading outside it is flagged instead of silently rendered.
    const normalized=value=>(value||'').replace(/\s+$/,'');
    if(!literal&&reading&&known.length&&!known.some(item=>normalized(item)===normalized(reading)))annotation.classList.add('unresolved');
    if(!literal&&known.length)syllable.title=character+'：'+known.join('、');
    const text=document.createElement('span');text.className='character';text.textContent=character;
    syllable.append(annotation,text);answer.append(syllable);
  });
  sentence.append(answer);
  return sentence;
}
// Marks records the dataset conversion refused; the ids come from the exact
// count so the list matches what a training run would do.
function renderRecords(rows){
  const list=document.getElementById('records');list.replaceChildren();
  if(!rows.length){
    const text=recordState==='trained'?'還沒有已訓練的資料。'
      :recordState==='excluded'?'沒有已排除的資料。'
      :'目前沒有待訓練資料。設定密碼並啟用收集後，提交注音文字就會出現在這裡。';
    list.innerHTML='<p class="empty">'+text+'</p>';
    return;
  }
  const exact=exactCountMatches();
  const skipped=new Set(exact&&Array.isArray(exact.skipped_ids)?exact.skipped_ids:[]);
  // With the manual-only default on, a pending record without a manual choice
  // will not take part in the next run; say so on the card.
  const onlyManual=manualOnlyTraining();
  for(const item of rows){
    const manual=(item.manual||[]).some(Boolean);
    list.append(recordCard(item,recordState,skipped.has(item.id),recordState==='pending'&&onlyManual&&!manual));
  }
}
function recordCard(item, viewState, unconvertible, willNotTrain){
  const card=document.createElement('article');card.className='record';  const head=document.createElement('div');head.className='record-head';
  const time=document.createElement('span');time.className='time';
  const date=new Date(item.committed_at);
  time.textContent=Number.isNaN(date.getTime())?item.committed_at:date.toLocaleString('zh-TW',
    {year:'numeric',month:'short',day:'numeric',hour:'2-digit',minute:'2-digit'});
  head.append(time);
  const actions=document.createElement('div');actions.className='actions';
  // Pending records all take part in the next run; unwanted ones are deleted,
  // exactly like the Windows manager.
  if(viewState==='pending'){
    const button=document.createElement('button');
    button.className='ghost tiny danger';button.textContent='刪除';
    button.onclick=async()=>{if(!confirm('確定刪除這筆訓練資料？'))return;
      await act('records/'+item.id+'/delete',{});};
    actions.append(button);
  }
  head.append(actions);
  if(item.text===false){
    // Sealed records keep their actions; only the text needs the password,
    // and the manager never returns it without one.
    const note=document.createElement('p');note.className='locked-note';note.textContent='內容已加密・解鎖後才能檢視';
    card.append(head,note);return card;
  }
  const aligned=(item.readings||[]).length>=Array.from(item.answer||'').length;
  if(!aligned){const alignment=document.createElement('span');alignment.className='align partial';
    alignment.textContent='部分對齊';head.append(alignment);}
  const foot=document.createElement('div');foot.className='record-foot';
  if((item.manual||[]).some(Boolean)){
    // Windows marks records that contain a manual candidate choice and gives
    // them three samples per epoch; show the same tag here.
    const revised=document.createElement('span');revised.className='revised';revised.textContent='曾經手動選字';
    foot.append(revised);
  }
  if(unconvertible){
    // The dataset conversion refused this record, so it stays pending and
    // would be skipped again; the exact count comes from count-trainable.
    const skipped=document.createElement('span');skipped.className='align partial';skipped.textContent='無法轉換（訓練時會跳過）';
    foot.append(skipped);
  }
  if(willNotTrain){
    const excluded=document.createElement('span');excluded.className='align partial';excluded.textContent='未手動選字（本次不會訓練）';
    foot.append(excluded);
  }
  if(viewState==='trained'){
    const tag=document.createElement('span');tag.className='align';tag.textContent='已訓練';
    foot.append(tag);
  }else if(viewState==='excluded'){
    const tag=document.createElement('span');tag.className='align partial';tag.textContent='已排除';
    foot.append(tag);
  }
  const readings=document.createElement('span');readings.className='readings';
  const readingLabel=document.createElement('span');readingLabel.textContent='逐字注音';
  const readingValue=document.createElement('strong');readingValue.textContent=readingSequence(item);
  readings.append(readingLabel,readingValue);foot.append(readings);
  card.append(head,composed(item),foot);
  return card;
}
// The training base mirrors the Windows manager's history choice: the newest
// run continues by default, an explicit run branches from that adapter, and
// "Base model" starts a fresh adapter. Selecting a base also loads its adapter
// structure into the advanced fields so a continued run stays compatible.
let baseRuns=[];
let baseParametersInitialized=false;
function runById(id){return baseRuns.find(run=>String(run.id)===String(id));}
function applyBaseParameters(){
  const value=document.getElementById('base-run').value;
  const run=value===''?(baseRuns.length?baseRuns[0]:null):(value==='0'?null:runById(value));
  document.getElementById('rank').value=String(run?run.rank:8);
  document.getElementById('alpha').value=String(run?run.alpha:16);
  document.getElementById('dropout').value=String(run?run.dropout:0);
  document.getElementById('target-modules').value=run?run.target_modules:'q_proj,v_proj';
}
function refreshBaseRuns(runs){
  baseRuns=runs;
  const select=document.getElementById('base-run');
  const previous=select.value;
  select.replaceChildren();
  const automatic=document.createElement('option');automatic.value='';
  automatic.textContent=runs.length?('自動（最新訓練 #'+runs[0].id+'）'):'自動（Base model）';
  select.append(automatic);
  const base=document.createElement('option');base.value='0';base.textContent='Base model（從頭訓練）';select.append(base);
  for(const run of runs){
    const item=document.createElement('option');item.value=String(run.id);
    item.textContent='#'+run.id+' '+run.completed_at+(run.parent_id?'（基底 #'+run.parent_id+'）':'');
    select.append(item);
  }
  if([...select.options].some(option=>option.value===previous))select.value=previous;
  if(!baseParametersInitialized){baseParametersInitialized=true;applyBaseParameters();}
}
const strengthLabelsForHistory={'ultra-low':'極低','low':'低','medium':'中','high':'高','advanced':'進階'};
// The history is drawn as the same node graph as the Windows manager: one
// circular node per training run, elbow lines to the parent, the leaf-based
// layout below ported from lora_history_tree.cpp. Selecting a node shows its
// details and offers the footer actions.
const historyNodeDiameter=76;
const historyLeafSpacing=180;
const historyLevelSpacing=100;
const historyPaddingX=40;
const historyPaddingY=28;
const historyZoomMin=0.6;
const historyZoomMax=2.5;
let historyZoom=1;
let historyRuns=[];
let historyCommonAncestor=null;
let selectedHistoryId=null;
let historyInitialized=false;
function appliedHistoryId(){
  const match=historyRuns.find(run=>run.model_path&&run.model_path===activeModelPath);
  return match?String(match.id):'0';
}
function fullParameterLines(request){
  return [
    `LoRA rank ${request.rank} · alpha ${request.alpha} · dropout ${request.dropout}`,
    `Target modules: ${request.target_modules}`,
    `Batch size ${request.batch_size} · gradient accumulation ${request.gradient_accumulation}`,
    `Epochs ${request.epochs} · max steps ${request.max_steps} · learning rate ${request.learning_rate}`,
    `Weight decay ${request.weight_decay} · warmup steps ${request.warmup_steps} · max gradient norm ${request.max_gradient_norm}`,
    `Save every ${request.save_every} · device ${request.device} · dtype ${request.dtype}`,
    `Seed ${request.seed} · shuffle ${request.shuffle?'on':'off'} · max sequence length ${request.max_sequence_length}`,
  ];
}
function historyDisplayTime(run){
  const date=new Date(run.completed_at);
  if(Number.isNaN(date.getTime()))return[run.completed_at,''];
  const text=date.toLocaleString('zh-TW',{year:'numeric',month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit'});
  const space=text.indexOf(' ');
  return space<0?[text,'']:[text.slice(0,space),text.slice(space+1)];
}
function historyTags(run){
  const tags=[];
  if(String(appliedHistoryId())===(run?String(run.id):'0'))tags.push('目前套用');
  if(run&&historyRuns.length&&String(historyRuns[0].id)===String(run.id))tags.push('最新訓練');
  return tags;
}
function historyDetailLines(run){
  const lines=[];
  if(!run){
    lines.push('原始模型 · 尚未個人化');
    return lines;
  }
  lines.push('新增 '+run.record_count+' 筆 · 累計 '+run.cumulative_count+' 筆');
  lines.push(run.optimizer_steps+' steps · 基底 '+(run.parent_id?('#'+run.parent_id):'Base'));
  if(run.request){
    if(run.only_manually_selected!==undefined)
      lines.push(run.only_manually_selected?'資料範圍：只訓練曾手動選字的句子':'資料範圍：所有句子');
    if(run.stabilize_intruders!==undefined)
      lines.push(run.stabilize_intruders?'降低模型遺忘：開啟':'降低模型遺忘：關閉');
    const strength=run.strength?strengthLabelsForHistory[run.strength]||run.strength:null;
    if(strength)lines.push('訓練強度：'+strength);
    else lines.push(...fullParameterLines(run.request));
  }else{
    lines.push('此歷史紀錄未保存完整訓練參數');
    lines.push('已知 LoRA rank '+run.rank+' · alpha '+run.alpha+' · dropout '+run.dropout);
    lines.push('Target modules: '+run.target_modules);
  }
  if(run.model_path)lines.push(run.model_path);
  return lines;
}
function renderHistoryDetails(){
  const box=document.getElementById('history-details');
  if(selectedHistoryId===null){box.hidden=true;return;}
  const run=historyRuns.find(item=>String(item.id)===String(selectedHistoryId));
  box.hidden=false;box.replaceChildren();
  const title=document.createElement('div');title.className='h-title';
  title.textContent=run?('訓練 #'+run.id+' · '+run.completed_at):'Base model · 原始模型';
  box.append(title);
  const tags=historyTags(run);
  if(tags.length){
    const tagrow=document.createElement('div');tagrow.className='tagrow';
    for(const text of tags){const tag=document.createElement('span');tag.className='tag';tag.textContent=text;tagrow.append(tag);}
    box.append(tagrow);
  }
  for(const text of historyDetailLines(run)){
    const line=document.createElement('div');
    if(/^(LoRA rank|Target modules|Batch size|Epochs|Weight decay|Save every|Seed|已知|此歷史紀錄)/.test(text))
      line.className='mono';
    line.textContent=text;box.append(line);
  }
}
function updateHistoryActions(){
  const actions=document.getElementById('history-actions');
  if(selectedHistoryId===null){actions.hidden=true;return;}
  actions.hidden=false;
  const selected=historyRuns.find(run=>String(run.id)===String(selectedHistoryId));
  document.getElementById('history-selected').textContent='已選擇 '+
    (selected?('訓練 #'+selected.id+' · '+selected.completed_at):'Base model');
  document.getElementById('history-tarjan').hidden=
    !(historyCommonAncestor!==null&&historyCommonAncestor!==undefined);
}
// Free panning and zooming of the history graph, matching the Windows picker:
// drag to pan, zoom around the pointer, clamped between 0.6x and 2.5x.
function setHistoryZoom(next,anchor){
  const graph=document.getElementById('runs');
  const viewport=graph.querySelector('.history-viewport');
  if(!viewport)return;
  const canvas=viewport.querySelector('.history-canvas');
  const baseWidth=parseFloat(canvas.style.width)||canvas.offsetWidth;
  const baseHeight=parseFloat(canvas.style.height)||canvas.offsetHeight;
  const previous=historyZoom;
  historyZoom=Math.min(historyZoomMax,Math.max(historyZoomMin,next));
  if(historyZoom===previous)return;
  const rect=graph.getBoundingClientRect();
  const point=anchor||{x:rect.left+rect.width/2,y:rect.top+rect.height/2};
  const localX=(graph.scrollLeft+(point.x-rect.left))/previous;
  const localY=(graph.scrollTop+(point.y-rect.top))/previous;
  viewport.style.width=(baseWidth*historyZoom)+'px';
  viewport.style.height=(baseHeight*historyZoom)+'px';
  canvas.style.transform='scale('+historyZoom+')';
  graph.scrollLeft=localX*historyZoom-(point.x-rect.left);
  graph.scrollTop=localY*historyZoom-(point.y-rect.top);
}
function installHistoryControls(){
  const graph=document.getElementById('runs');
  let panning=false,pointerId=0,startX=0,startY=0,startLeft=0,startTop=0;
  graph.addEventListener('pointerdown',event=>{
    if(event.button!==0)return;
    if(event.target.closest('.history-node'))return; // node clicks select
    panning=true;pointerId=event.pointerId;
    startX=event.clientX;startY=event.clientY;
    startLeft=graph.scrollLeft;startTop=graph.scrollTop;
    graph.classList.add('panning');
    graph.setPointerCapture(pointerId);
    event.preventDefault();
  });
  graph.addEventListener('pointermove',event=>{
    if(!panning||event.pointerId!==pointerId)return;
    graph.scrollLeft=startLeft-(event.clientX-startX);
    graph.scrollTop=startTop-(event.clientY-startY);
  });
  const finish=event=>{
    if(!panning||event.pointerId!==pointerId)return;
    panning=false;graph.classList.remove('panning');
    if(graph.hasPointerCapture(pointerId))graph.releasePointerCapture(pointerId);
  };
  graph.addEventListener('pointerup',finish);
  graph.addEventListener('pointercancel',finish);
  graph.addEventListener('wheel',event=>{
    // Plain scrolling stays with the page; Ctrl/Cmd zooms like the picker.
    if(!event.ctrlKey&&!event.metaKey)return;
    event.preventDefault();
    setHistoryZoom(historyZoom*Math.pow(1.12,-event.deltaY/120),{x:event.clientX,y:event.clientY});
  },{passive:false});
  document.getElementById('history-zoom-in').onclick=()=>setHistoryZoom(historyZoom*1.25);
  document.getElementById('history-zoom-out').onclick=()=>setHistoryZoom(historyZoom/1.25);
  document.getElementById('history-zoom-reset').onclick=()=>{
    const viewport=graph.querySelector('.history-viewport');
    if(!viewport)return;
    const canvas=viewport.querySelector('.history-canvas');
    historyZoom=1;
    viewport.style.width=canvas.style.width;
    viewport.style.height=canvas.style.height;
    canvas.style.transform='scale(1)';
    graph.scrollLeft=0;graph.scrollTop=0;
  };
}
function renderHistory(){
  const graph=document.getElementById('runs');graph.replaceChildren();  document.getElementById('history-summary').textContent=
    historyRuns.length?('共 '+historyRuns.length+' 次訓練'):'';
  if(!historyInitialized){historyInitialized=true;selectedHistoryId=appliedHistoryId();}
  const known=new Set(historyRuns.map(run=>String(run.id)));
  const children=new Map();
  for(const run of historyRuns){
    const parent=(run.parent_id&&known.has(String(run.parent_id)))?String(run.parent_id):'0';
    if(!children.has(parent))children.set(parent,[]);
    children.get(parent).push(run);
  }
  for(const list of children.values())list.sort((left,right)=>Number(left.id)-Number(right.id));
  const positions=new Map();
  let leafCount=0,deepest=0;
  const place=(id,depth)=>{
    deepest=Math.max(deepest,depth);
    const kids=children.get(id)||[];
    let x=0;
    if(!kids.length){
      x=historyPaddingX+90+leafCount*historyLeafSpacing;
      leafCount+=1;
    }else{
      const first=place(String(kids[0].id),depth+1);
      let last=first;
      for(let index=1;index<kids.length;++index)last=place(String(kids[index].id),depth+1);
      x=(first+last)/2;
    }
    positions.set(id,{x,y:historyPaddingY+26+depth*historyLevelSpacing});
    return x;
  };
  place('0',0);
  const width=Math.max(400,240+(leafCount-1)*historyLeafSpacing)+2*historyPaddingX;
  const height=68+deepest*historyLevelSpacing+historyNodeDiameter+2*historyPaddingY;
  const canvas=document.createElement('div');canvas.className='history-canvas';
  canvas.style.width=width+'px';canvas.style.height=height+'px';
  const namespace='http://www.w3.org/2000/svg';
  const svg=document.createElementNS(namespace,'svg');
  svg.setAttribute('class','history-links');
  svg.setAttribute('width',String(width));svg.setAttribute('height',String(height));
  const addLine=(x1,y1,x2,y2)=>{
    const line=document.createElementNS(namespace,'line');
    line.setAttribute('x1',String(x1));line.setAttribute('y1',String(y1));
    line.setAttribute('x2',String(x2));line.setAttribute('y2',String(y2));
    svg.append(line);
  };
  for(const run of historyRuns){
    const parentId=(run.parent_id&&positions.has(String(run.parent_id)))?String(run.parent_id):'0';
    const parent=positions.get(parentId),child=positions.get(String(run.id));
    if(!parent||!child)continue;
    const middle=(parent.y+historyNodeDiameter+child.y)/2;
    addLine(parent.x,parent.y+historyNodeDiameter,parent.x,middle);
    addLine(parent.x,middle,child.x,middle);
    addLine(child.x,middle,child.x,child.y);
  }
  canvas.append(svg);
  const addNode=(id,run)=>{
    const position=positions.get(id);
    if(!position)return;
    const button=document.createElement('button');
    button.type='button';button.className='history-node';
    if(String(selectedHistoryId)===id)button.classList.add('selected');
    if(String(appliedHistoryId())===id)button.classList.add('applied');
    button.style.left=(position.x-historyNodeDiameter/2)+'px';
    button.style.top=position.y+'px';
    button.setAttribute('role','treeitem');
    button.setAttribute('aria-label',run?('訓練 #'+run.id):'Base model');
    const face=document.createElement('span');face.className='history-face';
    const first=document.createElement('strong');
    const second=document.createElement('span');
    if(run){
      const parts=historyDisplayTime(run);
      first.textContent=parts[0];second.textContent=parts[1];
    }else{
      first.textContent='Base';second.textContent='model';
    }
    face.append(first,second);button.append(face);
    const tooltip=[];if(run)tooltip.push('訓練 #'+run.id+' · '+run.completed_at);
    else tooltip.push('Base model');
    tooltip.push(...historyTags(run),...historyDetailLines(run));
    button.title=tooltip.join('\n');
    button.onclick=()=>{selectedHistoryId=id;renderHistory();};
    canvas.append(button);
  };
  addNode('0',null);
  for(const run of historyRuns)addNode(String(run.id),run);
  // The canvas is scaled inside a viewport whose size follows the zoom, so the
  // scrollbars keep working while the graph pans and zooms like the Windows
  // history picker.
  const viewport=document.createElement('div');viewport.className='history-viewport';
  viewport.style.width=(width*historyZoom)+'px';
  viewport.style.height=(height*historyZoom)+'px';
  canvas.style.transform='scale('+historyZoom+')';
  viewport.append(canvas);
  graph.append(viewport);
  renderHistoryDetails();
  updateHistoryActions();
}
const jobLabels={fetch:'下載模型',check:'檢查模型更新',install:'安裝 Trainer','trainer-check':'檢查 Trainer 版本',train:'訓練及匯出模型',export:'重新匯出模型'};
async function refresh() {
  try {
    const state=await api('state');
    // A reinstalled manager restarts itself and reports a new build; reload so
    // the browser picks up the new interface without reopening the page.
    const build=String(state.build||'');
    if(loadedBuild===null)loadedBuild=build;
    else if(build!==loadedBuild){location.reload();return;}
    document.body.dataset.build=build;
    activeModelPath=state.active_model_path||'';
    protectionInfo=await api('protection');
    renderProtection(protectionInfo);
    if(!exactCountInitialized){exactCountInitialized=true;refreshExactCount();}
    const job=state.job;
    // Like the Windows dialog, the page checks the pinned trainer release once
    // when it opens; the check only compares versions and never downloads.
    if(!trainerCheckRequested){
      trainerCheckRequested=true;
      if(job.state==='idle')api('check-trainer',{}).then(()=>refresh()).catch(()=>{});
    }
    // A user-visible notice survives the poll until the job state changes or
    // its timeout elapses, so an error cannot flash for a moment and vanish.
    const jobSignature=job.kind+'/'+job.state;
    if(jobSignature!==lastJobSignature){
      lastJobSignature=jobSignature;noticeUntil=0;
      // A finished training changes the pending set; the exact count is
      // recomputed when the password is still available.
      if(job.kind==='train'&&job.state==='completed'){exactCount=null;exactCountKey='';refreshExactCount();}
    }
    // A failed background trainer check only updates the trainer line; it must
    // not pop an error notice every time the page is opened offline.
    const quietCheck=job.kind==='trainer-check'&&job.state==='failed';
    document.getElementById('log').textContent=job.log || '';
    document.getElementById('log').style.display=job.log?'block':'none';
    const progress=document.getElementById('progress');progress.style.display=job.state==='running'?'block':'none';
    if(job.percent!==null){progress.value=job.percent;}else{progress.removeAttribute('value');}
    document.getElementById('check').disabled=job.state==='running';
    document.getElementById('fetch').disabled=job.state==='running';
    document.getElementById('check-trainer').disabled=job.state==='running';
    document.getElementById('install-trainer').disabled=job.state==='running';
    jobRunning=job.state==='running';modelReady=!!state.model_ready;trainerReady=!!state.trainer_ready;
    updateTrainButton();
    document.getElementById('cancel').disabled=job.state!=='running';
    // Model, trainer and device rows follow the Windows setup area: an icon, a
    // title and a detail line each, with the actions on the right.
    const modelBusy=job.state==='running'&&(job.kind==='fetch'||job.kind==='check');
    const trainerBusy=job.state==='running'&&(job.kind==='install'||job.kind==='trainer-check');
    const modelIcon=document.getElementById('model-icon');
    const modelTitle=document.getElementById('model-title');
    const modelDetail=document.getElementById('model-detail');
    const modelProgress=document.getElementById('model-progress');
    document.getElementById('model-revision').textContent=state.model_ready?state.revision:'';
    if(modelBusy){
      modelIcon.className='setup-icon busy';modelIcon.textContent='↻';
      modelTitle.textContent=job.kind==='fetch'?'正在下載模型…':'正在檢查模型…';
      modelDetail.textContent=job.progress||'';
      modelProgress.style.display='block';modelProgress.removeAttribute('value');
    }else{
      modelProgress.style.display='none';
      if(state.model_update_available===true){
        modelIcon.className='setup-icon attention';modelIcon.textContent='↑';
        modelTitle.textContent='有可用更新';
        modelDetail.textContent='基礎模型已下載，可以開始訓練。';
      }else if(state.model_ready){
        modelIcon.className='setup-icon ready';modelIcon.textContent='✔';
        modelTitle.textContent='模型已就緒';
        modelDetail.textContent='基礎模型已下載，可以開始訓練。';
      }else{
        modelIcon.className='setup-icon attention';modelIcon.textContent='！';
        modelTitle.textContent='尚未下載';
        modelDetail.textContent='下載模型後才能開始訓練。';
      }
    }
    const fetchButton=document.getElementById('fetch');
    fetchButton.textContent=state.model_ready&&state.model_update_available===true?'更新模型':'下載模型';
    fetchButton.style.display=!modelBusy&&(!state.model_ready||state.model_update_available===true)?'':'none';

    const trainerIcon=document.getElementById('trainer-icon');
    const trainerTitle=document.getElementById('trainer-title');
    const trainerDetail=document.getElementById('trainer-status');
    const trainerRelease=document.getElementById('trainer-release');
    if(trainerBusy){
      trainerIcon.className='setup-icon busy';trainerIcon.textContent='↻';
      trainerTitle.textContent=job.kind==='install'?'正在安裝 LoRA 訓練器…':'正在檢查版本…';
      trainerDetail.textContent=job.progress||'';
    }else if(state.trainer_ready){
      trainerIcon.className='setup-icon ready';trainerIcon.textContent='✔';
      trainerTitle.textContent=state.trainer_version?('已安裝 '+state.trainer_version):'已安裝 LoRA 訓練器';
      trainerDetail.textContent=state.trainer_update_available===true
        ?'可下載此 submodule 對應的發行版。'
        :state.trainer_update_available===false
          ?'已安裝此 submodule 對應的發行版。'
          :'按「檢查版本」確認是否有更新。';
    }else{
      trainerIcon.className='setup-icon attention';trainerIcon.textContent='！';
      trainerTitle.textContent='尚未安裝 LoRA 訓練器';
      trainerDetail.textContent=state.trainer_version
        ?('已安裝版本 '+state.trainer_version+' 與目前 submodule 不符，請按「安裝／更新」。')
        :'按「安裝／更新」下載此 submodule 對應的發行版。';
    }
    trainerRelease.textContent=state.trainer_release_version
      ?('目前 submodule 對應發行版：'+state.trainer_release_version)
      :'尚未取得目前 submodule 對應的發行版資訊。';

    const deviceIcon=document.getElementById('device-icon');
    const deviceTitle=document.getElementById('device-title');
    const deviceDetail=document.getElementById('gpu-status');
    const gpu=state.gpu||'none';
    const gpuReady=state.trainer_gpu||'';
    if(gpu==='amd'||gpu==='nvidia'){
      const name=gpu==='amd'?'AMD GPU':'NVIDIA GPU';
      const backend=gpu==='amd'?'ROCm':'CUDA';
      const size=gpu==='amd'?'約 9.4 GB':'約 3.9 GB';
      const ready=gpuReady===(gpu==='amd'?'rocm':'cuda');
      deviceIcon.className='setup-icon '+(ready?'ready':'attention');
      deviceIcon.textContent=ready?'✔':'！';
      deviceTitle.textContent=name+'（'+backend+'）';
      deviceDetail.textContent=ready
        ?backend+' libtorch 已就緒，訓練會使用 GPU。'
        :'首次安裝／訓練前會下載 '+backend+' libtorch（'+size+'，只下載一次），之後訓練使用 GPU。';
    }else if(gpu==='apple'){
      deviceIcon.className='setup-icon ready';deviceIcon.textContent='✔';
      deviceTitle.textContent='Apple GPU（Metal）';
      deviceDetail.textContent='macOS 產物已內建 Metal（MPS），訓練會使用 GPU。';
    }else{
      deviceIcon.className='setup-icon ready';deviceIcon.textContent='✔';
      deviceTitle.textContent='CPU';
      deviceDetail.textContent='未偵測到可用的 GPU，訓練會使用 CPU。';
    }

    // An open page always mirrors the database: records typed while it is open
    // appear on the next poll and take part in the next training run.
    const counts=await api('pending-count');
    pendingCount=counts.count;pendingManual=counts.manual;
    trainedCount=counts.trained||0;excludedCount=counts.excluded||0;
    document.getElementById('count-pending').textContent=pendingCount;
    document.getElementById('count-trained').textContent=trainedCount;
    document.getElementById('count-excluded').textContent=excludedCount;
    updateEstimate();
    const manualFilter=document.getElementById('only-manual-records').checked?'&manual=1':'';
    let listing=await api('records?state='+recordState+manualFilter+'&offset='+recordOffset);
    if(recordOffset && recordOffset>=listing.total){
      recordOffset=Math.max(0,Math.ceil(listing.total/PAGE_SIZE)-1)*PAGE_SIZE;
      listing=await api('records?state='+recordState+manualFilter+'&offset='+recordOffset);
    }
    const page=Math.floor(recordOffset/PAGE_SIZE)+1;
    const pages=Math.max(1,Math.ceil((listing.total||0)/PAGE_SIZE));
    document.getElementById('page-summary').textContent='第 '+page+' / '+pages+' 頁'+
      (listing.total?' · '+(recordOffset+1)+'–'+(recordOffset+listing.rows.length)+' / '+listing.total+' 筆':'');
    document.getElementById('previous').disabled=recordOffset===0;
    document.getElementById('next').disabled=!listing.has_more;
    const records=listing.rows;
    recordRows=records;
    renderRecords(recordRows);
    const history=await api('history');
    historyRuns=Array.isArray(history.runs)?history.runs:[];
    historyCommonAncestor=(history.common_ancestor===undefined)?null:history.common_ancestor;
    renderHistory();
    refreshBaseRuns(historyRuns);
    // The job notice is rendered after the history so a completed training can
    // tell whether its model has already been applied.
    if(Date.now()>=noticeUntil && !quietCheck){
      const latest=historyRuns.length?historyRuns[0]:null;
      const latestApplied=latest&&latest.model_path&&latest.model_path===activeModelPath;
      const trained=(job.records!==null&&job.records!==undefined)?('（'+job.records+' 筆）'):'';
      message.hidden=job.state==='idle';
      message.className='notice'+(job.state==='failed'?' error':'');
      if(job.state==='running'){
        message.textContent=jobLabels[job.kind]+(job.records!==null&&job.records!==undefined?'・'+job.records+' 筆':'')+(job.progress?'・'+job.progress:'');
      }else if(job.state==='idle'){
        message.textContent='目前沒有工作';
      }else if(job.kind==='train'&&job.state==='completed'){
        message.textContent=latestApplied?('訓練完成'+trained+'，新模型已套用。'):('訓練完成'+trained+'，請套用新模型。');
      }else{
        message.textContent=(jobLabels[job.kind]||job.kind)+'：'+({completed:'完成',failed:'失敗',cancelled:'已取消'}[job.state]||job.state);
      }
    }
  } catch(error){showNotice(error.message,true);}
}
async function act(path, body){try{await api(path,body);await refresh();}catch(error){showNotice(error.message,true);}}
document.getElementById('fetch').onclick=()=>act('fetch',{});
document.getElementById('install-trainer').onclick=()=>act('install-trainer',{});
document.getElementById('check-trainer').onclick=()=>act('check-trainer',{});
document.getElementById('history-base').onclick=()=>{
  if(selectedHistoryId===null)return;
  const value=String(selectedHistoryId);
  const select=document.getElementById('base-run');
  if(![...select.options].some(option=>option.value===value))return;
  select.value=value;
  applyBaseParameters();
  const selected=historyRuns.find(run=>String(run.id)===value);
  showNotice(selected?('已將訓練 #'+selected.id+' 設為訓練基底。'):'已將 Base model 設為訓練基底。');
};
document.getElementById('history-apply').onclick=async()=>{
  if(selectedHistoryId===null)return;
  try{
    const id=String(selectedHistoryId);
    await api('use-model',{id:id==='0'?'base':id});
    await refresh();
    showNotice(id==='0'?'已套用 Base model（改用安裝的預設模型）。':'新模型已套用。');
  }catch(error){showNotice(error.message,true);}
};
document.getElementById('history-tarjan').onclick=()=>{
  if(historyCommonAncestor===null||historyCommonAncestor===undefined)return;
  selectedHistoryId=String(historyCommonAncestor);
  renderHistory();
  showNotice('已選擇最新兩次訓練的共同祖先 #'+historyCommonAncestor+'。');
};
installHistoryControls();
document.getElementById('check').onclick=()=>act('check',{});
document.getElementById('only-manual-records').onchange=()=>{recordOffset=0;refresh();};
document.getElementById('train').onclick=async()=>{
  try{
    const counts=await api('pending-count');
    pendingCount=counts.count;pendingManual=counts.manual;
    const manualOnly=manualOnlyTraining();
    const count=effectivePendingCount();
    if(!count)throw Error(manualOnly?'目前沒有曾手動選字的資料':'目前沒有尚未訓練的資料');
    let password='';
    if(protectionInfo.configured){
      // Training asks for its own password; it never reuses the review unlock.
      password=document.getElementById('train-password').value;
      if(!password)throw Error('請先輸入訓練密碼');
    }
    const options=Object.fromEntries(fields.map(([name])=>[name,document.getElementById(name).value]));
    options.device=document.getElementById('device').value;
    options.dtype=document.getElementById('dtype').value;
    options.shuffle=document.getElementById('shuffle').checked?'1':'0';
    // The confirmation shows the records that will actually be trained; the
    // dataset builder skips records it cannot convert.
    const counted=await api('count-trainable',{only_manually_selected:manualOnly,
      max_seq_length:options['max-seq-length'],password});
    if(!counted.records)throw Error('所選資料都無法轉換，請檢查資料或字表。');
    exactCount=counted;exactCountKey=manualOnly+'/'+options['max-seq-length'];
    updateEstimate();renderRecords(recordRows);
    if(!confirm(`以 ${counted.records} 筆資料開始訓練？`))return;
    await act('train',{strength:strengthSelect.value,only_manually_selected:manualOnly,
      stabilize_intruders:document.getElementById('stabilize-intruders').checked,
      base_run_id:document.getElementById('base-run').value,options,password});
    document.getElementById('train-password').value='';
  }catch(error){showNotice(error.message,true);}
};
document.getElementById('setup-cancel').onclick=()=>{document.getElementById('password-setup').hidden=true;};
document.getElementById('setup-confirm').onclick=async()=>{
  const password=document.getElementById('setup-password').value;
  const confirmation=document.getElementById('setup-confirmation').value;
  try{
    await api('protection',{action:'set-password',password,confirmation});
    document.getElementById('setup-password').value='';document.getElementById('setup-confirmation').value='';
    document.getElementById('password-setup').hidden=true;
    await refresh();
  }catch(error){showNotice(error.message,true);}
};
document.getElementById('cancel').onclick=()=>act('cancel',{});
document.getElementById('reload-records').onclick=()=>{recordOffset=0;refresh();};
document.getElementById('previous').onclick=()=>{recordOffset=Math.max(0,recordOffset-PAGE_SIZE);refresh();};
document.getElementById('next').onclick=()=>{recordOffset+=PAGE_SIZE;refresh();};
if (!token) {showNotice('請從輸入法選單重新開啟管理頁面。',true);}
else {
  // The character table decides the readings shown for each character.
  api('readings').then(table=>{readingsTable=table||{};}).catch(()=>{}).then(()=>{refresh();setInterval(refresh,3000);});
}
</script></html>)LLAVON";

constexpr std::string_view logo_data_uri =
    "data:image/png;base64,"
    "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";


std::string render_page() {
    std::string html(page);
    constexpr std::string_view placeholder = "@@LOGO@@";
    for (auto position = html.find(placeholder); position != std::string::npos;
         position = html.find(placeholder, position + logo_data_uri.size())) {
        html.replace(position, placeholder.size(), logo_data_uri);
    }
    return html;
}

#endif

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

#ifndef LLAVON_NATIVE_GUI
std::string random_token() {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string token;
    for (int i = 0; i < 32; ++i) { const auto byte = static_cast<unsigned char>(random()); token += hex[byte >> 4]; token += hex[byte & 15]; }
    return token;
}

void send_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const auto count = ::send(fd, data.data(), data.size(), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return;
        data.remove_prefix(static_cast<std::size_t>(count));
    }
}

// Two builds of the manager must not serve the same session: a stale process
// would keep showing an outdated interface after an update. The lock file
// records the owning PID and this stamp, and a newer build takes over an idle
// instance (a running job is left alone).
#endif
constexpr std::string_view build_stamp = "native-manager-0.2.0";

#ifndef LLAVON_NATIVE_GUI
std::string http_get_state(const std::string& url) {
    const auto scheme_end = url.find("://");
    const auto host_start = scheme_end == std::string::npos ? std::string::npos : scheme_end + 3;
    if (host_start == std::string::npos) return {};
    const auto slash = url.find('/', host_start);
    const auto authority = url.substr(host_start, slash == std::string::npos ? std::string::npos : slash - host_start);
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos) return {};
    int port = 0;
    const auto digits = authority.substr(colon + 1);
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size() || port <= 0 || port > 65535) return {};
    const auto hash = url.find('#');
    const std::string token = hash == std::string::npos ? std::string{} : url.substr(hash + 1);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};
    timeval timeout{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { ::close(fd); return {}; }
    send_all(fd, "GET /api/state HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                 "\r\nX-Llavon-Token: " + token + "\r\nConnection: close\r\n\r\n");
    std::string response;
    std::array<char, 4096> buffer{};
    while (response.size() < 65536) {
        const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) break;
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(fd);
    const auto body = response.find("\r\n\r\n");
    return body == std::string::npos ? std::string{} : response.substr(body + 4);
}

#endif
struct Request {
    std::string method, path, origin, host, token, content_type, body;
};

#ifndef LLAVON_NATIVE_GUI
Request read_request(int fd) {
    Request request;
    std::string raw;
    std::array<char, 4096> buffer{};
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) throw std::runtime_error("incomplete HTTP headers");
        raw.append(buffer.data(), static_cast<std::size_t>(count));
        if (raw.size() > 65536) throw std::runtime_error("HTTP request is too large");
    }
    const auto header_end = raw.find("\r\n\r\n");
    const auto first_line = raw.find("\r\n");
    const auto first_space = raw.find(' ');
    const auto second_space = raw.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos || second_space > first_line ||
        raw.substr(second_space + 1, first_line - second_space - 1) != "HTTP/1.1")
        throw std::runtime_error("invalid HTTP request");
    request.method = raw.substr(0, first_space);
    request.path = raw.substr(first_space + 1, second_space - first_space - 1);
    std::size_t length = 0;
    for (auto pos = first_line + 2; pos < header_end;) {
        const auto end = raw.find("\r\n", pos);
        if (end == std::string::npos || end > header_end) throw std::runtime_error("invalid HTTP header");
        const auto line = raw.substr(pos, end - pos);
        const auto colon = line.find(':');
        if (colon == std::string::npos) throw std::runtime_error("invalid HTTP header");
        std::string key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch){ return static_cast<char>(std::tolower(ch)); });
        const auto value = trim(line.substr(colon + 1));
        if (key == "host") request.host = value;
        else if (key == "origin") request.origin = value;
        else if (key == "x-llavon-token") request.token = value;
        else if (key == "content-type") request.content_type = value;
        else if (key == "content-length") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || length > 16384)
                throw std::runtime_error("invalid Content-Length");
        } else if (key == "transfer-encoding") throw std::runtime_error("chunked requests are not supported");
        pos = end + 2;
    }
    request.body = raw.substr(header_end + 4);
    if (request.body.size() > length) throw std::runtime_error("unexpected HTTP body");
    while (request.body.size() < length) {
        const auto count = ::recv(fd, buffer.data(), std::min(buffer.size(), length - request.body.size()), 0);
        if (count <= 0) throw std::runtime_error("incomplete HTTP body");
        request.body.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return request;
}

void respond(int fd, int status, std::string_view type, std::string_view body) {
    const std::string header = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Error") +
        "\r\nContent-Type: " + std::string(type) + "; charset=utf-8\r\nCache-Control: no-store\r\n" +
        "Referrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'none'; "
        "script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' data:\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    send_all(fd, header); send_all(fd, body);
}

#endif

fs::path executable_path(const char* argv0) {
#ifdef __APPLE__
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) return fs::weakly_canonical(buffer.c_str());
#else
    std::error_code error;
    const auto path = fs::read_symlink("/proc/self/exe", error);
    if (!error) return path;
#endif
    return fs::absolute(argv0);
}

fs::path default_state() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
        return fs::path(state) / "llavon-ime" / "training";
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("HOME is not set");
#ifdef __APPLE__
    return fs::path(home) / "Library" / "Application Support" / "llavon-ime" / "training";
#else
    return fs::path(home) / ".local" / "state" / "llavon-ime" / "training";
#endif
}

#ifndef LLAVON_NATIVE_GUI
void open_browser(const std::string& url, bool enabled) {
    if (!enabled) return;
    const pid_t child = ::fork();
    if (child == 0) {
        // xdg-open may wait for the browser. Do not block the HTTP server or
        // leave an unreaped browser child in the manager process.
        const pid_t launcher = ::fork();
        if (launcher < 0) _exit(127);
        if (launcher > 0) _exit(0);
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) { ::dup2(devnull, STDOUT_FILENO); ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
#ifdef __APPLE__
        ::execl("/usr/bin/open", "open", url.c_str(), static_cast<char*>(nullptr));
#else
        ::execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
#endif
        _exit(127);
    }
    if (child > 0) {
        int status;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            std::cerr << "Could not open a browser; open " << url << '\n';
    }
}

#endif

std::string last_log(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return {};
    const auto size = stream.tellg();
    if (size > 12000) stream.seekg(-12000, std::ios::end);
    else stream.seekg(0);
    std::string data((std::istreambuf_iterator<char>(stream)), {});
    if (size > 12000) { const auto line = data.find('\n'); if (line != std::string::npos) data.erase(0, line + 1); }
    return data;
}

using ime::unix_service::manager::configured_model_path;
using ime::unix_service::manager::use_model;
using ime::unix_service::manager::use_base_model;
using ime::unix_service::manager::trainer_path;
using ime::unix_service::manager::trainer_ready;

using ime::unix_service::manager::query_database;

struct Options {
    fs::path state, db, cli, tables, trainer;
    // Kept so a reinstalled manager can restart itself with the same command
    // line and keep serving the open page (see Gui::reexec).
    fs::path self;
    std::vector<std::string> arguments;
    bool browser = true;
    int idle_seconds = 120;
};

using ime::unix_service::manager::DatabaseHandle;

// Windows exposes the same override for its dev/test asset root.
fs::path assets_root(const Options& options) {
    if (const char* override = std::getenv("LLAVON_IME_LORA_ASSETS_DIR"); override && *override)
        return fs::absolute(override);
    return options.state / "assets";
}

fs::path runs_root(const Options& options) {
    if (const char* override = std::getenv("LLAVON_IME_LORA_ASSETS_DIR"); override && *override)
        return fs::absolute(override) / "runs";
    return options.state / "runs";
}

Options parse_options(int argc, char** argv) {
    Options options;
    options.state = default_state();
    if (const char* db = std::getenv("LLAVON_IME_TRAINING_DATABASE_PATH"); db && *db) options.db = db;
    const auto binary = executable_path(argv[0]);
    options.cli = binary.parent_path() / "llavon-ime-lora";
    options.tables = binary.parent_path().parent_path() / "share" / "llavon-ime" / "tables";
    if (!fs::exists(options.tables)) options.tables = LLAVON_IME_GUI_INSTALLED_TABLES_DIR;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (name == "--no-browser") { options.browser = false; continue; }
        if (i + 1 >= argc) throw std::invalid_argument("missing value for " + name);
        const std::string value = argv[++i];
        if (name == "--state-dir") options.state = value;
        else if (name == "--db") options.db = value;
        else if (name == "--cli") options.cli = value;
        else if (name == "--tables-dir") options.tables = value;
        else if (name == "--idle-seconds") {
            // Keep the historical stoi prefix grammar, including leading '+'.
            const auto seconds = llavon::ime::parse_decimal<int>(value, false);
            if (!seconds) throw std::invalid_argument("invalid idle timeout");
            options.idle_seconds = *seconds;
        }
        else throw std::invalid_argument("unknown option: " + name);
    }
    if (options.idle_seconds < 1) throw std::invalid_argument("idle timeout must be at least 1 second");
    options.state = fs::absolute(options.state);
    options.trainer = trainer_path(options.state);
    if (!options.db.empty()) options.db = fs::absolute(options.db);
    options.cli = fs::absolute(options.cli);
    options.tables = fs::absolute(options.tables);
    options.trainer = fs::absolute(options.trainer);
    options.self = binary;
    for (int i = 1; i < argc; ++i) options.arguments.emplace_back(argv[i]);
    return options;
}

struct Job {
    std::string kind, state = "idle";
    fs::path log, output;
};

class Gui {
public:
    explicit Gui(Options options) : options_(std::move(options)),
        db_(options_.db.empty() ? options_.state / "commits.sqlite3" : options_.db) {}

#ifdef LLAVON_NATIVE_GUI
    // A single-threaded helper owns the existing database and fork/exec logic.
    // The Widgets process speaks JSON lines over private anonymous pipes: no
    // TCP listener, browser, token file, or plaintext password in argv.
    int run() {
        fs::create_directories(options_.state);
        struct stat directory {};
        if (::lstat(options_.state.c_str(), &directory) || !S_ISDIR(directory.st_mode) ||
            directory.st_uid != ::getuid() || ::chmod(options_.state.c_str(), 0700))
            throw std::runtime_error("unsafe training data directory");
        lock_ = ::open((options_.state / "gui.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
        struct stat file {};
        if (lock_ < 0 || ::fstat(lock_, &file) || !S_ISREG(file.st_mode) || file.st_uid != ::getuid() ||
            ::fchmod(lock_, 0600) || ::fcntl(lock_, F_SETFD, FD_CLOEXEC) || ::flock(lock_, LOCK_EX | LOCK_NB))
            throw std::runtime_error("已有個人化管理器開啟，請先關閉舊版管理器");
        std::string pending;
        while (!stop_requested) {
            update_job();
            pollfd descriptor{STDIN_FILENO, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 200);
            if (ready < 0) { if (errno == EINTR) continue; break; }
            if (!ready) continue;
            std::array<char, 4096> buffer{};
            const auto count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
            if (count <= 0) break;
            pending.append(buffer.data(), static_cast<std::size_t>(count));
            if (pending.size() > 65536) throw std::runtime_error("管理器請求過長");
            for (auto newline = pending.find('\n'); newline != std::string::npos; newline = pending.find('\n')) {
                std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                json response{{"id", nullptr}};
                try {
                    auto input = json::parse(line);
                    response["id"] = input.at("id");
                    Request request;
                    request.method = input.value("method", "GET");
                    request.path = input.at("path").get<std::string>();
                    request.content_type = "application/json";
                    request.body = input.value("body", json::object()).dump();
                    response["result"] = dispatch(request);
                    sodium_memzero(request.body.data(), request.body.size());
                } catch (const std::exception& error) { response["error"] = error.what(); }
                sodium_memzero(line.data(), line.size());
                std::cout << response.dump() << std::endl;
            }
        }
        process_.terminate();
        ::close(lock_);
        return 0;
    }
#else
    int run() {
        fs::create_directories(options_.state);
        struct stat directory {};
        if (::lstat(options_.state.c_str(), &directory) || !S_ISDIR(directory.st_mode) || directory.st_uid != ::getuid() ||
            ::chmod(options_.state.c_str(), 0700)) throw std::runtime_error("unsafe training data directory");
        // A reinstalled build re-executes this manager (see reexec) and hands
        // the listening socket and page token over, so an open page keeps its
        // address and only has to reload when it sees the new build.
        const bool inherited = ::getenv("LLAVON_IME_GUI_INHERIT_FD") != nullptr;
        if (inherited) {
            try {
                const auto inherited_fd = llavon::ime::parse_decimal<int>(::getenv("LLAVON_IME_GUI_INHERIT_FD"), false);
                if (!inherited_fd) throw std::invalid_argument("invalid inherited descriptor");
                listen_ = *inherited_fd;
            } catch (...) { throw std::runtime_error("invalid inherited GUI socket"); }
            const char* token = ::getenv("LLAVON_IME_GUI_INHERIT_TOKEN");
            if (token && *token) token_ = token;
            ::unsetenv("LLAVON_IME_GUI_INHERIT_FD");
            ::unsetenv("LLAVON_IME_GUI_INHERIT_TOKEN");
        }
        {
            std::error_code error;
            self_mtime_ = fs::last_write_time(options_.self, error);
            watch_self_ = !error;
        }
        const auto lock_path = options_.state / "gui.lock";
        lock_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
        struct stat file {};
        if (lock_ < 0 || ::fstat(lock_, &file) || !S_ISREG(file.st_mode) || file.st_uid != ::getuid() || ::fchmod(lock_, 0600))
            throw std::runtime_error("unsafe GUI lock file");
        // Browser launchers and the CLI must not retain the singleton lock or
        // listening socket across exec (especially when a job outlives a tab).
        if (::fcntl(lock_, F_SETFD, FD_CLOEXEC)) throw std::runtime_error("cannot protect GUI lock descriptor");
        if (::flock(lock_, LOCK_EX | LOCK_NB)) {
            if (errno != EWOULDBLOCK) throw std::runtime_error("cannot lock GUI session");
            std::string url, stamp;
            pid_t owner = 0;
            {
                std::array<char, 512> info{};
                const auto size = ::pread(lock_, info.data(), info.size(), 0);
                if (size > 0) {
                    std::istringstream stream(std::string(info.data(), static_cast<std::size_t>(size)));
                    std::string pid_line, stamp_line;
                    std::getline(stream, url);
                    std::getline(stream, pid_line);
                    std::getline(stream, stamp_line);
                    url = trim(url); stamp = trim(stamp_line);
                    const auto pid_text = trim(pid_line);
                    if (!pid_text.empty()) {
                        owner = static_cast<pid_t>(llavon::ime::parse_decimal<long>(pid_text, false).value_or(0));
                    }
                }
            }
            bool acquired = false;
            // The inherited listener means this process re-executed itself;
            // the lock file still names this PID, so never signal it.
            if (owner > 0 && owner != ::getpid() && !stamp.empty() && stamp != std::string(build_stamp)) {
                bool busy = false;
                try {
                    const auto state = json::parse(http_get_state(url));
                    busy = state.value("job", json::object()).value("state", std::string{}) == "running";
                } catch (...) { busy = false; }
                if (!busy) {
                    (void)::kill(owner, SIGTERM);
                    for (int attempt = 0; attempt < 50; ++attempt) {
                        ::usleep(100000);
                        if (::flock(lock_, LOCK_EX | LOCK_NB) == 0) { acquired = true; break; }
                    }
                }
            }
            if (!acquired) {
                if (!url.starts_with("http://127.0.0.1:")) throw std::runtime_error("existing GUI is starting; retry");
                if (!options_.browser) std::cout << "URL=" << url << std::endl;
                open_browser(url, options_.browser);
                return 0;
            }
        }
        if (listen_ < 0) {
            listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_ < 0) throw std::runtime_error("cannot create GUI socket");
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = 0;
            if (::bind(listen_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(listen_, 8))
                throw std::runtime_error("cannot listen on loopback");
        }
        if (::fcntl(listen_, F_SETFD, FD_CLOEXEC)) throw std::runtime_error("cannot protect GUI socket descriptor");
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        if (::getsockname(listen_, reinterpret_cast<sockaddr*>(&address), &length))
            throw std::runtime_error("cannot find GUI port");
        port_ = ntohs(address.sin_port);
        if (token_.empty()) token_ = random_token();
        const std::string url = "http://127.0.0.1:" + std::to_string(port_) + "/#" + token_;
        const std::string lock_contents = url + "\n" + std::to_string(::getpid()) + "\n" + std::string(build_stamp) + "\n";
        if (::ftruncate(lock_, 0) ||
            ::pwrite(lock_, lock_contents.data(), lock_contents.size(), 0) != static_cast<ssize_t>(lock_contents.size()))
            throw std::runtime_error("cannot store GUI address");
        if (!options_.browser) std::cout << "URL=" << url << std::endl;
        open_browser(url, options_.browser);
        last_seen_ = Clock::now();
        while (!stop_requested) {
            update_job();
            if (!process_.running() && watch_self_) {
                // Reinstalling the manager must take effect without closing the
                // page: replace this process with the new binary when the file
                // changed on disk. A running job is never interrupted, and a
                // failed handover keeps the current build serving.
                std::error_code self_error;
                const auto mtime = fs::last_write_time(options_.self, self_error);
                if (!self_error && mtime != self_mtime_) {
                    if (!try_reexec()) {
                        self_mtime_ = mtime;
                        ++restart_failures_;
                        if (restart_failures_ >= 5) watch_self_ = false;
                    }
                }
            }
            if (!process_.running() && Clock::now() - last_seen_ > std::chrono::seconds(options_.idle_seconds)) break;
            pollfd descriptor{listen_, POLLIN, 0};
            const auto ready = ::poll(&descriptor, 1, 500);
            if (ready < 0 && errno != EINTR) throw std::runtime_error("GUI socket poll failed");
            if (ready <= 0 || !(descriptor.revents & POLLIN)) continue;
            const int client = ::accept(listen_, nullptr, nullptr);
            if (client < 0) continue;
            if (::fcntl(client, F_SETFD, FD_CLOEXEC)) { ::close(client); continue; }
            timeval timeout{2, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            try { handle(client); }
            catch (const std::exception& error) { respond(client, 400, "application/json", json{{"error", error.what()}}.dump()); }
            ::close(client);
        }
        process_.terminate();
        return 0;
    }
#endif

private:
#ifndef LLAVON_NATIVE_GUI
    // Hands the listening socket and page token to the reinstalled binary and
    // replaces this process image with it. Returns false when the replacement
    // could not start, so the caller keeps the current build serving.
    bool try_reexec() {
        if (::fcntl(listen_, F_SETFD, 0) != 0) return warn_reexec("cannot clear the socket close-on-exec flag");
        if (::setenv("LLAVON_IME_GUI_INHERIT_FD", std::to_string(listen_).c_str(), 1) != 0 ||
            ::setenv("LLAVON_IME_GUI_INHERIT_TOKEN", token_.c_str(), 1) != 0)
            return warn_reexec("cannot prepare the handover environment");
        std::string program = options_.self.string();
        std::vector<char*> argv;
        argv.reserve(options_.arguments.size() + 2);
        argv.push_back(program.data());
        for (auto& argument : options_.arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        ::execv(program.c_str(), argv.data());
        return warn_reexec(std::strerror(errno));
    }

    bool warn_reexec(const char* reason) {
        ::unsetenv("LLAVON_IME_GUI_INHERIT_FD");
        ::unsetenv("LLAVON_IME_GUI_INHERIT_TOKEN");
        (void)::fcntl(listen_, F_SETFD, FD_CLOEXEC);
        std::cerr << "LoRA manager: cannot restart with the reinstalled binary (" << reason
                  << "); keeping this build" << '\n';
        return false;
    }

#endif
    void update_job() {
        const auto completed = process_.poll();
        if (!completed) return;
        job_.state = process_.cancelling() ? "cancelled" : (*completed ? "completed" : "failed");
        if (job_.kind == "install" && job_.state == "completed" && !std::getenv("LLAVON_IME_LORA_CLI_PATH"))
            options_.trainer = options_.state / "tools" / "lora" / "llavon-lora";
        // A re-export requested by an apply is applied as soon as it succeeds.
        if (job_.kind == "export" && job_.state == "completed" && pending_export_apply_) {
            const auto model = *pending_export_apply_;
            pending_export_apply_.reset();
            try {
                use_model(model);
                prune_obsolete_models(model);
            } catch (const std::exception& error) {
                std::cerr << "LoRA manager: cannot apply the re-exported model: " << error.what() << '\n';
            }
        }
    }

    // The training password never reaches argv or the environment: it travels
    // through a private pipe that the CLI reads from descriptor 3.
    void start_job(const std::string& kind, std::vector<std::string> args, fs::path output,
                   const std::string& password = {}) {
        update_job();
        if (process_.running()) throw std::runtime_error("已有工作進行中");
        job_ = Job{.kind = kind, .state = "running", .log = options_.state / "gui-job.log", .output = std::move(output)};
        try {
            process_.launch(options_.cli, args, job_.log, password,
                kind == "train" || kind == "export" ? std::optional<fs::path>(options_.trainer) : std::nullopt);
        } catch (...) { job_.state = "failed"; throw; }
    }

    // Runs the manager CLI synchronously and returns its captured output; the
    // trainable-record count behind the confirmation dialog uses this.
    std::string run_cli_capture(const std::vector<std::string>& args, const std::string& password) {
        const auto output = options_.state / "gui-count.out";
        ime::unix_service::JobProcess capture;
        capture.launch(options_.cli, args, output, password);
        std::optional<bool> completed;
        const auto deadline = Clock::now() + std::chrono::minutes(2);
        while (!(completed = capture.poll()) && Clock::now() < deadline) ::usleep(10000);
        if (!completed) { capture.terminate(); throw std::runtime_error("count timed out"); }
        const auto text = last_log(output);
        std::error_code ignored;
        fs::remove(output, ignored);
        if (!*completed)
            throw std::runtime_error(text.empty() ? std::string("count failed") : trim(text));
        return text;
    }

    // Encrypted collection: the manager never returns typed text without a
    // password, and it forgets the derived key as soon as it is locked.
    json protection() const {
        json status{{"configured", false}, {"enabled", false}, {"unlocked", false}};
        if (fs::is_regular_file(db_)) {
            DatabaseHandle handle(db_);
            const auto stored = ime::unix_service::read_commit_protection(handle.get());
            status["configured"] = stored.configured;
            status["enabled"] = stored.enabled;
        }
        status["unlocked"] = cipher_.has_value() && cipher_->unlocked();
        return status;
    }

    void unlock_review(const std::string& password) {
        if (password.empty()) throw std::runtime_error("請輸入密碼");
        DatabaseHandle handle(db_);
        if (!ime::unix_service::read_commit_protection(handle.get()).configured)
            throw std::runtime_error("尚未設定密碼");
        cipher_.emplace();
        try { cipher_->unlock(handle.get(), password); }
        catch (...) { cipher_.reset(); throw; }
        discard_plaintext_datasets();
    }

    void lock_review() { cipher_.reset(); }

    void configure_password(const std::string& password, const std::string& confirmation) {
        if (password.empty()) throw std::runtime_error("密碼不可為空");
        if (password != confirmation) throw std::runtime_error("兩次輸入的密碼不同");
        ime::unix_service::CommitStore store(db_);
        store.configure_password(password);
        discard_plaintext_datasets();
    }

    void set_recording(bool enabled) {
        ime::unix_service::CommitStore store(db_);
        store.set_recording_enabled(enabled);
        if (!enabled) cipher_.reset();
    }

    // Keeps the LoRA models and the training history; only the conversation
    // records and the derived keys go away.
    void forget_conversation_data() {
        if (process_.running()) throw std::runtime_error("請先等待目前工作結束或取消");
        ime::unix_service::CommitStore store(db_);
        store.reset_conversation_data();
        cipher_.reset();
        discard_plaintext_datasets();
    }

    // A training run only needs its readable dataset while it runs, and a
    // killed process can leave one behind. Only application-owned filenames
    // inside a real run directory are removed; symlinks are never followed.
    void discard_plaintext_datasets() const {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(runs_root(options_), error)) {
            if (entry.is_symlink() || !entry.is_directory()) continue;
            std::error_code ignored;
            fs::remove(entry.path() / "training.jsonl", ignored);
            fs::remove(entry.path() / "training.jsonl.partial", ignored);
        }
    }

    // Windows keeps only the model that is applied: every other run's
    // quantized GGUF is removed, while each run's adapter stays so the model
    // can be exported again on demand. An empty keep path (the base model is
    // applied) removes all run models.
    void prune_obsolete_models(const fs::path& keep) const {
        std::error_code error;
        const auto rows = query_database(db_, "SELECT model_path FROM lora_runs ORDER BY id DESC", 1);
        if (rows.empty()) return;
        const auto root = fs::absolute(runs_root(options_), error).lexically_normal();
        if (error) return;
        std::optional<fs::path> kept;
        if (!keep.empty()) {
            kept = fs::absolute(keep, error).lexically_normal();
            if (error) return;
        }
        for (const auto& row : rows) {
            const auto candidate = fs::absolute(row[0].get<std::string>(), error).lexically_normal();
            if (error) { error.clear(); continue; }
            if (candidate.filename() != "personalized-Q4_K_M.gguf" ||
                candidate.parent_path().parent_path() != root) continue;
            if (kept && candidate == *kept) continue;
            std::error_code ignored;
            fs::remove(candidate, ignored);
        }
    }

    json records(const std::string& requested) const {
        const auto question = requested.find('?');
        const auto params = question == std::string::npos ? "" : requested.substr(question + 1);
        std::string state = "pending";
        int offset = 0;
        bool manual_only = false;
        for (std::size_t start = 0; start < params.size();) {
            const auto end = params.find('&', start);
            const auto part = params.substr(start, end == std::string::npos ? end : end - start);
            if (part.starts_with("state=")) state = part.substr(6);
            else if (part == "manual=1") manual_only = true;
            else if (part.starts_with("offset=")) {
                const auto digits = part.substr(7);
                const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), offset);
                if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size() || offset < 0 || offset > 100000)
                    throw std::runtime_error("invalid record offset");
            } else if (!part.empty()) throw std::runtime_error("invalid record filter");
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (state != "pending" && state != "excluded" && state != "trained")
            throw std::runtime_error("invalid record state");
        const std::string manual_filter = manual_only
            ? " AND EXISTS(SELECT 1 FROM readings r WHERE r.commit_id=commits.id AND r.manually_selected=1)"
            : "";
        const auto counted = query_database(db_,
            ("SELECT COUNT(*) FROM commits WHERE state='" + state + "'" + manual_filter).c_str(), 1);
        const int total = counted.empty() ? 0 : std::stoi(counted[0][0].get<std::string>());
        if (!fs::is_regular_file(db_)) return {{"rows",json::array()}, {"has_more",false}, {"total",total}};
        const bool unlocked = cipher_.has_value() && cipher_->unlocked();
        DatabaseHandle handle(db_);
        const bool configured = ime::unix_service::read_commit_protection(handle.get()).configured;
        if (configured && !unlocked) {
            // Sealed records: the page learns that they exist, never what they
            // say. Selection only needs the IDs, so reviewing stays possible.
            const auto rows = query_database(db_,
                ("SELECT id,committed_at FROM commits WHERE state='" + state + "'" + manual_filter + " "
                 "ORDER BY committed_at DESC,id DESC LIMIT " + std::to_string(kRecordsPerPage + 1) + " OFFSET " +
                 std::to_string(offset)).c_str(), 2);
            json locked_rows = json::array();
            for (const auto& row : rows)
                locked_rows.push_back({{"id",row[0]}, {"committed_at",row[1]}, {"context",""}, {"answer",""},
                                        {"readings",json::array()}, {"manual",json::array()}, {"text",false}});
            const bool sealed_more = locked_rows.size() > kRecordsPerPage;
            if (sealed_more) locked_rows.erase(locked_rows.end() - 1);
            return {{"rows",locked_rows}, {"has_more",sealed_more}, {"total",total}, {"locked",true}};
        }
        ime::unix_service::CommitCipher locked;
        const auto& cipher = unlocked ? *cipher_ : locked;
        json entries = json::array();
        for (const auto& item : ime::unix_service::read_commits(handle.get(), state, cipher, offset, kRecordsPerPage + 1,
                                                               manual_only)) {
            json readings = json::array(), manual = json::array();
            for (const auto& reading : item.readings) readings.push_back(reading);
            for (const bool value : item.manual) manual.push_back(value);
            entries.push_back({{"id",item.id}, {"committed_at",item.committed_at}, {"context",item.context},
                               {"answer",item.answer}, {"readings",readings}, {"manual",manual}, {"text",true}});
        }
        const bool has_more = entries.size() > kRecordsPerPage;
        if (has_more) entries.erase(entries.end() - 1);
        return {{"rows",entries}, {"has_more",has_more}, {"total",total}};
    }

    json run_entries() const {
        const auto columns = query_database(db_, "PRAGMA table_info(lora_runs)", 3);
        const bool extended = std::any_of(columns.begin(), columns.end(), [](const json& col){return col[1] == "rank";});
        const bool requested = std::any_of(columns.begin(), columns.end(),
                                           [](const json& col){return col[1] == "training_request_json";});
        const auto rows = query_database(db_, requested ?
            "SELECT completed_at,record_count,model_path,optimizer_steps,rank,alpha,dropout,target_modules,id,"
            "cumulative_record_count,COALESCE(parent_id,0),training_request_json FROM lora_runs ORDER BY id DESC LIMIT 20" :
            extended ?
            "SELECT completed_at,record_count,model_path,optimizer_steps,rank,alpha,dropout,target_modules,id,"
            "cumulative_record_count,COALESCE(parent_id,0) FROM lora_runs ORDER BY id DESC LIMIT 20" :
            "SELECT completed_at,record_count,model_path,id,SUM(record_count) OVER (ORDER BY id) "
            "FROM lora_runs ORDER BY id DESC LIMIT 20", requested ? 12 : (extended ? 11 : 5));
        json entries = json::array();
        for (const auto& row : rows) {
            json entry{{"completed_at",row[0]}, {"record_count",std::stoi(row[1].get<std::string>())},
                       {"model_path",row[2]}, {"cumulative_count",std::stoi(row[extended ? 9 : 4].get<std::string>())},
                       {"optimizer_steps",extended ? row[3] : json("0")},
                       {"rank",extended ? row[4] : json("8")}, {"alpha",extended ? row[5] : json("16")},
                       {"dropout",extended ? row[6] : json("0")},
                       {"target_modules",extended ? row[7] : json("q_proj,v_proj")},
                       {"id",extended ? row[8] : row[3]},
                       {"parent_id",extended ? std::stoll(row[10].get<std::string>()) : 0LL}};
            // The full request of a run decides the labels and details the
            // history shows; older runs simply carry none.
            if (requested && row[11].is_string()) {
                try {
                    const auto request = json::parse(row[11].get<std::string>());
                    entry["request"] = request;
                    entry["strength"] = request.value("strength", "");
                    entry["only_manually_selected"] = request.value("only_manually_selected", false);
                    if (request.contains("stabilize_intruders"))
                        entry["stabilize_intruders"] = request.value("stabilize_intruders", false);
                } catch (...) {}
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    json runs() const { return run_entries(); }

    // The history view adds the Tarjan shortcut of the Windows manager: the
    // common ancestor of the two newest runs when it is a third run.
    json history() const {
        json entries = run_entries();
        json common = nullptr;
        if (entries.size() >= 2) {
            const auto rows = query_database(db_, "SELECT id,COALESCE(parent_id,0) FROM lora_runs "
                                                  "ORDER BY id DESC LIMIT 20", 2);
            std::vector<ime::unix_service::LoraHistoryParent> lineage;
            lineage.reserve(rows.size());
            for (const auto& row : rows)
                lineage.push_back({std::stoll(row[0].get<std::string>()),
                                   std::stoll(row[1].get<std::string>())});
            if (lineage.size() >= 2) {
                const auto ancestor = ime::unix_service::tarjan_lca(lineage, lineage[0].id, lineage[1].id);
                if (ancestor && *ancestor != lineage[0].id && *ancestor != lineage[1].id) common = *ancestor;
            }
        }
        return {{"runs", std::move(entries)}, {"common_ancestor", common}};
    }

    // Counts per review category: pending (with its manually selected subset),
    // trained and excluded. The page shows one category at a time.
    json pending_count() const {
        json result{{"count", 0LL}, {"manual", 0LL}, {"trained", 0LL}, {"excluded", 0LL}};
        const auto rows = query_database(db_,
            "SELECT COUNT(*),COALESCE(SUM(EXISTS(SELECT 1 FROM readings r WHERE r.commit_id=commits.id "
            "AND r.manually_selected=1)),0) FROM commits WHERE state='pending'", 2);
        if (!rows.empty()) {
            result["count"] = std::stoll(rows[0][0].get<std::string>());
            result["manual"] = std::stoll(rows[0][1].get<std::string>());
        }
        for (const auto& row : query_database(db_, "SELECT state,COUNT(*) FROM commits GROUP BY state", 2)) {
            const auto state = row[0].get<std::string>();
            if (state == "trained") result["trained"] = std::stoll(row[1].get<std::string>());
            else if (state == "excluded") result["excluded"] = std::stoll(row[1].get<std::string>());
        }
        return result;
    }

    // The IME candidate table is reading -> characters; invert it once so the
    // page can look up every reading a character may have.
    json readings_table() {
        if (!readings_loaded_) {
            readings_loaded_ = true;
            try {
                const auto table = json::parse(std::ifstream(options_.tables / "bopomofo_char.json"));
                json inverse = json::object();
                for (auto entry = table.begin(); entry != table.end(); ++entry) {
                    if (!entry.value().is_array()) continue;
                    for (const auto& candidate : entry.value()) {
                        if (!candidate.is_string()) continue;
                        auto& readings = inverse[candidate.get<std::string>()];
                        if (!readings.is_array()) readings = json::array();
                        if (std::find(readings.begin(), readings.end(), entry.key()) == readings.end())
                            readings.push_back(entry.key());
                    }
                }
                readings_cache_ = std::move(inverse);
            } catch (...) { readings_cache_ = json::object(); }
        }
        return readings_cache_;
    }

    json state() {
        update_job();
        const auto data = last_log(job_.log);
        std::string progress;
        json percent = nullptr;
        json job_records = nullptr;
        if (job_.kind == "train") {
            const auto step = data.rfind("step=");
            if (step != std::string::npos) {
                progress = trim(data.substr(step, std::min<std::size_t>(50, data.size() - step)));
                int current = 0, total = 0;
                if (std::sscanf(data.c_str() + step, "step=%d/%d", &current, &total) == 2 && total > 0)
                    percent = 5 + 80 * std::clamp(static_cast<double>(current) / total, 0.0, 1.0);
            }
            const auto count = data.rfind("trainable=");
            if (count != std::string::npos) {
                int trained = 0;
                if (std::sscanf(data.c_str() + count, "trainable=%d", &trained) == 1) job_records = trained;
            }
        } else if (job_.kind == "fetch" && process_.running()) {
            const auto assets = assets_root(options_);
            if (fs::exists(assets)) {
                for (const auto& entry : fs::directory_iterator(assets)) {
                    if (!entry.is_directory()) continue;
                    const auto part = entry.path() / "model.safetensors.partial";
                    if (fs::is_regular_file(part)) progress = std::to_string(fs::file_size(part) / 1048576) + " MiB 已下載";
                }
            }
        } else if (job_.kind == "install" && process_.running()) {
            const auto partial = options_.state / "tools" / "lora" / "trainer.tar.gz.partial";
            if (fs::is_regular_file(partial)) progress = std::to_string(fs::file_size(partial) / 1048576) + " MiB 已下載";
        }
        const auto assets = assets_root(options_);
        const auto revision = trim(last_log(assets / "current.revision"));
        const bool ready = revision.size() == 40 && fs::is_regular_file(assets / revision / "config.json") &&
            fs::is_regular_file(assets / revision / "ime_vocab.json") &&
            fs::is_regular_file(assets / revision / "model.safetensors");
        json update = nullptr;
        if (job_.kind == "check" && job_.state == "completed") {
            if (data.find("update-available=true") != std::string::npos) update = true;
            else if (data.find("update-available=false") != std::string::npos) update = false;
        }
        // The trainer check reports the release of the pinned submodule commit
        // without installing anything, matching the Windows version display.
        json trainer_update = nullptr;
        std::string trainer_release;
        if (job_.kind == "trainer-check" && job_.state == "completed") {
            if (data.find("update-available=true") != std::string::npos) trainer_update = true;
            else if (data.find("update-available=false") != std::string::npos) trainer_update = false;
            const auto marker = data.find("release=");
            if (marker != std::string::npos) {
                const auto begin = marker + std::string("release=").size();
                const auto end = data.find_first_of(" \n", begin);
                trainer_release = data.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                if (trainer_release == "unknown") trainer_release.clear();
            }
        }
        std::string trainer_version;
        try {
            const auto stamp = json::parse(std::ifstream(options_.trainer.parent_path() / "trainer-release.json"));
            trainer_version = stamp.value("version", "");
        } catch (...) {}
        // The accelerator libraries the installed trainer already carries, so
        // the page can tell whether the GPU backend is ready or still has to
        // be downloaded before the first training run.
        std::string trainer_gpu;
        {
            std::error_code ignored;
            const auto directory = options_.trainer.parent_path();
            if (fs::is_regular_file(directory / "libtorch_hip.so", ignored) ||
                fs::is_regular_file(directory / "libtorch_hip.dylib", ignored))
                trainer_gpu = "rocm";
            else if (fs::is_regular_file(directory / "libtorch_cuda.so", ignored) ||
                     fs::is_regular_file(directory / "libtorch_cuda.dylib", ignored))
                trainer_gpu = "cuda";
        }
        return {{"job", {{"kind",job_.kind}, {"state",job_.state}, {"progress",progress}, {"percent",percent},
                         {"records",job_records}, {"log",data}}},
                {"model_ready", ready}, {"revision", ready ? revision : ""},
                {"model_update_available", update},
                {"active_model_path", configured_model_path()},
                {"gpu", ime::unix_service::gpu_vendor()},
                {"build", std::string(build_stamp)},
                {"trainer_version", trainer_version},
                {"trainer_gpu", trainer_gpu},
                {"trainer_release_version", trainer_release},
                {"trainer_update_available", trainer_update},
                {"trainer_ready", trainer_ready(options_.trainer, options_.state)}};
    }

    void command(const std::string& action, const std::string& id) {
        if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::invalid_argument("invalid record ID");
        const auto child = ::fork();
        if (child < 0) throw std::runtime_error("cannot start CLI");
        if (child == 0) {
            // Keep command output off the native JSON-lines reply channel.
            (void)::dup2(STDERR_FILENO, STDOUT_FILENO);
            ::execl(options_.cli.c_str(), options_.cli.c_str(), action.c_str(), "--db", db_.c_str(), "--id", id.c_str(),
                    static_cast<char*>(nullptr));
            _exit(127);
        }
        int status;
        while (::waitpid(child, &status, 0) < 0) { if (errno != EINTR) throw std::runtime_error("cannot wait for CLI"); }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) throw std::runtime_error("紀錄操作失敗");
    }

#ifndef LLAVON_NATIVE_GUI
    void handle(int fd) {
        const auto request = read_request(fd);
        const std::string host = "127.0.0.1:" + std::to_string(port_);
        if (request.host != host || (!request.origin.empty() && request.origin != "http://" + host)) {
            respond(fd, 403, "application/json", R"({"error":"forbidden origin"})"); return;
        }
        if (request.method == "GET" && (request.path == "/" || request.path.starts_with("/?"))) {
            respond(fd, 200, "text/html", render_page()); return;
        }
        if (request.token != token_) {
            respond(fd, 403, "application/json", R"({"error":"unauthorized"})"); return;
        }
        last_seen_ = Clock::now();
        const auto result = dispatch(request);
        respond(fd, 200, "application/json", result.dump());
    }
#endif

    json dispatch(const Request& request) {
        if (request.method == "GET") {
            const json result = request.path == "/api/state" ? state() :
                                request.path == "/api/protection" ? protection() :
                                request.path == "/api/pending-count" ? pending_count() :
                                request.path == "/api/readings" ? readings_table() :
                                request.path.starts_with("/api/records?") || request.path == "/api/records" ? records(request.path) :
                                request.path == "/api/runs" ? runs() :
                                request.path == "/api/history" ? history() : json{{"error","not found"}};
            return result;
        }
        if (request.method != "POST" || request.content_type != "application/json")
            throw std::runtime_error("unsupported request");
        const auto body = json::parse(request.body);
        if (!body.is_object()) throw std::runtime_error("expected JSON object");
        if (request.path == "/api/fetch") {
            start_job("fetch", {"fetch-model", "--output-dir", assets_root(options_).string()}, {});
        } else if (request.path == "/api/check") {
            start_job("check", {"check-model", "--output-dir", assets_root(options_).string()}, {});
        } else if (request.path == "/api/install-trainer") {
            start_job("install", {"install-trainer", "--output-dir", (options_.state / "tools" / "lora").string()}, {});
        } else if (request.path == "/api/check-trainer") {
            start_job("trainer-check", {"check-trainer", "--output-dir", (options_.state / "tools" / "lora").string()}, {});
        } else if (request.path == "/api/unlock") {
            const auto text = [&](const char* key) {
                return body.contains(key) && body[key].is_string() ? body[key].get<std::string>() : std::string{};
            };
            unlock_review(text("password"));
        } else if (request.path == "/api/lock") {
            lock_review();
        } else if (request.path == "/api/protection") {
            const auto text = [&](const char* key) {
                return body.contains(key) && body[key].is_string() ? body[key].get<std::string>() : std::string{};
            };
            const auto action = text("action");
            if (action == "set-password") configure_password(text("password"), text("confirmation"));
            else if (action == "enable") set_recording(true);
            else if (action == "disable") set_recording(false);
            else if (action == "forget") forget_conversation_data();
            else throw std::runtime_error("unknown protection action");
        } else if (request.path == "/api/count-trainable") {
            // The confirmation dialog shows the number of records that will
            // actually be trained, not the raw pending count.
            const auto assets = assets_root(options_);
            std::ifstream revision_file(assets / "current.revision");
            std::string revision;
            revision_file >> revision;
            if (revision.size() != 40 || revision.find_first_not_of("0123456789abcdef") != std::string::npos ||
                !fs::is_regular_file(assets / revision / "model.safetensors"))
                throw std::runtime_error("請先下載基礎模型");
            if (!fs::is_directory(options_.tables)) throw std::runtime_error("找不到輸入法字表");
            const bool manual_only = body.value("only_manually_selected", false);
            const auto max_length = body.value("max_seq_length", std::string("384"));
            if (max_length.empty() || max_length.size() > 4 ||
                max_length.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("無效的序列長度");
            std::string password;
            if (body.contains("password") && body["password"].is_string())
                password = body["password"].get<std::string>();
            {
                DatabaseHandle handle(db_);
                if (ime::unix_service::read_commit_protection(handle.get()).configured) {
                    if (password.empty()) throw std::runtime_error("請先輸入訓練密碼");
                    ime::unix_service::CommitCipher verify;
                    verify.unlock(handle.get(), password);
                }
            }
            std::vector<std::string> args{"count-dataset", "--db", db_.string(),
                "--model-dir", (assets / revision).string(), "--tables-dir", options_.tables.string(),
                "--max-seq-length", max_length, "--only-manually-selected", manual_only ? "1" : "0"};
            if (!password.empty()) args.insert(args.end(), {"--password-fd", "3"});
            const auto text = run_cli_capture(args, password);
            // The CLI prints one JSON report as its last line; warnings may
            // precede it.
            json report = json::object();
            try {
                const auto last = text.find_last_not_of(" \t\r\n");
                const auto begin = last == std::string::npos ? std::string::npos : text.rfind('\n', last);
                report = json::parse(begin == std::string::npos ? text : text.substr(begin + 1));
            } catch (...) { report = json::object(); }
            return json{{"records", report.value("trainable", 0)},
                          {"skipped", report.value("skipped", 0)},
                          {"samples", report.value("samples", 0)},
                          {"skipped_ids", report.contains("skipped_ids") ? report["skipped_ids"] : json::array()}};
        } else if (request.path == "/api/train") {
            if (!trainer_ready(options_.trainer, options_.state))
                throw std::runtime_error("LoRA Trainer 尚未安裝或版本不符，請安裝／更新 LoRA Trainer");
            const bool manual_only = body.value("only_manually_selected", false);
            if (pending_count().at(manual_only ? "manual" : "count").get<std::int64_t>() == 0)
                throw std::runtime_error(manual_only ? "目前沒有曾手動選字的資料" : "目前沒有尚未訓練的資料");
            const bool stabilize_intruders = body.value("stabilize_intruders", false);
            const auto strength = body.value("strength", std::string("advanced"));
            if (!ime::unix_service::lora_strength_from_name(strength))
                throw std::runtime_error("無效的訓練強度");
            std::string base_run_id;
            if (body.contains("base_run_id") && body["base_run_id"].is_string())
                base_run_id = body["base_run_id"].get<std::string>();
            if (!base_run_id.empty() &&
                (base_run_id.size() > 18 || base_run_id.find_first_not_of("0123456789") != std::string::npos))
                throw std::runtime_error("無效的訓練基底");
            const auto params = body.at("options");
            if (!params.is_object()) throw std::runtime_error("無效的訓練參數");
            const auto assets = assets_root(options_);
            std::ifstream revision_file(assets / "current.revision");
            std::string revision;
            revision_file >> revision;
            if (revision.size() != 40 || revision.find_first_not_of("0123456789abcdef") != std::string::npos ||
                !fs::is_regular_file(assets / revision / "model.safetensors"))
                throw std::runtime_error("請先下載基礎模型");
            if (!fs::is_directory(options_.tables)) throw std::runtime_error("找不到輸入法字表");
            const auto output = runs_root(options_) /
                (std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
            // Training asks for its own password; it is never shared with the
            // review unlock. The CLI decrypts the records on its own.
            std::string password;
            if (body.contains("password") && body["password"].is_string())
                password = body["password"].get<std::string>();
            {
                DatabaseHandle handle(db_);
                if (ime::unix_service::read_commit_protection(handle.get()).configured) {
                    if (password.empty()) throw std::runtime_error("請先輸入訓練密碼");
                    ime::unix_service::CommitCipher verify;
                    verify.unlock(handle.get(), password);  // wrong password fails before the job starts
                }
            }
            std::vector<std::string> args{"train", "--db", db_.string(), "--model-dir", (assets / revision).string(),
                "--tables-dir", options_.tables.string(), "--output-dir", output.string(),
                "--revision", revision};
            for (const auto key : {"rank", "alpha", "dropout", "batch-size", "gradient-accumulation", "epochs",
                                   "max-steps", "learning-rate", "weight-decay", "warmup-steps", "max-grad-norm",
                                   "save-every", "seed", "max-seq-length", "target-modules", "device", "dtype", "shuffle"}) {
                args.emplace_back(std::string("--") + key);
                const auto value = params.at(key).get<std::string>();
                if (value.size() > 100) throw std::runtime_error("訓練參數過長");
                args.push_back(value);
            }
            // The strength and the data scope decide the effective parameters;
            // non-advanced strengths ignore the individual fields above, like
            // the Windows manager resets them.
            args.insert(args.end(), {"--strength", strength, "--only-manually-selected", manual_only ? "1" : "0",
                                     "--stabilize-intruders", stabilize_intruders ? "1" : "0"});
            if (!base_run_id.empty()) args.insert(args.end(), {"--base-run-id", base_run_id});
            if (!password.empty()) args.insert(args.end(), {"--password-fd", "3"});
            start_job("train", std::move(args), output, password);
        } else if (request.path == "/api/cancel") {
            update_job();
            process_.cancel();
        } else if (request.path == "/api/use-model") {
            update_job();
            if (process_.running()) throw std::runtime_error("請等待目前工作完成");
            const auto id = body.at("id").get<std::string>();
            if (id == "base") {
                // Applying the base model clears the configured path, so the
                // input method uses its installed default model again; the
                // personalized models are removed like the Windows manager.
                use_base_model();
                prune_obsolete_models({});
            } else {
                if (id.empty() || id.size() > 18 || id.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("invalid training run");
                const auto rows = query_database(db_,
                    ("SELECT model_path,adapter_path,base_revision FROM lora_runs WHERE id=" + id).c_str(), 3);
                if (rows.size() != 1) throw std::runtime_error("找不到已完成模型");
                const fs::path model = rows[0][0].get<std::string>();
                const fs::path adapter = rows[0][1].get<std::string>();
                if (!fs::is_regular_file(model) || fs::file_size(model) == 0) {
                    // The GGUF was pruned with an earlier apply; re-export it
                    // from the retained adapter (Windows does the same) and
                    // apply it when the export finishes.
                    if (!fs::is_regular_file(adapter / "adapter_model.safetensors"))
                        throw std::runtime_error("此版本沒有模型檔案");
                    const auto revision = rows[0][2].get<std::string>();
                    pending_export_apply_ = model;
                    start_job("export", {"export-model", "--db", db_.string(), "--run-id", id,
                                         "--model-dir", (assets_root(options_) / revision).string()}, {});
                } else {
                    use_model(model);
                    prune_obsolete_models(model);
                }
            }
        } else if (request.path.starts_with("/api/records/")) {
            const auto end = request.path.rfind('/');
            const auto action = request.path.substr(end + 1);
            if (action != "exclude" && action != "delete") throw std::runtime_error("unknown record action");
            command(action, request.path.substr(std::string("/api/records/").size(),
                                                end - std::string("/api/records/").size()));
        } else {
            throw std::runtime_error("unknown manager action");
        }
        return json{{"ok",true}};
    }

    Options options_;
    fs::path db_;
    int lock_ = -1;
#ifndef LLAVON_NATIVE_GUI
    int listen_ = -1, port_ = 0;
    std::string token_;
#endif
    Job job_;
    ime::unix_service::JobProcess process_;
    Clock::time_point last_seen_{};
    json readings_cache_ = json::object();
    bool readings_loaded_ = false;
    // The manager watches its own binary so a reinstall replaces the running
    // build the same way the memory helper is replaced.
#ifndef LLAVON_NATIVE_GUI
    fs::file_time_type self_mtime_{};
    bool watch_self_ = false;
    int restart_failures_ = 0;
#endif
    // A model to apply once the running re-export job finishes.
    std::optional<fs::path> pending_export_apply_;
    // Only while the review password is entered; locking wipes the key.
    std::optional<ime::unix_service::CommitCipher> cipher_;
};

} // namespace

int main(int argc, char** argv) {
    ::umask(0077);
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGTERM, request_stop);
    ::signal(SIGINT, request_stop);
    try { return Gui(parse_options(argc, argv)).run(); }
    catch (const std::exception& error) {
        std::cerr << "llavon-ime-lora-gui: " << error.what() << '\n';
        return 1;
    }
}
