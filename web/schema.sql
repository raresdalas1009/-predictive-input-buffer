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
