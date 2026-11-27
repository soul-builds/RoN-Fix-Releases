// Local stand-in for the deployed log service, for the updater's end-to-end test (tools/run-thunk-tests.ps1):
//   node logserver/test/serve.mjs <port> <dump.json>
// Runs the real Worker code (src/index.js) with an in-memory D1; after every request the stored rows are
// written to dump.json for the C test to check.
import http from 'node:http';
import { writeFileSync } from 'node:fs';
import worker from '../src/index.js';
import { fakeD1 } from './fake-d1.mjs';

const [port, dump] = process.argv.slice(2);
const env = { DB: fakeD1(), ID_KEY: 'test-key', READ_TOKEN: 'test-token' };
writeFileSync(dump, '[]');
http.createServer(async (req, res) => {
  const chunks = [];
  for await (const c of req) chunks.push(c);
  const body = Buffer.concat(chunks);
  const r = await worker.fetch(new Request(`http://127.0.0.1:${port}${req.url}`, {
    method: req.method, headers: req.headers, body: req.method === 'POST' ? body : undefined,
  }), env);
  res.writeHead(r.status, Object.fromEntries(r.headers));
  res.end(Buffer.from(await r.arrayBuffer()));
  writeFileSync(dump, JSON.stringify(env.DB.rows));
}).listen(parseInt(port, 10), '127.0.0.1');
