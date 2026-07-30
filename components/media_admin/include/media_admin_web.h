// media_admin_web.h — SD 音乐后台单页应用（内嵌 HTML/CSS/JS，零外部依赖）。
// 视觉对齐设备 AmberGlow：深底 #141210 + 卡片 + 1px 描边 + amber #FFAE1F 点睛。
// 功能：目录浏览 / 面包屑 / 建目录 / 删除（含递归确认）/ 上传（多选 + 目录选择 +
// 拖拽文件夹）串行队列 / 409 冲突选择（覆盖·跳过·全部覆盖·全部跳过）/ 失败重试。

#pragma once

namespace media_admin_web {

inline const char* Html() {
    return R"HTML(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pinion · 文件管理</title>
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
</style>
</head>
<body>
  <h1>Pinion <span class="mono" style="color:var(--faint);font-size:13px">/ 文件管理</span></h1>
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

// ---- init ----
renderCrumbs(); loadList(); loadSpace();
</script>
</body>
</html>)HTML";
}

}  // namespace media_admin_web
