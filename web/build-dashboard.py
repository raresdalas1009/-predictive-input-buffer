# Makes dashboard-worker.js (one file for the Cloudflare dashboard editor) from src/worker.js
# and public/index.html. Run: python3 build-dashboard.py
import json, pathlib
here = pathlib.Path(__file__).parent
src = (here / "src/worker.js").read_text()
page = (here / "public/index.html").read_text()
lines = src.split("\n")
assert lines[0].startswith("// Click Before Frames: online counter + website.")
lines[0] = ("// Click Before Frames: online counter + website, in ONE file for the Cloudflare dashboard editor\n"
            "// (made from src/worker.js and public/index.html; paste all of it as the worker's code).")
out = "\n".join(lines)
a = '      if (url.pathname === "/api/demons") return await demonsList(env);'
assert out.count(a) == 1
out = out.replace(a, '      if (url.pathname === "/icon.png") return icon();\n' + a)
b = "      return env.ASSETS.fetch(request);"
assert out.count(b) == 1
out = out.replace(b, '      return new Response(PAGE, { headers: { "Content-Type": "text/html; charset=utf-8" } });')
out = out.rstrip("\n") + "\n\n// the website\nconst PAGE = " + json.dumps(page) + ";\n"
(here / "dashboard-worker.js").write_text(out)
