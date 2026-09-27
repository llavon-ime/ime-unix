#include "commit_store.hpp"
#include "gpu_vendor.hpp"
#include "lora_presets.hpp"

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
.records{display:grid;gap:10px}
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
.run{display:grid;gap:6px;margin-bottom:8px;padding:11px 13px;border:1px solid var(--line);border-radius:10px;background:var(--paper)}
.run-top{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.run-top strong{font-size:14px}
.run-top button{margin-left:auto}
.tagrow{display:flex;gap:6px;flex-wrap:wrap}
.path{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;color:var(--muted);overflow-wrap:anywhere}
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
        <span class="field-label">待訓練資料</span>
        <span id="selection-summary"></span>
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
      </div>
      <div id="model-status" class="statusline"></div>
      <div class="row"><button id="check" class="ghost">檢查更新</button><button id="fetch" class="primary">下載／更新模型</button></div>
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
      <label class="field"><span>訓練基底</span><select id="base-run"></select></label>
      <label class="field" id="train-password-field" hidden><span>訓練密碼</span><input id="train-password" type="password" autocomplete="current-password"></label>
      <p class="field-hint" id="train-password-hint" hidden>開始訓練時須重新輸入密碼；檢視資料的解鎖狀態不會共用。</p>
      <div class="estimate"><span id="estimated-steps">預計 steps：0</span><progress id="progress" max="100" style="display:none"></progress></div>
      <div class="row"><button id="train" class="primary">開始訓練</button><button id="cancel" class="ghost">取消目前工作</button></div>
      <div class="row" style="margin-top:16px"><button id="check-trainer" class="ghost">檢查版本</button><button id="install-trainer" class="ghost">安裝／更新 LoRA Trainer</button></div>
      <small id="gpu-status" class="hint"></small>
      <small id="trainer-status" class="hint"></small>
      <pre id="log" class="log"></pre>
    </div>
  </section>
  <section class="form-card">
    <div class="field-group">
      <div class="field-label-row">
        <span class="field-label">訓練歷程</span>
      </div>
      <div id="runs"></div>
    </div>
  </section>
  </div>
</main>
</div>
<script>
const token = location.hash.slice(1) || sessionStorage.getItem('llavon-token');
if (location.hash) { sessionStorage.setItem('llavon-token', token); history.replaceState(null, '', '/'); }
const message = document.getElementById('message');
let recordOffset=0;
const PAGE_SIZE=20;
// Feedback for a click must stay readable; the poll only writes the job status
// while no fresh notice is pinned.
let noticeUntil=0;
let lastJobSignature='';
let trainerCheckRequested=false;
function showNotice(text, error=false){
  noticeUntil=Date.now()+15000;
  message.hidden=false;message.className='notice'+(error?' error':'');message.textContent=text;
}

let pendingCount=0;
let pendingManual=0;
let readingsTable={};
let activeModelPath='';
let protectionInfo={configured:false,enabled:false,unlocked:false};
for(const tab of document.querySelectorAll('.tabs button')){
  tab.onclick=()=>{
    for(const button of document.querySelectorAll('.tabs button')){
      const active=button===tab;
      button.setAttribute('aria-selected',String(active));
      document.getElementById(button.dataset.panel).hidden=!active;
    }
  };
}
function manualOnlyTraining(){
  return document.getElementById('only-manually-selected').checked;
}
function effectivePendingCount(){
  return manualOnlyTraining()?pendingManual:pendingCount;
}
function updateEstimate(){
  // Pending records all take part in the next run; the manual filter mirrors
  // the Windows manager's "only train manually selected sentences" default.
  const count=effectivePendingCount();
  document.getElementById('selection-summary').textContent=manualOnlyTraining()
    ? '共 '+pendingCount+' 筆（符合條件 '+pendingManual+' 筆）'
    : '共 '+pendingCount+' 筆';
  const presets={'ultra-low':1,'low':1,'medium':2,'high':5};
  const values=strengthSelect.value==='advanced'
    ? ['batch-size','gradient-accumulation','epochs','max-steps'].map(name=>Number(document.getElementById(name).value))
    : [1,1,presets[strengthSelect.value],-1];
  if(values.every(Number.isInteger)&&values[0]>0&&values[1]>0&&values[2]>0&&(values[3]===-1||values[3]>0)){
    const epochs=Math.ceil(Math.ceil(count/values[0])/values[1])*values[2];
    document.getElementById('estimated-steps').textContent='預計 steps：'+(values[3]>0?Math.min(epochs,values[3]):epochs);
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
document.getElementById('only-manually-selected').onchange=updateEstimate;
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
function recordCard(item, viewState){
  const card=document.createElement('article');card.className='record';
  const head=document.createElement('div');head.className='record-head';
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
  const readings=document.createElement('span');readings.className='readings';
  const readingLabel=document.createElement('span');readingLabel.textContent='逐字注音';
  const readingValue=document.createElement('strong');readingValue.textContent=readingSequence(item);
  readings.append(readingLabel,readingValue);foot.append(readings);
  card.append(head,composed(item),foot);
  return card;
}
// The training base mirrors the Windows manager's history choice: the newest
// run continues by default, an explicit run branches from that adapter, and
// "Base model" starts a fresh adapter.
function refreshBaseRuns(runs){
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
}
const strengthLabelsForHistory={'ultra-low':'極低','low':'低','medium':'中','high':'高','advanced':'進階'};
function runCard(item){
  const card=document.createElement('article');card.className='run';
  const top=document.createElement('div');top.className='run-top';
  const time=document.createElement('strong');time.textContent=item.completed_at;
  const tags=document.createElement('div');tags.className='tagrow';
  const labels=['rank '+item.rank,'alpha '+item.alpha,'dropout '+item.dropout,item.target_modules,
      '本次 '+item.record_count+' 筆','累計 '+item.cumulative_count+' 筆','步數 '+item.optimizer_steps];
  if(item.strength)labels.push('強度 '+(strengthLabelsForHistory[item.strength]||item.strength));
  if(item.only_manually_selected!==undefined)
    labels.push(item.only_manually_selected?'資料範圍：手動選字':'資料範圍：所有句子');
  if(item.parent_id)labels.push('基底 #'+item.parent_id);
  for(const text of labels){
    const tag=document.createElement('span');tag.className='tag';tag.textContent=text;tags.append(tag);
  }
  if(item.model_path===activeModelPath){
    // Mirrors the Windows manager: a completed model that is already the
    // configured one shows the loaded state instead of another reload button.
    const loaded=document.createElement('span');loaded.className='tag';loaded.textContent='使用中・載入成功';
    top.append(time,tags,loaded);
  }else{
    const button=document.createElement('button');button.className='ghost tiny';
    button.textContent='立即套用';
    button.onclick=async()=>{
      try{await api('use-model',{id:item.id});await refresh();
        showNotice('新模型已套用。');}
      catch(error){
        let status=top.querySelector('.run-error');
        if(!status){status=document.createElement('span');status.className='tag error run-error';top.insertBefore(status,button);}
        status.textContent=error.message;
      }
    };
    top.append(time,tags,button);
  }
  card.append(top);
  const path=document.createElement('div');path.className='path';path.textContent=item.model_path;card.append(path);
  return card;
}
const jobLabels={fetch:'下載模型',check:'檢查模型更新',install:'安裝 Trainer','trainer-check':'檢查 Trainer 版本',train:'訓練及匯出模型'};
async function refresh() {
  try {
    const state=await api('state');
    activeModelPath=state.active_model_path||'';
    protectionInfo=await api('protection');
    renderProtection(protectionInfo);
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
    if(jobSignature!==lastJobSignature){lastJobSignature=jobSignature;noticeUntil=0;}
    // A failed background trainer check only updates the trainer line; it must
    // not pop an error notice every time the page is opened offline.
    const quietCheck=job.kind==='trainer-check'&&job.state==='failed';
    if(Date.now()>=noticeUntil && !quietCheck){
      message.hidden=job.state==='idle';
      message.className='notice'+(job.state==='failed'?' error':'');
      message.textContent=job.state==='running' ? jobLabels[job.kind]+(job.progress?'・'+job.progress:'')
        : job.state==='idle'?'目前沒有工作':job.kind==='train'&&job.state==='completed'?'訓練完成，請套用新模型。':(jobLabels[job.kind]||job.kind)+'：'+({completed:'完成',failed:'失敗',cancelled:'已取消'}[job.state]||job.state);
    }
    document.getElementById('log').textContent=job.log || '';
    document.getElementById('log').style.display=job.log?'block':'none';
    const progress=document.getElementById('progress');progress.style.display=job.state==='running'?'block':'none';
    if(job.percent!==null){progress.value=job.percent;}else{progress.removeAttribute('value');}
    document.getElementById('check').disabled=job.state==='running';
    document.getElementById('fetch').disabled=job.state==='running';
    document.getElementById('check-trainer').disabled=job.state==='running';
    document.getElementById('install-trainer').disabled=job.state==='running';
    document.getElementById('train').disabled=job.state==='running' || !state.model_ready || !state.trainer_ready;
    document.getElementById('cancel').disabled=job.state!=='running';
    const modelStatus=document.getElementById('model-status');modelStatus.replaceChildren();
    const modelChip=document.createElement('span');modelChip.className='chip '+(state.model_ready?'trained':'');
    modelChip.textContent=state.model_ready?'已就緒':'尚未下載';modelStatus.append(modelChip);
    if(state.model_ready){const revision=document.createElement('span');revision.className='revision';revision.textContent=state.revision;modelStatus.append(revision);}
    if(state.model_update_available===true){const update=document.createElement('span');update.className='tag';update.textContent='有新版本可用';modelStatus.append(update);}
    else if(state.model_update_available===false){const current=document.createElement('span');current.className='tag';current.textContent='已是最新版本';modelStatus.append(current);}
    // The trainer line mirrors the Windows manager: installed version, the
    // release of the pinned submodule commit, and an update hint.
    let trainerText=state.trainer_ready?'LoRA Trainer 已安裝':'找不到 llavon-lora，請安裝選配的 LoRA Trainer 元件。';
    if(state.trainer_ready&&state.trainer_version)trainerText+='（'+state.trainer_version+'）';
    if(state.trainer_update_available===true)trainerText+='・有新版本可安裝';
    else if(state.trainer_update_available===false)trainerText+='・已是最新版本';
    else if(state.trainer_release_version)trainerText+='・目前發行版 '+state.trainer_release_version;
    document.getElementById('trainer-status').textContent=trainerText;
    const gpu=state.gpu||'none';
    document.getElementById('gpu-status').textContent=
      gpu==='amd' ? '偵測到 AMD GPU：安裝／更新或開始訓練時會下載 ROCm libtorch（約 9.4 GB，只下載一次）' :
      gpu==='nvidia' ? '偵測到 NVIDIA GPU：安裝／更新或開始訓練時會下載 CUDA libtorch（約 3.9 GB，只下載一次）' :
      gpu==='apple' ? 'Apple GPU：macOS 產物已內建 Metal（MPS），不需額外下載' :
      '未偵測到可用的 GPU：使用 CPU 訓練';

    // An open page always mirrors the database: records typed while it is open
    // appear on the next poll and take part in the next training run.
    const counts=await api('pending-count');
    pendingCount=counts.count;pendingManual=counts.manual;
    updateEstimate();
    const manualFilter=document.getElementById('only-manual-records').checked?'&manual=1':'';
    let listing=await api('records?state=pending'+manualFilter+'&offset='+recordOffset);
    if(recordOffset && recordOffset>=listing.total){
      recordOffset=Math.max(0,Math.ceil(listing.total/PAGE_SIZE)-1)*PAGE_SIZE;
      listing=await api('records?state=pending'+manualFilter+'&offset='+recordOffset);
    }
    const page=Math.floor(recordOffset/PAGE_SIZE)+1;
    const pages=Math.max(1,Math.ceil((listing.total||0)/PAGE_SIZE));
    document.getElementById('page-summary').textContent='第 '+page+' / '+pages+' 頁'+
      (listing.total?' · '+(recordOffset+1)+'–'+(recordOffset+listing.rows.length)+' / '+listing.total+' 筆':'');
    document.getElementById('previous').disabled=recordOffset===0;
    document.getElementById('next').disabled=!listing.has_more;
    const records=listing.rows;
    const list=document.getElementById('records');list.replaceChildren();
    if (!records.length) list.innerHTML='<p class="empty">目前沒有待訓練資料。設定密碼並啟用收集後，提交注音文字就會出現在這裡。</p>';
    else for (const item of records) list.append(recordCard(item,'pending'));
    const runs=await api('runs'), view=document.getElementById('runs');view.replaceChildren();
    if (!runs.length) view.innerHTML='<p class="empty">尚無已完成模型。</p>';
    else for(const item of runs) view.append(runCard(item));
    refreshBaseRuns(runs);
  } catch(error){showNotice(error.message,true);}
}
async function act(path, body){try{await api(path,body);await refresh();}catch(error){showNotice(error.message,true);}}
document.getElementById('fetch').onclick=()=>act('fetch',{});
document.getElementById('install-trainer').onclick=()=>act('install-trainer',{});
document.getElementById('check-trainer').onclick=()=>act('check-trainer',{});
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
    if(!confirm(`以 ${count} 筆資料開始訓練？`))return;
    await act('train',{strength:strengthSelect.value,only_manually_selected:manualOnly,
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

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

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
constexpr std::string_view build_stamp = __DATE__ " " __TIME__;

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

struct Request {
    std::string method, path, origin, host, token, content_type, body;
};

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

fs::path config_file() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("HOME is not set");
#ifdef __APPLE__
    const fs::path root = std::getenv("XDG_CONFIG_HOME") ? fs::path(std::getenv("XDG_CONFIG_HOME")) : fs::path(home) / ".config";
    return root / "llavon-ime" / "config.json";
#else
    if (const char* value = std::getenv("LLAVON_IME_CONFIG_PATH"); value && *value) return value;
    const fs::path root = std::getenv("XDG_CONFIG_HOME") ? fs::path(std::getenv("XDG_CONFIG_HOME")) : fs::path(home) / ".config";
    return root / "fcitx5" / "conf" / "llavon-ime.conf";
#endif
}

// The model the input method currently points at; the history uses it to mark
// the run that is already loaded (Windows shows 載入成功 the same way).
std::string configured_model_path() {
    try {
        const auto config = config_file();
        std::ifstream input(config);
        if (!input) return {};
#ifdef __APPLE__
        return json::parse(input).value("model_path", std::string{});
#else
        std::string line;
        while (std::getline(input, line)) {
            if (!line.starts_with("ModelPath=")) continue;
            auto value = trim(line.substr(std::string("ModelPath=").size()));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            std::string unescaped;
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (value[index] == '\\' && index + 1 < value.size()) ++index;
                unescaped += value[index];
            }
            return unescaped;
        }
        return {};
#endif
    } catch (...) { return {}; }
}

void use_model(const fs::path& path) {
    const auto config = config_file();
    fs::create_directories(config.parent_path());
    const auto temporary = fs::path(config.string() + ".lora.partial");
#ifdef __APPLE__
    json values = json::object();
    if (fs::is_regular_file(config)) values = json::parse(std::ifstream(config));
    if (!values.is_object()) throw std::runtime_error("invalid input method settings");
    values["model_path"] = path.string();
    { std::ofstream output(temporary, std::ios::trunc); output << values.dump(2) << '\n';
      if (!output) throw std::runtime_error("cannot save input method settings"); }
#else
    std::vector<std::string> lines;
    std::ifstream input(config);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.starts_with("ModelPath=")) lines.push_back(line);
    }
    // Fcitx INI accepts quoted paths with spaces and backslashes.
    std::string escaped;
    for (char ch : path.string()) {
        if (ch == '\\' || ch == '"') escaped += '\\';
        escaped += ch;
    }
    { std::ofstream output(temporary, std::ios::trunc); for (const auto& value : lines) output << value << '\n';
      output << "ModelPath=\"" << escaped << "\"\n";
      if (!output) throw std::runtime_error("cannot save input method settings"); }
#endif
    if (::chmod(temporary.c_str(), 0600) != 0) throw std::runtime_error("cannot protect input method settings");
    fs::rename(temporary, config);
#ifdef __APPLE__
    (void)::notify_post("org.llavon-ime.lora.model-changed");
#else
    const auto child = ::fork();
    if (child == 0) { ::execlp("fcitx5-remote", "fcitx5-remote", "-r", static_cast<char*>(nullptr)); _exit(127); }
    if (child > 0) { int status; while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
#endif
}

bool has_pinned_trainer_stamp(const fs::path& executable) {
    try {
        return nlohmann::json::parse(std::ifstream(executable.parent_path() / "trainer-release.json")).at("commit")
               == LLAVON_IME_LORA_PINNED_COMMIT;
    } catch (...) { return false; }
}

// TorchSharp needs the native libraries next to the executable; a stale
// installation that only carries the executable must not look ready.
bool has_trainer_libraries(const fs::path& directory, int depth = 0) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        const auto name = entry.path().filename().string();
        if (name.ends_with(".so") || name.find(".so.") != std::string::npos || name.ends_with(".dylib")) return true;
        if (depth < 3 && entry.is_directory() && has_trainer_libraries(entry.path(), depth + 1)) return true;
    }
    return false;
}

bool trainer_usable(const fs::path& executable) {
    return fs::is_regular_file(executable) && has_pinned_trainer_stamp(executable) &&
           has_trainer_libraries(executable.parent_path());
}

fs::path trainer_path(const fs::path& state) {
    if (const char* override = std::getenv("LLAVON_IME_LORA_CLI_PATH"); override && *override)
        return fs::absolute(override);
    const auto managed = state / "tools" / "lora" / "llavon-lora";
    const auto system = fs::path(LLAVON_IME_INSTALLED_LORA_TRAINER_PATH);
    if (trainer_usable(managed)) return managed;
    if (trainer_usable(system)) return system;
    return fs::is_regular_file(managed) ? managed : system;
}

bool trainer_ready(const fs::path& executable, const fs::path& state) {
    if (!fs::is_regular_file(executable) || ::access(executable.c_str(), X_OK) != 0) return false;
    const auto managed = state / "tools" / "lora" / "llavon-lora";
    const auto system = fs::path(LLAVON_IME_INSTALLED_LORA_TRAINER_PATH);
    if (executable != managed && executable != system) return true;  // explicit development override
    return trainer_usable(executable);
}

json query_database(const fs::path& path, const char* sql, int columns) {    if (!fs::is_regular_file(path)) return json::array();
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        const std::string error = db ? sqlite3_errmsg(db) : "cannot open training database";
        sqlite3_close(db); throw std::runtime_error(error);
    }
    sqlite3_busy_timeout(db, 1000);
    sqlite3_stmt* stmt = nullptr;
    json result = json::array();
    try {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            json row = json::array();
            for (int i = 0; i < columns; ++i) {
                const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                row.push_back(text ? text : "");
            }
            result.push_back(std::move(row));
        }
        if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    } catch (...) { sqlite3_finalize(stmt); sqlite3_close(db); throw; }
    sqlite3_finalize(stmt); sqlite3_close(db);
    return result;
}

struct Options {
    fs::path state, db, cli, tables, trainer;
    bool browser = true;
    int idle_seconds = 120;
};

// The encrypted store helpers need a live connection, and the manager keeps no
// long-lived handle; one connection serves each request.
class DatabaseHandle {
public:
    explicit DatabaseHandle(const fs::path& path) {
        if (!fs::is_regular_file(path)) throw std::runtime_error("找不到訓練資料庫");
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            const std::string error = db_ ? sqlite3_errmsg(db_) : "cannot open training database";
            sqlite3_close(db_); db_ = nullptr; throw std::runtime_error(error);
        }
        sqlite3_busy_timeout(db_, 1000);
        ime::unix_service::initialize_commit_database(db_);
    }
    ~DatabaseHandle() { if (db_) sqlite3_close(db_); }
    DatabaseHandle(const DatabaseHandle&) = delete;
    DatabaseHandle& operator=(const DatabaseHandle&) = delete;
    sqlite3* get() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

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
        else if (name == "--idle-seconds") options.idle_seconds = std::stoi(value);
        else throw std::invalid_argument("unknown option: " + name);
    }
    if (options.idle_seconds < 1) throw std::invalid_argument("idle timeout must be at least 1 second");
    options.state = fs::absolute(options.state);
    options.trainer = trainer_path(options.state);
    if (!options.db.empty()) options.db = fs::absolute(options.db);
    options.cli = fs::absolute(options.cli);
    options.tables = fs::absolute(options.tables);
    options.trainer = fs::absolute(options.trainer);
    return options;
}

struct Job {
    std::string kind, state = "idle";
    pid_t pid = -1;
    fs::path log, output;
    bool cancelling = false;
};

class Gui {
public:
    explicit Gui(Options options) : options_(std::move(options)),
        db_(options_.db.empty() ? options_.state / "commits.sqlite3" : options_.db) {}

    int run() {
        fs::create_directories(options_.state);
        struct stat directory {};
        if (::lstat(options_.state.c_str(), &directory) || !S_ISDIR(directory.st_mode) || directory.st_uid != ::getuid() ||
            ::chmod(options_.state.c_str(), 0700)) throw std::runtime_error("unsafe training data directory");
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
                        try { owner = static_cast<pid_t>(std::stol(pid_text)); } catch (...) { owner = 0; }
                    }
                }
            }
            bool acquired = false;
            if (owner > 0 && !stamp.empty() && stamp != std::string(build_stamp)) {
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
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ < 0) throw std::runtime_error("cannot create GUI socket");
        if (::fcntl(listen_, F_SETFD, FD_CLOEXEC)) throw std::runtime_error("cannot protect GUI socket descriptor");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(listen_, 8))
            throw std::runtime_error("cannot listen on loopback");
        socklen_t length = sizeof(address);
        if (::getsockname(listen_, reinterpret_cast<sockaddr*>(&address), &length))
            throw std::runtime_error("cannot find GUI port");
        port_ = ntohs(address.sin_port);
        token_ = random_token();
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
            if (job_.pid < 0 && Clock::now() - last_seen_ > std::chrono::seconds(options_.idle_seconds)) break;
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
        if (job_.pid >= 0) {
            (void)::kill(-job_.pid, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                update_job();
                if (job_.pid < 0) break;
                ::usleep(100000);
            }
            if (job_.pid >= 0) { (void)::kill(-job_.pid, SIGKILL); (void)::waitpid(job_.pid, nullptr, 0); }
        }
        return 0;
    }

private:
    void update_job() {
        if (job_.pid < 0) return;
        int status;
        const auto ended = ::waitpid(job_.pid, &status, WNOHANG);
        if (ended == 0) return;
        if (ended < 0 && errno == EINTR) return;
        job_.state = job_.cancelling ? "cancelled" :
                     (ended > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "completed" : "failed");
        if (job_.kind == "install" && job_.state == "completed" && !std::getenv("LLAVON_IME_LORA_CLI_PATH"))
            options_.trainer = options_.state / "tools" / "lora" / "llavon-lora";
        job_.pid = -1;
    }

    // The training password never reaches argv or the environment: it travels
    // through a private pipe that the CLI reads from descriptor 3.
    void start_job(const std::string& kind, std::vector<std::string> args, fs::path output,
                   const std::string& password = {}) {
        update_job();
        if (job_.pid >= 0) throw std::runtime_error("已有工作進行中");
        job_ = Job{.kind = kind, .state = "running", .log = options_.state / "gui-job.log", .output = std::move(output)};
        std::ofstream(job_.log, std::ios::trunc).close();
        int password_pipe[2] = {-1, -1};
        if (!password.empty() && ::pipe(password_pipe) != 0) throw std::runtime_error("cannot pass the training password");
        const pid_t child = ::fork();
        if (child < 0) {
            if (password_pipe[0] >= 0) { ::close(password_pipe[0]); ::close(password_pipe[1]); }
            throw std::runtime_error("cannot start CLI");
        }
        if (child == 0) {
            ::setsid();
            if (password_pipe[0] >= 0) {
                // Descriptor 3 is the contract with the CLI; keep it when the
                // pipe already landed there.
                if (password_pipe[0] != 3 && ::dup2(password_pipe[0], 3) < 0) _exit(127);
                if (password_pipe[0] != 3) ::close(password_pipe[0]);
                ::close(password_pipe[1]);
            }
            if (kind == "train") ::setenv("LLAVON_IME_LORA_CLI_PATH", options_.trainer.c_str(), 1);
            const int log = ::open(job_.log.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
            if (log < 0 || ::dup2(log, STDOUT_FILENO) < 0 || ::dup2(log, STDERR_FILENO) < 0) _exit(127);
            ::close(log);
            std::vector<std::string> strings{options_.cli.string()};
            strings.insert(strings.end(), args.begin(), args.end());
            std::vector<char*> argv;
            for (auto& item : strings) argv.push_back(item.data());
            argv.push_back(nullptr);
            ::execv(argv[0], argv.data());
            _exit(127);
        }
        if (password_pipe[0] >= 0) {
            ::close(password_pipe[0]);
            std::string line = password + "\n";
            const auto written = ::write(password_pipe[1], line.data(), line.size());
            const bool complete = written == static_cast<ssize_t>(line.size());
            if (written > 0) sodium_memzero(line.data(), static_cast<std::size_t>(written));
            ::close(password_pipe[1]);
            if (!complete) { (void)::kill(-child, SIGTERM); (void)::waitpid(child, nullptr, 0); job_.pid = -1;
                             throw std::runtime_error("cannot pass the training password"); }
        }
        job_.pid = child;
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
        if (job_.pid >= 0) throw std::runtime_error("請先等待目前工作結束或取消");
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

    // Windows keeps only the latest completed model for inference: applying
    // the newest model removes older quantized GGUFs, while every run's adapter
    // stays so it can be exported again with its base revision.
    void prune_obsolete_models(const fs::path& applied) const {
        std::error_code error;
        const auto rows = query_database(db_, "SELECT model_path FROM lora_runs ORDER BY id DESC", 1);
        if (rows.empty()) return;
        const auto latest = fs::absolute(rows.front()[0].get<std::string>(), error).lexically_normal();
        if (error || !fs::is_regular_file(latest, error)) return;
        const auto active = fs::absolute(applied, error).lexically_normal();
        if (error || active != latest) return;
        const auto root = fs::absolute(runs_root(options_), error).lexically_normal();
        if (error) return;
        for (const auto& row : rows) {
            const auto candidate = fs::absolute(row[0].get<std::string>(), error).lexically_normal();
            if (error) { error.clear(); continue; }
            if (candidate == latest || candidate.filename() != "personalized-Q4_K_M.gguf" ||
                candidate.parent_path().parent_path() != root) continue;
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

    json runs() const {
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
            // The full request of a run decides the labels the history shows
            // (strength and data scope); older runs simply carry none.
            if (requested && row[11].is_string()) {
                try {
                    const auto request = json::parse(row[11].get<std::string>());
                    entry["strength"] = request.value("strength", "");
                    entry["only_manually_selected"] = request.value("only_manually_selected", false);
                } catch (...) {}
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    json pending_count() const {
        const auto rows = query_database(db_,
            "SELECT COUNT(*),COALESCE(SUM(EXISTS(SELECT 1 FROM readings r WHERE r.commit_id=commits.id "
            "AND r.manually_selected=1)),0) FROM commits WHERE state='pending'", 2);
        if (rows.empty()) return {{"count", 0LL}, {"manual", 0LL}};
        return {{"count", std::stoll(rows[0][0].get<std::string>())},
                {"manual", std::stoll(rows[0][1].get<std::string>())}};
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
        if (job_.kind == "train") {
            const auto step = data.rfind("step=");
            if (step != std::string::npos) {
                progress = trim(data.substr(step, std::min<std::size_t>(50, data.size() - step)));
                int current = 0, total = 0;
                if (std::sscanf(data.c_str() + step, "step=%d/%d", &current, &total) == 2 && total > 0)
                    percent = 5 + 80 * std::clamp(static_cast<double>(current) / total, 0.0, 1.0);
            }
        } else if (job_.kind == "fetch" && job_.pid >= 0) {
            const auto assets = assets_root(options_);
            if (fs::exists(assets)) {
                for (const auto& entry : fs::directory_iterator(assets)) {
                    if (!entry.is_directory()) continue;
                    const auto part = entry.path() / "model.safetensors.partial";
                    if (fs::is_regular_file(part)) progress = std::to_string(fs::file_size(part) / 1048576) + " MiB 已下載";
                }
            }
        } else if (job_.kind == "install" && job_.pid >= 0) {
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
            const auto stamp = json::parse(std::ifstream(options_.state / "tools" / "lora" / "trainer-release.json"));
            trainer_version = stamp.value("version", "");
        } catch (...) {}
        return {{"job", {{"kind",job_.kind}, {"state",job_.state}, {"progress",progress}, {"percent",percent}, {"log",data}}},
                {"model_ready", ready}, {"revision", ready ? revision : ""},
                {"model_update_available", update},
                {"active_model_path", configured_model_path()},
                {"gpu", ime::unix_service::gpu_vendor()},
                {"trainer_version", trainer_version},
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
            ::execl(options_.cli.c_str(), options_.cli.c_str(), action.c_str(), "--db", db_.c_str(), "--id", id.c_str(),
                    static_cast<char*>(nullptr));
            _exit(127);
        }
        int status;
        while (::waitpid(child, &status, 0) < 0) { if (errno != EINTR) throw std::runtime_error("cannot wait for CLI"); }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) throw std::runtime_error("紀錄操作失敗");
    }

    void handle(int fd) {
        const auto request = read_request(fd);
        const std::string host = "127.0.0.1:" + std::to_string(port_);
        if (request.host != host || (!request.origin.empty() && request.origin != "http://" + host)) {
            respond(fd, 403, "application/json", R"({"error":"forbidden origin"})"); return;
        }
        if (request.method == "GET" && request.path == "/") {
            respond(fd, 200, "text/html", render_page()); return;
        }
        if (request.token != token_) {
            respond(fd, 403, "application/json", R"({"error":"unauthorized"})"); return;
        }
        last_seen_ = Clock::now();
        if (request.method == "GET") {
            const json result = request.path == "/api/state" ? state() :
                                request.path == "/api/protection" ? protection() :
                                request.path == "/api/pending-count" ? pending_count() :
                                request.path == "/api/readings" ? readings_table() :
                                request.path.starts_with("/api/records?") || request.path == "/api/records" ? records(request.path) :
                                request.path == "/api/runs" ? runs() : json{{"error","not found"}};
            respond(fd, request.path.starts_with("/api/") && request.path != "/api/state" &&
                        request.path != "/api/protection" && request.path != "/api/pending-count" &&
                        request.path != "/api/readings" &&
                        request.path != "/api/records" && !request.path.starts_with("/api/records?") &&
                        request.path != "/api/runs" ? 404 : 200,
                    "application/json", result.dump()); return;
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
        } else if (request.path == "/api/train") {
            if (!trainer_ready(options_.trainer, options_.state))
                throw std::runtime_error("LoRA Trainer 尚未安裝或版本不符，請安裝／更新 LoRA Trainer");
            const bool manual_only = body.value("only_manually_selected", false);
            if (pending_count().at(manual_only ? "manual" : "count").get<std::int64_t>() == 0)
                throw std::runtime_error(manual_only ? "目前沒有曾手動選字的資料" : "目前沒有尚未訓練的資料");
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
            args.insert(args.end(), {"--strength", strength, "--only-manually-selected", manual_only ? "1" : "0"});
            if (!base_run_id.empty()) args.insert(args.end(), {"--base-run-id", base_run_id});
            if (!password.empty()) args.insert(args.end(), {"--password-fd", "3"});
            start_job("train", std::move(args), output, password);
        } else if (request.path == "/api/cancel") {
            update_job();
            if (job_.pid < 0) throw std::runtime_error("沒有執行中的工作");
            job_.cancelling = true;
            if (::kill(-job_.pid, SIGTERM) != 0 && errno != ESRCH) throw std::runtime_error("無法取消工作");
        } else if (request.path == "/api/use-model") {
            update_job();
            if (job_.pid >= 0) throw std::runtime_error("請等待目前工作完成");
            const auto id = body.at("id").get<std::string>();
            if (id.empty() || id.size() > 18 || id.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("invalid training run");
            const auto rows = query_database(db_, ("SELECT model_path FROM lora_runs WHERE id=" + id).c_str(), 1);
            if (rows.size() != 1) throw std::runtime_error("找不到已完成模型");
            const fs::path model = rows[0][0].get<std::string>();
            if (!fs::is_regular_file(model) || fs::file_size(model) == 0) throw std::runtime_error("模型檔案已不存在");
            use_model(model);
            prune_obsolete_models(model);
        } else if (request.path.starts_with("/api/records/")) {
            const auto end = request.path.rfind('/');
            const auto action = request.path.substr(end + 1);
            if (action != "exclude" && action != "delete") throw std::runtime_error("unknown record action");
            command(action, request.path.substr(std::string("/api/records/").size(),
                                                end - std::string("/api/records/").size()));
        } else {
            respond(fd, 404, "application/json", R"({"error":"not found"})"); return;
        }
        respond(fd, 200, "application/json", R"({"ok":true})");
    }

    Options options_;
    fs::path db_;
    int lock_ = -1, listen_ = -1, port_ = 0;
    std::string token_;
    Job job_;
    Clock::time_point last_seen_{};
    json readings_cache_ = json::object();
    bool readings_loaded_ = false;
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
