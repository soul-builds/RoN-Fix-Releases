-- RoN-Fix log service: one row per uploaded log piece, names and ids already replaced with codes.
CREATE TABLE IF NOT EXISTS uploads (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  received TEXT NOT NULL DEFAULT (datetime('now')),
  install TEXT NOT NULL,      -- random code of one RoN-Fix install (ronfix\install-id.txt)
  release INTEGER NOT NULL,   -- RoN-Fix release number
  file TEXT NOT NULL,         -- ronfix-net.log / ronfix-update.log / ronfix-sound.log
  offset INTEGER NOT NULL,    -- where in that file this piece starts
  bytes INTEGER NOT NULL,
  body TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS uploads_install_received ON uploads (install, received);
CREATE INDEX IF NOT EXISTS uploads_received ON uploads (received);
