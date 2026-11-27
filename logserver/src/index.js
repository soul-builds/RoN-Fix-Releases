// RoN-Fix log service (Cloudflare Worker + D1).
//
// Receives RoN-Fix's own log files from players who said yes in the setup (proxy/update.c, upload_logs; what is
// sent: PRIVACY.md). Before ANYTHING is stored, every player name and id is replaced with a code:
//   - the names/ids the client listed ("id ..." lines, from the core's ronfix-ids.txt),
//   - anything shaped like a Steam ID64 (7656119 + 10 digits) or a PlayFab entity id (16 hex digits),
//   - values of name=... properties.
// A code is "P-" + 10 hex digits of HMAC-SHA256(ID_KEY, value): the same player gets the same code in every
// upload, so problems can be followed per player, but the value cannot be recovered without ID_KEY, a secret
// that exists only in this Worker. The uploader's IP address is not stored. Rows older than KEEP_DAYS go.
//
//   POST /v1/upload   body: see update.c ("ronfix-upload 1" format), at most MAX_BODY bytes
//   GET  /v1/logs?after=<row id>&limit=<n>   Authorization: Bearer <READ_TOKEN>   -> JSON rows
//
// Secrets (wrangler secret put): ID_KEY, READ_TOKEN.  Bindings: DB (D1).  Vars: KEEP_DAYS (default 180).

export const MAX_BODY = 4 * 1024 * 1024;
const MAX_ROW = 1900 * 1024; // D1 rows hold up to 2 MB
const PER_DAY = 60; // uploads per install code per day
const LOG_NAME = /^ronfix-[a-z]+\.log$/;

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    try {
      if (req.method === 'POST' && url.pathname === '/v1/upload') return await upload(req, env);
      if (req.method === 'GET' && url.pathname === '/v1/logs') return await list(req, env, url);
    } catch (e) {
      return text(e instanceof Refused ? e.status : 500, e instanceof Refused ? e.message : 'server error');
    }
    return text(404, 'RoN-Fix log service');
  },
  async scheduled(_event, env) {
    const days = parseInt(env.KEEP_DAYS || '180', 10);
    await env.DB.prepare("DELETE FROM uploads WHERE received < datetime('now', ?)").bind(`-${days} days`).run();
  },
};

class Refused extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

function text(status, body) {
  return new Response(body + '\n', { status, headers: { 'content-type': 'text/plain; charset=utf-8' } });
}

// ---------------------------------------------------------------- parsing
export function parseUpload(bytes) {
  const dec = new TextDecoder('utf-8');
  let pos = 0;
  const line = () => {
    const i = bytes.indexOf(10, pos);
    if (i < 0) return null;
    const s = dec.decode(bytes.subarray(pos, i)).replace(/\r$/, '');
    pos = i + 1;
    return s;
  };
  if (line() !== 'ronfix-upload 1') throw new Refused(400, 'not a RoN-Fix upload');
  const up = { install: '', release: 0, ids: [], files: [] };
  for (let l = line(); l !== null; l = line()) {
    if (l === '') continue;
    const sp = l.indexOf(' ');
    const key = sp < 0 ? l : l.slice(0, sp);
    const rest = sp < 0 ? '' : l.slice(sp + 1);
    if (key === 'install') up.install = rest;
    else if (key === 'release') up.release = parseInt(rest, 10) || 0;
    else if (key === 'id') {
      if (rest.length >= 3 && rest.length <= 80 && up.ids.length < 2000) up.ids.push(rest);
    } else if (key === 'file') {
      const [name, off, len] = rest.split(' ');
      const n = parseInt(len, 10);
      if (!LOG_NAME.test(name || '') || !(n >= 0) || pos + n > bytes.length) throw new Refused(400, 'bad file section');
      up.files.push({ name, offset: parseInt(off, 10) || 0, text: dec.decode(bytes.subarray(pos, pos + n)) });
      pos += n;
    }
  }
  if (!/^[0-9a-f]{32}$/.test(up.install)) throw new Refused(400, 'bad install code');
  return up;
}

// ---------------------------------------------------------------- the replacement of names and ids
const STEAM_ID = /7656119\d{10}/g;
const PLAYFAB_ID = /\b[0-9A-F]{16}\b/g;
const NAME_VALUE = /\bname=([^,}\s|]+)/gi;
const escape = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

async function codeMaker(secret) {
  const key = await crypto.subtle.importKey('raw', new TextEncoder().encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  const cache = new Map();
  return async (value) => {
    if (!cache.has(value)) {
      const mac = new Uint8Array(await crypto.subtle.sign('HMAC', key, new TextEncoder().encode(value)));
      cache.set(value, 'P-' + [...mac.slice(0, 5)].map((b) => b.toString(16).padStart(2, '0')).join(''));
    }
    return cache.get(value);
  };
}

export async function scrub(textIn, ids, secret) {
  const found = new Set(ids);
  for (const m of textIn.matchAll(STEAM_ID)) found.add(m[0]);
  for (const m of textIn.matchAll(PLAYFAB_ID)) found.add(m[0]);
  for (const m of textIn.matchAll(NAME_VALUE)) if (m[1].length >= 1) found.add(m[1]);
  const values = [...found].filter((v) => v.length >= 1 && v.length <= 80).sort((a, b) => b.length - a.length);
  if (!values.length) return textIn;
  const code = await codeMaker(secret);
  const codes = new Map();
  for (const v of values) codes.set(v, await code(v));
  // whole words only: a player called "Ann" does not turn "Annual" into a code
  const re = new RegExp('(?<![A-Za-z0-9_])(?:' + values.map(escape).join('|') + ')(?![A-Za-z0-9_])', 'g');
  return textIn.replace(re, (m) => codes.get(m) ?? m);
}

// ---------------------------------------------------------------- endpoints
async function upload(req, env) {
  const len = parseInt(req.headers.get('content-length') || '0', 10);
  if (len > MAX_BODY) throw new Refused(413, 'too large');
  const bytes = new Uint8Array(await req.arrayBuffer());
  if (bytes.length > MAX_BODY) throw new Refused(413, 'too large');
  const up = parseUpload(bytes);
  const recent = await env.DB.prepare("SELECT COUNT(*) AS n FROM uploads WHERE install = ? AND received > datetime('now', '-1 day')")
    .bind(up.install).first();
  if (recent && recent.n >= PER_DAY) throw new Refused(429, 'too many uploads today');
  const rows = [];
  for (const f of up.files) {
    const clean = await scrub(f.text, up.ids, env.ID_KEY);
    for (let i = 0; i < clean.length; i += MAX_ROW) {
      const part = clean.slice(i, i + MAX_ROW);
      rows.push(env.DB.prepare('INSERT INTO uploads (install, release, file, offset, bytes, body) VALUES (?, ?, ?, ?, ?, ?)')
        .bind(up.install, up.release, f.name, f.offset, part.length, part));
    }
  }
  if (rows.length) await env.DB.batch(rows);
  return text(200, `ok ${rows.length}`);
}

async function list(req, env, url) {
  const auth = req.headers.get('authorization') || '';
  if (!env.READ_TOKEN || auth !== `Bearer ${env.READ_TOKEN}`) throw new Refused(401, 'no');
  const after = parseInt(url.searchParams.get('after') || '0', 10) || 0;
  const limit = Math.min(Math.max(parseInt(url.searchParams.get('limit') || '50', 10) || 50, 1), 200);
  const r = await env.DB.prepare('SELECT id, received, install, release, file, offset, bytes, body FROM uploads WHERE id > ? ORDER BY id LIMIT ?')
    .bind(after, limit).all();
  return new Response(JSON.stringify(r.results), { headers: { 'content-type': 'application/json' } });
}
