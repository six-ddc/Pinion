// web_admin_web.h — 设备后台单页应用（内嵌 HTML/CSS/JS，零外部依赖）。
// 视觉对齐设备 AmberGlow：深底 #141210 + 卡片 + 1px 描边 + amber #FFAE1F 点睛。
//
// 两个 tab：
//   配置 — 大模型 API Key / baseUrl / 整份 models JSON（高级）、火山语音双密钥；
//          写 NVS，保存后可一键重启生效。密钥只回掩码，永不回明文。
//   文件 — 目录浏览 / 面包屑 / 建目录 / 删除（含递归确认）/ 上传（多选 + 目录选择 +
//          拖拽文件夹）串行队列 / 409 冲突选择（覆盖·跳过·全部覆盖·全部跳过）/ 失败重试。
//
// 默认落在哪个 tab 由配置状态决定：没配全 → 配置（扫码引导页进来的场景），
// 配全了 → 文件（快捷面板进来的场景）；URL hash #config / #files 可显式指定。

#pragma once

namespace web_admin_web {

inline const char* Html() {
    return R"HTML(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pinion · 设备后台</title>
<style>
  :root{
    --bg:#141210; --card:#1e1b17; --card2:#26221c; --line:#2f2a22;
    --tx:#ece6da; --dim:#a89e8c; --faint:#6f665a; --amber:#FFAE1F;
    --red:#ff6b5c; --ok:#8fce6b;
  }
  *{box-sizing:border-box}
  body{margin:0;padding:20px 16px 120px;font-family:-apple-system,system-ui,"PingFang SC",sans-serif;
       background:var(--bg);color:var(--tx);max-width:760px;margin:0 auto}
  .mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
  h1{font-size:20px;margin:4px 0 2px;letter-spacing:.5px}
  .space{color:var(--dim);font-size:12px;margin:0 0 14px}
  .space .bar{display:inline-block;width:120px;height:6px;background:var(--card2);border-radius:3px;
              overflow:hidden;vertical-align:middle;margin-left:8px}
  .space .bar>i{display:block;height:100%;background:var(--amber)}
  .card{background:var(--card);border:1px solid var(--line);border-radius:12px;
        padding:14px 16px;margin-bottom:16px}
  .crumbs{font-size:14px;margin-bottom:10px;word-break:break-all}
  .crumbs a{color:var(--amber);cursor:pointer;text-decoration:none}
  .crumbs span{color:var(--faint);margin:0 6px}
  .drop{border:1.5px dashed var(--line);border-radius:12px;padding:20px 14px;text-align:center;
        color:var(--dim);font-size:13px;cursor:pointer;transition:.15s}
  .drop.hot{border-color:var(--amber);color:var(--tx);background:var(--card2)}
  .picks{display:flex;gap:10px;margin-top:12px;flex-wrap:wrap}
  .btn{background:var(--card2);color:var(--tx);border:1px solid var(--line);border-radius:8px;
       padding:8px 14px;font-size:13px;cursor:pointer}
  .btn.amber{border-color:var(--amber);color:var(--amber)}
  .btn:disabled{opacity:.4;cursor:not-allowed}
  ul{list-style:none;margin:0;padding:0}
  li.row{display:flex;align-items:center;gap:12px;padding:11px 2px;border-bottom:1px solid var(--line)}
  li.row:last-child{border-bottom:0}
  .ic{width:26px;text-align:center;color:var(--amber);font-size:15px}
  .meta{flex:1;min-width:0}
  .nm{font-size:15px;word-break:break-all}
  .nm.dir{cursor:pointer}
  .sub{color:var(--faint);font-size:11px;margin-top:2px}
  .del{color:var(--red);background:none;border:0;font-size:13px;cursor:pointer;padding:6px 8px}
  .empty{color:var(--faint);padding:14px 2px;font-size:13px}
  /* 上传队列 */
  #qwrap{display:none}
  .qhead{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}
  .qbar{height:6px;background:var(--card2);border-radius:3px;overflow:hidden;margin-bottom:12px}
  .qbar>i{display:block;height:100%;width:0;background:var(--amber);transition:.15s}
  .qi{display:flex;align-items:center;gap:10px;padding:8px 0;border-bottom:1px solid var(--line);font-size:13px}
  .qi:last-child{border-bottom:0}
  .qi .qp{flex:1;min-width:0}
  .qi .qn{word-break:break-all}
  .qi .qbar2{height:4px;background:var(--card2);border-radius:2px;overflow:hidden;margin-top:4px}
  .qi .qbar2>i{display:block;height:100%;width:0;background:var(--amber)}
  .qi .st{font-size:11px;white-space:nowrap}
  .st.wait{color:var(--faint)} .st.up{color:var(--amber)} .st.done{color:var(--ok)}
  .st.skip{color:var(--faint)} .st.err{color:var(--red)}
  .qi .rebtn{color:var(--amber);background:none;border:0;cursor:pointer;font-size:12px}
  /* 冲突选择条 */
  #conflict{position:fixed;left:0;right:0;bottom:0;background:var(--card);border-top:1px solid var(--amber);
            padding:14px 16px;display:none;z-index:20}
  #conflict .cm{font-size:13px;margin-bottom:10px;word-break:break-all}
  #conflict .cb{display:flex;gap:8px;flex-wrap:wrap}
  .toast{position:fixed;left:50%;bottom:24px;transform:translateX(-50%);background:var(--card2);
         color:var(--tx);border:1px solid var(--amber);padding:10px 18px;border-radius:22px;
         opacity:0;transition:.2s;pointer-events:none;max-width:80%;z-index:30;font-size:13px}
  .toast.show{opacity:1} .toast.err{border-color:var(--red);color:var(--red)}
  /* tab 条 */
  .tabs{display:flex;gap:8px;margin:10px 0 16px}
  .tab{flex:1;text-align:center;background:var(--card);border:1px solid var(--line);border-radius:10px;
       padding:10px 0;font-size:14px;color:var(--dim);cursor:pointer}
  .tab.on{border-color:var(--amber);color:var(--amber);background:var(--card2)}
  .view{display:none} .view.on{display:block}
  /* 配置页 */
  .sec{font-size:15px;font-weight:600;margin:0 0 2px}
  .secst{font-size:12px;margin:0 0 12px}
  .secst.ok{color:var(--ok)} .secst.no{color:var(--red)}
  .fld{margin-bottom:14px}
  .fld label{display:block;font-size:13px;color:var(--dim);margin-bottom:5px}
  .fld .hint{font-size:11px;color:var(--faint);margin-top:4px;line-height:1.5}
  .fld input,.fld textarea{width:100%;background:var(--card2);color:var(--tx);border:1px solid var(--line);
       border-radius:8px;padding:9px 10px;font-size:14px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
  .fld input:focus,.fld textarea:focus{outline:0;border-color:var(--amber)}
  .fld textarea{min-height:150px;resize:vertical;font-size:12px;line-height:1.5}
  .lbl{display:flex;justify-content:space-between;align-items:baseline}
  .clr{color:var(--red);background:none;border:0;font-size:11px;cursor:pointer;padding:0}
  .clr.on{color:var(--amber);text-decoration:underline}
  details{margin-bottom:14px} details>summary{cursor:pointer;font-size:13px;color:var(--amber)}
  .acts{display:flex;gap:10px;flex-wrap:wrap;margin-top:4px}
  /* 电台编辑行 */
  .rrow{border:1px solid var(--line);border-radius:8px;padding:8px 10px;margin-bottom:8px;background:var(--card2)}
  .rrow .r1{display:flex;gap:8px;align-items:center;margin-bottom:6px}
  .rrow input{background:var(--bg);color:var(--tx);border:1px solid var(--line);border-radius:6px;
       padding:7px 8px;font-size:13px;width:100%;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
  .rrow input:focus{outline:0;border-color:var(--amber)}
  .rrow .rname{flex:1;min-width:0}
  .rrow .rgenre{width:84px;flex:none}
  .rrow .rdel{color:var(--red);background:none;border:0;font-size:12px;cursor:pointer;padding:4px 4px;white-space:nowrap}
</style>
</head>
<body>
  <h1>Pinion <span class="mono" style="color:var(--faint);font-size:13px">/ 设备后台</span></h1>
  <div class="tabs">
    <div class="tab" id="tabCfg">配置</div>
    <div class="tab" id="tabFiles">文件</div>
  </div>

  <div class="view" id="viewCfg">
    <div class="card">
      <p class="sec">大模型</p>
      <p class="secst" id="llmSt">读取中…</p>
      <div class="fld">
        <div class="lbl"><label for="llmKey">API Key</label>
          <button class="clr" id="clrLlmKey" style="display:none">清除</button></div>
        <input id="llmKey" type="password" autocomplete="off" spellcheck="false" placeholder="sk-…">
        <div class="hint">留空 = 不修改。重启生效。</div>
      </div>
      <div class="fld">
        <div class="lbl"><label for="llmModel">模型</label>
          <button class="clr" id="clrLlmModel" style="display:none">清除</button></div>
        <input id="llmModel" type="text" autocomplete="off" spellcheck="false" placeholder="如 deepseek-v4-flash">
        <div class="hint">你要用的模型 ID，与 API Key 一起为必填。留空 = 不修改。</div>
      </div>
      <div class="fld">
        <div class="lbl"><label for="llmBase">Base URL（可选）</label>
          <button class="clr" id="clrLlmBase" style="display:none">清除</button></div>
        <input id="llmBase" type="text" autocomplete="off" spellcheck="false" placeholder="https://api.deepseek.com">
        <div class="hint">兼容 OpenAI Completions 接口的自建/代理端点。留空 = 用默认。</div>
      </div>
      <details>
        <summary>高级：整份 models JSON（换供应商 / 精确控制模型参数）</summary>
        <div class="fld" style="margin-top:10px">
          <div class="lbl"><label for="llmJson">models JSON</label>
            <button class="clr" id="clrLlmJson" style="display:none">清除</button></div>
          <textarea id="llmJson" spellcheck="false" placeholder='{"providers":{"deepseek":{"baseUrl":"…","api":"openai-completions","apiKey":"sk-…","models":[{"id":"…"}]}}}'></textarea>
          <div class="hint" id="jsonHint">填了这个就完全覆盖上面两项与内置清单，直接交给 agent 运行时。</div>
        </div>
      </details>
    </div>

    <div class="card">
      <p class="sec">语音（火山引擎）</p>
      <p class="secst" id="voiceSt">读取中…</p>
      <div class="fld">
        <div class="lbl"><label for="volcApp">App Key</label>
          <button class="clr" id="clrVolcApp" style="display:none">清除</button></div>
        <input id="volcApp" type="password" autocomplete="off" spellcheck="false">
      </div>
      <div class="fld">
        <div class="lbl"><label for="volcAk">Access Key</label>
          <button class="clr" id="clrVolcAk" style="display:none">清除</button></div>
        <input id="volcAk" type="password" autocomplete="off" spellcheck="false">
        <div class="hint">需开通：流式语音识别大模型 + 双向流式语音合成。缺这两项则按住说话与朗读不可用。</div>
      </div>
    </div>

    <div class="card">
      <p class="sec">网络电台</p>
      <p class="secst" id="radioSt">读取中…</p>
      <div id="radioRows"></div>
      <div class="acts">
        <button class="btn amber" id="radioAdd">+ 添加电台</button>
        <button class="btn" id="radioReset">恢复默认</button>
      </div>
      <div class="hint" style="margin-top:8px">名称 + 播放地址（HLS <span class="mono">.m3u8</span> 或 http(s) 直播流），分组可留空。
        留着不动 = 不修改；改过后在保存并重启后生效。与内置默认完全一致时按默认存储。</div>
    </div>

    <div class="card">
      <div class="acts">
        <button class="btn amber" id="saveApply">保存并重启</button>
        <button class="btn" id="saveOnly">只保存</button>
      </div>
      <div class="hint" style="margin-top:10px">配置在重启后生效。密钥只在提交时上传一次，页面永不回显明文。</div>
    </div>
  </div>

  <div class="view" id="viewFiles">
  <p class="space mono" id="space">SD --</p>

  <div class="card">
    <div class="crumbs" id="crumbs"></div>
    <div class="drop" id="drop">拖拽 .mp3 文件或文件夹到这里，或从下方选择<br>
      <span class="mono" style="font-size:11px;color:var(--faint)">当前目录上传 · 仅接收 .mp3</span></div>
    <div class="picks">
      <button class="btn" id="pickFiles">选择文件</button>
      <button class="btn" id="pickDir">选择文件夹</button>
      <button class="btn amber" id="newDir">新建目录</button>
    </div>
    <input id="fileInput" type="file" accept=".mp3" multiple style="display:none">
    <input id="dirInput" type="file" webkitdirectory directory multiple style="display:none">
  </div>

  <div class="card" id="qwrap">
    <div class="qhead"><b style="font-size:14px">上传队列</b><span class="mono" id="qsum" style="color:var(--dim);font-size:12px"></span></div>
    <div class="qbar"><i id="qbarFill"></i></div>
    <ul id="qlist"></ul>
  </div>

  <div class="card">
    <ul id="list"></ul>
    <div class="empty" id="empty" style="display:none">空目录</div>
  </div>

  <div id="conflict"><div class="cm" id="cmsg"></div>
    <div class="cb">
      <button class="btn amber" id="cOver">覆盖</button>
      <button class="btn" id="cSkip">跳过</button>
      <button class="btn" id="cOverAll">全部覆盖</button>
      <button class="btn" id="cSkipAll">全部跳过</button>
    </div>
  </div>
  </div><!-- /viewFiles -->
  <div class="toast mono" id="toast"></div>

<script>
"use strict";
var curDir = "";            // 当前浏览目录（"" = 两个根）
var queue = [], qActive = false;
var allOverwrite = false, allSkip = false;
var conflictResolver = null;

var $ = function(id){ return document.getElementById(id); };
function toast(m, err){ var t=$("toast"); t.textContent=m; t.className="toast mono show"+(err?" err":"");
  setTimeout(function(){ t.className="toast mono"+(err?" err":""); }, 2400); }
function fmt(n){ n=+n; if(n>=1073741824) return (n/1073741824).toFixed(2)+" GB";
  if(n>=1048576) return (n/1048576).toFixed(1)+" MB"; if(n>=1024) return (n/1024).toFixed(1)+" KB"; return n+" B"; }
function isMp3(name){ return /\.mp3$/i.test(name); }

// ---- 空间 ----
function loadSpace(){
  fetch("/api/space").then(function(r){return r.json();}).then(function(s){
    if(!s.mounted){ $("space").textContent="SD 未挂载"; return; }
    var used = s.total - s.free, pct = s.total ? (used/s.total*100) : 0;
    $("space").innerHTML = "SD " + fmt(s.free) + " 可用 / " + fmt(s.total) +
      " <span class='bar'><i style='width:"+pct.toFixed(0)+"%'></i></span>";
  }).catch(function(){ $("space").textContent="SD --"; });
}

// ---- 面包屑 ----
function renderCrumbs(){
  var c = $("crumbs"); c.innerHTML = "";
  var root = document.createElement("a"); root.textContent="SD"; root.onclick=function(){ go(""); };
  c.appendChild(root);
  if(curDir){
    var parts = curDir.split("/"), acc="";
    parts.forEach(function(p){
      acc = acc ? acc+"/"+p : p;
      var sep=document.createElement("span"); sep.textContent="›"; c.appendChild(sep);
      var a=document.createElement("a"); a.textContent=p; var target=acc;
      a.onclick=function(){ go(target); }; c.appendChild(a);
    });
  }
}

// ---- 列表 ----
function go(dir){
  var prev=curDir;
  curDir=dir; renderCrumbs();
  // 读取失败回滚：面包屑/curDir 已切到新目录但列表尚未替换，失败时退回旧目录，
  // 保持 面包屑·curDir·当前列表 三者一致（列表仍显示着 prev 的内容）。
  loadList(function(){ curDir=prev; renderCrumbs(); });
}
function loadList(onFail){
  fetch("/api/list?dir="+encodeURIComponent(curDir)).then(function(r){
    if(!r.ok) throw 0; return r.json();
  }).then(function(d){
    var ul=$("list"); ul.innerHTML="";
    var ents=d.entries||[];
    $("empty").style.display = ents.length ? "none" : "block";
    ents.forEach(function(e){ ul.appendChild(rowEl(e)); });
  }).catch(function(){ toast("读取目录失败", 1); if(onFail) onFail(); });
}
function rowEl(e){
  var li=document.createElement("li"); li.className="row";
  var ic=document.createElement("div"); ic.className="ic mono"; ic.textContent = e.is_dir ? "[]" : "♪";
  var meta=document.createElement("div"); meta.className="meta";
  var nm=document.createElement("div"); nm.className="nm"+(e.is_dir?" dir":""); nm.textContent=e.name;
  meta.appendChild(nm);
  if(!e.is_dir){ var sub=document.createElement("div"); sub.className="sub mono"; sub.textContent=fmt(e.size); meta.appendChild(sub); }
  if(e.is_dir){ var path = curDir ? curDir+"/"+e.name : e.name; nm.onclick=function(){ go(path); }; }
  var del=document.createElement("button"); del.className="del"; del.textContent="删除";
  del.onclick=function(){ delEntry(e); };
  li.appendChild(ic); li.appendChild(meta); li.appendChild(del);
  return li;
}
function relOf(name){ return curDir ? curDir+"/"+name : name; }
function delEntry(e){
  var rel = relOf(e.name);
  if(!confirm("删除 "+e.name+" ？")) return;
  doDelete(rel, false, function(status){
    if(status===200){ toast("已删除"); loadList(); loadSpace(); }
    else if(status===409){ if(confirm(e.name+" 非空，确认递归删除全部内容？"))
        doDelete(rel, true, function(s2){ if(s2===200){ toast("已删除"); loadList(); loadSpace(); } else toast("删除失败", 1); }); }
    else toast("删除失败", 1);
  });
}
function doDelete(rel, recursive, cb){
  fetch("/api/delete", {method:"POST", headers:{"Content-Type":"application/x-www-form-urlencoded"},
    body:"path="+encodeURIComponent(rel)+"&recursive="+(recursive?"1":"0")})
    .then(function(r){ cb(r.status); }).catch(function(){ cb(0); });
}

// ---- 新建目录 ----
$("newDir").onclick=function(){
  var name = prompt("新目录名（建在当前目录下）");
  if(!name) return;
  if(!curDir){ toast("请先进入 Music 或 Podcasts", 1); return; }
  fetch("/api/mkdir", {method:"POST", headers:{"Content-Type":"application/x-www-form-urlencoded"},
    body:"path="+encodeURIComponent(relOf(name))})
    .then(function(r){ if(r.ok){ toast("已创建"); loadList(); } else toast("创建失败", 1); })
    .catch(function(){ toast("创建失败", 1); });
};

// ---- 上传：收集来源 ----
$("pickFiles").onclick=function(){ $("fileInput").click(); };
$("pickDir").onclick=function(){ $("dirInput").click(); };
$("fileInput").onchange=function(){ addFiles(this.files, false); this.value=""; };
$("dirInput").onchange=function(){ addFiles(this.files, true); this.value=""; };

function baseGuard(){ if(!curDir){ toast("请先进入 Music 或 Podcasts 再上传", 1); return false; } return true; }

// FileList（含 webkitRelativePath，目录选择时有子路径）
function addFiles(files, useRel){
  if(!baseGuard()) return;
  var arr=[];
  for(var i=0;i<files.length;i++){
    var f=files[i];
    var sub = (useRel && f.webkitRelativePath) ? f.webkitRelativePath : f.name;
    arr.push({file:f, rel: curDir+"/"+sub});
  }
  enqueue(arr);
}

// 拖拽：文件与文件夹（webkitGetAsEntry 递归）
$("drop").onclick=function(){ $("fileInput").click(); };
$("drop").ondragover=function(e){ e.preventDefault(); this.className="drop hot"; };
$("drop").ondragleave=function(){ this.className="drop"; };
$("drop").ondrop=function(e){
  e.preventDefault(); this.className="drop";
  if(!baseGuard()) return;
  var items=e.dataTransfer.items, entries=[];
  for(var i=0;i<items.length;i++){ var en=items[i].webkitGetAsEntry && items[i].webkitGetAsEntry(); if(en) entries.push(en); }
  if(entries.length){ collectEntries(entries, function(arr){ enqueue(arr); }); }
  else if(e.dataTransfer.files.length){ addFiles(e.dataTransfer.files, false); }
};
function collectEntries(entries, done){
  var out=[], pending=0, finished=false;
  function checkDone(){ if(finished && pending===0) done(out); }
  function walk(entry){
    if(entry.isFile){
      pending++;
      entry.file(function(f){ out.push({file:f, rel: curDir+entry.fullPath}); pending--; checkDone(); },
                 function(){ pending--; checkDone(); });
    } else if(entry.isDirectory){
      var reader=entry.createReader();
      var readBatch=function(){
        pending++;
        reader.readEntries(function(batch){
          pending--;
          if(batch.length){ batch.forEach(walk); readBatch(); }
          else checkDone();
        }, function(){ pending--; checkDone(); });
      };
      readBatch();
    }
  }
  entries.forEach(walk);
  finished=true; checkDone();
}

// ---- 上传队列 ----
function enqueue(arr){
  // 新批次 = 入队时队列为空或现存条目全部已终态（done/skip/err）。新批次开始时
  // 重置「全部覆盖/全部跳过」选择，并把已终态条目清出队列（进行中的保留），使
  // 冲突选择与进度汇总严格限定在当前批次内，绝不跨批次泄漏。
  var isTerm = function(q){ return q.status==="done"||q.status==="skip"||q.status==="err"; };
  if(queue.every(isTerm)){
    allOverwrite=false; allSkip=false;
    queue = queue.filter(function(q){ return !isTerm(q); });  // 全终态 -> 清空；有进行中则此分支不进
  }
  arr.forEach(function(it){
    var mp3 = isMp3(it.file.name);
    queue.push({file:it.file, rel:it.rel.replace(/^\/+/,""), size:it.file.size,
                status: mp3 ? "wait" : "skip", progress:0, tries:0, over:0, el:null, mp3:mp3});
  });
  renderQueue();
  if(!qActive) runQueue();
}
function renderQueue(){
  $("qwrap").style.display = queue.length ? "block" : "none";
  var ul=$("qlist"); ul.innerHTML="";
  queue.forEach(function(it, idx){
    var li=document.createElement("div"); li.className="qi"; it.el=li;
    var p=document.createElement("div"); p.className="qp";
    var nm=document.createElement("div"); nm.className="qn"; nm.textContent=it.rel+"  ("+fmt(it.size)+")";
    p.appendChild(nm);
    var bw=document.createElement("div"); bw.className="qbar2"; var bi=document.createElement("i");
    bi.style.width=(it.progress*100)+"%"; bw.appendChild(bi); p.appendChild(bw); it.fill=bi;
    var st=document.createElement("span"); st.className="st "+it.status;
    st.textContent = it.status==="skip" ? "非 mp3·跳过" : it.status==="done" ? "完成" :
                     it.status==="err" ? "失败" : it.status==="up" ? "上传中" : "等待";
    li.appendChild(p); li.appendChild(st); it.stEl=st;
    if(it.status==="err"){ var rb=document.createElement("button"); rb.className="rebtn"; rb.textContent="重试";
      rb.onclick=function(){ it.status="wait"; it.tries=0; updateItem(it); if(!qActive) runQueue(); };
      li.appendChild(rb); }
    ul.appendChild(li);
  });
  updateSummary();
}
function updateItem(it){
  if(!it.el) return;
  it.stEl.className="st "+it.status;
  it.stEl.textContent = it.status==="skip" ? "非 mp3·跳过" : it.status==="done" ? "完成" :
                        it.status==="err" ? "失败" : it.status==="up" ? "上传中" : "等待";
  if(it.fill) it.fill.style.width=(it.progress*100)+"%";
  if(it.status==="err" && it.el && !it.el.querySelector(".rebtn")){
    var rb=document.createElement("button"); rb.className="rebtn"; rb.textContent="重试";
    rb.onclick=function(){ it.status="wait"; it.tries=0; updateItem(it); if(!qActive) runQueue(); };
    it.el.appendChild(rb);
  }
}
function updateSummary(){
  var total=queue.length, done=queue.filter(function(q){return q.status==="done"||q.status==="skip";}).length;
  $("qsum").textContent = done+" / "+total;
  $("qbarFill").style.width = total ? (done/total*100)+"%" : "0";
}
function runQueue(){
  qActive=true;
  var next = queue.find(function(q){ return q.status==="wait"; });
  if(!next){ qActive=false; updateSummary(); loadList(); loadSpace(); return; }
  uploadItem(next, function(){ runQueue(); });
}
function uploadItem(it, done){
  it.status="up"; it.progress=0; updateItem(it);
  var over = (it.over || allOverwrite) ? 1 : 0;  // 条目自身覆盖标志（单个「覆盖」选择）优先，自动重试沿用
  send(it, over, function(status){
    if(status===200){ it.status="done"; it.progress=1; updateItem(it); updateSummary(); done(); return; }
    if(status===409){
      if(allSkip){ it.status="skip"; updateItem(it); updateSummary(); done(); return; }
      askConflict(it.rel, function(choice){
        if(choice==="over"){ it.over=1; send(it,1,function(s2){ finishItem(it,s2,done); }); }
        else if(choice==="overAll"){ allOverwrite=true; it.over=1; send(it,1,function(s2){ finishItem(it,s2,done); }); }
        else if(choice==="skipAll"){ allSkip=true; it.status="skip"; updateItem(it); updateSummary(); done(); }
        else { it.status="skip"; updateItem(it); updateSummary(); done(); }  // skip this
      });
      return;
    }
    finishItem(it, status, done);
  });
}
function finishItem(it, status, done){
  if(status===200){ it.status="done"; it.progress=1; }
  else if(it.tries<1){ it.tries++; it.status="wait"; updateItem(it); return uploadItem(it, done); }  // 自动重试 1 次
  else { it.status="err"; }
  updateItem(it); updateSummary(); done();
}
function send(it, overwrite, cb){
  var xhr=new XMLHttpRequest();
  xhr.open("POST", "/api/upload?path="+encodeURIComponent(it.rel)+"&overwrite="+overwrite);
  xhr.upload.onprogress=function(e){ if(e.lengthComputable){ it.progress=e.loaded/e.total; updateItem(it); } };
  xhr.onload=function(){ cb(xhr.status); };
  xhr.onerror=function(){ cb(0); };
  xhr.send(it.file);
}

// ---- 冲突选择条 ----
function askConflict(rel, cb){
  conflictResolver=cb;
  $("cmsg").textContent = "已存在：" + rel;
  $("conflict").style.display="block";
}
function resolveConflict(choice){ $("conflict").style.display="none"; var r=conflictResolver; conflictResolver=null; if(r) r(choice); }
$("cOver").onclick=function(){ resolveConflict("over"); };
$("cSkip").onclick=function(){ resolveConflict("skip"); };
$("cOverAll").onclick=function(){ resolveConflict("overAll"); };
$("cSkipAll").onclick=function(){ resolveConflict("skipAll"); };

// ---- tab 切换（文件页首次显示时才拉 SD 数据，未配置的设备不必为此转磁盘）----
var filesInited = false;
function showTab(which, fromClick){
  var cfg = (which === "config");
  $("viewCfg").className   = "view" + (cfg ? " on" : "");
  $("viewFiles").className = "view" + (cfg ? "" : " on");
  $("tabCfg").className    = "tab"  + (cfg ? " on" : "");
  $("tabFiles").className  = "tab"  + (cfg ? "" : " on");
  if(!cfg && !filesInited){ filesInited = true; renderCrumbs(); loadList(); loadSpace(); }
  if(fromClick) location.hash = cfg ? "#config" : "#files";
}
$("tabCfg").onclick   = function(){ showTab("config", 1); };
$("tabFiles").onclick = function(){ showTab("files", 1); };

// ---- 配置页 ----
// 输入框留空 = 该项不修改（所以密钥永不需要回显明文）；要抹掉已存的值就点「清除」，
// 提交时该字段以空值发出。
var CFG_FIELDS = [
  {id:"llmKey",   name:"llm_key",   clr:"clrLlmKey"},
  {id:"llmModel", name:"llm_model", clr:"clrLlmModel"},
  {id:"llmBase",  name:"llm_base",  clr:"clrLlmBase"},
  {id:"llmJson",  name:"llm_json",  clr:"clrLlmJson"},
  {id:"volcApp",  name:"volc_app",  clr:"clrVolcApp"},
  {id:"volcAk",   name:"volc_ak",   clr:"clrVolcAk"}
];
var cleared = {};

function resetClears(){
  cleared = {};
  CFG_FIELDS.forEach(function(f){
    $(f.clr).className = "clr"; $(f.clr).textContent = "清除";
  });
}
CFG_FIELDS.forEach(function(f){
  $(f.clr).onclick = function(){
    cleared[f.name] = !cleared[f.name];
    $(f.clr).className = "clr" + (cleared[f.name] ? " on" : "");
    $(f.clr).textContent = cleared[f.name] ? "将清除（点此取消）" : "清除";
    if(cleared[f.name]) $(f.id).value = "";
  };
});

function showClr(id, on){ $(id).style.display = on ? "inline" : "none"; }

// ---- 网络电台编辑器 ----
// 与密钥字段同约定：留着不动 = 不提交（radioDirty=false）；改过才作为 radio_json 提交。
var radioDefaults = [];      // 内置默认（「恢复默认」的来源）
var radioDirty = false;      // 有无编辑
var radioMax = 3960;

function radioRowEl(s){
  var d=document.createElement("div"); d.className="rrow";
  var r1=document.createElement("div"); r1.className="r1";
  var nm=document.createElement("input"); nm.className="rname"; nm.placeholder="电台名"; nm.value=(s&&s.name)||"";
  var ge=document.createElement("input"); ge.className="rgenre"; ge.placeholder="分组"; ge.value=(s&&s.genre)||"";
  var del=document.createElement("button"); del.className="rdel"; del.textContent="删除";
  del.onclick=function(){ d.remove(); radioDirty=true; updateRadioMeter(); };
  r1.appendChild(nm); r1.appendChild(ge); r1.appendChild(del);
  var url=document.createElement("input"); url.className="rurl"; url.placeholder="https://….m3u8"; url.value=(s&&s.url)||"";
  [nm,ge,url].forEach(function(i){ i.oninput=function(){ radioDirty=true; updateRadioMeter(); }; });
  d.appendChild(r1); d.appendChild(url);
  return d;
}
function renderRadioRows(list){
  var box=$("radioRows"); box.innerHTML="";
  (list||[]).forEach(function(s){ box.appendChild(radioRowEl(s)); });
}
function readRadioRows(){
  var rows=[], box=$("radioRows");
  for(var i=0;i<box.children.length;i++){
    var d=box.children[i];
    var name=d.querySelector(".rname").value.trim();
    var genre=d.querySelector(".rgenre").value.trim();
    var url=d.querySelector(".rurl").value.trim();
    if(name==="" && url==="") continue;   // 完全空行：忽略
    rows.push({name:name, genre:genre, url:url});
  }
  return rows;
}
function updateRadioMeter(){
  var rows=readRadioRows();
  var bytes = rows.length ? new Blob([JSON.stringify(rows)]).size : 0;  // 估算入库体积
  var st=$("radioSt"), over=bytes>radioMax;
  st.textContent = rows.length + " 台（未保存）· 约 " + bytes + "/" + radioMax + "B" +
                   (over ? " —— 超出上限，请删减" : "");
  st.className = "secst" + (over ? " no" : (rows.length ? " ok" : " no"));
}
// 服务端权威状态（未编辑时显示）。
function renderRadio(r){
  if(!r) return;
  radioDefaults = r.defaults || [];
  radioMax = r.max_bytes || 3960;
  renderRadioRows(r.stations);
  radioDirty = false;
  var st=$("radioSt");
  st.textContent = (r.is_custom ? "✓ 自定义 " : "内置默认 ") + (r.count||0) + " 台" +
                   (r.is_custom ? (" · " + (r.bytes||0) + "/" + radioMax + "B") : "");
  st.className = "secst" + ((r.count||0) ? " ok" : " no");
}
$("radioAdd").onclick = function(){ $("radioRows").appendChild(radioRowEl(null)); radioDirty=true; updateRadioMeter(); };
$("radioReset").onclick = function(){ renderRadioRows(radioDefaults); radioDirty=true; updateRadioMeter(); };

// 收集电台字段：无编辑 -> {body:null}；有编辑做客户端校验，返回 {body} 或 {err}。
function collectRadio(){
  if(!radioDirty) return {body:null};
  var rows=readRadioRows();
  if(rows.length===0) return {err:"电台列表不能为空（保留至少一个台，或点「恢复默认」）"};
  for(var i=0;i<rows.length;i++){
    if(!rows[i].name) return {err:"第 "+(i+1)+" 个电台缺名称"};
    if(!/^https?:\/\//i.test(rows[i].url)) return {err:"第 "+(i+1)+" 个电台地址需以 http:// 或 https:// 开头"};
  }
  return {body:"radio_json="+encodeURIComponent(JSON.stringify(rows))};
}

function renderConfig(c){
  var l = c.llm || {}, v = c.voice || {};
  var st = $("llmSt");
  if(l.configured){
    var how = l.json_override ? ("整份 JSON " + l.json_bytes + "B") : ("Key " + l.key_mask);
    st.textContent = "✓ 已配置 · " + how + (l.model ? " · 模型 " + l.model : "");
    st.className = "secst ok";
  } else {
    st.textContent = "✗ 未配置 —— 填入 API Key 与模型并重启后即可对话";
    st.className = "secst no";
  }
  var vst = $("voiceSt");
  if(v.configured){
    vst.textContent = "✓ 已配置 · App " + v.app_mask + " · AK " + v.ak_mask;
    vst.className = "secst ok";
  } else {
    vst.textContent = "✗ 未配置 —— 按住说话与朗读不可用";
    vst.className = "secst no";
  }
  $("llmKey").placeholder   = l.key_mask ? (l.key_mask + "（已配置，留空不改）") : "sk-…";
  $("llmModel").placeholder = l.model_id ? (l.model_id + "（已配置，留空不改）") : "如 deepseek-v4-flash";
  $("llmBase").placeholder  = l.base_url ? l.base_url : "https://api.deepseek.com";
  $("jsonHint").textContent = l.json_override
    ? ("当前已存 " + l.json_bytes + " 字节，覆盖上面三项。留空 = 不修改。")
    : ("填了这个就完全覆盖上面三项，直接交给 agent 运行时。上限 " +
       ((c.limits && c.limits.models_json_max) || 3500) + " 字节。");
  showClr("clrLlmKey",   !!l.key_mask);
  showClr("clrLlmModel", !!l.model_id);
  showClr("clrLlmBase",  !!l.base_url);
  showClr("clrLlmJson",  !!l.json_override);
  showClr("clrVolcApp", !!v.app_mask);
  showClr("clrVolcAk",  !!v.ak_mask);
  renderRadio(c.radio);
}

function loadConfig(cb){
  fetch("/api/config").then(function(r){ return r.json(); }).then(function(c){
    renderConfig(c); if(cb) cb(c);
  }).catch(function(){
    $("llmSt").textContent = "读取配置失败"; $("llmSt").className = "secst no";
    if(cb) cb(null);
  });
}

// 收集要提交的字段；无改动返回 null。
function collectConfig(){
  var parts = [];
  for(var i=0;i<CFG_FIELDS.length;i++){
    var f = CFG_FIELDS[i];
    if(cleared[f.name]){ parts.push(f.name + "="); continue; }
    var val = $(f.id).value.trim();
    if(val === "") continue;
    parts.push(f.name + "=" + encodeURIComponent(val));
  }
  return parts.length ? parts.join("&") : null;
}

function saveConfig(then){
  var raw = $("llmJson").value.trim();
  if(raw !== "" && !cleared["llm_json"]){
    try { JSON.parse(raw); } catch(e){ toast("models JSON 语法错误：" + e.message, 1); return; }
  }
  var radio = collectRadio();
  if(radio.err){ toast(radio.err, 1); return; }
  var parts = [];
  var fields = collectConfig();
  if(fields !== null) parts.push(fields);
  if(radio.body !== null) parts.push(radio.body);
  if(parts.length === 0){ toast("没有要保存的改动", 1); return; }
  var body = parts.join("&");
  var btns = [$("saveApply"), $("saveOnly")];
  btns.forEach(function(b){ b.disabled = true; });
  fetch("/api/config", {method:"POST", headers:{"Content-Type":"application/x-www-form-urlencoded"},
                        body:body})
    .then(function(r){ return r.json().then(function(j){ return {s:r.status, j:j}; },
                                           function(){ return {s:r.status, j:{}}; }); })
    .then(function(res){
      btns.forEach(function(b){ b.disabled = false; });
      if(res.s !== 200){
        var FLD = {llm_key:"API Key", llm_model:"模型", llm_base:"Base URL", llm_json:"models JSON",
                   volc_app:"App Key", volc_ak:"Access Key", radio_json:"电台列表（名称/地址/体积超限）"};
        toast("保存失败：" + (res.j.field ? ((FLD[res.j.field]||res.j.field) + " 内容不合法") : (res.j.error || res.s)), 1);
        return;
      }
      CFG_FIELDS.forEach(function(f){ $(f.id).value = ""; });
      resetClears();
      loadConfig();
      if(then) then(); else toast("已保存 · 重启后生效");
    }).catch(function(){
      btns.forEach(function(b){ b.disabled = false; });
      toast("保存失败", 1);
    });
}

$("saveOnly").onclick = function(){ saveConfig(null); };
$("saveApply").onclick = function(){
  saveConfig(function(){
    fetch("/api/config/apply", {method:"POST"}).then(function(r){ return r.json(); })
      .then(function(j){ toast(j.sim ? "已保存 · sim 需手动重启 pi_sim" : "已保存 · 设备正在重启…"); })
      .catch(function(){ toast("已保存 · 重启请求失败，请手动重启设备", 1); });
  });
};

// ---- init ----
// 默认 tab：没配全 → 配置（扫码引导进来的场景）；配全了 → 文件（快捷面板进来的场景）。
loadConfig(function(c){
  var h = location.hash;
  if(h === "#files"){ showTab("files"); return; }
  if(h === "#config"){ showTab("config"); return; }
  var done = c && c.llm && c.llm.configured && c.voice && c.voice.configured;
  showTab(done ? "files" : "config");
});
</script>
</body>
</html>)HTML";
}

}  // namespace web_admin_web
