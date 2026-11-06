-- one row per game (an anonymous random id the mod makes once)
CREATE TABLE IF NOT EXISTS players (
  id TEXT PRIMARY KEY,
  platform TEXT NOT NULL,
  version TEXT NOT NULL,
  first_seen INTEGER NOT NULL,
  last_seen INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS players_last_seen ON players (last_seen);

-- people who logged in on the website with Discord
CREATE TABLE IF NOT EXISTS users (
  discord_id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  avatar TEXT,
  first_login INTEGER NOT NULL,
  last_login INTEGER NOT NULL
);

-- the global brain: runs autoplay finished (test 90)
CREATE TABLE IF NOT EXISTS runs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_key TEXT UNIQUE NOT NULL,
  level_id INTEGER NOT NULL,
  level_hash TEXT NOT NULL,
  level_name TEXT,
  tps INTEGER NOT NULL,
  steps INTEGER NOT NULL,
  inputs TEXT NOT NULL,
  version TEXT,
  created INTEGER NOT NULL,
  last_confirm INTEGER NOT NULL,
  confirms INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX IF NOT EXISTS runs_level ON runs (level_id, level_hash, tps);
