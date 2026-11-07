-- one row per game (an anonymous random id the mod makes once)
CREATE TABLE IF NOT EXISTS players (
  id TEXT PRIMARY KEY,
  platform TEXT NOT NULL,
  version TEXT NOT NULL,
  first_seen INTEGER NOT NULL,
  last_seen INTEGER NOT NULL
);
-- (v3.0.2: no index on last_seen: it doubled every write)

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

-- test 91: linking games to Discord, and who finished what
ALTER TABLE players ADD COLUMN discord_id TEXT;
CREATE TABLE IF NOT EXISTS link_codes (code TEXT PRIMARY KEY, player_id TEXT NOT NULL, discord_id TEXT, created INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS finishes (player_id TEXT NOT NULL, run_key TEXT NOT NULL, level_id INTEGER NOT NULL, at INTEGER NOT NULL, PRIMARY KEY (player_id, run_key));
CREATE TABLE IF NOT EXISTS run_owners (run_key TEXT PRIMARY KEY, player_id TEXT NOT NULL);
