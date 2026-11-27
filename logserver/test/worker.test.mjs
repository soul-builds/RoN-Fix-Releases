// node --test logserver/test
import test from 'node:test';
import assert from 'node:assert/strict';
import worker, { parseUpload, scrub, MAX_BODY } from '../src/index.js';
import { fakeD1 } from './fake-d1.mjs';

const enc = (s) => new TextEncoder().encode(s);
const INSTALL = '0123456789abcdef0123456789abcdef';
function body(files, ids = [], install = INSTALL) {
  let s = `ronfix-upload 1\ninstall ${install}\nrelease 7\n` + ids.map((i) => `id ${i}\n`).join('');
  for (const [name, text] of files) s += `file ${name} 0 ${enc(text).length}\n${text}\n`;
  return s;
}
const env = () => ({ DB: fakeD1(), ID_KEY: 'test-key', READ_TOKEN: 'test-token' });
const post = (e, b) => worker.fetch(new Request('https://x/v1/upload', { method: 'POST', body: b }), e);

test('parse: header, ids, file sections with exact byte lengths (UTF-8 too)', () => {
  const up = parseUpload(enc(body([['ronfix-net.log', 'héllo\nline 2\n'], ['ronfix-update.log', 'u']], ['Soul'])));
  assert.equal(up.install, INSTALL);
  assert.equal(up.release, 7);
  assert.deepEqual(up.ids, ['Soul']);
  assert.equal(up.files.length, 2);
  assert.equal(up.files[0].text, 'héllo\nline 2\n');
  assert.equal(up.files[1].name, 'ronfix-update.log');
});

test('parse: refuses other formats, bad install codes, other file names, lengths past the end', () => {
  assert.throws(() => parseUpload(enc('hello\n')));
  assert.throws(() => parseUpload(enc(body([['ronfix-net.log', 'x']], [], 'nothex'))));
  assert.throws(() => parseUpload(enc(body([['..\\boot.ini', 'x']]))));
  assert.throws(() => parseUpload(enc(`ronfix-upload 1\ninstall ${INSTALL}\nfile ronfix-net.log 0 999\nshort\n`)));
});

test('scrub: listed names, Steam IDs, PlayFab ids and name= values become stable codes', async () => {
  const t = 'NET + player joined: Soul | id 1A2B3C4D5E6F7081 | Steam | 76561198000000042\n' +
    'PF post: member {name=Soul,platformId=76561198000000042,versionId=2.0}\nLOBBY slot 1: Annabel\nAnnual Soulful';
  const out = await scrub(t, ['Soul', 'Annabel'], 'k');
  assert.ok(!out.includes('76561198000000042') && !out.includes('1A2B3C4D5E6F7081') && !/\bSoul\b/.test(out) && !out.includes('Annabel'));
  assert.ok(out.includes('versionId=2.0'), 'other values stay readable');
  assert.ok(out.includes('Annual Soulful'), 'whole words only');
  const codes = out.match(/P-[0-9a-f]{10}/g);
  assert.ok(codes.length >= 5);
  const again = await scrub('Soul', ['Soul'], 'k');
  assert.ok(out.includes(again), 'the same player gets the same code');
  assert.notEqual(await scrub('Soul', ['Soul'], 'other key'), again, 'codes depend on the secret');
});

test('upload: stored scrubbed, readable with the token only', async () => {
  const e = env();
  const r = await post(e, body([['ronfix-net.log', 'joined: Soul 76561198000000042\n']], ['Soul']));
  assert.equal(r.status, 200);
  assert.equal(e.DB.rows.length, 1);
  assert.ok(!e.DB.rows[0].body.includes('Soul') && !e.DB.rows[0].body.includes('7656119'));
  const no = await worker.fetch(new Request('https://x/v1/logs'), e);
  assert.equal(no.status, 401);
  const yes = await worker.fetch(new Request('https://x/v1/logs?after=0', { headers: { authorization: 'Bearer test-token' } }), e);
  const rows = await yes.json();
  assert.equal(rows.length, 1);
  assert.equal(rows[0].file, 'ronfix-net.log');
});

test('upload: limits (size, uploads per day), nothing stored when refused', async () => {
  const e = env();
  const big = new Uint8Array(MAX_BODY + 10);
  assert.equal((await post(e, big)).status, 413);
  for (let i = 0; i < 60; i++) assert.equal((await post(e, body([['ronfix-net.log', 'x' + i]]))).status, 200);
  assert.equal((await post(e, body([['ronfix-net.log', 'one too many']]))).status, 429);
  assert.equal(e.DB.rows.length, 60);
  assert.equal((await post(e, 'garbage\n')).status, 400);
});

test('scheduled: rows older than KEEP_DAYS are deleted', async () => {
  const e = env();
  await post(e, body([['ronfix-net.log', 'old']]));
  await post(e, body([['ronfix-net.log', 'new']]));
  e.DB.rows[0].received = '2000-01-01 00:00:00';
  await worker.scheduled({}, { ...e, KEEP_DAYS: '180' });
  assert.deepEqual(e.DB.rows.map((r) => r.body), ['new']);
});
