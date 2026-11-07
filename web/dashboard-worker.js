// Click Before Frames: online counter + website, in ONE file for the Cloudflare dashboard editor
// (made from src/worker.js and public/index.html; paste all of it as the worker's code).
//
//   POST /api/ping      the mod, every 90 s: { id, platform, version } (anonymous random id)
//   GET  /api/stats     the website: online / offline / total, by platform and version
//   GET  /auth/login    log in with Discord (OAuth2, "identify" only)
//   GET  /auth/callback Discord sends you back here
//   GET  /auth/logout
//   GET  /api/me        who is logged in (or null)
//   POST /api/runs      the global brain: a run autoplay finished (level, its fingerprint, TPS, inputs)
//   GET  /api/runs      the best shared run of a level (most confirmed)
//   GET  /api/latest    the newest mod version uploaded on the admin page
//   GET  /download/latest   that file
//   GET  /admin, POST /api/admin/upload   upload a new version (your Discord account only)
//   POST /api/link/start, GET /link, POST /api/link/confirm, GET /api/link/poll   link a game to your Discord
//   GET  /api/account   a linked game's stats (by its random id)
//   everything else     the website in public/

// v3.0.2: the free D1 plan allows 100k rows written and 5M read a day. A game's row is written at
// most every 9 minutes (its pings in between change nothing), so "online" = seen in the last 11
// minutes; the numbers are counted at most once a minute and shared from Cloudflare's cache.
const ONLINE_FOR = 660;            // seconds since the last write that still count as online
const WRITE_EVERY = 540;           // a game's row is written at most this often
const COUNT_CACHE = 60;            // the counts and the stats are recounted at most this often
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
      if (url.pathname === "/api/link/start" && request.method === "POST") return await linkStart(request, env);
      if (url.pathname === "/link") return await linkPage(request, env);
      if (url.pathname === "/api/link/confirm" && request.method === "POST") return await linkConfirm(request, env);
      if (url.pathname === "/api/link/poll") return await linkPoll(url, env);
      if (url.pathname === "/api/account") return await account(url, env);
      if (url.pathname === "/api/runs" && request.method === "POST") return await runUpload(request, env);
      if (url.pathname === "/api/runs" && request.method === "GET") return await runGet(url, env);
      if (url.pathname === "/api/beaten") return await beatenList(env);
      if (url.pathname === "/api/latest") return await latest(env);
      if (url.pathname === "/download/latest") return await download(env);
      if (url.pathname === "/admin") return await adminPage(request, env);
      if (url.pathname === "/api/admin/upload" && request.method === "POST") return await adminUpload(request, env);
      if (url.pathname.startsWith("/api/") || url.pathname.startsWith("/auth/")) return json({ error: "not found" }, 404);
      return new Response(PAGE, { headers: { "Content-Type": "text/html; charset=utf-8" } });
    } catch (e) {
      return json({ error: "server error", detail: String((e && e.message) || e).slice(0, 300) }, 500);
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
  // written only when the last write is old (or the version changed): most pings write nothing
  await env.DB.prepare(
    `INSERT INTO players (id, platform, version, first_seen, last_seen) VALUES (?1, ?2, ?3, ?4, ?4)
     ON CONFLICT(id) DO UPDATE SET platform = ?2, version = ?3, last_seen = ?4
     WHERE players.last_seen < ?5 OR players.version != ?3`
  ).bind(id, platform, version, now, now - WRITE_EVERY).run();
  const s = await cachedCounts(env, now);
  return json({ online: s.online, offline: s.offline, total: s.total });
}

// ---------- numbers for the website (and the mod's footer) ----------

// counted at most once a minute; everyone gets the cached numbers in between
async function cachedCounts(env, now) {
  const key = new Request("https://cache.cbf/counts");
  const cache = caches.default;
  const hit = await cache.match(key);
  if (hit) return await hit.json();
  const c = await counts(env, now);
  await cache.put(key, new Response(JSON.stringify(c), { headers: { "Cache-Control": `max-age=${COUNT_CACHE}` } }));
  return c;
}

async function counts(env, now) {
  const r = await env.DB.prepare(
    `SELECT COUNT(*) AS total, SUM(CASE WHEN last_seen > ?1 THEN 1 ELSE 0 END) AS online FROM players`
  ).bind(now - ONLINE_FOR).first();
  const total = r?.total || 0;
  const online = r?.online || 0;
  return { total, online, offline: total - online };
}

async function stats(env) {
  const key = new Request("https://cache.cbf/stats");
  const hit = await caches.default.match(key);
  if (hit) return new Response(hit.body, { headers: { "Content-Type": "application/json", "Cache-Control": "public, max-age=15" } });
  const res = await statsFresh(env);
  await caches.default.put(key, new Response(await res.clone().text(), { headers: { "Cache-Control": `max-age=${COUNT_CACHE}` } }));
  return res;
}

async function statsFresh(env) {
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
  let beaten = { n: 0 }, runsN = { n: 0 }, recent = { results: [] };
  try {
    beaten = await env.DB.prepare(`SELECT COUNT(DISTINCT level_id) AS n FROM runs`).first();
    runsN = await env.DB.prepare(`SELECT COUNT(*) AS n, COALESCE(SUM(confirms), 0) AS c FROM runs`).first();
    recent = await env.DB.prepare(
      `SELECT level_id, MAX(level_name) AS name, MAX(created) AS at, SUM(confirms) AS c FROM runs GROUP BY level_id ORDER BY at DESC LIMIT 10`
    ).all();
  } catch {}
  return json(
    {
      ...c,
      platforms: (plat.results || []).map((p) => ({ name: p.platform, online: p.online || 0, offline: p.total - (p.online || 0) })),
      versions: (vers.results || []).map((v) => ({ name: v.version, players: v.players, online: v.online || 0 })),
      discordUsers: users?.n || 0,
      levelsBeaten: beaten?.n || 0,
      runsShared: runsN?.n || 0,
      runsConfirmed: runsN?.c || 0,
      recentBeaten: (recent.results || []).map((r) => ({ id: r.level_id, name: r.name || `Level ${r.level_id}`, at: r.at, confirms: r.c })),
      discordInvite: env.DISCORD_INVITE || "https://discord.gg/hSMYGcfcR",
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
  const next = (url.searchParams.get("next") || "/").startsWith("/") ? url.searchParams.get("next") || "/" : "/";
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
      "Set-Cookie": cookie("cbf_state", `${state}.${b64url(new TextEncoder().encode(next.slice(0, 200)))}`, 600),
    },
  });
}

async function callback(request, env) {
  const url = new URL(request.url);
  const code = url.searchParams.get("code");
  const state = url.searchParams.get("state");
  const saved = (readCookie(request, "cbf_state") || "").split(".");
  if (!code || !state || state !== saved[0]) return redirectHome("login=failed");
  let next = "/";
  try {
    next = saved[1] ? new TextDecoder().decode(unb64url(saved[1])) : "/";
  } catch {}
  if (!next.startsWith("/") || next.startsWith("//")) next = "/";
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
  const headers = new Headers({ Location: next });
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

// ---------- the global brain: runs autoplay finished ----------
// A run is a list of [step, down] button changes from the level's start, for one level version
// (fingerprint) and TPS. The same run finished again by another game counts as a confirmation; the
// most confirmed run of a level is the one handed out.

const MAX_INPUTS = 40000;

async function sha256(text) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function runUpload(request, env) {
  let b;
  try {
    b = await request.json();
  } catch {
    return json({ error: "bad json" }, 400);
  }
  const level = Number(b.level);
  const tps = Number(b.tps);
  const hash = String(b.hash || "").replace(/[^0-9a-f]/g, "").slice(0, 32);
  const inputs = b.inputs;
  if (!Number.isInteger(level) || level <= 0 || !Number.isInteger(tps) || tps < 60 || tps > 2000 || hash.length < 8) {
    return json({ error: "bad run" }, 400);
  }
  if (!Array.isArray(inputs) || inputs.length === 0 || inputs.length > MAX_INPUTS) return json({ error: "bad inputs" }, 400);
  let last = -1;
  for (const i of inputs) {
    if (!Array.isArray(i) || i.length !== 2 || !Number.isInteger(i[0]) || i[0] < last || (i[1] !== 0 && i[1] !== 1)) {
      return json({ error: "bad inputs" }, 400);
    }
    last = i[0];
  }
  const text = JSON.stringify(inputs);
  const key = await sha256(`${level}|${hash}|${tps}|${text}`);
  const name = String(b.name || "").replace(/[\u0000-\u001f<>]/g, "").slice(0, 60);
  const version = String(b.version || "?").replace(/[^0-9A-Za-z.\-+]/g, "").slice(0, 24);
  const player = /^[0-9a-f]{32}$/.test(String(b.id || "")) ? String(b.id) : null;
  const now = Math.floor(Date.now() / 1000);
  if (player) {
    // who finished it (for "your stats"): one row per game and run
    try {
      await env.DB.prepare(`INSERT OR IGNORE INTO finishes (player_id, run_key, level_id, at) VALUES (?1, ?2, ?3, ?4)`).bind(player, key, level, now).run();
    } catch {}
  }
  const had = await env.DB.prepare(`SELECT id FROM runs WHERE run_key = ?1`).bind(key).first();
  if (had) {
    await env.DB.prepare(`UPDATE runs SET confirms = confirms + 1, last_confirm = ?2 WHERE id = ?1`).bind(had.id, now).run();
    return json({ ok: true, confirmed: true });
  }
  await env.DB.prepare(
    `INSERT INTO runs (run_key, level_id, level_hash, level_name, tps, steps, inputs, version, created, last_confirm, confirms)
     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?9, 1)`
  ).bind(key, level, hash, name, tps, last, text, version, now).run();
  if (player) {
    try {
      await env.DB.prepare(`INSERT OR IGNORE INTO run_owners (run_key, player_id) VALUES (?1, ?2)`).bind(key, player).run();
    } catch {}
  }
  return json({ ok: true, confirmed: false });
}

async function runGet(url, env) {
  const level = Number(url.searchParams.get("level"));
  const tps = Number(url.searchParams.get("tps"));
  const hash = String(url.searchParams.get("hash") || "").replace(/[^0-9a-f]/g, "").slice(0, 32);
  if (!Number.isInteger(level) || level <= 0) return json({ error: "bad level" }, 400);
  const r = await env.DB.prepare(
    `SELECT level_id, level_hash, tps, steps, inputs, confirms, version FROM runs
     WHERE level_id = ?1 AND level_hash = ?2 AND tps = ?3 ORDER BY confirms DESC, last_confirm DESC LIMIT 1`
  ).bind(level, hash, tps).first();
  if (!r) {
    // v3.0.2: what the brain has for this level instead (other TPS, other versions), so the game can say why
    let others = { tps: [], versions: 0 };
    try {
      const t = await env.DB.prepare(`SELECT DISTINCT tps FROM runs WHERE level_id = ?1 AND level_hash = ?2`).bind(level, hash).all();
      const v = await env.DB.prepare(`SELECT COUNT(DISTINCT level_hash) AS n FROM runs WHERE level_id = ?1 AND level_hash != ?2`).bind(level, hash).first();
      others = { tps: (t.results || []).map((x) => x.tps), versions: v?.n || 0 };
    } catch {}
    return json({ found: false, others });
  }
  return json({ found: true, level: r.level_id, tps: r.tps, steps: r.steps, confirms: r.confirms, version: r.version, inputs: JSON.parse(r.inputs) });
}

// ---------- updates: the newest version, uploaded on /admin ----------

async function latest(env) {
  if (!env.FILES) return json({ version: null });
  const meta = await env.FILES.get("latest-meta", "json");
  if (!meta) return json({ version: null });
  return json({ version: meta.version, notes: meta.notes || "", size: meta.size, url: "/download/latest" }, 200, { "Cache-Control": "public, max-age=60" });
}

async function download(env) {
  if (!env.FILES) return json({ error: "no file" }, 404);
  const meta = await env.FILES.get("latest-meta", "json");
  const file = await env.FILES.get("latest-file", "arrayBuffer");
  if (!file || !meta) return json({ error: "no file" }, 404);
  return new Response(file, {
    headers: {
      "Content-Type": "application/octet-stream",
      "Content-Disposition": `attachment; filename="basu.input-buffer-${meta.version}.geode"`,
    },
  });
}

async function isAdmin(request, env) {
  const me = await currentUser(request, env);
  return me && env.ADMIN_DISCORD_ID && me.id === String(env.ADMIN_DISCORD_ID) ? me : null;
}

async function adminPage(request, env) {
  const me = await isAdmin(request, env);
  const body = !me
    ? `<p>Only the mod's owner can upload. <a href="/auth/login">Log in with Discord</a> first${env.ADMIN_DISCORD_ID ? "" : " (ADMIN_DISCORD_ID isn't set on the worker yet)"}.</p>`
    : `<form method="post" action="/api/admin/upload" enctype="multipart/form-data">
         <label>Version (like v3.0.1)<br><input name="version" required pattern="v?[0-9]+\\.[0-9]+\\.[0-9]+.*"></label><br><br>
         <label>What's new (shown in game)<br><textarea name="notes" rows="4" cols="50"></textarea></label><br><br>
         <label>The .geode file<br><input type="file" name="file" accept=".geode" required></label><br><br>
         <button>Upload</button>
       </form>`;
  const meta = env.FILES ? await env.FILES.get("latest-meta", "json") : null;
  return new Response(
    `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>CBF admin</title>
     <body style="font-family:system-ui;background:#0c0a1a;color:#eee;padding:24px;max-width:640px;margin:auto">
     <h1>Click Before Frames: upload a version</h1>
     <p>Now online: <b>${meta ? meta.version : "nothing yet"}</b></p>${body}<p><a href="/" style="color:#a66bff">Back to the site</a></p></body>`,
    { headers: { "Content-Type": "text/html; charset=utf-8" } }
  );
}

async function adminUpload(request, env) {
  if (!(await isAdmin(request, env))) return json({ error: "not allowed" }, 403);
  if (!env.FILES) return json({ error: "the FILES storage isn't connected to the worker" }, 500);
  const form = await request.formData();
  let version = String(form.get("version") || "").trim();
  if (!/^v?\d+\.\d+\.\d+/.test(version)) return json({ error: "bad version" }, 400);
  if (!version.startsWith("v")) version = "v" + version;
  const file = form.get("file");
  if (!file || typeof file === "string" || file.size < 1000 || file.size > 24 * 1024 * 1024) return json({ error: "bad file" }, 400);
  const data = await file.arrayBuffer();
  await env.FILES.put("latest-file", data);
  await env.FILES.put("latest-meta", JSON.stringify({ version, notes: String(form.get("notes") || "").slice(0, 500), size: data.byteLength, at: Date.now() }));
  return new Response(null, { status: 302, headers: { Location: "/admin" } });
}

// ---------- linking a game to Discord ----------
// The game asks for a code, opens /link?code=… in the browser; you log in with Discord there and
// press Link; the game asks every few seconds whether it's linked. A code lasts 10 minutes.

const LINK_MINUTES = 10;

async function linkStart(request, env) {
  let b;
  try {
    b = await request.json();
  } catch {
    return json({ error: "bad json" }, 400);
  }
  const id = String(b.id || "");
  if (!/^[0-9a-f]{32}$/.test(id)) return json({ error: "bad id" }, 400);
  const alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  const bytes = new Uint8Array(6);
  crypto.getRandomValues(bytes);
  const code = [...bytes].map((x) => alphabet[x % alphabet.length]).join("");
  const now = Math.floor(Date.now() / 1000);
  await env.DB.prepare(`DELETE FROM link_codes WHERE created < ?1`).bind(now - LINK_MINUTES * 60).run();
  await env.DB.prepare(`INSERT INTO link_codes (code, player_id, created) VALUES (?1, ?2, ?3)`).bind(code, id, now).run();
  const origin = new URL(request.url).origin;
  return json({ code, url: `${origin}/link?code=${code}`, minutes: LINK_MINUTES });
}

async function linkCode(env, code) {
  if (!/^[A-Z0-9]{6}$/.test(code || "")) return null;
  const now = Math.floor(Date.now() / 1000);
  return await env.DB.prepare(`SELECT code, player_id, discord_id, created FROM link_codes WHERE code = ?1 AND created > ?2`)
    .bind(code, now - LINK_MINUTES * 60).first();
}

async function linkPage(request, env) {
  const url = new URL(request.url);
  const code = String(url.searchParams.get("code") || "").toUpperCase();
  const row = await linkCode(env, code);
  const me = await currentUser(request, env);
  let body;
  if (!row) {
    body = `<p>This code isn't valid any more. Press <b>Link with Discord</b> in the game again.</p>`;
  } else if (row.discord_id) {
    body = `<p>Done! Your game is linked. You can close this page and go back to Geometry Dash.</p>`;
  } else if (!me) {
    body = `<p>Link your game to your Discord account. First log in:</p>
      <p><a class="btn" href="/auth/login?next=${encodeURIComponent("/link?code=" + code)}">Log in with Discord</a></p>`;
  } else {
    body = `<p>Link the game with code <b>${code}</b> to <b>${escapeHtml(me.name)}</b>?</p>
      <form method="post" action="/api/link/confirm"><input type="hidden" name="code" value="${code}"><button class="btn">Link</button></form>`;
  }
  return new Response(
    `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Link your game</title>
     <style>body{font-family:system-ui;background:#0c0a1a;color:#eee;padding:24px;max-width:560px;margin:auto}
     .btn{display:inline-block;background:#5865f2;color:#fff;border:0;border-radius:999px;padding:10px 18px;font-size:15px;text-decoration:none;cursor:pointer}
     a{color:#a66bff}</style>
     <h1>Click Before Frames</h1>${body}<p><a href="/">Back to the site</a></p>`,
    { headers: { "Content-Type": "text/html; charset=utf-8" } }
  );
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

async function linkConfirm(request, env) {
  const me = await currentUser(request, env);
  const form = await request.formData();
  const code = String(form.get("code") || "").toUpperCase();
  const row = await linkCode(env, code);
  if (!me || !row) return new Response(null, { status: 302, headers: { Location: `/link?code=${code}` } });
  await env.DB.prepare(`UPDATE link_codes SET discord_id = ?2 WHERE code = ?1`).bind(code, me.id).run();
  await env.DB.prepare(`UPDATE players SET discord_id = ?2 WHERE id = ?1`).bind(row.player_id, me.id).run();
  return new Response(null, { status: 302, headers: { Location: `/link?code=${code}` } });
}

async function linkPoll(url, env) {
  const row = await linkCode(env, String(url.searchParams.get("code") || "").toUpperCase());
  if (!row) return json({ linked: false, expired: true });
  if (!row.discord_id) return json({ linked: false });
  const u = await env.DB.prepare(`SELECT name FROM users WHERE discord_id = ?1`).bind(row.discord_id).first();
  return json({ linked: true, name: u?.name || "" });
}

// a game's stats: who it's linked to, what its autoplay beat and shared
async function account(url, env) {
  const id = String(url.searchParams.get("id") || "");
  if (!/^[0-9a-f]{32}$/.test(id)) return json({ error: "bad id" }, 400);
  const p = await env.DB.prepare(`SELECT first_seen, discord_id FROM players WHERE id = ?1`).bind(id).first();
  if (!p) return json({ linked: false });
  let user = null;
  if (p.discord_id) {
    user = await env.DB.prepare(`SELECT discord_id, name, avatar FROM users WHERE discord_id = ?1`).bind(p.discord_id).first();
  }
  // everything the games linked to the same Discord account did
  const ids = p.discord_id
    ? (await env.DB.prepare(`SELECT id FROM players WHERE discord_id = ?1`).bind(p.discord_id).all()).results.map((r) => r.id)
    : [id];
  const marks = ids.map((_, i) => `?${i + 1}`).join(",");
  let beaten = 0, finishes = 0, shared = 0, othersUsed = 0, recent = [];
  try {
    const f = await env.DB.prepare(`SELECT COUNT(DISTINCT level_id) AS lv, COUNT(*) AS n FROM finishes WHERE player_id IN (${marks})`).bind(...ids).first();
    beaten = f?.lv || 0;
    finishes = f?.n || 0;
    const o = await env.DB.prepare(
      `SELECT COUNT(*) AS n, COALESCE(SUM(r.confirms - 1), 0) AS used FROM run_owners o JOIN runs r ON r.run_key = o.run_key WHERE o.player_id IN (${marks})`
    ).bind(...ids).first();
    shared = o?.n || 0;
    othersUsed = o?.used || 0;
    recent = (await env.DB.prepare(
      `SELECT f.level_id AS id, MAX(r.level_name) AS name, MAX(f.at) AS at FROM finishes f LEFT JOIN runs r ON r.run_key = f.run_key
       WHERE f.player_id IN (${marks}) GROUP BY f.level_id ORDER BY at DESC LIMIT 5`
    ).bind(...ids).all()).results;
  } catch {}
  return json({
    linked: !!user,
    name: user?.name || "",
    avatar: user?.avatar ? `https://cdn.discordapp.com/avatars/${user.discord_id}/${user.avatar}.png?size=64` : null,
    since: p.first_seen,
    games: ids.length,
    levelsBeaten: beaten,
    finishes,
    runsShared: shared,
    othersFinishedWithYours: othersUsed,
    recent: recent.map((r) => ({ id: r.id, name: r.name || `Level ${r.id}` })),
  });
}

// every level autoplay has finished 0-100% (the site's list), counted at most once a minute
async function beatenList(env) {
  const key = new Request("https://cache.cbf/beaten");
  const hit = await caches.default.match(key);
  if (hit) return new Response(hit.body, { headers: { "Content-Type": "application/json", "Cache-Control": "public, max-age=30" } });
  let rows = [];
  try {
    rows = (await env.DB.prepare(
      `SELECT level_id AS id, MAX(level_name) AS name, SUM(confirms) AS finishes, MAX(last_confirm) AS at
       FROM runs GROUP BY level_id ORDER BY at DESC LIMIT 2000`
    ).all()).results || [];
  } catch {}
  const body = JSON.stringify({ levels: rows.map((r) => ({ id: r.id, name: r.name || `Level ${r.id}`, finishes: r.finishes || 1, at: r.at })) });
  await caches.default.put(key, new Response(body, { headers: { "Cache-Control": "max-age=60" } }));
  return new Response(body, { headers: { "Content-Type": "application/json", "Cache-Control": "public, max-age=30" } });
}

// the website
const PAGE = "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>Click Before Frames</title>\n<meta name=\"description\" content=\"Click Before Frames: who's playing right now.\">\n<link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">\n<link href=\"https://fonts.googleapis.com/css2?family=Poppins:wght@400;600;800&display=swap\" rel=\"stylesheet\">\n<style>\n  :root {\n    --bg: #0c0a1a;\n    --bg2: #160f2e;\n    --card: rgba(255, 255, 255, 0.05);\n    --card-edge: rgba(190, 160, 255, 0.22);\n    --text: #f1eeff;\n    --muted: #a49fc4;\n    --accent: #a66bff;\n    --accent2: #5fd7ff;\n    --good: #66e09a;\n    --bar: rgba(255, 255, 255, 0.08);\n  }\n  * { box-sizing: border-box; }\n  html, body { margin: 0; }\n  html { background: var(--bg); }\n  [hidden] { display: none !important; }\n  body {\n    min-height: 100vh;\n    font-family: Poppins, system-ui, sans-serif;\n    color: var(--text);\n    background: radial-gradient(1200px 700px at 15% -10%, #3a1d78 0%, transparent 60%),\n                radial-gradient(900px 600px at 110% 20%, #123a5a 0%, transparent 55%),\n                linear-gradient(160deg, var(--bg2), var(--bg));\n    background-attachment: fixed;\n    overflow-x: hidden;\n  }\n  #stars { position: fixed; inset: 0; z-index: 0; pointer-events: none; }\n  .wrap { position: relative; z-index: 1; max-width: 1100px; margin: 0 auto; padding: 28px 16px 60px; }\n  header { display: flex; align-items: center; justify-content: space-between; gap: 12px; flex-wrap: wrap; margin-bottom: 26px; }\n  .brand { display: flex; align-items: center; gap: 12px; }\n  .logo { width: 42px; height: 42px; border-radius: 12px; background: linear-gradient(135deg, var(--accent), var(--accent2));\n          display: grid; place-items: center; font-weight: 800; font-size: 16px; color: #0c0a1a; box-shadow: 0 0 24px rgba(166, 107, 255, .45); }\n  .brand h1 { margin: 0; font-size: 20px; letter-spacing: .3px; }\n  .brand small { display: block; color: var(--muted); font-size: 12px; font-weight: 400; }\n  .actions { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }\n  .btn { display: inline-flex; align-items: center; gap: 8px; padding: 9px 16px; border-radius: 999px; font: 600 13px Poppins, sans-serif;\n         color: var(--text); text-decoration: none; border: 1px solid var(--card-edge); background: var(--card); cursor: pointer;\n         transition: transform .15s ease, background .2s ease, box-shadow .2s ease; }\n  .btn:hover { transform: translateY(-1px); background: rgba(255, 255, 255, .09); }\n  .btn.discord { background: #5865f2; border-color: #7983ff; box-shadow: 0 6px 20px rgba(88, 101, 242, .35); }\n  .btn.discord:hover { background: #4752e0; }\n  .me { display: inline-flex; align-items: center; gap: 8px; font-size: 13px; }\n  .me img { width: 28px; height: 28px; border-radius: 50%; border: 1px solid var(--card-edge); }\n  .grid3 { display: grid; grid-template-columns: repeat(3, 1fr); gap: 16px; }\n  .grid2 { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-top: 16px; }\n  .card { position: relative; padding: 20px 22px; border-radius: 20px; background: var(--card); border: 1px solid var(--card-edge);\n          backdrop-filter: blur(14px); -webkit-backdrop-filter: blur(14px); overflow: hidden;\n          opacity: 0; transform: translateY(14px); animation: rise .6s cubic-bezier(.2, .8, .2, 1) forwards; }\n  .card:nth-child(2) { animation-delay: .07s; } .card:nth-child(3) { animation-delay: .14s; }\n  .grid2 .card { animation-delay: .2s; } .grid2 .card + .card { animation-delay: .27s; }\n  @keyframes rise { to { opacity: 1; transform: none; } }\n  .card.hero { background: linear-gradient(135deg, rgba(166, 107, 255, .32), rgba(95, 215, 255, .10)); }\n  .label { display: flex; align-items: center; gap: 8px; color: var(--muted); font-size: 13px; font-weight: 600; }\n  .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--muted); }\n  .dot.live { background: var(--good); box-shadow: 0 0 0 0 rgba(102, 224, 154, .7); animation: pulse 2s infinite; }\n  .dot.purple { background: var(--accent); }\n  @keyframes pulse { 70% { box-shadow: 0 0 0 9px rgba(102, 224, 154, 0); } 100% { box-shadow: 0 0 0 0 rgba(102, 224, 154, 0); } }\n  .big { font-size: 46px; font-weight: 800; margin: 6px 0 2px; line-height: 1.1; font-variant-numeric: tabular-nums; }\n  .sub { color: var(--muted); font-size: 12.5px; }\n  .bar { height: 6px; border-radius: 6px; background: var(--bar); margin-top: 14px; overflow: hidden; }\n  .bar > i { display: block; height: 100%; width: 0; border-radius: 6px; background: linear-gradient(90deg, var(--accent), var(--accent2));\n             transition: width 1s cubic-bezier(.2, .8, .2, 1); }\n  h2 { margin: 0; font-size: 17px; }\n  .head { display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 4px; }\n  .head span { color: var(--muted); font-size: 12px; }\n  .row { margin-top: 16px; }\n  .row .top { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 14px; font-weight: 600; }\n  .row .top > span:last-child { text-align: right; flex-shrink: 0; }\n  .row .top span { color: var(--muted); font-size: 12px; font-weight: 400; }\n  .row .top b { color: var(--text); }\n  .badge { white-space: nowrap; font-style: normal; font-size: 10px; padding: 2px 8px; border-radius: 999px; margin-left: 6px; background: linear-gradient(90deg, var(--accent), #ff6fb0); color: #fff; }\n  .legend { display: flex; gap: 16px; margin-top: 16px; padding-top: 12px; border-top: 1px solid rgba(255, 255, 255, .07); color: var(--muted); font-size: 12px; }\n  .legend i { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; }\n  .thanks { margin-bottom: 16px; display: flex; align-items: center; justify-content: space-between; gap: 16px; flex-wrap: wrap; }\n  .thanks .msg { font-size: 15px; font-weight: 600; }\n  .thanks .msg span { color: var(--muted); font-weight: 400; display: block; font-size: 13px; margin-top: 2px; }\n  .levels { margin-top: 16px; }\n  .levels input { width: 100%; margin-top: 12px; padding: 10px 14px; border-radius: 12px; border: 1px solid var(--card-edge); background: rgba(0, 0, 0, .25);\n                  color: var(--text); font: 14px Poppins, sans-serif; outline: none; }\n  .levels input:focus { border-color: var(--accent); }\n  .lvl-list { margin-top: 10px; max-height: 420px; overflow-y: auto; display: grid; grid-template-columns: repeat(auto-fill, minmax(240px, 1fr)); gap: 8px; }\n  .lvl { padding: 10px 12px; border-radius: 12px; background: rgba(255, 255, 255, .04); border: 1px solid rgba(255, 255, 255, .06); }\n  .lvl b { display: block; font-size: 14px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }\n  .lvl span { color: var(--muted); font-size: 12px; }\n  .empty { color: var(--muted); font-size: 13px; margin-top: 14px; }\n  @media (max-width: 760px) {\n    .grid3, .grid2 { grid-template-columns: 1fr; }\n    .big { font-size: 40px; }\n  }\n  @media (prefers-reduced-motion: reduce) {\n    .card { animation: none; opacity: 1; transform: none; }\n    .dot.live { animation: none; }\n  }\n</style>\n</head>\n<body>\n<canvas id=\"stars\"></canvas>\n<div class=\"wrap\">\n  <header>\n    <div class=\"brand\">\n      <div class=\"logo\">CBF</div>\n      <div><h1>Click Before Frames</h1><small>the input buffer for Geometry Dash</small></div>\n    </div>\n    <div class=\"actions\">\n      <a class=\"btn\" id=\"invite\" href=\"#\" target=\"_blank\" rel=\"noopener\" hidden>Join the Discord</a>\n      <span id=\"account\"></span>\n    </div>\n  </header>\n\n  <section class=\"card thanks\">\n    <div class=\"msg\">\ud83d\udc9c Thanks for using Click Before Frames!<span>Every level your autoplay beats helps everyone's autoplay get better.</span></div>\n    <a class=\"btn discord\" id=\"joinDiscord\" href=\"#\" target=\"_blank\" rel=\"noopener\" hidden>Join our Discord</a>\n  </section>\n\n  <section class=\"grid3\">\n    <div class=\"card hero\">\n      <div class=\"label\"><span class=\"dot live\"></span>Online now</div>\n      <div class=\"big\" id=\"online\">\u2013</div>\n      <div class=\"sub\" id=\"onlinePct\">&nbsp;</div>\n      <div class=\"bar\"><i id=\"onlineBar\"></i></div>\n    </div>\n    <div class=\"card\">\n      <div class=\"label\"><span class=\"dot\"></span>Offline</div>\n      <div class=\"big\" id=\"offline\" style=\"color: var(--muted)\">\u2013</div>\n      <div class=\"sub\">Not playing right now</div>\n    </div>\n    <div class=\"card\">\n      <div class=\"label\"><span class=\"dot purple\"></span>Players</div>\n      <div class=\"big\" id=\"total\">\u2013</div>\n      <div class=\"sub\">Everyone who's ever opened Click Before Frames</div>\n    </div>\n  </section>\n\n  <section class=\"grid2\">\n    <div class=\"card\">\n      <div class=\"head\"><h2>Devices</h2><span id=\"devCount\"></span></div>\n      <div class=\"sub\">Who's playing, by device</div>\n      <div id=\"devices\"></div>\n      <div class=\"legend\"><span><i style=\"background: var(--good)\"></i>Online</span><span><i style=\"background: var(--muted)\"></i>Offline</span></div>\n    </div>\n    <div class=\"card\">\n      <div class=\"head\"><h2>Versions</h2><span id=\"verCount\"></span></div>\n      <div class=\"sub\">Which version this week's players are on</div>\n      <div id=\"versions\"></div>\n    </div>\n  </section>\n\n  <section class=\"grid2\">\n    <div class=\"card hero\">\n      <div class=\"label\"><span class=\"dot purple\"></span>Autoplay brain</div>\n      <div class=\"big\" id=\"beaten\">\u2013</div>\n      <div class=\"sub\">Levels autoplay has beaten from 0% to 100%</div>\n      <div class=\"sub\" id=\"runs\" style=\"margin-top:8px\">&nbsp;</div>\n    </div>\n    <div class=\"card\">\n      <div class=\"head\"><h2>Recently beaten</h2><span>shared runs</span></div>\n      <div class=\"sub\">Every game's autoplay learns these runs</div>\n      <div id=\"recent\"></div>\n    </div>\n  </section>\n\n  <section class=\"card levels\">\n    <div class=\"head\"><h2>Every level beaten by autoplay</h2><span id=\"lvlCount\"></span></div>\n    <div class=\"sub\">All the levels autoplay has finished from 0% to 100% so far</div>\n    <input id=\"lvlSearch\" type=\"search\" placeholder=\"Search a level name or ID\">\n    <div class=\"lvl-list\" id=\"lvlList\"></div>\n  </section>\n</div>\n\n<script>\n  const fmt = (n) => Number(n || 0).toLocaleString(\"en-US\");\n  const esc = (s) => String(s).replace(/[&<>\"']/g, (c) => ({ \"&\": \"&amp;\", \"<\": \"&lt;\", \">\": \"&gt;\", '\"': \"&quot;\", \"'\": \"&#39;\" }[c]));\n\n  // numbers count up to their new value\n  function countTo(el, value) {\n    const from = Number(el.dataset.v || 0);\n    el.dataset.v = value;\n    const start = performance.now();\n    const step = (t) => {\n      const k = Math.min(1, (t - start) / 700);\n      const e = 1 - Math.pow(1 - k, 3);\n      el.textContent = fmt(Math.round(from + (value - from) * e));\n      if (k < 1) requestAnimationFrame(step);\n    };\n    requestAnimationFrame(step);\n  }\n\n  function rows(list, render) {\n    return list.map(render).join(\"\") || '<div class=\"empty\">Nothing yet.</div>';\n  }\n\n  async function load() {\n    try {\n      const r = await fetch(\"/api/stats\");\n      if (!r.ok) throw new Error();\n      const s = await r.json();\n      countTo(document.getElementById(\"online\"), s.online);\n      countTo(document.getElementById(\"offline\"), s.offline);\n      countTo(document.getElementById(\"total\"), s.total);\n      const pct = s.total ? (s.online / s.total) * 100 : 0;\n      document.getElementById(\"onlinePct\").textContent = `${pct < 1 && s.online ? \"<1\" : Math.round(pct)}% of all players`;\n      document.getElementById(\"onlineBar\").style.width = `${Math.max(pct, s.online ? 1.5 : 0)}%`;\n\n      const maxDev = Math.max(1, ...s.platforms.map((p) => p.online + p.offline));\n      document.getElementById(\"devCount\").textContent = `${s.platforms.length} devices`;\n      document.getElementById(\"devices\").innerHTML = rows(s.platforms, (p) => `\n        <div class=\"row\"><div class=\"top\">${esc(p.name)}<span><b>${fmt(p.online)}</b> online \u00b7 ${fmt(p.offline)} offline</span></div>\n        <div class=\"bar\"><i style=\"width:${((p.online + p.offline) / maxDev) * 100}%\"></i></div></div>`);\n\n      const vTotal = s.versions.reduce((a, v) => a + v.players, 0) || 1;\n      document.getElementById(\"verCount\").textContent = `${s.versions.length} versions`;\n      document.getElementById(\"versions\").innerHTML = rows(s.versions, (v, i) => `\n        <div class=\"row\"><div class=\"top\"><span style=\"color:var(--text);font-size:14px;font-weight:600\">${esc(v.name)}${i === 0 ? '<em class=\"badge\">MOST USED</em>' : \"\"}</span>\n        <span><b>${fmt(v.players)}</b> player${v.players === 1 ? \"\" : \"s\"} \u00b7 ${Math.round((v.players / vTotal) * 100)}% \u00b7 ${fmt(v.online)} online</span></div>\n        <div class=\"bar\"><i style=\"width:${(v.players / vTotal) * 100}%\"></i></div></div>`);\n\n      countTo(document.getElementById(\"beaten\"), s.levelsBeaten || 0);\n      document.getElementById(\"runs\").textContent = `${fmt(s.runsShared || 0)} runs shared \u00b7 ${fmt(s.runsConfirmed || 0)} finishes`;\n      document.getElementById(\"recent\").innerHTML = rows(s.recentBeaten || [], (r) => `\n        <div class=\"row\"><div class=\"top\">${esc(r.name)}<span>id ${r.id} \u00b7 ${fmt(r.confirms)} finish${r.confirms === 1 ? \"\" : \"es\"}</span></div></div>`);\n      const inv = document.getElementById(\"invite\");\n      if (s.discordInvite) {\n        inv.href = s.discordInvite; inv.hidden = false;\n        const j = document.getElementById(\"joinDiscord\"); j.href = s.discordInvite; j.hidden = false;\n      }\n\n    } catch {\n\n    }\n  }\n\n  async function account() {\n    const box = document.getElementById(\"account\");\n    try {\n      const me = await (await fetch(\"/api/me\")).json();\n      if (me) {\n        box.innerHTML = `<span class=\"me\">${me.avatar ? `<img src=\"${esc(me.avatar)}\" alt=\"\">` : \"\"}${esc(me.name)}</span>\n          <a class=\"btn\" href=\"/auth/logout\">Log out</a>`;\n        return;\n      }\n    } catch {}\n    box.innerHTML = `<a class=\"btn discord\" href=\"/auth/login\">Log in with Discord</a>`;\n  }\n\n  // a slow star field\n  (function stars() {\n    const c = document.getElementById(\"stars\"), g = c.getContext(\"2d\");\n    const reduce = matchMedia(\"(prefers-reduced-motion: reduce)\").matches;\n    let w, h, list = [];\n    const size = () => {\n      w = c.width = innerWidth * devicePixelRatio; h = c.height = innerHeight * devicePixelRatio;\n      c.style.width = innerWidth + \"px\"; c.style.height = innerHeight + \"px\";\n      list = Array.from({ length: Math.round((innerWidth * innerHeight) / 9000) }, () => ({\n        x: Math.random() * w, y: Math.random() * h, r: (Math.random() * 1.2 + 0.3) * devicePixelRatio, t: Math.random() * 6.28, s: 0.6 + Math.random() }));\n    };\n    const draw = (time) => {\n      g.clearRect(0, 0, w, h);\n      for (const p of list) {\n        const a = 0.35 + 0.35 * Math.sin(p.t + time / 900 * p.s);\n        g.fillStyle = `rgba(230,225,255,${a})`;\n        g.beginPath(); g.arc(p.x, p.y, p.r, 0, 6.283); g.fill();\n      }\n      if (!reduce) requestAnimationFrame(draw);\n    };\n    addEventListener(\"resize\", size); size(); requestAnimationFrame(draw);\n  })();\n\n  if (new URLSearchParams(location.search).get(\"login\") === \"failed\") {\n    history.replaceState(null, \"\", \"/\");\n    alert(\"Discord login didn't go through. Try again.\");\n  }\n  let allLevels = [];\n  function showLevels() {\n    const q = document.getElementById(\"lvlSearch\").value.trim().toLowerCase();\n    const list = allLevels.filter((l) => !q || l.name.toLowerCase().includes(q) || String(l.id).includes(q));\n    document.getElementById(\"lvlList\").innerHTML = list.map((l) => `\n      <div class=\"lvl\"><b>${esc(l.name)}</b><span>ID ${l.id} \u00b7 ${fmt(l.finishes)} finish${l.finishes === 1 ? \"\" : \"es\"}</span></div>`).join(\"\")\n      || '<div class=\"empty\">No levels here yet.</div>';\n  }\n  async function loadLevels() {\n    try {\n      const r = await (await fetch(\"/api/beaten\")).json();\n      allLevels = r.levels || [];\n      document.getElementById(\"lvlCount\").textContent = `${fmt(allLevels.length)} levels`;\n      showLevels();\n    } catch {}\n  }\n  document.getElementById(\"lvlSearch\").addEventListener(\"input\", showLevels);\n  loadLevels();\n  setInterval(loadLevels, 120000);\n  account();\n  load();\n  setInterval(load, 30000);\n</script>\n</body>\n</html>\n";
