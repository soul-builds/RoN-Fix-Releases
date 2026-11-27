"""Downloads the logs players sent (logserver/, names and ids already replaced with codes) into
diagnostics/uploads/<install code>/<log name>, appending in upload order. Remembers the last row it fetched.

    python -I tools/fetch-logs.py

The read token is in %USERPROFILE%\\.ronfix-keys\\logserver.ini (READ_TOKEN=...). diagnostics/uploads/ is not in git.
"""
import json, os, urllib.request

BASE = 'https://ronfix-logs.ronfix-logs.workers.dev'
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'diagnostics', 'uploads')
STATE = os.path.join(OUT, 'last-id.txt')


def token():
    with open(os.path.join(os.path.expanduser('~'), '.ronfix-keys', 'logserver.ini')) as f:
        for line in f:
            if line.startswith('READ_TOKEN='):
                return line.split('=', 1)[1].strip()
    raise SystemExit('no READ_TOKEN')


def main():
    os.makedirs(OUT, exist_ok=True)
    last = int(open(STATE).read()) if os.path.exists(STATE) else 0
    tok, got = token(), 0
    while True:
        req = urllib.request.Request(f'{BASE}/v1/logs?after={last}&limit=200',
                                     headers={'Authorization': 'Bearer ' + tok, 'User-Agent': 'RoN-Fix-fetch'})
        rows = json.loads(urllib.request.urlopen(req, timeout=60).read())
        if not rows:
            break
        for r in rows:
            d = os.path.join(OUT, r['install'])
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, r['file']), 'a', encoding='utf-8', newline='') as f:
                f.write(f"\n===== upload {r['id']} received {r['received']} UTC, release {r['release']}, offset {r['offset']}\n")
                f.write(r['body'])
            last = r['id']
            got += 1
        with open(STATE, 'w') as f:
            f.write(str(last))
    print(f'{got} new pieces; installs so far: {len([n for n in os.listdir(OUT) if os.path.isdir(os.path.join(OUT, n))])}')


if __name__ == '__main__':
    main()
