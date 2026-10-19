/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HUMAN_DETECT_DASHBOARD_H
#define HUMAN_DETECT_DASHBOARD_H

static constexpr char kDashboardHtml[] = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>N32H787 人形检测</title>
<style>
:root{--ink:#17201e;--muted:#68736e;--line:#d7ddd8;--paper:#f3f5f2;--panel:#fff;--green:#0f7856;--green-soft:#e7f2ed;--amber:#a56318;--red:#b42318;--red-soft:#fbecea;--blue:#245f86}
*{box-sizing:border-box}
body{margin:0;background:var(--paper);color:var(--ink);font:14px/1.45 ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif}
header{background:#19352c;color:#fff;border-bottom:4px solid #6cb896}
.header-inner{max-width:1160px;margin:auto;padding:18px 24px;display:flex;align-items:center;justify-content:space-between;gap:16px}
h1{margin:0;font-size:20px;font-weight:650;letter-spacing:0}
.subtitle{margin:3px 0 0;color:#c7d7d0;font-size:12px}
.connection{display:inline-flex;align-items:center;gap:8px;border:1px solid #668378;border-radius:999px;padding:6px 10px;color:#e8f1ed;font-size:12px;white-space:nowrap}
.dot{width:7px;height:7px;border-radius:50%;background:#9daca5}
.connection.online .dot{background:#73d6aa;box-shadow:0 0 0 3px rgba(115,214,170,.16)}
.connection.offline .dot{background:#f28a7e}
main{max-width:1160px;margin:auto;padding:22px 24px 30px}
.layout{display:grid;grid-template-columns:minmax(0,1.65fr) minmax(300px,.85fr);gap:20px;align-items:start}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px}
.section-head{padding:14px 16px;display:flex;align-items:center;justify-content:space-between;gap:14px}
.eyebrow{display:block;color:var(--muted);font-size:10px;font-weight:700;letter-spacing:.1em;text-transform:uppercase}
h2{margin:3px 0 0;font-size:16px;font-weight:650;letter-spacing:0}
button{font:inherit;font-size:12px;font-weight:650;color:#174d3a;background:#edf5f1;border:1px solid #a8c5b7;border-radius:6px;padding:7px 11px;cursor:pointer}
button:hover{background:#e2eee8}
button:focus-visible{outline:2px solid var(--blue);outline-offset:2px}
.viewport{position:relative;aspect-ratio:4/3;background:#0b0f0d;border-top:1px solid var(--line);border-bottom:1px solid var(--line);overflow:hidden}
.viewport img{display:block;width:100%;height:100%;object-fit:contain}
.preview-note{position:absolute;inset:0;display:grid;place-items:center;padding:24px;color:#dce7e1;background:rgba(11,15,13,.86);text-align:center}
.preview-note[hidden]{display:none}
.stream-meta{padding:11px 16px;display:flex;flex-wrap:wrap;gap:8px 20px;color:var(--muted);font-size:12px}
.stream-state{color:var(--green);font-weight:650}
.side{display:grid;gap:14px}
.person-panel{padding:16px}
.person-state{margin-top:5px;font-size:25px;line-height:1.15;font-weight:700}
.person-state.clear{color:var(--green)}.person-state.alert{color:var(--red)}
.person-meta{display:flex;justify-content:space-between;gap:12px;margin-top:9px;color:var(--muted);font-size:12px}
.metrics{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}
.metric{background:var(--panel);border:1px solid var(--line);border-radius:7px;padding:12px;min-height:76px}
.metric-label{color:var(--muted);font-size:11px}
.metric-value{margin-top:5px;font-size:20px;font-weight:650;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
.metric-note{margin-top:3px;color:var(--muted);font-size:11px}
.lower{display:grid;grid-template-columns:1fr 1fr;gap:20px;margin-top:20px}
.lower .panel{padding:16px}
.box-list{display:grid;gap:7px;margin-top:12px}
.box-row{display:grid;grid-template-columns:28px 1fr auto;gap:10px;align-items:center;background:#f7f9f7;border:1px solid #e0e5e1;border-radius:5px;padding:8px 9px;font-size:12px;font-variant-numeric:tabular-nums}
.box-id{color:var(--muted)}.box-score{color:var(--blue);font-weight:650}
.empty{color:var(--muted);font-size:12px;margin-top:12px}
.counters{display:grid;gap:1px;margin-top:12px;background:var(--line);border:1px solid var(--line);border-radius:5px;overflow:hidden}
.counter-row{display:flex;justify-content:space-between;gap:16px;background:#fff;padding:8px 10px;font-size:12px}
.counter-row span:first-child{color:var(--muted)}.counter-row span:last-child{font-variant-numeric:tabular-nums;font-weight:600}
.counter-row.warn span:last-child{color:var(--red)}
footer{max-width:1160px;margin:auto;padding:0 24px 24px;color:var(--muted);font-size:11px;display:flex;justify-content:space-between;gap:16px;flex-wrap:wrap}
@media(max-width:820px){.layout,.lower{grid-template-columns:1fr}.side{grid-row:1}.metrics{grid-template-columns:repeat(4,minmax(0,1fr))}}
@media(max-width:560px){.header-inner{padding:15px 16px;flex-wrap:wrap}main{padding:16px}footer{padding:0 16px 18px}.metrics{grid-template-columns:repeat(2,minmax(0,1fr))}.section-head{align-items:flex-start}.subtitle{max-width:210px}}
</style>
</head>
<body>
<header>
  <div class="header-inner">
    <div>
      <h1>N32H787 人形检测</h1>
      <p class="subtitle">实时位置检测与相机预览</p>
    </div>
    <div id="connection" class="connection"><span class="dot"></span><span id="connectionText">连接中</span></div>
  </div>
</header>
<main>
  <div class="layout">
    <section class="panel">
      <div class="section-head">
        <div><span class="eyebrow">实时预览</span><h2>检测画面</h2></div>
        <button id="reconnect" type="button">重新连接</button>
      </div>
      <div class="viewport">
        <img id="preview" alt="实时人形检测画面">
        <div id="previewMessage" class="preview-note">正在连接相机</div>
      </div>
      <div class="stream-meta">
        <span>320 × 240</span><span>灰度图</span><span id="streamState" class="stream-state">连接中</span>
      </div>
    </section>

    <aside class="side">
      <section class="panel person-panel">
        <span class="eyebrow">人物状态</span>
        <div id="person" class="person-state clear">等待首帧</div>
        <div class="person-meta"><span id="boxCount">尚未出帧</span><span id="frameAge">无画面</span></div>
      </section>
      <div class="metrics">
        <article class="metric"><div class="metric-label">推理帧率</div><div id="fps" class="metric-value">--</div><div class="metric-note">滚动窗口</div></article>
        <article class="metric"><div class="metric-label">推理耗时</div><div id="inferTime" class="metric-value">--</div><div class="metric-note">上一帧</div></article>
        <article class="metric"><div class="metric-label">累计帧数</div><div id="frames" class="metric-value">--</div><div class="metric-note">已完成</div></article>
        <article class="metric"><div class="metric-label">运行时间</div><div id="uptime" class="metric-value">--</div><div class="metric-note">服务</div></article>
      </div>
    </aside>
  </div>

  <div class="lower">
    <section class="panel">
      <span class="eyebrow">检测输出</span><h2>当前目标框</h2>
      <div id="boxList" class="box-list"></div>
    </section>
    <section class="panel">
      <span class="eyebrow">服务状态</span><h2>计数器</h2>
      <div class="counters">
        <div class="counter-row"><span>相机</span><span id="camera">--</span></div>
        <div class="counter-row"><span>相机错误</span><span id="cameraErrors">--</span></div>
        <div class="counter-row"><span>推理错误</span><span id="inferenceErrors">--</span></div>
        <div class="counter-row"><span>预览错误</span><span id="previewErrors">--</span></div>
        <div class="counter-row"><span>预览帧数</span><span id="previewFrames">--</span></div>
        <div class="counter-row"><span>抓图 / 预览次数</span><span id="sessions">--</span></div>
      </div>
    </section>
  </div>
</main>
<footer><span id="updated">等待状态</span><span>human-detect</span></footer>

<script>
(function(){
  "use strict";
  var $=function(id){return document.getElementById(id);};
  var preview=$("preview");
  var stream="/preview.mjpg?fps=5";
  var retryTimer=0;
  var streamStarted=false;
  var cameraOnline=false;

  function formatDuration(ms){
    if(!Number.isFinite(ms)||ms<0) return "--";
    if(ms<1000) return Math.round(ms)+" 毫秒";
    if(ms<60000) return (ms/1000).toFixed(1)+" 秒";
    if(ms<3600000){var m=Math.floor(ms/60000);return m+" 分 "+Math.floor((ms%60000)/1000)+" 秒";}
    var h=Math.floor(ms/3600000);return h+" 小时 "+Math.floor((ms%3600000)/60000)+" 分";
  }
  function percent(v){return (v*100/65535).toFixed(1)+"%";}
  function startPreview(){
    clearTimeout(retryTimer);
    if(!cameraOnline) return;
    $("previewMessage").hidden=false;
    $("previewMessage").textContent="正在连接相机";
    $("streamState").textContent="连接中";
    preview.src=stream+"&t="+Date.now();
  }
  function retryPreview(){if(cameraOnline) retryTimer=setTimeout(startPreview,2500);}
  function renderBoxes(s){
    var list=$("boxList");
    while(list.firstChild) list.removeChild(list.firstChild);
    var boxes=Array.isArray(s.boxes)?s.boxes:[];
    $("boxCount").textContent=boxes.length?(boxes.length+" 个目标"):"未检测到人物";
    if(!boxes.length){var empty=document.createElement("div");empty.className="empty";empty.textContent="当前没有检测框";list.appendChild(empty);return;}
    boxes.forEach(function(b,i){
      var row=document.createElement("div");row.className="box-row";
      var id=document.createElement("span");id.className="box-id";id.textContent="#"+String(i+1);
      var area=document.createElement("span");area.textContent="x "+percent(b.x0)+" - "+percent(b.x1)+", y "+percent(b.y0)+" - "+percent(b.y1);
      var score=document.createElement("span");score.className="box-score";score.textContent=String(b.score);
      row.appendChild(id);row.appendChild(area);row.appendChild(score);list.appendChild(row);
    });
  }
  function render(s){
    var online=!!s.camera_ready;
    cameraOnline=online;
    $("connection").className="connection "+(online?"online":"offline");
    $("connectionText").textContent=online?"已连接":"相机不可用";
    $("camera").textContent=online?"在线":"离线";
    $("person").textContent=s.have_result===false?"等待首帧":(s.human_detected?"检测到人物":"画面无人");
    $("person").className="person-state "+(s.human_detected?"alert":"clear");
    $("fps").textContent=Number.isFinite(Number(s.inference_fps))?Number(s.inference_fps).toFixed(2):"--";
    $("inferTime").textContent=Number.isFinite(Number(s.inference_us))?Math.round(Number(s.inference_us)/100)/10+" ms":"--";
    $("frames").textContent=Number(s.frames||0).toLocaleString();
    $("uptime").textContent=formatDuration(Number(s.uptime_ms));
    $("frameAge").textContent=s.frames?formatDuration(s.uptime_ms-s.last_frame_ms)+" 前":"无画面";
    $("cameraErrors").textContent=s.camera_errors;
    $("inferenceErrors").textContent=s.inference_errors;
    $("previewErrors").textContent=s.preview_errors;
    $("previewFrames").textContent=s.preview_frames;
    $("sessions").textContent=s.snapshots+" / "+s.previews;
    $("cameraErrors").parentNode.classList.toggle("warn",Number(s.camera_errors)>0);
    $("inferenceErrors").parentNode.classList.toggle("warn",Number(s.inference_errors)>0);
    $("previewErrors").parentNode.classList.toggle("warn",Number(s.preview_errors)>0);
    renderBoxes(s);
    if(s.preview_active){$("previewMessage").hidden=true;$("streamState").textContent="串流中";}
    else if(streamStarted){$("streamState").textContent="等待重连";$("previewMessage").hidden=false;$("previewMessage").textContent="预览连接已断开";}
    else if(online && !preview.getAttribute("src")) startPreview();
    $("updated").textContent="状态更新于 "+new Date().toLocaleTimeString();
  }
  async function poll(){
    try{
      var response=await fetch("/status",{cache:"no-store"});
      if(!response.ok) throw new Error("HTTP "+response.status);
      render(await response.json());
    }catch(error){
      $("connection").className="connection offline";
      $("connectionText").textContent="状态离线";
      $("updated").textContent="无法获取状态";
    }
    setTimeout(poll,1000);
  }
  preview.addEventListener("load",function(){streamStarted=true;$("previewMessage").hidden=true;});
  preview.addEventListener("error",function(){
    $("previewMessage").hidden=false;
    $("previewMessage").textContent="预览暂不可用";
    $("streamState").textContent="已断开";
    streamStarted=false;
    retryPreview();
  });
  $("reconnect").addEventListener("click",function(){
    preview.removeAttribute("src");
    streamStarted=false;
    retryTimer=setTimeout(startPreview,500);
  });
  startPreview();
  poll();
}());
</script>
</body>
</html>)HTML";

#endif
