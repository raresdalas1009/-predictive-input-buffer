// Click Before Frames: Astral macro converter.
// GET /convert?file=<Astral file name>  ->  the macro as GDR (MessagePack), for the mod's autoplay.
// The .ast file is downloaded from Astral's API and converted with Astral's own converter
// (obot_converter, used with the Astral developer's permission). Results are cached for a day.
import "./shim.js";
import { initSync, Converter, Format } from "./obot_converter.js";
import wasmModule from "./obot_converter_bg.wasm";

const ASTRAL = "https://astral-api.gwddos.workers.dev";
let ready = false;

function reply(body, status = 200, type = "application/json") {
  return new Response(body, { status, headers: { "Content-Type": type, "Access-Control-Allow-Origin": "*" } });
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname !== "/convert") return reply(JSON.stringify({ ok: true, use: "/convert?file=<name>.ast" }));
    const file = url.searchParams.get("file") || "";
    if (!/^[A-Za-z0-9_.-]{1,120}\.ast$/.test(file) || file.includes("..")) return reply(JSON.stringify({ error: "bad file" }), 400);
    const cache = caches.default;
    const key = new Request(`https://cache.cbf/astral/${file}`);
    const hit = env.LOCAL_TEST ? null : await cache.match(key);
    if (hit) return hit;
    let data;
    if (request.method === "POST" && env.LOCAL_TEST) {
      data = new Uint8Array(await request.arrayBuffer());  // local testing only
    } else {
      const src = await fetch(`${ASTRAL}/api/download/${encodeURIComponent(file)}`);
      if (!src.ok) return reply(JSON.stringify({ error: `astral ${src.status}` }), 502);
      data = new Uint8Array(await src.arrayBuffer());
    }
    let out;
    try {
      if (!ready) {
        initSync(wasmModule);
        ready = true;
      }
      const c = new Converter();
      try {
        c.load(data, Format.AstV9);
        out = c.save(Format.GDR);
      } finally {
        c.free();
      }
    } catch (e) {
      return reply(JSON.stringify({ error: `convert: ${String((e && e.message) || e)}` }), 422);
    }
    const res = new Response(out, {
      headers: { "Content-Type": "application/octet-stream", "Cache-Control": "public, max-age=86400", "Access-Control-Allow-Origin": "*" },
    });
    ctx.waitUntil(cache.put(key, res.clone()));
    return res;
  },
};
