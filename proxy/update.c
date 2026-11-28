/* RoN-Fix updater: ronfix-update.dll (updater v1, 2026-11-26).
 *
 * Loaded by the loader (CrossplayNetLib.dll) on a background thread, some seconds after the game started, and
 * only when ronfix.ini [update] Enabled is not 0. Kept out of the loader because the loader is a static import
 * of the game: if Windows (Smart App Control) refuses a file, it must be this optional one, never the one the
 * game cannot start without (2026-11-26: an unsigned loader with this code in it was blocked, error 4551).
 *
 * ronfix_update(game folder, log handle): fetches manifest.txt + manifest.txt.sig from the release feed (GitHub
 * Releases of soul-builds/RoN-Fix (named RoN-Fix-Releases until 2026-11-27; GitHub redirects the old name),
 * or ronfix.ini [update] Url), checks the ECDSA P-256 signature
 * against the public keys in update_keys.h, downloads the files that changed, checks every SHA-256 and swaps
 * them in for the next start. Files in use are renamed into ronfix\previous\ first; Windows allows that for
 * loaded DLLs. Only RoN-Fix's own files can be targets (target_ok). No CRT.
 *
 * After the update check it also removes the ReShade that RoN-Fix 0.12's "better graphics" option installed
 * (graphics_cleanup): the setup has done that since 0.13, but players who only ever update never run the setup. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include "update_keys.h"
#include "graphics012.h"

/* v2 (RoN-Fix 0.15): a release Windows refused is tried again only after a week
 * v3: removes 0.12's better graphics (ReShade) for players who only ever update */
#define UPDATER_VERSION 3
#define REFUSED_RETRY_DAYS 7
#define FEED_DEFAULT L"https://github.com/soul-builds/RoN-Fix/releases/latest/download/"
#define AGENT L"RoN-Fix-updater/1"
/* RoN-Fix's log server (logserver/, Cloudflare account of soul_builds, deployed 2026-11-27). Only used with
 * ronfix.ini [upload] Consent=1. */
#define LOGS_DEFAULT L"https://ronfix-logs.ronfix-logs.workers.dev/v1/upload"
#define MAX_MANIFEST (64 * 1024)
#define MAX_FILE (48u * 1024 * 1024)
#define MAX_FILES 16
#define NAME_MAX_ 60

static WCHAR gdir[MAX_PATH]; /* game folder, ends in '\' */
static WCHAR ini[MAX_PATH];
static HANDLE ulog = INVALID_HANDLE_VALUE;

#include "ronfix_util.h"

/* ------------------------------------------------------------------ hashes and signatures */
static BCRYPT_ALG_HANDLE sha_alg, ec_alg;

static int sha_begin(BCRYPT_HASH_HANDLE *h)
{
	if (!sha_alg && BCryptOpenAlgorithmProvider(&sha_alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) {
		sha_alg = 0;
		return 0;
	}
	return BCryptCreateHash(sha_alg, h, NULL, 0, NULL, 0, 0) == 0;
}

static int sha_end(BCRYPT_HASH_HANDLE h, unsigned char out[32])
{
	int ok = BCryptFinishHash(h, out, 32, 0) == 0;
	BCryptDestroyHash(h);
	return ok;
}

static int sha_buf(const void *p, DWORD n, unsigned char out[32])
{
	BCRYPT_HASH_HANDLE h;
	if (!sha_begin(&h))
		return 0;
	BCryptHashData(h, (PUCHAR)p, n, 0);
	return sha_end(h, out);
}

/* SHA-256 of a file; 0 if it cannot be read (missing) */
static int sha_file(const WCHAR *path, unsigned char out[32])
{
	static unsigned char chunk[16384];
	BCRYPT_HASH_HANDLE h;
	DWORD got;
	HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	if (!sha_begin(&h)) {
		CloseHandle(f);
		return 0;
	}
	while (ReadFile(f, chunk, sizeof chunk, &got, NULL) && got)
		BCryptHashData(h, chunk, got, 0);
	CloseHandle(f);
	return sha_end(h, out);
}

#ifdef RONFIX_TEST
static const unsigned char *test_keys[2];
#endif

/* which key signed it: 1 primary, 2 backup, 0 none */
static int sig_ok(const void *data, DWORD n, const unsigned char sig[64])
{
	const unsigned char *keys[2] = { update_key_primary, update_key_backup };
	unsigned char digest[32];
#ifdef RONFIX_TEST
	if (test_keys[0])
		keys[0] = test_keys[0], keys[1] = test_keys[1] ? test_keys[1] : test_keys[0];
#endif
	if (!ec_alg && BCryptOpenAlgorithmProvider(&ec_alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) != 0) {
		ec_alg = 0;
		return 0;
	}
	if (!sha_buf(data, n, digest))
		return 0;
	for (int k = 0; k < 2; k++) {
		BCRYPT_KEY_HANDLE key;
		if (BCryptImportKeyPair(ec_alg, NULL, BCRYPT_ECCPUBLIC_BLOB, &key, (PUCHAR)keys[k], 72, 0) != 0)
			continue;
		long st = BCryptVerifySignature(key, NULL, digest, 32, (PUCHAR)sig, 64, 0);
		BCryptDestroyKey(key);
		if (st == 0)
			return k + 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ HTTP (winhttp, loaded on first use) */
static struct {
	HINTERNET(WINAPI *Open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
	HINTERNET(WINAPI *Connect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
	HINTERNET(WINAPI *OpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
	BOOL(WINAPI *SendRequest)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
	BOOL(WINAPI *ReceiveResponse)(HINTERNET, LPVOID);
	BOOL(WINAPI *QueryHeaders)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
	BOOL(WINAPI *ReadData)(HINTERNET, LPVOID, DWORD, LPDWORD);
	BOOL(WINAPI *CloseHandle)(HINTERNET);
	BOOL(WINAPI *CrackUrl)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
	BOOL(WINAPI *SetTimeouts)(HINTERNET, int, int, int, int);
} wh;

static int wh_load(void)
{
	if (wh.Open)
		return 1;
	HMODULE m = LoadLibraryExW(L"winhttp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!m)
		return 0;
	void **slot = (void **)&wh;
	static const char *const names[] = { "WinHttpOpen", "WinHttpConnect", "WinHttpOpenRequest", "WinHttpSendRequest",
		                                 "WinHttpReceiveResponse", "WinHttpQueryHeaders", "WinHttpReadData",
		                                 "WinHttpCloseHandle", "WinHttpCrackUrl", "WinHttpSetTimeouts" };
	for (int k = 0; k < 10; k++)
		if (!(slot[k] = (void *)GetProcAddress(m, names[k]))) {
			zero(&wh, sizeof wh);
			return 0;
		}
	return 1;
}

/* GET url (or POST body when body != 0). With out != INVALID_HANDLE_VALUE the response goes to that file and
 * into *hash; else into buf. Returns the response length, or -1 (network error, status != 200, or longer
 * than max). */
static long http_req(const WCHAR *url, const void *body, DWORD blen, char *buf, DWORD max, HANDLE out,
                     BCRYPT_HASH_HANDLE hash, DWORD *status)
{
	static unsigned char chunk[16384];
	WCHAR host[256];
	URL_COMPONENTS uc;
	long total = -1;
	HINTERNET s = 0, c = 0, r = 0;
	*status = 0;
	if (!wh_load())
		return -1;
	zero(&uc, sizeof uc);
	uc.dwStructSize = sizeof uc;
	uc.dwHostNameLength = (DWORD)-1;
	uc.dwUrlPathLength = (DWORD)-1;
	uc.dwSchemeLength = (DWORD)-1;
	if (!wh.CrackUrl(url, 0, 0, &uc) || uc.dwHostNameLength + 1 > 256 || !uc.lpszUrlPath)
		return -1;
	copy(host, uc.lpszHostName, uc.dwHostNameLength * sizeof(WCHAR));
	host[uc.dwHostNameLength] = 0;
	s = wh.Open(AGENT, 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, Windows 8.1+ */, NULL, NULL, 0);
	if (!s)
		s = wh.Open(AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
	if (!s)
		return -1;
	wh.SetTimeouts(s, 15000, 15000, 20000, 30000);
	if (!(c = wh.Connect(s, host, uc.nPort, 0)))
		goto done;
	/* lpszUrlPath points into url and runs to its end, query string included */
	if (!(r = wh.OpenRequest(c, body ? L"POST" : L"GET", uc.lpszUrlPath, NULL, NULL, NULL,
	                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)))
		goto done;
	if (body ? !wh.SendRequest(r, L"Content-Type: text/plain; charset=utf-8\r\n", (DWORD)-1, (LPVOID)body, blen, blen, 0)
	         : !wh.SendRequest(r, NULL, 0, NULL, 0, 0, 0))
		goto done;
	if (!wh.ReceiveResponse(r, NULL))
		goto done;
	DWORD code = 0, len = sizeof code;
	if (!wh.QueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &len, NULL))
		goto done;
	*status = code;
	if (code != 200)
		goto done;
	DWORD got, n = 0;
	for (;;) {
		if (!wh.ReadData(r, chunk, sizeof chunk, &got))
			goto done;
		if (!got)
			break;
		if (got > max - n) /* longer than allowed */
			goto done;
		if (out != INVALID_HANDLE_VALUE) {
			DWORD w;
			if (!WriteFile(out, chunk, got, &w, NULL) || w != got)
				goto done;
			BCryptHashData(hash, chunk, got, 0);
		} else
			copy(buf + n, chunk, got);
		n += got;
	}
	total = (long)n;
done:
	if (r)
		wh.CloseHandle(r);
	if (c)
		wh.CloseHandle(c);
	wh.CloseHandle(s);
	return total;
}
#define http_get(url, buf, max, out, hash, status) http_req(url, 0, 0, buf, max, out, hash, status)

/* ------------------------------------------------------------------ manifest
 *   ronfix-manifest 1
 *   version <release number, integer>
 *   name <shown name, e.g. 0.7>
 *   game <riseofnations.exe TimeDateStamp hex> <SizeOfImage hex>     (optional, repeatable: builds it is for)
 *   file <target path in the game folder> <asset name in the feed> <sha256 hex> <size>
 * Unknown lines are ignored (newer manifests stay readable). */
struct mfile {
	char target[NAME_MAX_ + 4];
	char asset[NAME_MAX_ + 4];
	unsigned char sha[32];
	DWORD size;
	int changed;
};
static struct manifest {
	int version;
	char name[32];
	int ngame;
	DWORD game_stamp[4], game_size[4];
	int nfiles;
	struct mfile f[MAX_FILES];
} man;

static int name_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static int name_ok(const char *s)
{
	int n = 0;
	for (; s[n]; n++)
		if (!name_char(s[n]) || n >= NAME_MAX_)
			return 0;
	return n > 0 && s[0] != '.';
}

/* Where an update may write: the loader, the sound stand-in, and RoN-Fix's own files (ronfix-*, ronfix\*).
 * Never the originals (*_orig.dll) or game data: a stolen key still cannot replace arbitrary game files. */
static int target_ok(const char *t)
{
	if (streq(t, "CrossplayNetLib.dll") || streq(t, "dssl.dll"))
		return 1;
	if (starts(t, "ronfix\\"))
		return name_ok(t + 7);
	return starts(t, "ronfix-") && name_ok(t);
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int parse_hex32(const char *s, DWORD *v)
{
	DWORD x = 0;
	int n = 0;
	for (; s[n]; n++) {
		int d = hexval(s[n]);
		if (d < 0 || n >= 8)
			return 0;
		x = x << 4 | (DWORD)d;
	}
	*v = x;
	return n > 0;
}

static int parse_dec(const char *s, DWORD *v)
{
	DWORD x = 0;
	int n = 0;
	for (; s[n]; n++) {
		if (s[n] < '0' || s[n] > '9' || n >= 9)
			return 0;
		x = x * 10 + (DWORD)(s[n] - '0');
	}
	*v = x;
	return n > 0;
}

/* splits one line in place into up to 6 words */
static int words(char *line, char **w)
{
	int n = 0;
	char *p = line;
	while (*p && n < 6) {
		while (*p == ' ' || *p == '\t')
			*p++ = 0;
		if (!*p)
			break;
		w[n++] = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}
	return n;
}

/* text is modified. 1 = ok */
static int parse_manifest(char *text, long len, struct manifest *m)
{
	char *w[6];
	int first = 1;
	zero(m, sizeof *m);
	m->version = -1;
	text[len] = 0;
	for (char *line = text; line && *line;) {
		char *next = line;
		while (*next && *next != '\n')
			next++;
		if (*next)
			*next++ = 0;
		else
			next = 0;
		for (char *e = line; *e; e++)
			if (*e == '\r')
				*e = 0;
		int n = words(line, w);
		if (first) {
			if (n != 2 || !streq(w[0], "ronfix-manifest") || !streq(w[1], "1"))
				return 0;
			first = 0;
		} else if (n == 2 && streq(w[0], "version")) {
			DWORD v;
			if (!parse_dec(w[1], &v))
				return 0;
			m->version = (int)v;
		} else if (n == 2 && streq(w[0], "name")) {
			int k = 0;
			for (; w[1][k] && k < 31; k++)
				m->name[k] = name_char(w[1][k]) ? w[1][k] : '?';
			m->name[k] = 0;
		} else if (n == 3 && streq(w[0], "game")) {
			if (m->ngame >= 4 || !parse_hex32(w[1], &m->game_stamp[m->ngame]) || !parse_hex32(w[2], &m->game_size[m->ngame]))
				return 0;
			m->ngame++;
		} else if (n >= 1 && streq(w[0], "file")) {
			struct mfile *f = &m->f[m->nfiles];
			if (n != 5 || m->nfiles >= MAX_FILES || !target_ok(w[1]) || !name_ok(w[2]) || lstrlenA(w[3]) != 64 ||
			    !parse_dec(w[4], &f->size) || f->size > MAX_FILE)
				return 0;
			for (int k = 0; k < 32; k++) {
				int hi = hexval(w[3][2 * k]), lo = hexval(w[3][2 * k + 1]);
				if (hi < 0 || lo < 0)
					return 0;
				f->sha[k] = (unsigned char)(hi << 4 | lo);
			}
			lstrcpynA(f->target, w[1], sizeof f->target);
			lstrcpynA(f->asset, w[2], sizeof f->asset);
			m->nfiles++;
		}
		line = next;
	}
	return !first && m->version >= 0 && m->nfiles > 0;
}

/* ------------------------------------------------------------------ install state */
static int installed_version(void)
{
	WCHAR p[MAX_PATH];
	char b[32];
	DWORD got = 0, v = 0;
	if (!wjoin(p, MAX_PATH, gdir, L"ronfix\\version.txt", 0))
		return 0;
	HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	ReadFile(f, b, sizeof b - 1, &got, NULL);
	CloseHandle(f);
	b[got] = 0;
	for (DWORD k = 0; k < got && b[k] >= '0' && b[k] <= '9'; k++)
		v = v * 10 + (DWORD)(b[k] - '0');
	return (int)v;
}

static void write_version(const struct manifest *m)
{
	WCHAR p[MAX_PATH], t[MAX_PATH];
	char b[64];
	DWORD w;
	int n = wsprintfA(b, "%d %s\r\n", m->version, m->name);
	if (!wjoin(p, MAX_PATH, gdir, L"ronfix\\version.txt", 0) || !wjoin(t, MAX_PATH, gdir, L"ronfix\\version.tmp", 0))
		return;
	HANDLE f = CreateFileW(t, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	WriteFile(f, b, (DWORD)n, &w, NULL);
	CloseHandle(f);
	MoveFileExW(t, p, MOVEFILE_REPLACE_EXISTING);
}

static int pe_build(const WCHAR *path, DWORD *stamp, DWORD *size)
{
	unsigned char h[1024];
	DWORD got = 0;
	HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	                       OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	ReadFile(f, h, sizeof h, &got, NULL);
	CloseHandle(f);
	if (got < 0x40)
		return 0;
	DWORD e = *(DWORD *)(h + 0x3c);
	if (e + 24 + 60 > got)
		return 0;
	*stamp = *(DWORD *)(h + e + 8);
	*size = *(DWORD *)(h + e + 24 + 56);
	return 1;
}

#ifndef ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION
#define ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION 4551L
#endif
/* Would Windows load this DLL? Maps it as an image without running any of it (no DllMain): Smart App Control
 * and code integrity decide at that moment (error 4551 / 577). An update Windows would refuse is NOT installed,
 * because a refused loader would stop the game from starting at all. 0 = refused. */
#ifdef RONFIX_TEST
static int test_refuse_image, test_image_checks, test_day;
#endif
static int image_allowed(const WCHAR *p, DWORD *err)
{
#ifdef RONFIX_TEST
	test_image_checks++;
	if (test_refuse_image) {
		*err = ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION;
		return 0;
	}
#endif
	/* no "Bad Image" message box over the game when Windows refuses it (see loader.c quiet_load) */
	DWORD old = 0;
	BOOL set = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old);
	HMODULE m = LoadLibraryExW(p, NULL, DONT_RESOLVE_DLL_REFERENCES);
	DWORD e = GetLastError();
	if (set)
		SetThreadErrorMode(old, NULL);
	SetLastError(e);
	*err = 0;
	if (m) {
		FreeLibrary(m);
		return 1;
	}
	*err = GetLastError();
	return *err != ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION && *err != ERROR_INVALID_IMAGE_HASH;
}

/* MoveFileEx with a few retries: a virus scanner may hold a freshly written file for a moment */
static int move_retry(const WCHAR *from, const WCHAR *to)
{
	for (int k = 0; k < 6; k++) {
		if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING))
			return 1;
		DWORD e = GetLastError();
		if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION)
			return 0;
		Sleep(500);
	}
	return 0;
}

/* days since 1 March of year 0 (only differences are used): 32-bit integer arithmetic, no CRT */
static int day_number(void)
{
#ifdef RONFIX_TEST
	if (test_day)
		return test_day;
#endif
	SYSTEMTIME t;
	GetSystemTime(&t);
	int y = t.wYear - (t.wMonth <= 2), m = t.wMonth, d = t.wDay;
	int era = y / 400, yoe = y - era * 400;
	int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy;
}

/* ------------------------------------------------------------------ the update */
#ifdef RONFIX_TEST
static int test_update_result;
#endif

static int update_check(void)
{
	/* static: runs once, on one thread, and -nostdlib has no __chkstk for frames over 4 KB */
	static char mbuf[MAX_MANIFEST + 1], nb[600];
	static unsigned char sig[96];
	static WCHAR feed[400], url[512], p[MAX_PATH], q[MAX_PATH], wa[NAME_MAX_ + 4], wt[NAME_MAX_ + 4];
	static WCHAR target[MAX_PATH], prev[MAX_PATH], pend[MAX_PATH], state[MAX_PATH], num[16];
	DWORD status;
	int installed = installed_version();

	GetPrivateProfileStringW(L"update", L"Url", FEED_DEFAULT, feed, 380, ini);
	int fl = lstrlenW(feed);
	if (fl == 0 || (feed[fl - 1] != '/' && fl + 1 < 380))
		feed[fl++] = '/', feed[fl] = 0;

	wjoin(url, 512, feed, L"manifest.txt", 0);
	long n = http_get(url, mbuf, MAX_MANIFEST, INVALID_HANDLE_VALUE, 0, &status);
	if (n < 0) {
		ulogf("update check: no manifest from %s (HTTP %lu, error %lu): offline, or no release yet", narrow(feed, nb, sizeof nb),
		      status, GetLastError());
		return 1;
	}
	wjoin(url, 512, feed, L"manifest.txt.sig", 0);
	long s = http_get(url, (char *)sig, sizeof sig, INVALID_HANDLE_VALUE, 0, &status);
	if (s != 64) {
		ulogf("update REJECTED: signature file missing or wrong size (%ld bytes, HTTP %lu)", s, status);
		return 2;
	}
	int key = sig_ok(mbuf, (DWORD)n, sig);
	if (!key) {
		ulogf("update REJECTED: the manifest's signature does not match RoN-Fix's keys");
		return 2;
	}
	if (!parse_manifest(mbuf, n, &man)) {
		ulogf("update REJECTED: signed manifest is not readable by updater v%d", UPDATER_VERSION);
		return 2;
	}
	if (man.version <= installed) {
		ulogf("up to date: installed release %d, feed has release %d (%s)", installed, man.version, man.name);
		return 0;
	}
	/* A release Windows refused on this PC (Smart App Control) is tried again only after REFUSED_RETRY_DAYS: each try
	 * test-loads its DLLs, and a refusal can show a Windows notice. A newer release is tried at once. */
	wjoin(state, MAX_PATH, gdir, L"ronfix\\update-state.ini", 0);
	int refused = (int)GetPrivateProfileIntW(L"refused", L"Release", 0, state);
	int since = day_number() - (int)GetPrivateProfileIntW(L"refused", L"Day", 0, state);
	if (refused == man.version && since >= 0 && since < REFUSED_RETRY_DAYS) {
		ulogf("RoN-Fix %s (release %d) was refused by Windows on this PC %d day(s) ago: next try in %d day(s)", man.name,
		      man.version, since, REFUSED_RETRY_DAYS - since);
		return 6;
	}
	if (man.ngame) {
		DWORD st = 0, sz = 0;
		int match = 0;
		wjoin(p, MAX_PATH, gdir, L"riseofnations.exe", 0);
		if (pe_build(p, &st, &sz))
			for (int k = 0; k < man.ngame; k++)
				match |= man.game_stamp[k] == st && man.game_size[k] == sz;
		if (!match) {
			ulogf("RoN-Fix %s is for another game build (this exe: %08lx/%lx): not installed", man.name, st, sz);
			return 3;
		}
	}
	ulogf("RoN-Fix %s (release %d) is available, installed release %d; signed with the %s key", man.name, man.version,
	      installed, key == 1 ? "primary" : "backup");

	/* 1. which files differ */
	int changed = 0;
	for (int k = 0; k < man.nfiles; k++) {
		unsigned char cur[32];
		struct mfile *f = &man.f[k];
		widen(f->target, wt, NAME_MAX_ + 4);
		wjoin(p, MAX_PATH, gdir, wt, 0);
		f->changed = 1;
		if (sha_file(p, cur)) {
			int same = 1;
			for (int b = 0; b < 32; b++)
				same &= cur[b] == f->sha[b];
			f->changed = !same;
		}
		changed += f->changed;
	}
	/* 2. download them into ronfix\pending */
	wjoin(p, MAX_PATH, gdir, L"ronfix", 0);
	CreateDirectoryW(p, NULL);
	wjoin(p, MAX_PATH, gdir, L"ronfix\\pending", 0);
	CreateDirectoryW(p, NULL);
	wjoin(p, MAX_PATH, gdir, L"ronfix\\previous", 0);
	CreateDirectoryW(p, NULL);
	for (int k = 0; k < man.nfiles; k++) {
		struct mfile *f = &man.f[k];
		BCRYPT_HASH_HANDLE h;
		unsigned char got[32];
		int same = 1;
		if (!f->changed)
			continue;
		widen(f->asset, wa, NAME_MAX_ + 4);
		wjoin(url, 512, feed, wa, 0);
		wjoin(p, MAX_PATH, gdir, L"ronfix\\pending\\", wa);
		wjoin(q, MAX_PATH, p, L".part", 0);
		HANDLE out = CreateFileW(q, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (out == INVALID_HANDLE_VALUE || !sha_begin(&h)) {
			if (out != INVALID_HANDLE_VALUE)
				CloseHandle(out);
			ulogf("update stopped: cannot write %s (error %lu)", narrow(q, nb, sizeof nb), GetLastError());
			return 4;
		}
		long len = http_get(url, 0, f->size, out, h, &status);
		CloseHandle(out);
		sha_end(h, got);
		for (int b = 0; b < 32; b++)
			same &= got[b] == f->sha[b];
		if (len != (long)f->size || !same) {
			DeleteFileW(q);
			ulogf("update stopped: %s %s (got %ld of %lu bytes, HTTP %lu)", f->asset,
			      len < 0 ? "did not download" : "does not match the manifest", len, f->size, status);
			return 4;
		}
		if (!move_retry(q, p)) {
			DeleteFileW(q);
			ulogf("update stopped: cannot move %s into place (error %lu)", f->asset, GetLastError());
			return 4;
		}
		int tl = lstrlenA(f->target);
		DWORD ce;
		if (tl > 4 && lower_w((WCHAR)f->target[tl - 3]) == 'd' && lower_w((WCHAR)f->target[tl - 2]) == 'l' &&
		    lower_w((WCHAR)f->target[tl - 1]) == 'l' && !image_allowed(p, &ce)) {
			DeleteFileW(p);
			ulogf("update NOT installed: Windows would refuse to load the new %s (error %lu, Smart App Control or code "
			      "integrity); keeping RoN-Fix %d as it is, next try in %d days", f->target, ce, installed, REFUSED_RETRY_DAYS);
			wsprintfW(num, L"%d", man.version);
			WritePrivateProfileStringW(L"refused", L"Release", num, state);
			wsprintfW(num, L"%d", day_number());
			WritePrivateProfileStringW(L"refused", L"Day", num, state);
			return 6;
		}
	}
	/* 3. swap: the loader itself last, so a half-done swap never leaves a new loader with old files */
	int moved_old[MAX_FILES], moved_new[MAX_FILES], order[MAX_FILES], no = 0;
	for (int k = 0; k < man.nfiles; k++)
		if (man.f[k].changed && !streq(man.f[k].target, "CrossplayNetLib.dll"))
			order[no++] = k;
	for (int k = 0; k < man.nfiles; k++)
		if (man.f[k].changed && streq(man.f[k].target, "CrossplayNetLib.dll"))
			order[no++] = k;
	int fail = -1;
	for (int i = 0; i < no; i++) {
		struct mfile *f = &man.f[order[i]];
		moved_old[i] = moved_new[i] = 0;
		widen(f->target, wt, NAME_MAX_ + 4);
		widen(f->asset, wa, NAME_MAX_ + 4);
		wjoin(target, MAX_PATH, gdir, wt, 0);
		wjoin(prev, MAX_PATH, gdir, L"ronfix\\previous\\", wa);
		wjoin(pend, MAX_PATH, gdir, L"ronfix\\pending\\", wa);
		if (GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES) {
			if (!move_retry(target, prev)) {
				fail = i;
				break;
			}
			moved_old[i] = 1;
		}
		if (!move_retry(pend, target)) {
			fail = i;
			break;
		}
		moved_new[i] = 1;
	}
	if (fail >= 0) {
		DWORD e = GetLastError();
		for (int i = fail; i >= 0; i--) { /* undo in reverse */
			struct mfile *f = &man.f[order[i]];
			widen(f->target, wt, NAME_MAX_ + 4);
			widen(f->asset, wa, NAME_MAX_ + 4);
			wjoin(target, MAX_PATH, gdir, wt, 0);
			wjoin(prev, MAX_PATH, gdir, L"ronfix\\previous\\", wa);
			wjoin(pend, MAX_PATH, gdir, L"ronfix\\pending\\", wa);
			if (moved_new[i])
				move_retry(target, pend);
			if (moved_old[i])
				move_retry(prev, target);
		}
		ulogf("update stopped while swapping %s (error %lu): every file put back as it was", man.f[order[fail]].target, e);
		return 5;
	}
	write_version(&man);
	ulogf("updated to RoN-Fix %s (release %d): %d of %d files replaced (old ones in ronfix\\previous); active from the next start",
	      man.name, man.version, changed, man.nfiles);
	return 0;
}

/* ------------------------------------------------------------------ 0.12's better graphics (ReShade): removal
 * RoN-Fix 0.12's setup could install ReShade 6.8.0 as dxgi.dll. With the Steam overlay on, ReShade can stop the
 * game at start-up, so 0.13 took the option out and its setup removes the files (setup/graphics.c). A player who
 * only ever updates never runs that setup, so the updater removes them too, once per game start. The files and the
 * rules are in graphics012.h: only with the marker file 0.12 wrote, dxgi.dll only when it is byte for byte ReShade
 * 6.8.0's file, the shader folders only when empty.
 *
 * The updater runs inside the game, and the game has that dxgi.dll loaded: Windows refuses to delete a loaded DLL
 * but lets it be renamed. So for most players it takes two game starts:
 *   1. dxgi.dll is moved to ronfix\previous (GFX_ASIDE), which switches ReShade off from the next start. Nothing
 *      else is touched: ReShade is still at work in this process (it reads its shaders again when the display is
 *      set up anew, keeps ReShade.log open and writes ReShade.ini back when the game closes).
 *   2. ReShade is no longer loaded: the moved DLL, the listed files, the empty folders and the marker go.
 * When dxgi.dll is not loaded, deleting it works and everything goes in one start.
 *
 * One difference from the setup, where the player pressed Install: if a dxgi.dll is there that is NOT ours, the
 * player may run their own ReShade on the ini and shaders 0.12 left, so here nothing at all is removed (the setup
 * removes the listed files in that case, and only leaves the dxgi.dll alone). The marker stays, so the check comes
 * again at every start, and the setup can still remove the files.
 *
 * Returns 0 nothing of 0.12's graphics here (no marker), 1 all removed, 2 not finished (dxgi.dll moved aside, or a
 * file still in use): goes on at the next start, 3 another dxgi.dll: left alone, 4 dxgi.dll could not be read or
 * moved: tried again at the next start. */
#ifdef RONFIX_TEST
static const char *test_reshade_sha; /* SHA-256 (hex) of the file the tests use in ReShade's place */
#endif

static int sha_is(const unsigned char d[32], const char *hex)
{
	for (int k = 0; k < 32; k++)
		if (hexval(hex[2 * k]) != d[k] >> 4 || hexval(hex[2 * k + 1]) != (d[k] & 15))
			return 0; /* also at the end of a hex string that is too short */
	return 1;
}

static int there(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static int graphics_cleanup(void)
{
	static WCHAR marker[MAX_PATH], dll[MAX_PATH], aside[MAX_PATH], p[MAX_PATH];
	static char nb[200];
	const char *want = RESHADE32_SHA;
	unsigned char sha[32];
	int moved = 0, gone = 0;
#ifdef RONFIX_TEST
	if (test_reshade_sha)
		want = test_reshade_sha;
#endif
	if (!wjoin(marker, MAX_PATH, gdir, GFX_MARKER, 0) || !there(marker))
		return 0;
	if (!wjoin(dll, MAX_PATH, gdir, GFX_DLL, 0) || !wjoin(aside, MAX_PATH, gdir, GFX_ASIDE, 0))
		return 0;
	int have = there(dll);
	if (have && !sha_file(dll, sha)) {
		ulogf("better graphics from RoN-Fix 0.12: dxgi.dll could not be read (error %lu), nothing removed; trying again at "
		      "the next start", GetLastError());
		return 4;
	}
	int ours = have && sha_is(sha, want);
	/* 1. dxgi.dll first: it is the file that switches ReShade on */
	if (ours) {
		if (DeleteFileW(dll))
			gone++;
		else {
			DWORD e = GetLastError();
			if (wjoin(p, MAX_PATH, gdir, GFX_ASIDE_DIR, 0))
				CreateDirectoryW(p, NULL);
			if (!move_retry(dll, aside)) {
				ulogf("better graphics from RoN-Fix 0.12: ReShade's dxgi.dll could not be deleted (error %lu) or moved out of "
				      "the game folder (error %lu), nothing removed; trying again at the next start", e, GetLastError());
				return 4;
			}
			moved = 1;
		}
	}
	/* 2. a dxgi.dll moved aside (now or at an earlier start) can go once no game has it loaded any more */
	if (there(aside)) {
		if (DeleteFileW(aside))
			gone++;
		else if (moved) {
			ulogf("better graphics (ReShade) from RoN-Fix 0.12 is being removed: with Steam's overlay on it can stop the game "
			      "at start-up. The game has its dxgi.dll loaded, so that was moved to ronfix\\previous: no ReShade from the "
			      "next start, when the rest of its files go too");
			return 2;
		} else {
			ulogf("better graphics from RoN-Fix 0.12: ReShade's dxgi.dll in ronfix\\previous cannot be deleted yet (error "
			      "%lu); the rest of its files go at a later start", GetLastError());
			return 2;
		}
	}
	if (have && !ours) {
		ulogf("better graphics from RoN-Fix 0.12: the dxgi.dll in the game folder is not the ReShade 6.8.0 that RoN-Fix "
		      "installed, so it and the ReShade files are left alone");
		return 3;
	}
	/* 3. ReShade is not loaded: the files 0.12 listed as its own, then the folders if nothing of the player's is in them */
	int left = 0;
	DWORD err = 0;
	const WCHAR *first = L"";
	for (int k = 0; k < GFX_N; k++) {
		if (!wjoin(p, MAX_PATH, gdir, gfx_files[k], 0) || !there(p))
			continue;
		if (DeleteFileW(p))
			gone++;
		else {
			if (!left)
				err = GetLastError(), first = gfx_files[k];
			left++;
		}
	}
	for (int k = GFX_NDIRS - 1; k >= 0; k--)
		if (wjoin(p, MAX_PATH, gdir, gfx_dirs[k], 0))
			RemoveDirectoryW(p);
	if (left) {
		ulogf("better graphics from RoN-Fix 0.12: ReShade is switched off and %d of its files are deleted, but %d could not "
		      "be (first: %s, error %lu); trying again at the next start", gone, left, narrow(first, nb, sizeof nb), err);
		return 2;
	}
	if (!DeleteFileW(marker)) {
		ulogf("better graphics from RoN-Fix 0.12: its files are deleted (%d), but not ronfix\\graphics.txt (error %lu); "
		      "trying again at the next start", gone, GetLastError());
		return 2;
	}
	ulogf("better graphics (ReShade) from RoN-Fix 0.12 is removed: with Steam's overlay on it could stop the game at "
	      "start-up. %d file(s) deleted; anything of your own in reshade-shaders is kept", gone);
	return 1;
}

/* ------------------------------------------------------------------ log upload (only with the player's yes)
 * ronfix.ini [upload] Consent=1 is set by the setup after it showed what is sent (PRIVACY.md). Once per game
 * start this sends what RoN-Fix's own logs (ronfix-net/-update/-sound.log) gained since the last upload, at
 * most UP_FILE_MAX per file (the newest part), with the game folder and the Windows profile folder replaced
 * HERE, so the player's Windows user name never leaves the PC. Player names and ids in the logs are listed
 * from ronfix-ids.txt (written by the core): the server replaces each of them, and anything shaped like a
 * Steam or PlayFab id, with a code before storing anything. A random install code (ronfix\install-id.txt)
 * groups one PC's uploads. Offsets advance only after the server said 200, so nothing is lost offline.
 *
 *   ronfix-upload 1
 *   install <32 hex>
 *   release <n>
 *   id <a player name or id>            (repeated)
 *   file <log name> <offset> <length>   then exactly <length> bytes, then "\n"   (repeated) */
#define UP_FILE_MAX (1024u * 1024)
#define UP_MAX (3u * UP_FILE_MAX + 128 * 1024)
static const WCHAR *const up_logs[3] = { L"ronfix-net.log", L"ronfix-update.log", L"ronfix-sound.log" };

static char *up_buf;
static DWORD up_len;

static int up_put(const void *p, DWORD n)
{
	if (n > UP_MAX - up_len)
		return 0;
	copy(up_buf + up_len, p, n);
	up_len += n;
	return 1;
}

static int up_str(const char *s) { return up_put(s, (DWORD)lstrlenA(s)); }

static char lower_a(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* copies in[0..n) to out, replacing (case-insensitively) each pattern with its stand-in */
static DWORD scrub_paths(const char *in, DWORD n, char *out, DWORD cap, const char *const *pat, const char *const *rep, int np)
{
	DWORD o = 0;
	for (DWORD i = 0; i < n;) {
		int hit = -1;
		for (int k = 0; k < np && hit < 0; k++) {
			const char *p = pat[k];
			DWORD j = 0;
			if (!p[0])
				continue;
			while (p[j] && i + j < n && lower_a(in[i + j]) == lower_a(p[j]))
				j++;
			if (!p[j])
				hit = k;
		}
		if (hit >= 0) {
			for (const char *r = rep[hit]; *r && o < cap; r++)
				out[o++] = *r;
			i += (DWORD)lstrlenA(pat[hit]);
		} else if (o < cap)
			out[o++] = in[i++];
		else
			break;
	}
	return o;
}

static void install_code(char out[33])
{
	static const char hx[] = "0123456789abcdef";
	WCHAR p[MAX_PATH];
	DWORD got = 0;
	out[0] = 0;
	wjoin(p, MAX_PATH, gdir, L"ronfix\\install-id.txt", 0);
	HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f != INVALID_HANDLE_VALUE) {
		ReadFile(f, out, 32, &got, NULL);
		CloseHandle(f);
	}
	int ok = got == 32;
	for (DWORD k = 0; ok && k < 32; k++)
		ok = hexval(out[k]) >= 0;
	if (ok) {
		out[32] = 0;
		return;
	}
	unsigned char r[16];
	if (BCryptGenRandom(NULL, r, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		return;
	for (int k = 0; k < 16; k++)
		out[2 * k] = hx[r[k] >> 4], out[2 * k + 1] = hx[r[k] & 15];
	out[32] = 0;
	WCHAR d[MAX_PATH];
	wjoin(d, MAX_PATH, gdir, L"ronfix", 0);
	CreateDirectoryW(d, NULL);
	f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f != INVALID_HANDLE_VALUE) {
		DWORD w;
		WriteFile(f, out, 32, &w, NULL);
		CloseHandle(f);
	}
}

/* 0 sent (or nothing new), 1 no consent / no server, 2 failed (tried again next start) */
static int upload_logs(void)
{
	static WCHAR url[400], state[MAX_PATH], p[MAX_PATH], prof[MAX_PATH];
	static char gdir8[MAX_PATH * 3], prof8[MAX_PATH * 3], line[200];
	DWORD start[3], end[3], status;
	int any = 0, sent[3] = { 0, 0, 0 };
	if (!GetPrivateProfileIntW(L"upload", L"Consent", 0, ini))
		return 1;
	GetPrivateProfileStringW(L"upload", L"Url", LOGS_DEFAULT, url, 400, ini);
	if (!url[0]) {
		ulogf("logs not sent: no log server set (ronfix.ini [upload] Url)");
		return 1;
	}
	wjoin(state, MAX_PATH, gdir, L"ronfix\\upload.ini", 0);
	for (int k = 0; k < 3; k++) {
		WIN32_FILE_ATTRIBUTE_DATA a;
		wjoin(p, MAX_PATH, gdir, up_logs[k], 0);
		end[k] = GetFileAttributesExW(p, GetFileExInfoStandard, &a) ? a.nFileSizeLow : 0;
		start[k] = (DWORD)GetPrivateProfileIntW(L"offsets", up_logs[k], 0, state);
		if (start[k] > end[k]) /* the log was deleted or started over */
			start[k] = 0;
		if (end[k] - start[k] > UP_FILE_MAX)
			start[k] = end[k] - UP_FILE_MAX;
		any |= end[k] > start[k];
	}
	if (!any)
		return 0;
	if (!up_buf && !(up_buf = HeapAlloc(GetProcessHeap(), 0, UP_MAX)))
		return 2;
	char *raw = HeapAlloc(GetProcessHeap(), 0, UP_FILE_MAX), *clean = HeapAlloc(GetProcessHeap(), 0, UP_FILE_MAX + 4096);
	if (!raw || !clean) {
		if (raw)
			HeapFree(GetProcessHeap(), 0, raw);
		if (clean)
			HeapFree(GetProcessHeap(), 0, clean);
		return 2;
	}
	/* the folders whose names can hold the Windows user name */
	narrow(gdir, gdir8, sizeof gdir8);
	prof[0] = 0;
	GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
	narrow(prof, prof8, sizeof prof8);
	const char *pat[2] = { gdir8, prof8 }, *rep[2] = { "<game>\\", "<profile>" };

	char code[33];
	install_code(code);
	up_len = 0;
	wsprintfA(line, "ronfix-upload 1\ninstall %s\nrelease %d\n", code, installed_version());
	up_str(line);
	/* player names and ids the core wrote, one per line */
	wjoin(p, MAX_PATH, gdir, L"ronfix-ids.txt", 0);
	HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (f != INVALID_HANDLE_VALUE) {
		DWORD got = 0;
		ReadFile(f, raw, 64 * 1024, &got, NULL);
		CloseHandle(f);
		for (DWORD i = 0; i < got;) {
			DWORD e = i;
			while (e < got && raw[e] != '\n' && raw[e] != '\r')
				e++;
			if (e - i >= 3 && e - i <= 80) {
				up_str("id ");
				up_put(raw + i, e - i);
				up_str("\n");
			}
			i = e + 1;
		}
	}
	for (int k = 0; k < 3; k++) {
		DWORD got = 0;
		if (end[k] <= start[k])
			continue;
		wjoin(p, MAX_PATH, gdir, up_logs[k], 0);
		f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
		if (f == INVALID_HANDLE_VALUE)
			continue;
		SetFilePointer(f, (LONG)start[k], NULL, FILE_BEGIN);
		ReadFile(f, raw, end[k] - start[k], &got, NULL);
		CloseHandle(f);
		DWORD n = scrub_paths(raw, got, clean, UP_FILE_MAX + 4096, pat, rep, 2);
		char nb[64];
		wsprintfA(line, "file %s %lu %lu\n", narrow(up_logs[k], nb, sizeof nb), start[k], n);
		if (!up_str(line) || !up_put(clean, n) || !up_str("\n"))
			break;
		end[k] = start[k] + got;
		sent[k] = 1;
	}
	HeapFree(GetProcessHeap(), 0, raw);
	HeapFree(GetProcessHeap(), 0, clean);
	static char reply[512];
	long r = http_req(url, up_buf, up_len, reply, sizeof reply - 1, INVALID_HANDLE_VALUE, 0, &status);
	if (r < 0) {
		ulogf("logs not sent (HTTP %lu, error %lu): trying again next start", status, GetLastError());
		return 2;
	}
	for (int k = 0; k < 3; k++) {
		WCHAR v[16];
		char a[16];
		if (!sent[k])
			continue;
		wsprintfA(a, "%lu", end[k]);
		widen(a, v, 16);
		WritePrivateProfileStringW(L"offsets", up_logs[k], v, state);
	}
	ulogf("logs sent: %lu bytes (thank you)", up_len);
	return 0;
}

/* ------------------------------------------------------------------ export */
/* Returns 0 up to date or updated, 1 no feed, 2 manifest refused, 3 other game build, 4 download failed, 6 Windows
 * would refuse a new DLL (not installed),
 * 5 swap failed (rolled back). log may be INVALID_HANDLE_VALUE. */
__declspec(dllexport) int __cdecl ronfix_update(const WCHAR *gamedir, HANDLE log)
{
	if (!wjoin(gdir, MAX_PATH, gamedir, 0, 0) || !wjoin(ini, MAX_PATH, gdir, L"ronfix.ini", 0))
		return 1;
	ulog = log;
	ulogf("updater v%d: checking for a new RoN-Fix", UPDATER_VERSION);
	int r = update_check();
	graphics_cleanup(); /* after the check: a new release is fetched whatever happens here; before the upload: its lines go along */
	upload_logs();
	return r;
}

#ifndef RONFIX_TEST
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(inst);
	return TRUE;
}
#endif
