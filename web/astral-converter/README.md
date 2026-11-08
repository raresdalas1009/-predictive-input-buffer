# cbf-astral: Astral macro converter

The mod's autoplay looks for macros on Astral's database too. Astral macros are `.ast`
files, so this Worker converts them to GDR: `GET /convert?file=<file>.ast` downloads the
file from Astral's API and converts it with Astral's own converter (`obot_converter`,
used with the Astral developer's permission), cached for a day.

Deploy: GitHub, Actions, "Deploy Astral converter", Run workflow (needs the
`CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID` repository secrets).
