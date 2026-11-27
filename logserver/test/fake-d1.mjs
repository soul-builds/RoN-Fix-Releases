// In-memory stand-in for the few D1 statements the log service uses (tests only).
export function fakeD1() {
  const rows = [];
  let next = 1;
  const now = () => new Date().toISOString().replace('T', ' ').slice(0, 19);
  const ago = (iso) => (Date.now() - Date.parse(iso.replace(' ', 'T') + 'Z')) / 86400000;
  function exec(sql, a) {
    if (sql.startsWith('SELECT COUNT(*)')) return { first: { n: rows.filter((r) => r.install === a[0] && ago(r.received) < 1).length } };
    if (sql.startsWith('INSERT INTO uploads')) {
      const [install, release, file, offset, bytes, body] = a;
      rows.push({ id: next++, received: now(), install, release, file, offset, bytes, body });
      return {};
    }
    if (sql.startsWith('SELECT id, received')) return { all: rows.filter((r) => r.id > a[0]).slice(0, a[1]) };
    if (sql.startsWith('DELETE FROM uploads')) {
      const days = -parseInt(a[0], 10);
      for (let i = rows.length - 1; i >= 0; i--) if (ago(rows[i].received) > days) rows.splice(i, 1);
      return {};
    }
    throw new Error('fake D1 does not know: ' + sql);
  }
  const stmt = (sql, a = []) => ({
    bind: (...b) => stmt(sql, b),
    first: async () => exec(sql, a).first ?? null,
    all: async () => ({ results: exec(sql, a).all ?? [] }),
    run: async () => (exec(sql, a), { success: true }),
    _run: () => exec(sql, a),
  });
  return {
    rows,
    prepare: (sql) => stmt(sql),
    batch: async (list) => list.map((s) => (s._run(), { success: true })),
  };
}
