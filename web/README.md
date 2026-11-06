# Click Before Frames: online counter + website

One Cloudflare Worker does both: the API the mod pings (`/api/ping`), the numbers (`/api/stats`),
"Log in with Discord" (`/auth/*`) and the website itself (`public/`). Players are stored in a D1
(SQLite) database. All of it fits Cloudflare's free plan.

## What the mod sends

Every 90 seconds, if **Show Online Players** is on in the mod's settings:
`{ id, platform, version }`, where `id` is a random 32-character id the mod makes once. No name,
account, level or gameplay data. When the game closes it sends `closing: true` so it stops counting as online.

## Set it up (once)

You need Node.js 18+ and a free Cloudflare account.

1. In this folder: `npm install`, then `npx wrangler login` (opens the browser).
2. Make the database: `npx wrangler d1 create cbf-online`. Copy the `database_id` it prints into
   `wrangler.toml`, then run `npm run db:init`.
3. Discord login: on https://discord.com/developers/applications make a **New Application**.
   - **OAuth2 → General:** copy the **Client ID** into `DISCORD_CLIENT_ID` in `wrangler.toml`.
   - **Reset Secret**, then run `npx wrangler secret put DISCORD_CLIENT_SECRET` and paste it there
     (never in chat or in a file).
   - **Redirects:** add `https://<your-worker-address>/auth/callback` (you get the address in step 5;
     come back and add it then).
4. Run `npx wrangler secret put SESSION_SECRET` and paste any long random text (it signs login cookies).
5. `npm run deploy`. It prints the address, like `https://cbf-online.<you>.workers.dev`. Open it.
6. Optional: a server invite link in `DISCORD_INVITE` (wrangler.toml) shows a "Join the Discord" button.
   Deploy again after changing `wrangler.toml`.

Send the address from step 5 to put in the mod (`ONLINE_URL` in `src/main.cpp`).

## Try it on your PC

`npm run db:init-local`, make a `.dev.vars` file with `DISCORD_CLIENT_SECRET=...` and
`SESSION_SECRET=...` (it's ignored by git), then `npm run dev` and open http://localhost:8787.
