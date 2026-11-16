PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS cats (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    avatar_path TEXT NOT NULL DEFAULT '',
    reference_paths_json TEXT NOT NULL DEFAULT '[]',
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS boxes (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('open', 'covered')),
    color TEXT NOT NULL,
    bounds_json TEXT NOT NULL,
    entry_json TEXT,
    exit_top_json TEXT
);

CREATE TABLE IF NOT EXISTS events (
    id TEXT PRIMARY KEY,
    cat_id TEXT NOT NULL DEFAULT 'unknown',
    box_id TEXT NOT NULL,
    started_at TEXT NOT NULL,
    ended_at TEXT,
    confidence REAL NOT NULL DEFAULT 0 CHECK(confidence >= 0 AND confidence <= 1),
    inferred INTEGER NOT NULL DEFAULT 1,
    status TEXT NOT NULL CHECK(status IN ('active', 'counted', 'discarded', 'error')),
    clip_path TEXT NOT NULL DEFAULT '',
    cover_path TEXT NOT NULL DEFAULT '',
    model_error TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS events_started_at ON events(started_at);
CREATE INDEX IF NOT EXISTS events_cat_box ON events(cat_id, box_id);

CREATE TABLE IF NOT EXISTS model_calls (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id TEXT,
    tier TEXT NOT NULL,
    model TEXT NOT NULL,
    status TEXT NOT NULL,
    latency_ms INTEGER NOT NULL,
    prompt_tokens INTEGER,
    completion_tokens INTEGER,
    error TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS model_calls_event ON model_calls(event_id);
