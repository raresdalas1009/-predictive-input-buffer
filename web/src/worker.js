// Click Before Frames: online counter + website.
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
      if (url.pathname === "/api/runs" && request.method === "POST") return await runUpload(request, env);
      if (url.pathname === "/api/runs" && request.method === "GET") return await runGet(url, env);
      if (url.pathname === "/api/latest") return await latest(env);
      if (url.pathname === "/download/latest") return await download(env);
      if (url.pathname === "/admin") return await adminPage(request, env);
      if (url.pathname === "/api/admin/upload" && request.method === "POST") return await adminUpload(request, env);
      if (url.pathname.startsWith("/api/") || url.pathname.startsWith("/auth/")) return json({ error: "not found" }, 404);
      return env.ASSETS.fetch(request);
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
  const now = Math.floor(Date.now() / 1000);
  const had = await env.DB.prepare(`SELECT id FROM runs WHERE run_key = ?1`).bind(key).first();
  if (had) {
    await env.DB.prepare(`UPDATE runs SET confirms = confirms + 1, last_confirm = ?2 WHERE id = ?1`).bind(had.id, now).run();
    return json({ ok: true, confirmed: true });
  }
  await env.DB.prepare(
    `INSERT INTO runs (run_key, level_id, level_hash, level_name, tps, steps, inputs, version, created, last_confirm, confirms)
     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?9, 1)`
  ).bind(key, level, hash, name, tps, last, text, version, now).run();
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
  if (!r) return json({ found: false });
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
