// Click Before Frames: online counter + website.
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
