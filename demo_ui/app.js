const THEMES = [
  { id: 1,  name: "静默纸页", en: "Quiet Paper", note: "克制、温暖、留白", layout: "editorial" },
  { id: 2,  name: "黑曜指挥舱", en: "Obsidian Command", note: "高密度暗色控制台", layout: "command" },
  { id: 3,  name: "瑞士信号", en: "Swiss Signal", note: "网格、红色、强排版", layout: "swiss" },
  { id: 4,  name: "工业黄线", en: "Industrial Line", note: "硬朗、工具感、醒目", layout: "industrial" },
  { id: 5,  name: "极光网络", en: "Aurora Mesh", note: "流动光场与空间感", layout: "aurora" },
  { id: 6,  name: "桌面周刊", en: "Desktop Journal", note: "杂志式信息编排", layout: "journal" },
  { id: 7,  name: "柔软连接", en: "Soft Link", note: "亲和、低压力、圆润", layout: "soft" },
  { id: 8,  name: "绿色终端", en: "Green Terminal", note: "命令行与复古屏幕", layout: "terminal" },
  { id: 9,  name: "几何协作", en: "Bauhaus Link", note: "原色几何与模块", layout: "bauhaus" },
  { id: 10, name: "连接蓝图", en: "Connection Blueprint", note: "工程图纸与精密标注", layout: "blueprint" },
  { id: 11, name: "墨迹留白", en: "Ink & Air", note: "东方极简与安静状态", layout: "ink" },
  { id: 12, name: "像素工作站", en: "Pixel Workstation", note: "九十年代桌面趣味", layout: "pixel" },
  { id: 13, name: "珊瑚便当", en: "Coral Bento", note: "轻快模块与高可读性", layout: "bento" },
  { id: 14, name: "轨道控制", en: "Orbital Control", note: "航天仪表与任务视角", layout: "orbital" },
  { id: 15, name: "北境薄雾", en: "Nordic Mist", note: "冷静、轻盈、现代", layout: "nordic" },
  { id: 16, name: "翡翠电波", en: "Emerald Deco", note: "装饰艺术与精致质感", layout: "deco" },
  { id: 17, name: "直接连接", en: "Direct / Brutal", note: "超高对比与零装饰", layout: "brutal" },
  { id: 18, name: "日落工作室", en: "Sunset Studio", note: "温暖、生活化、有呼吸感", layout: "sunset" },
  { id: 19, name: "透明桌面", en: "Clear Desktop", note: "Windows 原生感的轻透明", layout: "fluent" },
  { id: 20, name: "信号画布", en: "Signal Canvas", note: "数据地图与自由空间", layout: "canvas" },
];

const icon = (name) => ({
  devices: '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="13" rx="2"/><path d="M8 21h8M12 17v4"/></svg>',
  assist: '<svg viewBox="0 0 24 24"><circle cx="6" cy="12" r="3"/><circle cx="18" cy="6" r="3"/><circle cx="18" cy="18" r="3"/><path d="m9 11 6-4M9 13l6 4"/></svg>',
  history: '<svg viewBox="0 0 24 24"><path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5M12 7v5l3 2"/></svg>',
  settings: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.9l.1.1-2.8 2.8-.1-.1a1.7 1.7 0 0 0-1.9-.3 1.7 1.7 0 0 0-1 1.6v.2h-4V21a1.7 1.7 0 0 0-1-1.6 1.7 1.7 0 0 0-1.9.3l-.1.1L4.2 17l.1-.1a1.7 1.7 0 0 0 .3-1.9A1.7 1.7 0 0 0 3 14H2.8v-4H3a1.7 1.7 0 0 0 1.6-1 1.7 1.7 0 0 0-.3-1.9L4.2 7 7 4.2l.1.1a1.7 1.7 0 0 0 1.9.3A1.7 1.7 0 0 0 10 3v-.2h4V3a1.7 1.7 0 0 0 1 1.6 1.7 1.7 0 0 0 1.9-.3l.1-.1L19.8 7l-.1.1a1.7 1.7 0 0 0-.3 1.9 1.7 1.7 0 0 0 1.6 1h.2v4H21a1.7 1.7 0 0 0-1.6 1Z"/></svg>',
  refresh: '<svg viewBox="0 0 24 24"><path d="M20 6v5h-5M4 18v-5h5"/><path d="M6.1 9a7 7 0 0 1 11.5-2.6L20 9M4 15l2.4 2.6A7 7 0 0 0 17.9 15"/></svg>',
}[name]);

const nav = () => `
  <nav class="nav-list">
    <button>${icon("devices")}<span>设备</span></button>
    <button class="active">${icon("assist")}<span>协助</span></button>
    <button>${icon("history")}<span>记录</span></button>
  </nav>`;

const device = (compact = false) => `<article class="device-card ${compact ? "compact" : ""}">
  <span class="device-symbol">▱</span>
  <div><strong>LAPTOP-LAN</strong><small>192.168.100.23 · 39000</small></div>
  <i></i><span class="online">在线</span><button>连接</button>
</article>`;

const manual = () => `<section class="manual-connect">
  <div class="section-label"><span>03</span><div><strong>通过地址连接</strong><small>适用于校园网或广播受限网络</small></div></div>
  <label><span>IP 地址</span><input value="192.168.100.112" /></label>
  <label class="port"><span>端口</span><input value="39000" /></label>
  <button class="primary">连接</button>
</section>`;

const localControls = () => `<div class="local-controls">
  <div class="role-switch"><button class="active">被控端</button><button>控制端</button></div>
  <label><span>监听端口</span><input value="39000" /></label>
  <button class="primary">开始监听</button>
  <p class="waiting"><i></i> 等待远程设备连接</p>
</div>`;

const radar = () => `<div class="radar" aria-hidden="true"><span></span><span></span><span></span><b>WRC</b><i></i><i></i><i></i></div>`;

function standardShell(theme, body, options = {}) {
  return `<div class="desktop theme-${theme.id} layout-${theme.layout}">
    <div class="titlebar"><span>${options.title || "winRemoteControl"}</span><div>—　□　×</div></div>
    ${body}
  </div>`;
}

function renderNordic(theme) {
  const side = `<aside class="sidebar"><div class="brand"><b>W</b><span>winRemote<br/>Control</span></div>${nav()}<div class="recent"><small>最近连接</small><p><i></i> DESKTOP-HOME</p><p><i class="off"></i> OFFICE-PC</p></div><button class="settings">${icon("settings")}<span>设置</span></button></aside>`;
  return standardShell(theme, `${side}<main class="workspace nordic-workspace">
    <header class="main-header"><div><span class="eyebrow">LOCAL CONNECTION / 01</span><h1>远程协助</h1><p>在同一网络中，让两台设备自然相连。</p></div><div class="nordic-state-switch"><button data-view="discovery">连接前</button><button data-view="connected">连接后</button></div></header>
    <section class="nordic-discovery"><div class="split"><section class="local-panel"><div class="panel-head"><span>01</span><h2>本机等待连接</h2></div>${radar()}${localControls()}</section><section class="devices-panel"><div class="panel-head"><span>02</span><h2>可用设备</h2><button class="icon-button">${icon("refresh")}</button></div>${device()}<p class="scanning">◌ 正在寻找同一局域网设备…</p></section></div>${manual()}</section>
    <section class="nordic-connected">
      <header class="session-heading"><div><span class="status-badge"><i></i> 已连接</span><h2>DESKTOP-EXAMPLE</h2><p>192.168.100.23 · 局域网直连</p></div><div class="session-latency"><small>当前延迟</small><strong>42 <em>ms</em></strong></div></header>
      <div class="session-layout">
        <article class="remote-preview">
          <div class="preview-screen"><div class="wallpaper-fold fold-a"></div><div class="wallpaper-fold fold-b"></div><div class="wallpaper-fold fold-c"></div><span class="desktop-icon">□<small>此电脑</small></span><span class="taskbar">⊞　⌕</span></div>
          <div class="preview-caption"><div><i></i><span><strong>实时画面预览</strong><small>1920 × 1080 · 60 FPS</small></span></div><button class="primary">进入桌面　→</button></div>
        </article>
        <aside class="session-panel"><span class="eyebrow">SESSION / LIVE</span><h3>会话详情</h3><dl><div><dt>连接方式</dt><dd>局域网直连</dd></div><div><dt>视频编码</dt><dd>H.264</dd></div><div><dt>控制权限</dt><dd>键盘与鼠标</dd></div><div><dt>在线时长</dt><dd>00:03:18</dd></div></dl><button class="quiet-button">画质设置</button><button class="danger-button">断开连接</button></aside>
      </div>
    </section>
  </main>`);
}

function renderTheme(t) {
  const sharedHeader = `<header class="main-header"><div><span class="eyebrow">LOCAL CONNECTION / 01</span><h1>远程协助</h1><p>在同一网络中，让两台设备自然相连。</p></div><div class="ready"><i></i>局域网已就绪</div></header>`;
  const side = `<aside class="sidebar"><div class="brand"><b>W</b><span>winRemote<br/>Control</span></div>${nav()}<div class="recent"><small>最近连接</small><p><i></i> DESKTOP-HOME</p><p><i class="off"></i> OFFICE-PC</p></div><button class="settings">${icon("settings")}<span>设置</span></button></aside>`;

  if (t.id === 15) return renderNordic(t);

  if (["editorial", "soft", "nordic", "fluent"].includes(t.layout)) {
    return standardShell(t, `${side}<main class="workspace">${sharedHeader}<div class="split"><section class="local-panel"><div class="panel-head"><span>01</span><h2>本机等待连接</h2></div>${radar()}${localControls()}</section><section class="devices-panel"><div class="panel-head"><span>02</span><h2>可用设备</h2><button class="icon-button">${icon("refresh")}</button></div>${device()}<p class="scanning">◌ 正在寻找同一局域网设备…</p></section></div>${manual()}</main>`);
  }

  if (["command", "aurora", "orbital", "terminal"].includes(t.layout)) {
    return standardShell(t, `<main class="command-shell"><header class="command-top"><div class="brand"><b>W</b><span>WRC / LOCAL NODE</span></div><div class="command-tabs"><button>概览</button><button class="active">远程协助</button><button>会话记录</button></div><div class="ready"><i></i>DISCOVERY ACTIVE</div></header><section class="command-grid"><aside class="status-rail"><small>LOCAL STATUS</small><strong>READY</strong><p>NODE 7A-31<br/>LAN / 39000</p><div class="pulse-orb"></div><button class="primary">开始监听</button></aside><section class="command-center"><div class="panel-head"><span>CONTROL SURFACE</span><h1>建立一条<br/>清晰的连接</h1></div>${radar()}<div class="role-switch"><button class="active">被控端</button><button>控制端</button></div></section><aside class="device-feed"><div class="panel-head"><h2>附近设备</h2><button class="icon-button">${icon("refresh")}</button></div>${device(true)}<div class="feed-line"><i></i><span>扫描局域网</span><b>LIVE</b></div><div class="feed-line"><i></i><span>信令端口</span><b>39000</b></div></aside></section>${manual()}</main>`);
  }

  if (["swiss", "journal", "ink", "brutal"].includes(t.layout)) {
    return standardShell(t, `<main class="poster-shell"><header class="poster-head"><div class="brand"><b>WRC</b><span>局域网远程桌面</span></div><div class="poster-nav">设备　 <b>协助</b>　记录　设置</div></header><section class="poster-lead"><div><span class="giant-no">01</span><p>REMOTE<br/>ASSISTANCE</p></div><h1>让两台电脑<br/><em>近在眼前。</em></h1><div class="ready"><i></i>局域网已就绪</div></section><section class="poster-columns"><article class="poster-local"><div class="section-label"><span>A</span><div><strong>本机等待连接</strong><small>允许另一台设备查看并控制此电脑</small></div></div>${localControls()}</article><article class="poster-devices"><div class="section-label"><span>B</span><div><strong>发现了一台设备</strong><small>来自当前局域网</small></div></div>${device()}</article></section>${manual()}</main>`);
  }

  if (["industrial", "blueprint", "pixel", "deco"].includes(t.layout)) {
    return standardShell(t, `<div class="tool-shell"><header class="tool-head"><div class="brand"><b>WRC</b><span>REMOTE LINK UTILITY</span></div><div>NET: <strong>ONLINE</strong>　/　PORT: 39000</div></header><aside class="tool-nav">${nav()}<button class="settings">${icon("settings")}<span>设置</span></button></aside><main class="tool-main"><div class="tool-title"><span>SESSION / NEW</span><h1>远程协助</h1><p>局域网连接工具</p></div><section class="tool-status"><div><small>当前角色</small><strong>被控端</strong></div><div><small>发现服务</small><strong>运行中</strong></div><div><small>活动会话</small><strong>0</strong></div></section><section class="tool-panels"><article><div class="panel-head"><h2>本机接入点</h2><span>01</span></div>${radar()}${localControls()}</article><article><div class="panel-head"><h2>网络设备</h2><span>02</span></div>${device()}<p class="scanning">扫描范围 / LOCAL NETWORK</p></article></section>${manual()}</main></div>`);
  }

  return standardShell(t, `<main class="free-shell"><header><div class="brand"><b>W</b><span>winRemoteControl</span></div><nav>设备　 <b>协助</b>　记录　设置</nav><div class="ready"><i></i>在线</div></header><section class="free-intro"><span>LOCAL / DIRECT / SAFE</span><h1>远程协助，<br/>简单一点。</h1><p>发现身边的设备，或通过地址直接连接。</p></section><section class="free-cards"><article class="free-local"><small>THIS DEVICE</small><h2>等待被连接</h2>${radar()}${localControls()}</article><article class="free-device"><small>NEARBY</small><h2>可用设备</h2>${device()}</article></section>${manual()}</main>`);
}

function initGallery() {
  const gallery = document.querySelector("#gallery");
  if (!gallery) return;
  gallery.innerHTML = THEMES.map(t => `<article class="gallery-card">
    <a class="thumb" href="preview.html?theme=${t.id}"><iframe src="preview.html?theme=${t.id}&embed=1" title="${t.name}" loading="lazy" tabindex="-1"></iframe><span class="open-hint">打开预览 ↗</span></a>
    <div class="card-meta"><span>${String(t.id).padStart(2, "0")}</span><div><h2>${t.name}</h2><p>${t.en} · ${t.note}</p></div></div>
  </article>`).join("");
}

function initPreview() {
  const app = document.querySelector("#app");
  if (!app) return;
  const params = new URLSearchParams(location.search);
  const id = Math.min(20, Math.max(1, Number(params.get("theme")) || 1));
  const theme = THEMES[id - 1];
  app.innerHTML = renderTheme(theme);
  document.title = `${String(id).padStart(2, "0")} ${theme.name} · WRC`;
  document.querySelector("#previewIndex").textContent = String(id).padStart(2, "0");
  document.querySelector("#previewName").textContent = `${theme.name} / ${theme.en}`;
  if (params.get("embed") === "1") document.body.classList.add("embedded");
  if (id === 15) {
    const setNordicState = state => {
      const desktop = document.querySelector(".theme-15");
      if (!desktop) return;
      desktop.classList.toggle("show-connected", state === "connected");
      desktop.querySelectorAll("[data-view]").forEach(button => button.classList.toggle("active", button.dataset.view === state));
    };
    setNordicState(params.get("state") === "connected" ? "connected" : "discovery");
    document.querySelectorAll("[data-view]").forEach(button => button.addEventListener("click", () => setNordicState(button.dataset.view)));
    document.querySelector(".nordic-discovery .device-card button")?.addEventListener("click", () => setNordicState("connected"));
  }
  const go = delta => location.href = `preview.html?theme=${((id - 1 + delta + 20) % 20) + 1}`;
  document.querySelector("#prevTheme").onclick = () => go(-1);
  document.querySelector("#nextTheme").onclick = () => go(1);
  addEventListener("keydown", e => { if (e.key === "ArrowLeft") go(-1); if (e.key === "ArrowRight") go(1); });
  document.querySelectorAll("button").forEach(button => button.addEventListener("click", () => {
    if (button.closest(".preview-toolbar")) return;
    button.classList.add("clicked");
    setTimeout(() => button.classList.remove("clicked"), 300);
  }));
}

initGallery();
initPreview();
