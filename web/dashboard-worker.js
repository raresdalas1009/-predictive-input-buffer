// Click Before Frames: online counter + website, in ONE file for the Cloudflare dashboard editor
// (made from src/worker.js and public/index.html; paste all of it as the worker's code).
//
//   POST /api/ping      the mod, every 90 s: { id, platform, version } (anonymous random id)
//   GET  /api/stats     the website: online / offline / total, by platform and version
//   GET  /auth/login    log in with Discord (OAuth2, "identify" only)
//   GET  /auth/callback Discord sends you back here
//   GET  /auth/logout
//   GET  /api/me        who is logged in (or null)
//   everything else     the website in public/

const ONLINE_FOR = 180;            // seconds since the last ping that still count as online
const VERSION_DAYS = 7;            // the versions list: games seen in the last week
const PLATFORMS = ["Windows", "Android", "iOS", "macOS", "Linux"];
const SESSION_DAYS = 30;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    try {
      if (url.pathname === "/api/ping" && request.method === "POST") return await ping(request, env);
      if (url.pathname === "/api/stats" && request.method === "GET") return await stats(env);
      if (url.pathname === "/api/me") return json(await currentUser(request, env));
      if (url.pathname === "/auth/login") return login(request, env);
      if (url.pathname === "/auth/callback") return await callback(request, env);
      if (url.pathname === "/auth/logout") return logout(request);
      if (url.pathname.startsWith("/api/") || url.pathname.startsWith("/auth/")) return json({ error: "not found" }, 404);
      return new Response(PAGE, { headers: { "Content-Type": "text/html; charset=utf-8" } });
    } catch (e) {
      return json({ error: "server error" }, 500);
    }
  },
};

// ---------- the mod's ping ----------

async function ping(request, env) {
  let body;
  try {
    body = await request.json();
  } catch {
    return json({ error: "bad json" }, 400);
  }
  const id = String(body.id || "");
  if (!/^[0-9a-f]{32}$/.test(id)) return json({ error: "bad id" }, 400);
  const platform = PLATFORMS.includes(body.platform) ? body.platform : "Other";
  const version = String(body.version || "?").replace(/[^0-9A-Za-z.\-+]/g, "").slice(0, 24) || "?";
  const now = Math.floor(Date.now() / 1000);
  await env.DB.prepare(
    `INSERT INTO players (id, platform, version, first_seen, last_seen) VALUES (?1, ?2, ?3, ?4, ?4)
     ON CONFLICT(id) DO UPDATE SET platform = ?2, version = ?3, last_seen = ?4`
  ).bind(id, platform, version, now).run();
  if (body.closing === true) {
    // the game is closing: not online any more
    await env.DB.prepare(`UPDATE players SET last_seen = ?2 WHERE id = ?1`).bind(id, now - ONLINE_FOR - 1).run();
  }
  const s = await counts(env, now);
  return json({ online: s.online, offline: s.offline, total: s.total });
}

// ---------- numbers for the website (and the mod's footer) ----------

async function counts(env, now) {
  const r = await env.DB.prepare(
    `SELECT COUNT(*) AS total, SUM(CASE WHEN last_seen > ?1 THEN 1 ELSE 0 END) AS online FROM players`
  ).bind(now - ONLINE_FOR).first();
  const total = r?.total || 0;
  const online = r?.online || 0;
  return { total, online, offline: total - online };
}

async function stats(env) {
  const now = Math.floor(Date.now() / 1000);
  const c = await counts(env, now);
  const plat = await env.DB.prepare(
    `SELECT platform, COUNT(*) AS total, SUM(CASE WHEN last_seen > ?1 THEN 1 ELSE 0 END) AS online
     FROM players GROUP BY platform ORDER BY total DESC`
  ).bind(now - ONLINE_FOR).all();
  const vers = await env.DB.prepare(
    `SELECT version, COUNT(*) AS players, SUM(CASE WHEN last_seen > ?1 THEN 1 ELSE 0 END) AS online
     FROM players WHERE last_seen > ?2 GROUP BY version ORDER BY players DESC LIMIT 12`
  ).bind(now - ONLINE_FOR, now - VERSION_DAYS * 86400).all();
  const users = await env.DB.prepare(`SELECT COUNT(*) AS n FROM users`).first();
  return json(
    {
      ...c,
      platforms: (plat.results || []).map((p) => ({ name: p.platform, online: p.online || 0, offline: p.total - (p.online || 0) })),
      versions: (vers.results || []).map((v) => ({ name: v.version, players: v.players, online: v.online || 0 })),
      discordUsers: users?.n || 0,
      discordInvite: env.DISCORD_INVITE || "",
      at: now,
    },
    200,
    { "Cache-Control": "public, max-age=15" }
  );
}

// ---------- Discord login ----------

function login(request, env) {
  const url = new URL(request.url);
  const state = randomHex(16);
  const redirect = `${url.origin}/auth/callback`;
  const q = new URLSearchParams({
    client_id: env.DISCORD_CLIENT_ID,
    response_type: "code",
    redirect_uri: redirect,
    scope: "identify",
    state,
    prompt: "none",
  });
  return new Response(null, {
    status: 302,
    headers: {
      Location: `https://discord.com/oauth2/authorize?${q}`,
      "Set-Cookie": cookie("cbf_state", state, 600),
    },
  });
}

async function callback(request, env) {
  const url = new URL(request.url);
  const code = url.searchParams.get("code");
  const state = url.searchParams.get("state");
  if (!code || !state || state !== readCookie(request, "cbf_state")) return redirectHome("login=failed");
  const tokenRes = await fetch("https://discord.com/api/oauth2/token", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({
      client_id: env.DISCORD_CLIENT_ID,
      client_secret: env.DISCORD_CLIENT_SECRET,
      grant_type: "authorization_code",
      code,
      redirect_uri: `${url.origin}/auth/callback`,
    }),
  });
  if (!tokenRes.ok) return redirectHome("login=failed");
  const token = await tokenRes.json();
  const meRes = await fetch("https://discord.com/api/users/@me", { headers: { Authorization: `Bearer ${token.access_token}` } });
  if (!meRes.ok) return redirectHome("login=failed");
  const me = await meRes.json();
  const name = me.global_name || me.username || "Discord user";
  const now = Math.floor(Date.now() / 1000);
  await env.DB.prepare(
    `INSERT INTO users (discord_id, name, avatar, first_login, last_login) VALUES (?1, ?2, ?3, ?4, ?4)
     ON CONFLICT(discord_id) DO UPDATE SET name = ?2, avatar = ?3, last_login = ?4`
  ).bind(me.id, name, me.avatar || null, now).run();
  // the Discord token isn't kept: the session only says who you are, signed so it can't be faked
  const session = await sign(env, { id: me.id, name, avatar: me.avatar || null, exp: now + SESSION_DAYS * 86400 });
  const headers = new Headers({ Location: "/" });
  headers.append("Set-Cookie", cookie("cbf_session", session, SESSION_DAYS * 86400));
  headers.append("Set-Cookie", cookie("cbf_state", "", 0));
  return new Response(null, { status: 302, headers });
}

function logout() {
  return new Response(null, { status: 302, headers: { Location: "/", "Set-Cookie": cookie("cbf_session", "", 0) } });
}

async function currentUser(request, env) {
  const raw = readCookie(request, "cbf_session");
  if (!raw) return null;
  const data = await verify(env, raw);
  if (!data || data.exp < Math.floor(Date.now() / 1000)) return null;
  const avatar = data.avatar ? `https://cdn.discordapp.com/avatars/${data.id}/${data.avatar}.png?size=64` : null;
  return { id: data.id, name: data.name, avatar };
}

// ---------- helpers ----------

function json(data, status = 200, extra = {}) {
  return new Response(JSON.stringify(data), { status, headers: { "Content-Type": "application/json", ...extra } });
}

function redirectHome(query) {
  return new Response(null, { status: 302, headers: { Location: `/?${query}` } });
}

function cookie(name, value, maxAge) {
  return `${name}=${value}; Path=/; Max-Age=${maxAge}; HttpOnly; Secure; SameSite=Lax`;
}

function readCookie(request, name) {
  const all = request.headers.get("Cookie") || "";
  for (const part of all.split(";")) {
    const [k, ...v] = part.trim().split("=");
    if (k === name) return v.join("=");
  }
  return null;
}

function randomHex(bytes) {
  const a = new Uint8Array(bytes);
  crypto.getRandomValues(a);
  return [...a].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function b64url(bytes) {
  let s = btoa(String.fromCharCode(...new Uint8Array(bytes)));
  return s.replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function unb64url(s) {
  s = s.replace(/-/g, "+").replace(/_/g, "/");
  while (s.length % 4) s += "=";
  return Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
}

async function hmacKey(env) {
  return crypto.subtle.importKey("raw", new TextEncoder().encode(env.SESSION_SECRET), { name: "HMAC", hash: "SHA-256" }, false, [
    "sign",
    "verify",
  ]);
}

async function sign(env, data) {
  const body = b64url(new TextEncoder().encode(JSON.stringify(data)));
  const mac = await crypto.subtle.sign("HMAC", await hmacKey(env), new TextEncoder().encode(body));
  return `${body}.${b64url(mac)}`;
}

async function verify(env, token) {
  const [body, mac] = token.split(".");
  if (!body || !mac) return null;
  const ok = await crypto.subtle.verify("HMAC", await hmacKey(env), unb64url(mac), new TextEncoder().encode(body));
  if (!ok) return null;
  try {
    return JSON.parse(new TextDecoder().decode(unb64url(body)));
  } catch {
    return null;
  }
}

// the website
const PAGE = "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>Click Before Frames</title>\n<meta name=\"description\" content=\"Click Before Frames: who's playing right now.\">\n<link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">\n<link href=\"https://fonts.googleapis.com/css2?family=Poppins:wght@400;600;800&display=swap\" rel=\"stylesheet\">\n<style>\n  :root {\n    --bg: #0c0a1a;\n    --bg2: #160f2e;\n    --card: rgba(255, 255, 255, 0.05);\n    --card-edge: rgba(190, 160, 255, 0.22);\n    --text: #f1eeff;\n    --muted: #a49fc4;\n    --accent: #a66bff;\n    --accent2: #5fd7ff;\n    --good: #66e09a;\n    --bar: rgba(255, 255, 255, 0.08);\n  }\n  * { box-sizing: border-box; }\n  html, body { margin: 0; }\n  html { background: var(--bg); }\n  [hidden] { display: none !important; }\n  body {\n    min-height: 100vh;\n    font-family: Poppins, system-ui, sans-serif;\n    color: var(--text);\n    background: radial-gradient(1200px 700px at 15% -10%, #3a1d78 0%, transparent 60%),\n                radial-gradient(900px 600px at 110% 20%, #123a5a 0%, transparent 55%),\n                linear-gradient(160deg, var(--bg2), var(--bg));\n    background-attachment: fixed;\n    overflow-x: hidden;\n  }\n  #stars { position: fixed; inset: 0; z-index: 0; pointer-events: none; }\n  .wrap { position: relative; z-index: 1; max-width: 1100px; margin: 0 auto; padding: 28px 16px 60px; }\n  header { display: flex; align-items: center; justify-content: space-between; gap: 12px; flex-wrap: wrap; margin-bottom: 26px; }\n  .brand { display: flex; align-items: center; gap: 12px; }\n  .logo { width: 42px; height: 42px; border-radius: 12px; background: linear-gradient(135deg, var(--accent), var(--accent2));\n          display: grid; place-items: center; font-weight: 800; font-size: 16px; color: #0c0a1a; box-shadow: 0 0 24px rgba(166, 107, 255, .45); }\n  .brand h1 { margin: 0; font-size: 20px; letter-spacing: .3px; }\n  .brand small { display: block; color: var(--muted); font-size: 12px; font-weight: 400; }\n  .actions { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }\n  .btn { display: inline-flex; align-items: center; gap: 8px; padding: 9px 16px; border-radius: 999px; font: 600 13px Poppins, sans-serif;\n         color: var(--text); text-decoration: none; border: 1px solid var(--card-edge); background: var(--card); cursor: pointer;\n         transition: transform .15s ease, background .2s ease, box-shadow .2s ease; }\n  .btn:hover { transform: translateY(-1px); background: rgba(255, 255, 255, .09); }\n  .btn.discord { background: #5865f2; border-color: #7983ff; box-shadow: 0 6px 20px rgba(88, 101, 242, .35); }\n  .btn.discord:hover { background: #4752e0; }\n  .me { display: inline-flex; align-items: center; gap: 8px; font-size: 13px; }\n  .me img { width: 28px; height: 28px; border-radius: 50%; border: 1px solid var(--card-edge); }\n  .grid3 { display: grid; grid-template-columns: repeat(3, 1fr); gap: 16px; }\n  .grid2 { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-top: 16px; }\n  .card { position: relative; padding: 20px 22px; border-radius: 20px; background: var(--card); border: 1px solid var(--card-edge);\n          backdrop-filter: blur(14px); -webkit-backdrop-filter: blur(14px); overflow: hidden;\n          opacity: 0; transform: translateY(14px); animation: rise .6s cubic-bezier(.2, .8, .2, 1) forwards; }\n  .card:nth-child(2) { animation-delay: .07s; } .card:nth-child(3) { animation-delay: .14s; }\n  .grid2 .card { animation-delay: .2s; } .grid2 .card + .card { animation-delay: .27s; }\n  @keyframes rise { to { opacity: 1; transform: none; } }\n  .card.hero { background: linear-gradient(135deg, rgba(166, 107, 255, .32), rgba(95, 215, 255, .10)); }\n  .label { display: flex; align-items: center; gap: 8px; color: var(--muted); font-size: 13px; font-weight: 600; }\n  .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--muted); }\n  .dot.live { background: var(--good); box-shadow: 0 0 0 0 rgba(102, 224, 154, .7); animation: pulse 2s infinite; }\n  .dot.purple { background: var(--accent); }\n  @keyframes pulse { 70% { box-shadow: 0 0 0 9px rgba(102, 224, 154, 0); } 100% { box-shadow: 0 0 0 0 rgba(102, 224, 154, 0); } }\n  .big { font-size: 46px; font-weight: 800; margin: 6px 0 2px; line-height: 1.1; font-variant-numeric: tabular-nums; }\n  .sub { color: var(--muted); font-size: 12.5px; }\n  .bar { height: 6px; border-radius: 6px; background: var(--bar); margin-top: 14px; overflow: hidden; }\n  .bar > i { display: block; height: 100%; width: 0; border-radius: 6px; background: linear-gradient(90deg, var(--accent), var(--accent2));\n             transition: width 1s cubic-bezier(.2, .8, .2, 1); }\n  h2 { margin: 0; font-size: 17px; }\n  .head { display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 4px; }\n  .head span { color: var(--muted); font-size: 12px; }\n  .row { margin-top: 16px; }\n  .row .top { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 14px; font-weight: 600; }\n  .row .top > span:last-child { text-align: right; flex-shrink: 0; }\n  .row .top span { color: var(--muted); font-size: 12px; font-weight: 400; }\n  .row .top b { color: var(--text); }\n  .badge { white-space: nowrap; font-style: normal; font-size: 10px; padding: 2px 8px; border-radius: 999px; margin-left: 6px; background: linear-gradient(90deg, var(--accent), #ff6fb0); color: #fff; }\n  .legend { display: flex; gap: 16px; margin-top: 16px; padding-top: 12px; border-top: 1px solid rgba(255, 255, 255, .07); color: var(--muted); font-size: 12px; }\n  .legend i { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; }\n  footer { margin-top: 28px; color: var(--muted); font-size: 12px; text-align: center; }\n  .empty { color: var(--muted); font-size: 13px; margin-top: 14px; }\n  @media (max-width: 760px) {\n    .grid3, .grid2 { grid-template-columns: 1fr; }\n    .big { font-size: 40px; }\n  }\n  @media (prefers-reduced-motion: reduce) {\n    .card { animation: none; opacity: 1; transform: none; }\n    .dot.live { animation: none; }\n  }\n</style>\n</head>\n<body>\n<canvas id=\"stars\"></canvas>\n<div class=\"wrap\">\n  <header>\n    <div class=\"brand\">\n      <div class=\"logo\">CBF</div>\n      <div><h1>Click Before Frames</h1><small>the input buffer for Geometry Dash</small></div>\n    </div>\n    <div class=\"actions\">\n      <a class=\"btn\" id=\"invite\" href=\"#\" target=\"_blank\" rel=\"noopener\" hidden>Join the Discord</a>\n      <span id=\"account\"></span>\n    </div>\n  </header>\n\n  <section class=\"grid3\">\n    <div class=\"card hero\">\n      <div class=\"label\"><span class=\"dot live\"></span>Online now</div>\n      <div class=\"big\" id=\"online\">\u2013</div>\n      <div class=\"sub\" id=\"onlinePct\">&nbsp;</div>\n      <div class=\"bar\"><i id=\"onlineBar\"></i></div>\n    </div>\n    <div class=\"card\">\n      <div class=\"label\"><span class=\"dot\"></span>Offline</div>\n      <div class=\"big\" id=\"offline\" style=\"color: var(--muted)\">\u2013</div>\n      <div class=\"sub\">Not playing right now</div>\n    </div>\n    <div class=\"card\">\n      <div class=\"label\"><span class=\"dot purple\"></span>Players</div>\n      <div class=\"big\" id=\"total\">\u2013</div>\n      <div class=\"sub\">Everyone who's ever opened Click Before Frames</div>\n    </div>\n  </section>\n\n  <section class=\"grid2\">\n    <div class=\"card\">\n      <div class=\"head\"><h2>Devices</h2><span id=\"devCount\"></span></div>\n      <div class=\"sub\">Who's playing, by device</div>\n      <div id=\"devices\"></div>\n      <div class=\"legend\"><span><i style=\"background: var(--good)\"></i>Online</span><span><i style=\"background: var(--muted)\"></i>Offline</span></div>\n    </div>\n    <div class=\"card\">\n      <div class=\"head\"><h2>Versions</h2><span id=\"verCount\"></span></div>\n      <div class=\"sub\">Which version this week's players are on</div>\n      <div id=\"versions\"></div>\n    </div>\n  </section>\n\n  <footer>Counts are anonymous: each game sends a random id, its device type and version every 90 seconds. Turn it off in the mod's settings.<br><span id=\"updated\"></span></footer>\n</div>\n\n<script>\n  const fmt = (n) => Number(n || 0).toLocaleString(\"en-US\");\n  const esc = (s) => String(s).replace(/[&<>\"']/g, (c) => ({ \"&\": \"&amp;\", \"<\": \"&lt;\", \">\": \"&gt;\", '\"': \"&quot;\", \"'\": \"&#39;\" }[c]));\n\n  // numbers count up to their new value\n  function countTo(el, value) {\n    const from = Number(el.dataset.v || 0);\n    el.dataset.v = value;\n    const start = performance.now();\n    const step = (t) => {\n      const k = Math.min(1, (t - start) / 700);\n      const e = 1 - Math.pow(1 - k, 3);\n      el.textContent = fmt(Math.round(from + (value - from) * e));\n      if (k < 1) requestAnimationFrame(step);\n    };\n    requestAnimationFrame(step);\n  }\n\n  function rows(list, render) {\n    return list.map(render).join(\"\") || '<div class=\"empty\">Nothing yet.</div>';\n  }\n\n  async function load() {\n    try {\n      const r = await fetch(\"/api/stats\");\n      if (!r.ok) throw new Error();\n      const s = await r.json();\n      countTo(document.getElementById(\"online\"), s.online);\n      countTo(document.getElementById(\"offline\"), s.offline);\n      countTo(document.getElementById(\"total\"), s.total);\n      const pct = s.total ? (s.online / s.total) * 100 : 0;\n      document.getElementById(\"onlinePct\").textContent = `${pct < 1 && s.online ? \"<1\" : Math.round(pct)}% of all players`;\n      document.getElementById(\"onlineBar\").style.width = `${Math.max(pct, s.online ? 1.5 : 0)}%`;\n\n      const maxDev = Math.max(1, ...s.platforms.map((p) => p.online + p.offline));\n      document.getElementById(\"devCount\").textContent = `${s.platforms.length} devices`;\n      document.getElementById(\"devices\").innerHTML = rows(s.platforms, (p) => `\n        <div class=\"row\"><div class=\"top\">${esc(p.name)}<span><b>${fmt(p.online)}</b> online \u00b7 ${fmt(p.offline)} offline</span></div>\n        <div class=\"bar\"><i style=\"width:${((p.online + p.offline) / maxDev) * 100}%\"></i></div></div>`);\n\n      const vTotal = s.versions.reduce((a, v) => a + v.players, 0) || 1;\n      document.getElementById(\"verCount\").textContent = `${s.versions.length} versions`;\n      document.getElementById(\"versions\").innerHTML = rows(s.versions, (v, i) => `\n        <div class=\"row\"><div class=\"top\"><span style=\"color:var(--text);font-size:14px;font-weight:600\">${esc(v.name)}${i === 0 ? '<em class=\"badge\">MOST USED</em>' : \"\"}</span>\n        <span><b>${fmt(v.players)}</b> player${v.players === 1 ? \"\" : \"s\"} \u00b7 ${Math.round((v.players / vTotal) * 100)}% \u00b7 ${fmt(v.online)} online</span></div>\n        <div class=\"bar\"><i style=\"width:${(v.players / vTotal) * 100}%\"></i></div></div>`);\n\n      const inv = document.getElementById(\"invite\");\n      if (s.discordInvite) { inv.href = s.discordInvite; inv.hidden = false; }\n      document.getElementById(\"updated\").textContent = `Updated ${new Date().toLocaleTimeString()}`;\n    } catch {\n      document.getElementById(\"updated\").textContent = \"Couldn't reach the server, trying again soon.\";\n    }\n  }\n\n  async function account() {\n    const box = document.getElementById(\"account\");\n    try {\n      const me = await (await fetch(\"/api/me\")).json();\n      if (me) {\n        box.innerHTML = `<span class=\"me\">${me.avatar ? `<img src=\"${esc(me.avatar)}\" alt=\"\">` : \"\"}${esc(me.name)}</span>\n          <a class=\"btn\" href=\"/auth/logout\">Log out</a>`;\n        return;\n      }\n    } catch {}\n    box.innerHTML = `<a class=\"btn discord\" href=\"/auth/login\">Log in with Discord</a>`;\n  }\n\n  // a slow star field\n  (function stars() {\n    const c = document.getElementById(\"stars\"), g = c.getContext(\"2d\");\n    const reduce = matchMedia(\"(prefers-reduced-motion: reduce)\").matches;\n    let w, h, list = [];\n    const size = () => {\n      w = c.width = innerWidth * devicePixelRatio; h = c.height = innerHeight * devicePixelRatio;\n      c.style.width = innerWidth + \"px\"; c.style.height = innerHeight + \"px\";\n      list = Array.from({ length: Math.round((innerWidth * innerHeight) / 9000) }, () => ({\n        x: Math.random() * w, y: Math.random() * h, r: (Math.random() * 1.2 + 0.3) * devicePixelRatio, t: Math.random() * 6.28, s: 0.6 + Math.random() }));\n    };\n    const draw = (time) => {\n      g.clearRect(0, 0, w, h);\n      for (const p of list) {\n        const a = 0.35 + 0.35 * Math.sin(p.t + time / 900 * p.s);\n        g.fillStyle = `rgba(230,225,255,${a})`;\n        g.beginPath(); g.arc(p.x, p.y, p.r, 0, 6.283); g.fill();\n      }\n      if (!reduce) requestAnimationFrame(draw);\n    };\n    addEventListener(\"resize\", size); size(); requestAnimationFrame(draw);\n  })();\n\n  if (new URLSearchParams(location.search).get(\"login\") === \"failed\") {\n    history.replaceState(null, \"\", \"/\");\n    alert(\"Discord login didn't go through. Try again.\");\n  }\n  account();\n  load();\n  setInterval(load, 30000);\n</script>\n</body>\n</html>\n";
