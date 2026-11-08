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
//   GET  /api/runs      the best shared run of a level (most replayed to the end, fewest fails)
//   POST /api/runs/result  a game replayed a brain run: to the end, or it kept dying
//   GET  /api/latest    the newest mod version uploaded on the admin page
//   GET  /download/latest   that file
//   GET  /admin, POST /api/admin/upload   upload a new version (your Discord account only)
//   POST /api/link/start, GET /link, POST /api/link/confirm, GET /api/link/poll   link a game to your Discord
//   GET  /api/account   a linked game's stats (by its random id)
//   GET  /api/demons    the CBF demonlist; POST /api/admin/demons, /api/admin/demons/delete (admin only)
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
      if (url.pathname === "/api/me") {
        const me = await currentUser(request, env);
        return json(me ? { ...me, admin: !!env.ADMIN_DISCORD_ID && me.id === String(env.ADMIN_DISCORD_ID) } : null);
      }
      if (url.pathname === "/api/demons") return await demonsList(env);
      if (url.pathname === "/icon.png") return icon();
      if (url.pathname === "/api/admin/demons" && request.method === "POST") return await demonSave(request, env);
      if (url.pathname === "/api/admin/demons/delete" && request.method === "POST") return await demonDelete(request, env);
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
      if (url.pathname === "/api/runs/result" && request.method === "POST") return await runResult(request, env);
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
      completions: await completions(env),
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
  // v3.1.1: where the player was (every 3 steps and at each click): [step, x, y, rotation(, y2, rotation2)]
  let fixText = null;
  if (Array.isArray(b.fixes) && b.fixes.length > 0) {
    if (b.fixes.length > 40000) return json({ error: "bad fixes" }, 400);
    let lastStep = -1;
    for (const f of b.fixes) {
      if (!Array.isArray(f) || (f.length !== 4 && f.length !== 6) || !f.every((v) => typeof v === "number" && Number.isFinite(v)) ||
          !Number.isInteger(f[0]) || f[0] < lastStep) {
        return json({ error: "bad fixes" }, 400);
      }
      lastStep = f[0];
    }
    fixText = JSON.stringify(b.fixes);
  }
  await ensureResults(env);
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
    await env.DB.prepare(`UPDATE runs SET confirms = confirms + 1, last_confirm = ?2, fixes = COALESCE(fixes, ?3) WHERE id = ?1`).bind(had.id, now, fixText).run();
    return json({ ok: true, confirmed: true });
  }
  await env.DB.prepare(
    `INSERT INTO runs (run_key, level_id, level_hash, level_name, tps, steps, inputs, version, created, last_confirm, confirms, fixes)
     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?9, 1, ?10)`
  ).bind(key, level, hash, name, tps, last, text, version, now, fixText).run();
  if (player) {
    try {
      await env.DB.prepare(`INSERT OR IGNORE INTO run_owners (run_key, player_id) VALUES (?1, ?2)`).bind(key, player).run();
    } catch {}
  }
  return json({ ok: true, confirmed: false });
}

// v3.1.1: runs with position records come first (a run of clicks alone drifts off in another game;
// v3.1.0 games also shared runs a Hyperbolus macro's records carried), then the ones others replayed
// to the end, and runs that keep failing go last. A run of clicks alone that failed more often than
// it was replayed to the end isn't handed out at all.
// (Before, every run had 1 "finish" and the newest one was handed out, even one that never worked
// for anybody else.) The columns are added to an older database on first use.
let resultsReady = false;
async function ensureResults(env) {
  if (resultsReady) return;
  try { await env.DB.prepare(`ALTER TABLE runs ADD COLUMN fails INTEGER NOT NULL DEFAULT 0`).run(); } catch {}
  try { await env.DB.prepare(`ALTER TABLE runs ADD COLUMN replays INTEGER NOT NULL DEFAULT 0`).run(); } catch {}
  try { await env.DB.prepare(`ALTER TABLE runs ADD COLUMN fixes TEXT`).run(); } catch {}
  try {
    await env.DB.prepare(
      `CREATE TABLE IF NOT EXISTS run_results (run_key TEXT NOT NULL, player_id TEXT NOT NULL, ok INTEGER NOT NULL, at INTEGER NOT NULL, PRIMARY KEY (run_key, player_id))`
    ).run();
  } catch {}
  resultsReady = true;
}

async function runGet(url, env) {
  await ensureResults(env);
  const level = Number(url.searchParams.get("level"));
  const tps = Number(url.searchParams.get("tps"));
  const hash = String(url.searchParams.get("hash") || "").replace(/[^0-9a-f]/g, "").slice(0, 32);
  if (!Number.isInteger(level) || level <= 0) return json({ error: "bad level" }, 400);
  // runs this game already tried and failed with
  const skip = String(url.searchParams.get("skip") || "")
    .split(",")
    .map((k) => k.replace(/[^0-9a-f]/g, ""))
    .filter((k) => k.length === 64)
    .slice(0, 20);
  const holes = skip.map((_, i) => `?${i + 4}`).join(",");
  const r = await env.DB.prepare(
    `SELECT run_key, level_id, level_hash, tps, steps, inputs, confirms, replays, fails, version, fixes FROM runs
     WHERE level_id = ?1 AND level_hash = ?2 AND tps = ?3 AND NOT (fixes IS NULL AND fails > replays) ${skip.length ? `AND run_key NOT IN (${holes})` : ""}
     ORDER BY (fixes IS NOT NULL) DESC, (replays + confirms - fails) DESC, replays DESC, fails ASC, created ASC LIMIT 1`
  ).bind(level, hash, tps, ...skip).first();
  if (!r) {
    // v3.0.2: what the brain has for this level instead (other TPS, other versions), so the game can say why
    let others = { tps: [], versions: 0, tried: skip.length };
    try {
      const t = await env.DB.prepare(`SELECT DISTINCT tps FROM runs WHERE level_id = ?1 AND level_hash = ?2`).bind(level, hash).all();
      const v = await env.DB.prepare(`SELECT COUNT(DISTINCT level_hash) AS n FROM runs WHERE level_id = ?1 AND level_hash != ?2`).bind(level, hash).first();
      others = { tps: (t.results || []).map((x) => x.tps), versions: v?.n || 0, tried: skip.length };
    } catch {}
    return json({ found: false, others });
  }
  return json({
    found: true, key: r.run_key, level: r.level_id, tps: r.tps, steps: r.steps, confirms: r.confirms,
    replays: r.replays || 0, fails: r.fails || 0, version: r.version, inputs: JSON.parse(r.inputs),
    fixes: r.fixes ? JSON.parse(r.fixes) : [],
  });
}

// v3.1.1: a game played a brain run: to the end (ok) or it kept dying (not ok). One result a game a run.
async function runResult(request, env) {
  await ensureResults(env);
  let b;
  try {
    b = await request.json();
  } catch {
    return json({ error: "bad json" }, 400);
  }
  const key = String(b.key || "").replace(/[^0-9a-f]/g, "");
  const player = /^[0-9a-f]{32}$/.test(String(b.id || "")) ? String(b.id) : null;
  if (key.length !== 64 || !player) return json({ error: "bad result" }, 400);
  const ok = b.ok ? 1 : 0;
  const now = Math.floor(Date.now() / 1000);
  const had = await env.DB.prepare(`SELECT ok FROM run_results WHERE run_key = ?1 AND player_id = ?2`).bind(key, player).first();
  if (had && had.ok === ok) return json({ ok: true, counted: false });
  await env.DB.prepare(
    `INSERT INTO run_results (run_key, player_id, ok, at) VALUES (?1, ?2, ?3, ?4)
     ON CONFLICT (run_key, player_id) DO UPDATE SET ok = ?3, at = ?4`
  ).bind(key, player, ok, now).run();
  // a game that failed it before and now finished it (or the other way): the old result comes off
  const col = ok ? "replays" : "fails";
  const back = had ? (ok ? ", fails = MAX(fails - 1, 0)" : ", replays = MAX(replays - 1, 0)") : "";
  await env.DB.prepare(`UPDATE runs SET ${col} = ${col} + 1${back} WHERE run_key = ?1`).bind(key).run();
  if (ok) {
    try {
      const lv = await env.DB.prepare(`SELECT level_id FROM runs WHERE run_key = ?1`).bind(key).first();
      if (lv) await env.DB.prepare(`INSERT OR IGNORE INTO finishes (player_id, run_key, level_id, at) VALUES (?1, ?2, ?3, ?4)`).bind(player, key, lv.level_id, now).run();
    } catch {}
  }
  return json({ ok: true, counted: true });
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
     <p>Now online: <b>${meta ? meta.version : "nothing yet"}</b></p>${body}${me ? await demonAdminSection(env) : ""}<p><a href="/" style="color:#a66bff">Back to the site</a></p></body>`,
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

// ---------- v3.2 website: who beat what, and the CBF demonlist ----------

// the latest levels finished, with the Discord name of whoever's game finished it (when it's linked)
async function completions(env) {
  try {
    const r = await env.DB.prepare(
      `SELECT f.level_id AS id, MAX(r.level_name) AS name, MAX(f.at) AS at, MAX(u.name) AS by
       FROM finishes f
       LEFT JOIN runs r ON r.run_key = f.run_key
       LEFT JOIN players p ON p.id = f.player_id
       LEFT JOIN users u ON u.discord_id = p.discord_id
       GROUP BY f.level_id ORDER BY at DESC LIMIT 30`
    ).all();
    return (r.results || []).map((x) => ({ id: x.id, name: x.name || `Level ${x.id}`, by: x.by || null, at: x.at }));
  } catch {
    return [];
  }
}

// demons beaten by players with the mod (not autoplay); only the admin adds them
let demonsReady = false;
async function ensureDemons(env) {
  if (demonsReady) return;
  await env.DB.prepare(
    `CREATE TABLE IF NOT EXISTS demons (id INTEGER PRIMARY KEY AUTOINCREMENT, position INTEGER NOT NULL, level_id INTEGER,
     name TEXT NOT NULL, creator TEXT, verifier TEXT, player TEXT, player_discord TEXT, avatar TEXT, video TEXT, added INTEGER NOT NULL)`
  ).run();
  demonsReady = true;
}

async function demonsList(env) {
  await ensureDemons(env);
  const r = await env.DB.prepare(`SELECT * FROM demons ORDER BY position ASC, added ASC LIMIT 500`).all();
  return json({ demons: r.results || [] }, 200, { "Cache-Control": "public, max-age=20" });
}

const clip = (v, n) => String(v || "").replace(/[\u0000-\u001f<>]/g, "").trim().slice(0, n);

async function demonSave(request, env) {
  if (!(await isAdmin(request, env))) return json({ error: "not allowed" }, 403);
  await ensureDemons(env);
  const f = await request.formData();
  const name = clip(f.get("name"), 80);
  if (!name) return json({ error: "the level name is needed" }, 400);
  const position = Math.max(1, Math.min(9999, parseInt(f.get("position"), 10) || 9999));
  const levelId = parseInt(f.get("level_id"), 10) || null;
  let video = clip(f.get("video"), 300);
  if (video && !/^https:\/\//.test(video)) video = "";
  let player = clip(f.get("player"), 60), discord = clip(f.get("player_discord"), 30).replace(/[^0-9]/g, ""), avatar = null;
  if (discord) {
    const u = await env.DB.prepare(`SELECT name, avatar FROM users WHERE discord_id = ?1`).bind(discord).first();
    if (u) {
      if (!player) player = u.name;
      if (u.avatar) avatar = `https://cdn.discordapp.com/avatars/${discord}/${u.avatar}.png?size=128`;
    }
  }
  const id = parseInt(f.get("id"), 10);
  const now = Math.floor(Date.now() / 1000);
  // the ones at and below this position move down one
  await env.DB.prepare(`UPDATE demons SET position = position + 1 WHERE position >= ?1 AND id != ?2`).bind(position, id || 0).run();
  if (id) {
    await env.DB.prepare(
      `UPDATE demons SET position=?2, level_id=?3, name=?4, creator=?5, verifier=?6, player=?7, player_discord=?8, avatar=?9, video=?10 WHERE id=?1`
    ).bind(id, position, levelId, name, clip(f.get("creator"), 60), clip(f.get("verifier"), 60), player, discord || null, avatar, video || null).run();
  } else {
    await env.DB.prepare(
      `INSERT INTO demons (position, level_id, name, creator, verifier, player, player_discord, avatar, video, added) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)`
    ).bind(position, levelId, name, clip(f.get("creator"), 60), clip(f.get("verifier"), 60), player, discord || null, avatar, video || null, now).run();
  }
  await renumberDemons(env);
  return new Response(null, { status: 302, headers: { Location: "/admin#demonlist" } });
}

async function demonDelete(request, env) {
  if (!(await isAdmin(request, env))) return json({ error: "not allowed" }, 403);
  await ensureDemons(env);
  const f = await request.formData();
  await env.DB.prepare(`DELETE FROM demons WHERE id = ?1`).bind(parseInt(f.get("id"), 10) || 0).run();
  await renumberDemons(env);
  return new Response(null, { status: 302, headers: { Location: "/admin#demonlist" } });
}

async function renumberDemons(env) {
  const r = await env.DB.prepare(`SELECT id FROM demons ORDER BY position ASC, added ASC`).all();
  const rows = r.results || [];
  if (!rows.length) return;
  await env.DB.batch(rows.map((x, i) => env.DB.prepare(`UPDATE demons SET position = ?2 WHERE id = ?1`).bind(x.id, i + 1)));
}

async function demonAdminSection(env) {
  await ensureDemons(env);
  const list = (await env.DB.prepare(`SELECT * FROM demons ORDER BY position ASC`).all()).results || [];
  const users = (await env.DB.prepare(`SELECT discord_id, name FROM users ORDER BY last_login DESC LIMIT 300`).all()).results || [];
  const opts = users.map((u) => `<option value="${escapeHtml(u.discord_id)}">${escapeHtml(u.name)}</option>`).join("");
  const rows = list.map((d) => `<tr><td>#${d.position}</td><td>${escapeHtml(d.name)}${d.level_id ? ` <small>(${d.level_id})</small>` : ""}</td>
      <td>${escapeHtml(d.player || "")}</td><td><form method="post" action="/api/admin/demons/delete" onsubmit="return confirm('Remove ${escapeHtml(d.name).replace(/'/g, "")}?')">
      <input type="hidden" name="id" value="${d.id}"><button>Remove</button></form></td></tr>`).join("");
  return `<h2 id="demonlist">Demonlist</h2>
    <p>Demons beaten with CBF by a player (not autoplay). Adding one at a position moves the ones below it down.</p>
    <table style="width:100%;border-collapse:collapse">${rows || "<tr><td>Nothing on the list yet.</td></tr>"}</table>
    <form method="post" action="/api/admin/demons" style="margin-top:16px;display:grid;gap:8px">
      <label>Position <input name="position" type="number" min="1" value="${list.length + 1}" required></label>
      <label>Level name <input name="name" required maxlength="80"></label>
      <label>Level ID (for the thumbnail) <input name="level_id" type="number"></label>
      <label>Creator <input name="creator" maxlength="60"></label>
      <label>Verified by <input name="verifier" maxlength="60"></label>
      <label>Beaten with CBF by (a Discord account that logged in on the site) <select name="player_discord"><option value="">(type a name below instead)</option>${opts}</select></label>
      <label>…or the player's name <input name="player" maxlength="60"></label>
      <label>Video link (optional) <input name="video" type="url" placeholder="https://"></label>
      <button>Add to the demonlist</button>
    </form>`;
}

const ICON_PNG = "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";
function icon() {
  const bin = atob(ICON_PNG), bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Response(bytes, { headers: { "Content-Type": "image/png", "Cache-Control": "public, max-age=86400" } });
}

// the website
const PAGE = "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>Click Before Frames</title>\n<meta name=\"description\" content=\"Click Before Frames: players online, devices, versions, levels beaten and the CBF demonlist.\">\n<link rel=\"icon\" href=\"/icon.png\">\n<link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">\n<link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" crossorigin>\n<link href=\"https://fonts.googleapis.com/css2?family=Archivo+Black&family=VT323&family=Inter:wght@400;500;600;700;800&family=Space+Mono:wght@400;700&display=swap\" rel=\"stylesheet\">\n<style>\n  :root {\n    --bg: #0a0a0c;\n    --text: #f4f4f8;\n    --muted: #a9abc4;\n    --dim: #7c7f99;\n    --line: rgba(255, 255, 255, 0.10);\n    --glass: rgba(20, 20, 26, 0.62);\n    --blue: #3b4f9e;\n    --blue2: #5065c2;\n    --purple: #5a3a8f;\n    --purple2: #7b4fb8;\n    --grey: #6b6b70;\n    --red: #ff2e3b;\n    --iris: #2e6bff;\n    --card: rgba(77, 92, 168, 0.62);\n    --card2: rgba(88, 74, 160, 0.62);\n    color-scheme: dark;\n  }\n  * { box-sizing: border-box; }\n  [hidden] { display: none !important; }\n  html, body { margin: 0; }\n  body {\n    background: var(--bg);\n    color: var(--text);\n    font-family: Inter, system-ui, sans-serif;\n    min-height: 100vh;\n    overflow-x: hidden;\n  }\n  #static {\n    position: fixed; inset: 0; width: 100%; height: 100%;\n    z-index: 0; opacity: 0.5; pointer-events: none;\n  }\n  /* a CRT's lines, and a brighter band rolling down the screen */\n  .scan {\n    position: fixed; inset: 0; z-index: 0; pointer-events: none;\n    background: repeating-linear-gradient(180deg, rgba(0, 0, 0, 0.32) 0 1px, transparent 1px 3px);\n  }\n  .roll {\n    position: fixed; left: 0; right: 0; top: -30vh; height: 30vh; z-index: 0; pointer-events: none;\n    background: linear-gradient(180deg, transparent, rgba(255, 255, 255, 0.045) 45%, rgba(255, 255, 255, 0.07) 50%, rgba(255, 255, 255, 0.045) 55%, transparent);\n    animation: roll 7s linear infinite;\n  }\n  @keyframes roll { to { transform: translateY(160vh); } }\n  .vignette {\n    position: fixed; inset: 0; z-index: 0; pointer-events: none;\n    background: radial-gradient(ellipse at 50% 40%, transparent 30%, rgba(0, 0, 0, 0.75) 100%);\n  }\n  .wrap { position: relative; z-index: 1; max-width: 1440px; margin: 0 auto; padding: 28px 40px 40px; }\n\n  /* top bar */\n  .bar {\n    display: flex; align-items: center; gap: 22px; flex-wrap: wrap;\n    padding: 12px 26px; border: 1px solid var(--line); border-radius: 36px;\n    background: var(--glass); backdrop-filter: blur(6px);\n  }\n  .brand { display: flex; align-items: center; gap: 14px; text-decoration: none; color: var(--text); }\n  .brand img { width: 58px; height: 58px; border-radius: 12px; }\n  .brand h1 { margin: 0; font-family: \"Archivo Black\", Inter, sans-serif; font-size: clamp(22px, 3.2vw, 44px); letter-spacing: -0.02em; line-height: 1; white-space: nowrap; }\n  nav { display: flex; gap: 10px; margin-left: 12px; }\n  .tab {\n    position: relative; display: inline-flex; align-items: center; gap: 10px;\n    padding: 9px 20px 9px 14px; border-radius: 999px; cursor: pointer;\n    font: 700 clamp(15px, 1.6vw, 22px) Inter, sans-serif; letter-spacing: 0.02em;\n    color: var(--muted); background: rgba(255, 255, 255, 0.04);\n    border: 1px solid var(--line); transition: color 0.2s, background 0.2s, box-shadow 0.25s, border-color 0.2s;\n  }\n  .tab:hover { color: var(--text); border-color: rgba(46, 107, 255, 0.5); }\n  .tab .iris {\n    width: 18px; height: 18px; border-radius: 50%; flex: none;\n    background: radial-gradient(circle at 50% 50%, #3a0006 0 22%, var(--iris) 24% 58%, #0b1430 62% 100%);\n    box-shadow: inset 0 0 0 1.5px rgba(255, 255, 255, 0.7);\n    transition: transform 0.3s;\n  }\n  .tab.active {\n    color: var(--text);\n    background: linear-gradient(180deg, rgba(255, 255, 255, 0.12), rgba(255, 255, 255, 0.03));\n    border-color: rgba(255, 46, 59, 0.55);\n    box-shadow: 0 0 22px rgba(255, 46, 59, 0.22), inset 0 0 12px rgba(46, 107, 255, 0.18);\n  }\n  .tab.active .iris { background: radial-gradient(circle at 50% 50%, var(--red) 0 24%, var(--iris) 27% 58%, #0b1430 62% 100%); transform: scale(1.12); }\n  .account { margin-left: auto; display: flex; align-items: center; gap: 12px; }\n  .login { color: var(--text); text-decoration: none; font: italic 600 clamp(16px, 2vw, 30px) Inter, sans-serif; }\n  .login:hover { color: #c9d2ff; }\n  .me { display: flex; align-items: center; gap: 10px; font-weight: 600; }\n  .me img { width: 34px; height: 34px; border-radius: 50%; }\n  .me a { color: var(--dim); font-size: 13px; text-decoration: none; }\n\n  /* layout */\n  .grid { display: grid; grid-template-columns: 310px minmax(0, 1fr); gap: 34px; margin-top: 38px; }\n  .side {\n    display: flex; flex-direction: column; gap: 20px; padding: 22px 16px;\n    border: 1px solid var(--line); border-radius: 44px; background: var(--glass);\n  }\n  /* v3.2: every box is an old TV: a plastic case, a curved glass screen with scan lines, snow\n     and a glare, and a chin with two knobs, a speaker grille and a red power light */\n  .tv {\n    position: relative; display: flex; flex-direction: column;\n    padding: 12px 12px 0; border-radius: 30px;\n    background: linear-gradient(180deg, #34323b 0%, #222027 55%, #18171c 100%);\n    box-shadow: inset 0 2px 0 rgba(255, 255, 255, 0.10), inset 0 -3px 0 rgba(0, 0, 0, 0.5),\n      0 14px 28px rgba(0, 0, 0, 0.55), 0 0 0 1px rgba(0, 0, 0, 0.6);\n  }\n  .tv::before { /* the case's moulding around the screen */\n    content: \"\"; position: absolute; inset: 7px 7px 37px; border-radius: 26px; pointer-events: none;\n    box-shadow: inset 0 0 0 2px rgba(0, 0, 0, 0.55), inset 0 0 0 3px rgba(255, 255, 255, 0.05);\n  }\n  .screen {\n    position: relative; overflow: hidden; isolation: isolate;\n    border-radius: 24px / 30px;\n    box-shadow: inset 0 0 40px rgba(0, 0, 0, 0.65), inset 0 0 6px rgba(0, 0, 0, 0.9);\n    text-shadow: 0 0 6px rgba(200, 215, 255, 0.35);\n  }\n  .screen::before { /* scan lines and snow */\n    content: \"\"; position: absolute; inset: 0; z-index: 2; pointer-events: none; mix-blend-mode: overlay; opacity: 0.55;\n    background:\n      repeating-linear-gradient(180deg, rgba(0, 0, 0, 0.35) 0 1px, transparent 1px 3px),\n      url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='160' height='160'%3E%3Cfilter id='n'%3E%3CfeTurbulence type='fractalNoise' baseFrequency='1.1' numOctaves='1'/%3E%3C/filter%3E%3Crect width='160' height='160' filter='url(%23n)'/%3E%3C/svg%3E\");\n    animation: snow 0.4s steps(4) infinite;\n  }\n  @keyframes snow { 0% { background-position: 0 0, 0 0; } 25% { background-position: 0 1px, -40px 30px; } 50% { background-position: 0 0, 60px -50px; } 75% { background-position: 0 2px, -70px -20px; } 100% { background-position: 0 0, 20px 60px; } }\n  .screen::after { /* the curved glass: a glare at the top left, darker edges */\n    content: \"\"; position: absolute; inset: 0; z-index: 3; pointer-events: none; border-radius: inherit;\n    background:\n      radial-gradient(120% 70% at 22% 0%, rgba(255, 255, 255, 0.13), transparent 45%),\n      radial-gradient(140% 120% at 50% 50%, transparent 58%, rgba(0, 0, 0, 0.45) 100%);\n  }\n  .screen > * { position: relative; z-index: 1; }\n  .chin { height: 38px; display: flex; align-items: center; gap: 8px; padding: 0 14px; flex: none; }\n  .knob {\n    width: 15px; height: 15px; border-radius: 50%; flex: none;\n    background: radial-gradient(circle at 35% 30%, #77737f, #2c2a31 60%, #141317);\n    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.8), inset 0 -1px 1px rgba(0, 0, 0, 0.6);\n    position: relative;\n  }\n  .knob::after { content: \"\"; position: absolute; left: 6.5px; top: 2px; width: 2px; height: 5px; border-radius: 1px; background: #c9c5d2; opacity: 0.7; }\n  .knob + .knob::after { transform: rotate(48deg); transform-origin: 1px 5.5px; }\n  .grille { flex: 1; height: 12px; border-radius: 3px; background: repeating-linear-gradient(90deg, rgba(0, 0, 0, 0.65) 0 2px, rgba(255, 255, 255, 0.05) 2px 5px); }\n  .brand-tag { font: 400 14px \"VT323\", monospace; color: #8e8a98; letter-spacing: 0.12em; }\n  .led { width: 7px; height: 7px; border-radius: 50%; background: #ff2e3b; box-shadow: 0 0 6px #ff2e3b, 0 0 12px rgba(255, 46, 59, 0.6); animation: led 3.2s ease-in-out infinite; }\n  @keyframes led { 0%, 100% { opacity: 1; } 45% { opacity: 0.55; } 50% { opacity: 1; } }\n\n  .stat {\n    padding: 14px 22px 16px; height: 168px;\n  }\n  .stat h3 { margin: 0; font: 400 40px/1 \"VT323\", monospace; letter-spacing: 0.02em; }\n  .num { font: 800 54px Inter, sans-serif; letter-spacing: -0.02em; line-height: 1.1; font-variant-numeric: tabular-nums; }\n  .online { background: linear-gradient(160deg, #4a5fb8, #34428e); }\n  .online .row { display: flex; align-items: center; gap: 12px; margin-top: 10px; }\n  .online .meter { width: 7px; height: 70px; border-radius: 4px; background: #1d2550; position: relative; overflow: hidden; }\n  .online .meter i { position: absolute; left: 0; right: 0; top: 0; height: 40%; background: #3c7bff; border-radius: 4px; transition: height 0.8s; }\n  .spark { width: 64px; height: 64px; flex: none; animation: twinkle 2.6s ease-in-out infinite; filter: drop-shadow(0 0 8px rgba(53, 220, 90, 0.6)); }\n  @keyframes twinkle { 0%, 100% { transform: scale(1) rotate(0deg); } 50% { transform: scale(0.82) rotate(12deg); } }\n  .offline { background: linear-gradient(160deg, #7a7a80, #5c5c62); }\n  .offline .num { text-align: center; margin-top: 4px; }\n  .offline .mono { font: 400 30px \"VT323\", monospace; color: #c8c8cc; letter-spacing: 0.04em; }\n  .dots::after { content: \"\"; animation: dots 1.6s steps(4) infinite; }\n  @keyframes dots { 0% { content: \"\"; } 25% { content: \".\"; } 50% { content: \"..\"; } 75% { content: \"...\"; } }\n  .players { background: linear-gradient(160deg, #6c45ad, #4d2f86); }\n  .players .row { display: flex; flex-direction: column; align-items: flex-start; gap: 0; margin-top: 2px; }\n  .players small { font: 400 17px/1.1 \"VT323\", monospace; color: #e4dcf5; }\n\n  .panel {\n    position: relative; border: 1px solid var(--line); border-radius: 50px;\n    background: var(--glass); padding: 34px 28px;\n  }\n  .three { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: clamp(14px, 2vw, 30px); align-items: start; }\n  .box {\n    padding: 16px 14px 14px 18px; height: 600px; display: flex; flex-direction: column;\n    background: linear-gradient(170deg, var(--card), var(--card2));\n  }\n  .box h2 { margin: 0 0 6px 4px; font: 400 clamp(34px, 3.4vw, 52px)/1 \"VT323\", monospace; letter-spacing: 0.02em; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; flex: none; }\n  .line { display: flex; align-items: center; justify-content: space-between; gap: 10px; padding: 11px 0; border-bottom: 1px solid rgba(255, 255, 255, 0.06); }\n  .line:last-child { border-bottom: 0; }\n  .line .k { min-width: 0; font-size: clamp(16px, 1.6vw, 25px); font-weight: 500; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }\n  .line .v { text-align: right; font-size: 14px; color: #e3e6ff; line-height: 1.2; white-space: nowrap; }\n  .line .v span { display: block; color: var(--muted); }\n  .line .v span.by { color: #e3e6ff; font-weight: 600; max-width: 120px; overflow: hidden; text-overflow: ellipsis; }\n  .empty { color: var(--muted); padding: 14px 6px; }\n  .scroll { flex: 1; min-height: 0; overflow-y: auto; padding-right: 6px; scrollbar-gutter: stable; }\n  .scroll::-webkit-scrollbar { width: 6px; }\n  .scroll::-webkit-scrollbar-thumb { background: rgba(255, 255, 255, 0.18); border-radius: 3px; }\n\n  /* demonlist */\n  .hint { position: absolute; top: 18px; right: 52px; color: var(--muted); font-size: 16px; }\n  .demons { display: flex; flex-direction: column; gap: 18px; max-height: 470px; overflow-y: auto; padding: 8px 6px 4px; }\n  .demons::-webkit-scrollbar { width: 6px; }\n  .demons::-webkit-scrollbar-thumb { background: rgba(255, 255, 255, 0.18); border-radius: 3px; }\n  .demon {\n    display: grid; grid-template-columns: 54px 150px 1fr auto; align-items: center; gap: 16px;\n    padding: 12px 22px 12px 14px; border-radius: 22px; min-height: 146px;\n    background: linear-gradient(180deg, rgba(150, 150, 156, 0.62), rgba(120, 120, 128, 0.58));\n    border: 1px solid rgba(255, 255, 255, 0.16);\n    transition: transform 0.2s, box-shadow 0.2s;\n  }\n  .demon:hover { transform: translateY(-2px); box-shadow: 0 10px 30px rgba(0, 0, 0, 0.35), 0 0 0 1px rgba(255, 46, 59, 0.35); }\n  .demon .rank { font: 500 38px Inter, sans-serif; }\n  .demon .thumb { width: 150px; height: 116px; border-radius: 10px; object-fit: cover; background: #2b2b30 url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='40' height='40'%3E%3Cfilter id='n'%3E%3CfeTurbulence baseFrequency='.9'/%3E%3C/filter%3E%3Crect width='40' height='40' filter='url(%23n)' opacity='.5'/%3E%3C/svg%3E\"); }\n  .demon .name { font: 700 38px/1 Inter, sans-serif; letter-spacing: -0.01em; }\n  .demon .meta { font-size: 21px; color: #e6e6ea; margin-top: 2px; }\n  .demon .meta em { font-style: normal; color: #b6b6bd; }\n  .demon .who { text-align: right; display: flex; flex-direction: column; align-items: flex-end; gap: 8px; }\n  .demon .who span { font-size: 21px; }\n  .pfp { width: 86px; height: 86px; border-radius: 50%; object-fit: cover; background: #2f6bff; display: grid; place-items: center; font: 600 28px Inter, sans-serif; }\n  .demon a.video { color: #c9d2ff; font-size: 14px; }\n\n  /* footer */\n  .foot {\n    grid-column: 2; position: relative; display: flex; align-items: flex-end; gap: 24px;\n    padding: 18px 30px 16px; border: 1px solid var(--line); border-radius: 40px; background: var(--glass);\n  }\n  .foot p { margin: 0; color: var(--dim); font-size: clamp(14px, 1.4vw, 21px); line-height: 1.25; flex: 1; min-width: 0; }\n  .join { display: flex; align-items: center; gap: 22px; }\n  .join b { font: 500 30px Inter, sans-serif; white-space: nowrap; }\n  .psst { position: absolute; top: 12px; right: 44px; color: var(--muted); font-size: 18px; }\n  .eye { display: block; width: 180px; height: 112px; cursor: pointer; flex: none; }\n  .eye svg { width: 100%; height: 100%; overflow: visible; }\n\n  @media (max-width: 1150px) {\n    .grid { grid-template-columns: 1fr; }\n    .side { flex-direction: row; flex-wrap: wrap; }\n    .side .tv { flex: 1 1 240px; }\n    .foot { grid-column: 1; }\n    .three { grid-template-columns: 1fr; gap: 22px; }\n    .box { height: auto; max-height: 360px; }\n  }\n  @media (max-width: 700px) {\n    .wrap { padding: 14px; }\n    .bar { padding: 12px 16px; border-radius: 26px; gap: 12px; }\n    .brand img { width: 40px; height: 40px; }\n    nav { margin-left: 0; }\n    .account { margin-left: 0; }\n    .grid { margin-top: 18px; gap: 18px; }\n    .side { flex-direction: column; padding: 14px; border-radius: 30px; gap: 14px; }\n    .side .tv { flex: none; }\n    .stat h3 { font-size: 26px; }\n    .num { font-size: 44px; }\n    .panel { padding: 22px 12px; border-radius: 30px; min-height: 0; }\n    .demons { max-height: none; }\n    .demon {\n      grid-template-columns: 44px 1fr; grid-template-areas: \"rank thumb\" \"info info\" \"who who\";\n      min-height: 0; padding: 12px; gap: 10px;\n    }\n    .demon .rank { grid-area: rank; font-size: 28px; }\n    .demon .thumb { grid-area: thumb; width: 100%; height: 130px; }\n    .demon .info { grid-area: info; }\n    .demon .who { grid-area: who; flex-direction: row-reverse; justify-content: flex-end; align-items: center; text-align: left; }\n    .demon .name { font-size: 26px; }\n    .demon .meta, .demon .who span { font-size: 16px; }\n    .pfp { width: 48px; height: 48px; font-size: 20px; }\n    .foot { flex-direction: column; align-items: flex-start; border-radius: 30px; padding: 18px; }\n    .foot p { font-size: 14px; }\n    .psst { position: static; order: -1; font-size: 14px; }\n    .join { gap: 12px; flex-wrap: wrap; }\n    .join b { font-size: 20px; }\n    .eye { width: 130px; height: 81px; }\n    .hint { position: static; margin: 0 0 10px 6px; }\n  }\n  @media (prefers-reduced-motion: reduce) { .spark, .dots::after, .roll, .screen::before, .led { animation: none; } }\n</style>\n</head>\n<body>\n<canvas id=\"static\"></canvas>\n<div class=\"scan\"></div>\n<div class=\"roll\"></div>\n<div class=\"vignette\"></div>\n<div class=\"wrap\">\n  <header class=\"bar\">\n    <a class=\"brand\" href=\"/\"><img src=\"/icon.png\" alt=\"\"><h1>CLICK BEFORE FRAMES</h1></a>\n    <nav>\n      <button class=\"tab active\" data-tab=\"stats\"><i class=\"iris\"></i>Stats</button>\n      <button class=\"tab\" data-tab=\"demons\"><i class=\"iris\"></i>Demonlist</button>\n    </nav>\n    <div class=\"account\" id=\"account\"><a class=\"login\" href=\"/auth/login\">Log in with discord</a></div>\n  </header>\n\n  <div class=\"grid\">\n    <aside class=\"side\">\n      <div class=\"tv\"><div class=\"screen stat online\">\n        <h3>Online now</h3>\n        <div class=\"row\">\n          <div class=\"meter\"><i id=\"onlineBar\"></i></div>\n          <svg class=\"spark\" viewBox=\"0 0 64 64\" aria-hidden=\"true\"><path d=\"M32 2 C35 22 42 29 62 32 C42 35 35 42 32 62 C29 42 22 35 2 32 C22 29 29 22 32 2Z\" fill=\"#35d65a\"/></svg>\n          <div class=\"num\" id=\"online\">0</div>\n        </div>\n      </div><div class=\"chin\"><i class=\"knob\"></i><i class=\"knob\"></i><span class=\"grille\"></span><span class=\"brand-tag\">CBF</span><i class=\"led\"></i></div></div>\n      <div class=\"tv\"><div class=\"screen stat offline\">\n        <h3>Offline</h3>\n        <div class=\"num\" id=\"offline\">0</div>\n        <div class=\"mono\">not playing<span class=\"dots\"></span></div>\n      </div><div class=\"chin\"><i class=\"knob\"></i><i class=\"knob\"></i><span class=\"grille\"></span><span class=\"brand-tag\">CBF</span><i class=\"led\"></i></div></div>\n      <div class=\"tv\"><div class=\"screen stat players\">\n        <h3>Players</h3>\n        <div class=\"row\"><div class=\"num\" id=\"total\">0</div><small>If you've ever opened CBF, you're one of them</small></div>\n      </div><div class=\"chin\"><i class=\"knob\"></i><i class=\"knob\"></i><span class=\"grille\"></span><span class=\"brand-tag\">CBF</span><i class=\"led\"></i></div></div>\n    </aside>\n\n    <section class=\"panel\">\n      <div id=\"tab-stats\" class=\"three\">\n        <div class=\"tv\"><div class=\"screen box\"><h2>Devices</h2><div class=\"scroll\" id=\"devices\"><div class=\"empty\">Loading\u2026</div></div></div><div class=\"chin\"><i class=\"knob\"></i><i class=\"knob\"></i><span class=\"grille\"></span><span class=\"brand-tag\">CBF</span><i class=\"led\"></i></div></div>\n        <div class=\"tv\"><div class=\"screen box\"><h2>Versions</h2><div class=\"scroll\" id=\"versions\"><div class=\"empty\">Loading\u2026</div></div></div><div class=\"chin\"><i class=\"knob\"></i><i class=\"knob\"></i><span class=\"grille\"></span><span class=\"brand-tag\">CBF</span><i class=\"led\"></i></div></div>\n        <div class=\"tv\"><div class=\"screen box\"><h2>Completions</h2><div class=\"scroll\" id=\"completions\"><div class=\"empty\">Loading\u2026</div></div></div><div class=\"chin\"><i class=\"knob\"></i><i class=\"knob\"></i><span class=\"grille\"></span><span class=\"brand-tag\">CBF</span><i class=\"led\"></i></div></div>\n      </div>\n      <div id=\"tab-demons\" hidden>\n        <div class=\"hint\">this is scrollable btw</div>\n        <div class=\"demons\" id=\"demons\"><div class=\"empty\">Loading\u2026</div></div>\n      </div>\n    </section>\n\n    <footer class=\"foot\">\n      <p>Counts are anonymous: each game sends a random id, its device type and version every 90 seconds, and autoplay's finished runs (the level and the button presses, nothing about you). Both can be turned off in the mod's settings.</p>\n      <div class=\"psst\">Psst, i'm clickable!</div>\n      <div class=\"join\">\n        <b>Join the discord:</b>\n        <a class=\"eye\" id=\"eye\" href=\"https://discord.gg/hSMYGcfcR\" target=\"_blank\" rel=\"noopener\" aria-label=\"Join the Discord\">\n          <svg viewBox=\"0 0 200 124\">\n            <defs>\n              <radialGradient id=\"sclera\" cx=\"50%\" cy=\"46%\" r=\"62%\">\n                <stop offset=\"0\" stop-color=\"#ffffff\"/>\n                <stop offset=\"0.62\" stop-color=\"#eeeef3\"/>\n                <stop offset=\"0.88\" stop-color=\"#cfc9d4\"/>\n                <stop offset=\"1\" stop-color=\"#b79aa6\"/>\n              </radialGradient>\n              <radialGradient id=\"irisG\" cx=\"50%\" cy=\"50%\" r=\"50%\">\n                <stop offset=\"0\" stop-color=\"#0d1a44\"/>\n                <stop offset=\"0.42\" stop-color=\"#1d4fd8\"/>\n                <stop offset=\"0.72\" stop-color=\"#2e6bff\"/>\n                <stop offset=\"0.9\" stop-color=\"#173b9a\"/>\n                <stop offset=\"1\" stop-color=\"#061030\"/>\n              </radialGradient>\n              <radialGradient id=\"pupilG\" cx=\"45%\" cy=\"40%\" r=\"60%\">\n                <stop offset=\"0\" stop-color=\"#ff5a5a\"/>\n                <stop offset=\"0.45\" stop-color=\"#e0141f\"/>\n                <stop offset=\"1\" stop-color=\"#3d0005\"/>\n              </radialGradient>\n              <radialGradient id=\"shade\" cx=\"50%\" cy=\"10%\" r=\"80%\">\n                <stop offset=\"0.55\" stop-color=\"#000\" stop-opacity=\"0\"/>\n                <stop offset=\"1\" stop-color=\"#000\" stop-opacity=\"0.38\"/>\n              </radialGradient>\n              <filter id=\"glow\" x=\"-50%\" y=\"-50%\" width=\"200%\" height=\"200%\"><feGaussianBlur stdDeviation=\"2.4\" result=\"b\"/><feMerge><feMergeNode in=\"b\"/><feMergeNode in=\"SourceGraphic\"/></feMerge></filter>\n              <linearGradient id=\"lidG\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\"><stop offset=\"0\" stop-color=\"#2a2530\"/><stop offset=\"1\" stop-color=\"#121016\"/></linearGradient>\n              <linearGradient id=\"lidG2\" x1=\"0\" y1=\"1\" x2=\"0\" y2=\"0\"><stop offset=\"0\" stop-color=\"#2a2530\"/><stop offset=\"1\" stop-color=\"#16131b\"/></linearGradient>\n              <clipPath id=\"eyeClip\"><path d=\"M8 62 C46 8 154 8 192 62 C154 116 46 116 8 62Z\"/></clipPath>\n            </defs>\n            <path d=\"M8 62 C46 8 154 8 192 62 C154 116 46 116 8 62Z\" fill=\"#000\" opacity=\"0.35\" transform=\"translate(0 3)\"/>\n            <g clip-path=\"url(#eyeClip)\">\n              <rect width=\"200\" height=\"124\" fill=\"url(#sclera)\"/>\n              <g stroke=\"#c0394a\" stroke-width=\"0.7\" fill=\"none\" opacity=\"0.32\">\n                <path d=\"M10 62 C26 58 34 66 46 60 C52 57 56 60 60 58\"/>\n                <path d=\"M190 62 C176 66 168 58 156 63 C150 66 146 62 140 64\"/>\n                <path d=\"M16 72 C28 70 34 76 44 74\"/>\n                <path d=\"M184 52 C172 54 166 48 158 51\"/>\n              </g>\n              <g id=\"look\">\n                <circle cx=\"100\" cy=\"62\" r=\"34\" fill=\"url(#irisG)\"/>\n                <g stroke=\"#7fa6ff\" stroke-width=\"0.6\" opacity=\"0.35\" id=\"fibers\"></g>\n                <circle cx=\"100\" cy=\"62\" r=\"34\" fill=\"none\" stroke=\"#04091f\" stroke-width=\"2.5\" opacity=\"0.8\"/>\n                <circle id=\"pupil\" cx=\"100\" cy=\"62\" r=\"15\" fill=\"url(#pupilG)\" filter=\"url(#glow)\"/>\n                <text x=\"100\" y=\"66.5\" text-anchor=\"middle\" font-family=\"Inter, sans-serif\" font-weight=\"800\" font-size=\"11\" fill=\"#fff\" opacity=\"0.92\">CBF</text>\n                <ellipse cx=\"88\" cy=\"48\" rx=\"7\" ry=\"4.5\" fill=\"#fff\" opacity=\"0.85\" transform=\"rotate(-25 88 48)\"/>\n                <circle cx=\"114\" cy=\"74\" r=\"2.2\" fill=\"#fff\" opacity=\"0.5\"/>\n              </g>\n              <rect width=\"200\" height=\"124\" fill=\"url(#shade)\"/>\n              <path id=\"lidTop\" fill=\"url(#lidG)\"/>\n              <path id=\"lidBot\" fill=\"url(#lidG2)\"/>\n              <path id=\"lash\" fill=\"none\" stroke=\"#050408\" stroke-width=\"3\" stroke-linecap=\"round\"/>\n            </g>\n            <path d=\"M8 62 C46 8 154 8 192 62 C154 116 46 116 8 62Z\" fill=\"none\" stroke=\"#0b0a10\" stroke-width=\"3\"/>\n            <path d=\"M14 50 C52 -2 148 -2 186 50\" fill=\"none\" stroke=\"#ffffff\" stroke-opacity=\"0.18\" stroke-width=\"2\"/>\n          </svg>\n        </a>\n      </div>\n    </footer>\n  </div>\n</div>\n\n<script>\n  const fmt = (n) => Number(n || 0).toLocaleString(\"en-US\");\n  const esc = (s) => String(s ?? \"\").replace(/[&<>\"']/g, (c) => ({ \"&\": \"&amp;\", \"<\": \"&lt;\", \">\": \"&gt;\", '\"': \"&quot;\", \"'\": \"&#39;\" }[c]));\n  const reduce = matchMedia(\"(prefers-reduced-motion: reduce)\").matches;\n  const BLANK = \"data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7\";\n\n  function countTo(el, value) {\n    const from = Number(el.dataset.v || 0);\n    el.dataset.v = value;\n    if (reduce) { el.textContent = fmt(value); return; }\n    const start = performance.now();\n    const step = (t) => {\n      const k = Math.min(1, (t - start) / 700);\n      el.textContent = fmt(Math.round(from + (value - from) * (1 - Math.pow(1 - k, 3))));\n      if (k < 1) requestAnimationFrame(step);\n    };\n    requestAnimationFrame(step);\n  }\n  const list = (items, render) => items.map(render).join(\"\") || '<div class=\"empty\">Nothing yet.</div>';\n  const DEVICE = { windows: \"Windows\", win: \"Windows\", mac: \"MacOS\", macos: \"MacOS\", android: \"Android\", android32: \"Android\", android64: \"Android\", ios: \"iOS\" };\n\n  async function load() {\n    try {\n      const s = await (await fetch(\"/api/stats\")).json();\n      countTo(document.getElementById(\"online\"), s.online);\n      countTo(document.getElementById(\"offline\"), s.offline);\n      countTo(document.getElementById(\"total\"), s.total);\n      document.getElementById(\"onlineBar\").style.height = `${Math.max(s.total ? (s.online / s.total) * 100 : 0, s.online ? 6 : 0)}%`;\n      const dev = {};\n      for (const p of s.platforms || []) {\n        const n = DEVICE[String(p.name).toLowerCase()] || p.name;\n        dev[n] = dev[n] || { online: 0, total: 0 };\n        dev[n].online += p.online; dev[n].total += p.online + p.offline;\n      }\n      document.getElementById(\"devices\").innerHTML = list(Object.entries(dev).sort((a, b) => b[1].total - a[1].total), ([n, d]) =>\n        `<div class=\"line\"><div class=\"k\">${esc(n)}</div><div class=\"v\">${fmt(d.online)} users online<span>${fmt(d.total)} users total</span></div></div>`);\n      document.getElementById(\"versions\").innerHTML = list(s.versions || [], (v) =>\n        `<div class=\"line\"><div class=\"k\">${esc(v.name)}</div><div class=\"v\">${fmt(v.players)} user${v.players === 1 ? \"\" : \"s\"} using</div></div>`);\n      const comp = s.completions && s.completions.length ? s.completions : (s.recentBeaten || []).map((r) => ({ name: r.name, by: null }));\n      document.getElementById(\"completions\").innerHTML = list(comp, (c) =>\n        `<div class=\"line\"><div class=\"k\" title=\"${esc(c.name)}\">${esc(c.name)}</div><div class=\"v\">Beaten by:<span class=\"by\">${esc(c.by || \"a player\")}</span></div></div>`);\n      if (s.discordInvite) document.getElementById(\"eye\").href = s.discordInvite;\n    } catch {}\n  }\n\n  async function loadDemons() {\n    const box = document.getElementById(\"demons\");\n    try {\n      const r = await (await fetch(\"/api/demons\")).json();\n      box.innerHTML = list(r.demons || [], (d, i) => `\n        <div class=\"demon\">\n          <div class=\"rank\">#${d.position || i + 1}</div>\n          <img class=\"thumb\" src=\"${Number(d.level_id) ? `https://levelthumbs.prevter.me/thumbnail/${Number(d.level_id)}/small` : BLANK}\" alt=\"\" loading=\"lazy\" onerror=\"this.onerror=null;this.src=BLANK\">\n          <div class=\"info\">\n            <div class=\"name\">${esc(d.name)}</div>\n            <div class=\"meta\">By: ${esc(d.creator || \"?\")} <em>Verified by: ${esc(d.verifier || \"?\")}</em></div>\n            ${d.video ? `<a class=\"video\" href=\"${esc(d.video)}\" target=\"_blank\" rel=\"noopener\">Watch the run</a>` : \"\"}\n          </div>\n          <div class=\"who\"><span>Beaten with CBF by: ${esc(d.player || \"?\")}</span>\n            ${d.avatar ? `<img class=\"pfp\" src=\"${esc(d.avatar)}\" alt=\"\">` : `<div class=\"pfp\">${esc((d.player || \"?\").slice(0, 1).toUpperCase())}</div>`}\n          </div>\n        </div>`);\n      if (!(r.demons || []).length) box.innerHTML = '<div class=\"empty\">No demons beaten with CBF on the list yet.</div>';\n    } catch { box.innerHTML = '<div class=\"empty\">The demonlist couldn\\'t be loaded.</div>'; }\n  }\n\n  async function account() {\n    const box = document.getElementById(\"account\");\n    try {\n      const me = await (await fetch(\"/api/me\")).json();\n      if (me) {\n        box.innerHTML = `<span class=\"me\">${me.avatar ? `<img src=\"${esc(me.avatar)}\" alt=\"\">` : \"\"}${esc(me.name)}\n          ${me.admin ? '<a href=\"/admin\">admin</a>' : \"\"}<a href=\"/auth/logout\">log out</a></span>`;\n      }\n    } catch {}\n  }\n\n  // tabs\n  for (const t of document.querySelectorAll(\".tab\")) {\n    t.addEventListener(\"click\", () => {\n      for (const o of document.querySelectorAll(\".tab\")) o.classList.toggle(\"active\", o === t);\n      document.getElementById(\"tab-stats\").hidden = t.dataset.tab !== \"stats\";\n      document.getElementById(\"tab-demons\").hidden = t.dataset.tab !== \"demons\";\n      if (t.dataset.tab === \"demons\") loadDemons();\n      history.replaceState(null, \"\", t.dataset.tab === \"demons\" ? \"#demonlist\" : \"#\");\n      blink();\n    });\n  }\n  if (location.hash === \"#demonlist\") document.querySelector('.tab[data-tab=\"demons\"]').click();\n\n  // animated static\n  (function staticNoise() {\n    // TV snow: fine grain at half the screen's resolution, grains that smear sideways a little,\n    // a few torn lines, and the whole picture flickering. 6 frames are made once and shown in a\n    // random order at random offsets, so it never repeats visibly and costs almost nothing.\n    const c = document.getElementById(\"static\"), g = c.getContext(\"2d\");\n    let frames = [], W = 0, H = 0, last = 0;\n    function make() {\n      W = c.width = Math.ceil(innerWidth / 2) + 64;\n      H = c.height = Math.ceil(innerHeight / 2) + 64;\n      c.style.width = W * 2 + \"px\"; c.style.height = H * 2 + \"px\";\n      frames = [];\n      for (let f = 0; f < 6; f++) {\n        const img = g.createImageData(W, H), px = new Uint32Array(img.data.buffer);\n        let carry = 0;\n        for (let y = 0; y < H; y++) {\n          const tear = Math.random() < 0.012 ? 40 + Math.random() * 60 : 0;   // a bright torn line\n          for (let x = 0; x < W; x++) {\n            // mostly dark grains, some bright ones; each pulls a bit of its left neighbour (sideways smear)\n            let v = Math.random();\n            v = v * v * 235;\n            v = v * 0.7 + carry * 0.3 + tear;\n            carry = v;\n            const b = v > 255 ? 255 : v | 0;\n            px[y * W + x] = 0xff000000 | (b << 16) | (b << 8) | b;\n          }\n        }\n        frames.push(img);\n      }\n    }\n    make();\n    addEventListener(\"resize\", () => { clearTimeout(make.t); make.t = setTimeout(make, 200); });\n    const draw = (t) => {\n      if (t - last > 40) {\n        last = t;\n        const img = frames[(Math.random() * frames.length) | 0];\n        g.putImageData(img, -((Math.random() * 64) | 0), -((Math.random() * 64) | 0));\n        c.style.opacity = (0.42 + Math.random() * 0.12).toFixed(3);   // the picture flickers\n      }\n      if (!reduce) requestAnimationFrame(draw);\n    };\n    requestAnimationFrame(draw);\n  })();\n\n  // the eye: follows the cursor, blinks at random, its red pupil breathes and flares\n  const fibers = document.getElementById(\"fibers\");\n  for (let a = 0; a < 360; a += 9) {\n    const r = (a * Math.PI) / 180;\n    const l = document.createElementNS(\"http://www.w3.org/2000/svg\", \"line\");\n    l.setAttribute(\"x1\", 100 + Math.cos(r) * 17); l.setAttribute(\"y1\", 62 + Math.sin(r) * 17);\n    l.setAttribute(\"x2\", 100 + Math.cos(r) * 32); l.setAttribute(\"y2\", 62 + Math.sin(r) * 32);\n    fibers.appendChild(l);\n  }\n  const eye = document.getElementById(\"eye\"), look = document.getElementById(\"look\"), pupil = document.getElementById(\"pupil\");\n  const lidTop = document.getElementById(\"lidTop\"), lidBot = document.getElementById(\"lidBot\");\n  const lash = document.getElementById(\"lash\");\n  let tx = 0, ty = 0, cx = 0, cy = 0, closed = 0, closing = 0, hold = 0, flare = 0, hover = false;\n  // the lids follow the eye's own curve: open, the upper one sits above the eye and the lower one\n  // below; closed, both curves meet past the middle, so the whole eye is covered\n  function lids(k) {\n    const up = 8 + (70 - 8) * k, down = 116 - (116 - 56) * k;\n    const top = 62 - 54 * (1 - k), bot = 62 + 54 * (1 - k);\n    lidTop.setAttribute(\"d\", `M-2 -2 H202 V62 H192 C154 ${up.toFixed(1)} 46 ${up.toFixed(1)} 8 62 H-2Z`);\n    lidBot.setAttribute(\"d\", `M-2 126 H202 V62 H192 C154 ${down.toFixed(1)} 46 ${down.toFixed(1)} 8 62 H-2Z`);\n    lash.setAttribute(\"d\", `M8 62 C46 ${up.toFixed(1)} 154 ${up.toFixed(1)} 192 62`);\n    lash.style.opacity = k > 0.02 ? 1 : 0;\n  }\n  addEventListener(\"pointermove\", (e) => {\n    const r = eye.getBoundingClientRect();\n    const dx = e.clientX - (r.left + r.width / 2), dy = e.clientY - (r.top + r.height / 2);\n    const dist = Math.hypot(dx, dy) || 1, k = Math.min(1, dist / 260);\n    tx = (dx / dist) * 34 * k; ty = (dy / dist) * 16 * k;\n  });\n  eye.addEventListener(\"pointerenter\", () => { hover = true; flare = 1; });\n  eye.addEventListener(\"pointerleave\", () => { hover = false; });\n  function blink(twice) {\n    closing = 1;\n    if (twice) setTimeout(() => (closing = 1), 260);\n  }\n  (function schedule() {\n    setTimeout(() => { blink(Math.random() < 0.25); schedule(); }, 1800 + Math.random() * 4200);\n  })();\n  (function flares() {\n    setTimeout(() => { flare = 1; flares(); }, 2500 + Math.random() * 6000);\n  })();\n  (function tick(t) {\n    cx += (tx - cx) * 0.14; cy += (ty - cy) * 0.14;\n    look.setAttribute(\"transform\", `translate(${cx.toFixed(2)} ${cy.toFixed(2)})`);\n    // close fast, stay shut a moment, open a little slower\n    if (closing) { closed = Math.min(1, closed + 0.28); if (closed >= 1) { closing = 0; hold = 5; } }\n    else if (hold > 0) hold--;\n    else closed = Math.max(0, closed - 0.14);\n    lids(closed);\n    flare = Math.max(0, flare - 0.025);\n    const r = 14 + Math.sin(t / 900) * 1.2 + (hover ? 3 : 0) + flare * 4;\n    pupil.setAttribute(\"r\", r.toFixed(2));\n    pupil.style.filter = `url(#glow) brightness(${1 + flare * 0.8})`;\n    if (!reduce) requestAnimationFrame(tick);\n  })(0);\n\n  if (new URLSearchParams(location.search).get(\"login\") === \"failed\") {\n    history.replaceState(null, \"\", \"/\");\n    alert(\"Discord login didn't go through. Try again.\");\n  }\n  account();\n  load();\n  setInterval(load, 30000);\n</script>\n</body>\n</html>\n";
