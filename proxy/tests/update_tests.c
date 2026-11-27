/* Tests for proxy/update.c (ronfix-update.dll: finds, checks and installs new RoN-Fix releases).
 * Build + run: tools/run-thunk-tests.ps1, which also starts a local HTTP server for the end-to-end update test
 * (env RONFIX_TEST_FEED = its URL, RONFIX_TEST_SERVE = the folder it serves, RONFIX_TEST_GAME = a scratch
 * "game folder"). Without those, the update test is skipped and counted as a failure. */
#define RONFIX_TEST
#include "../update.c"

static char *out;
static int out_left, fails, passes;
static void say(const char *s)
{
	while (*s && out_left > 1) {
		*out++ = *s++;
		out_left--;
	}
	*out = 0;
}
static void check(int ok, const char *what)
{
	char b[700];
	wsprintfA(b, "%s %s\r\n", ok ? "PASS" : "FAIL", what);
	say(b);
	ok ? passes++ : fails++;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)inst, (void)reason, (void)reserved;
	return TRUE;
}

/* ------------------------------------------------------------------ files */
static int write_file(const WCHAR *path, const void *data, DWORD n)
{
	DWORD w;
	HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	int ok = WriteFile(f, data, n, &w, NULL) && w == n;
	CloseHandle(f);
	return ok;
}

static DWORD read_file(const WCHAR *path, void *buf, DWORD cap)
{
	DWORD got = 0;
	HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return (DWORD)-1;
	ReadFile(f, buf, cap, &got, NULL);
	CloseHandle(f);
	return got;
}

static int file_is(const WCHAR *dir, const WCHAR *name, const char *want)
{
	static char b[256];
	WCHAR p[MAX_PATH];
	wjoin(p, MAX_PATH, dir, name, 0);
	DWORD n = read_file(p, b, sizeof b - 1);
	if (n == (DWORD)-1)
		return 0;
	b[n] = 0;
	return streq(b, want);
}

static void hex(const unsigned char *d, int n, char *o)
{
	static const char x[] = "0123456789abcdef";
	for (int k = 0; k < n; k++)
		o[2 * k] = x[d[k] >> 4], o[2 * k + 1] = x[d[k] & 15];
	o[2 * n] = 0;
}

/* ------------------------------------------------------------------ a signing key made for the test */
static BCRYPT_KEY_HANDLE tkey;
static unsigned char tpub[72];

static int make_key(void)
{
	DWORD got;
	if (!ec_alg && BCryptOpenAlgorithmProvider(&ec_alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) != 0)
		return 0;
	if (BCryptGenerateKeyPair(ec_alg, &tkey, 256, 0) || BCryptFinalizeKeyPair(tkey, 0))
		return 0;
	if (BCryptExportKey(tkey, NULL, BCRYPT_ECCPUBLIC_BLOB, tpub, sizeof tpub, &got, 0) || got != 72)
		return 0;
	test_keys[0] = tpub;
	test_keys[1] = 0;
	return 1;
}

static int sign(const void *data, DWORD n, unsigned char sig[64])
{
	unsigned char d[32];
	DWORD got;
	return sha_buf(data, n, d) && BCryptSignHash(tkey, NULL, d, 32, sig, 64, &got, 0) == 0 && got == 64;
}

/* ------------------------------------------------------------------ unit tests */
static void test_names(void)
{
	check(target_ok("CrossplayNetLib.dll") && target_ok("dssl.dll") && target_ok("ronfix-core.dll") &&
	          target_ok("ronfix\\RoN-Fix.exe"),
	      "allowed update targets: loader, sound stand-in, ronfix-*, ronfix\\*");
	check(!target_ok("CrossplayNetLib_orig.dll") && !target_ok("dssl_orig.dll") && !target_ok("riseofnations.exe") &&
	          !target_ok("Data\\balance.xml") && !target_ok("ronfix\\..\\riseofnations.exe") &&
	          !target_ok("ronfix\\a\\b.dll") && !target_ok("ronfix-..dll\\x") && !target_ok("C:\\x.dll") &&
	          !target_ok("ronfix\\") && !target_ok("ronfix\\.hidden"),
	      "refused update targets: originals, game files, paths that climb out, nested or absolute paths");
	check(has_switch(L"\"C:\\G\\riseofnations.exe\" -original", L"-original") &&
	          has_switch(L"riseofnations.exe -ORIGINAL -x", L"-original") &&
	          !has_switch(L"riseofnations.exe -originals", L"-original") &&
	          !has_switch(L"C:\\x-original\\riseofnations.exe", L"-original"),
	      "-original is matched as a separate word, any case");
}

static void test_manifest(void)
{
	static char t[2048];
	static struct manifest m;
	lstrcpyA(t, "ronfix-manifest 1\r\nversion 7\r\nname 0.7\r\ngame 6674863f bb4000\r\nsomething new 1 2 3\r\n"
	            "file ronfix-core.dll ronfix-core.dll 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff 50176\r\n"
	            "file ronfix\\RoN-Fix.exe RoN-Fix-Setup.exe ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100 9\n");
	int ok = parse_manifest(t, lstrlenA(t), &m);
	check(ok && m.version == 7 && streq(m.name, "0.7") && m.ngame == 1 && m.game_stamp[0] == 0x6674863f &&
	          m.game_size[0] == 0xbb4000 && m.nfiles == 2 && streq(m.f[1].target, "ronfix\\RoN-Fix.exe") &&
	          streq(m.f[1].asset, "RoN-Fix-Setup.exe") && m.f[0].size == 50176 && m.f[0].sha[1] == 0x11 &&
	          m.f[1].sha[0] == 0xff,
	      "manifest: version, name, game build, two files, unknown line ignored");
	lstrcpyA(t, "ronfix-manifest 1\nversion 7\nfile riseofnations.exe x.exe 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff 5\n");
	check(!parse_manifest(t, lstrlenA(t), &m), "manifest refused: a file outside the allowed targets");
	lstrcpyA(t, "ronfix-manifest 1\nversion 7\nfile ronfix-core.dll ronfix-core.dll 0011 5\n");
	check(!parse_manifest(t, lstrlenA(t), &m), "manifest refused: a short hash");
	lstrcpyA(t, "ronfix-manifest 1\nversion 7\nfile ronfix-core.dll ronfix-core.dll 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff 5 extra\n");
	check(!parse_manifest(t, lstrlenA(t), &m), "manifest refused: a file line with an extra word");
	lstrcpyA(t, "ronfix-manifest 2\nversion 7\n");
	check(!parse_manifest(t, lstrlenA(t), &m), "manifest refused: unknown format version");
	lstrcpyA(t, "ronfix-manifest 1\nversion 7\n");
	check(!parse_manifest(t, lstrlenA(t), &m), "manifest refused: no files");
}

static void test_signature(void)
{
	static const char msg[] = "ronfix-manifest 1\nversion 7\n";
	unsigned char sig[64];
	if (!make_key() || !sign(msg, sizeof msg - 1, sig)) {
		check(0, "signature: could not make a test key");
		return;
	}
	check(sig_ok(msg, sizeof msg - 1, sig) == 1, "signature: a manifest signed with the key verifies");
	static char bad[sizeof msg];
	copy(bad, msg, sizeof msg);
	bad[26] = '8';
	check(!sig_ok(bad, sizeof msg - 1, sig), "signature: one changed byte fails");
	sig[5] ^= 1;
	check(!sig_ok(msg, sizeof msg - 1, sig), "signature: a changed signature fails");
	/* the real keys are well-formed P-256 public blobs */
	BCRYPT_KEY_HANDLE k;
	int both = BCryptImportKeyPair(ec_alg, NULL, BCRYPT_ECCPUBLIC_BLOB, &k, (PUCHAR)update_key_primary, 72, 0) == 0;
	if (both)
		BCryptDestroyKey(k);
	both = both && BCryptImportKeyPair(ec_alg, NULL, BCRYPT_ECCPUBLIC_BLOB, &k, (PUCHAR)update_key_backup, 72, 0) == 0;
	if (both)
		BCryptDestroyKey(k);
	check(both, "the embedded primary and backup public keys import");
}

/* ------------------------------------------------------------------ end to end, over HTTP */
static WCHAR serve[MAX_PATH], game[MAX_PATH];

static int publish(const char *manifest_text, int good_sig)
{
	WCHAR p[MAX_PATH];
	unsigned char sig[64];
	DWORD n = (DWORD)lstrlenA(manifest_text);
	wjoin(p, MAX_PATH, serve, L"manifest.txt", 0);
	if (!write_file(p, manifest_text, n) || !sign(manifest_text, n, sig))
		return 0;
	if (!good_sig)
		sig[10] ^= 0x40;
	wjoin(p, MAX_PATH, serve, L"manifest.txt.sig", 0);
	return write_file(p, sig, 64);
}

static void file_line(char *o, const char *target, const char *asset, const char *content)
{
	unsigned char d[32];
	char h[65];
	sha_buf(content, (DWORD)lstrlenA(content), d);
	hex(d, 32, h);
	wsprintfA(o, "file %s %s %s %d\n", target, asset, h, lstrlenA(content));
}

static void test_update(void)
{
	static char m[4096], l1[200], l2[200], l3[200];
	static WCHAR feedw[300], p[MAX_PATH], q[MAX_PATH], sysdll[MAX_PATH];
	if (!GetEnvironmentVariableW(L"RONFIX_TEST_FEED", feedw, 300) || !GetEnvironmentVariableW(L"RONFIX_TEST_SERVE", serve, MAX_PATH) ||
	    !GetEnvironmentVariableW(L"RONFIX_TEST_GAME", game, MAX_PATH)) {
		check(0, "update over HTTP: RONFIX_TEST_FEED/SERVE/GAME not set (run via tools/run-thunk-tests.ps1)");
		return;
	}
	if (!tkey && !make_key()) {
		check(0, "update over HTTP: no test key");
		return;
	}
	wjoin(gdir, MAX_PATH, game, L"\\", 0);
	wjoin(serve, MAX_PATH, serve, L"\\", 0);
	wjoin(ini, MAX_PATH, gdir, L"ronfix.ini", 0);
	WritePrivateProfileStringW(L"update", L"Url", feedw, ini);
	wjoin(p, MAX_PATH, gdir, L"ronfix-update.log", 0);
	ulog = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	/* the "installed" release 3: an old loader, a core that is LOADED right now (a copy of a system DLL),
	 * and a sound stand-in the new release does not change */
	wjoin(p, MAX_PATH, gdir, L"CrossplayNetLib.dll", 0);
	write_file(p, "old loader", 10);
	wjoin(p, MAX_PATH, gdir, L"ronfix-core.dll", 0);
	GetSystemDirectoryW(sysdll, MAX_PATH); /* SysWOW64 in a 32-bit process */
	wjoin(sysdll, MAX_PATH, sysdll, L"\\version.dll", 0);
	CopyFileW(sysdll, p, FALSE);
	HMODULE held_img = LoadLibraryW(p); /* mapped as an image, the way the game holds the real core */
	wjoin(p, MAX_PATH, gdir, L"dssl.dll", 0);
	write_file(p, "sound v1", 8);
	wjoin(p, MAX_PATH, gdir, L"ronfix", 0);
	CreateDirectoryW(p, NULL);
	wjoin(p, MAX_PATH, gdir, L"ronfix\\version.txt", 0);
	write_file(p, "3 0.6\r\n", 7);

	/* the feed: release 4 changes the loader and the core */
	wjoin(p, MAX_PATH, serve, L"CrossplayNetLib.dll", 0);
	write_file(p, "new loader 4", 12);
	wjoin(p, MAX_PATH, serve, L"ronfix-core.dll", 0);
	write_file(p, "new core 4", 10);
	wjoin(p, MAX_PATH, serve, L"dssl.dll", 0);
	write_file(p, "sound v1", 8);
	file_line(l1, "CrossplayNetLib.dll", "CrossplayNetLib.dll", "new loader 4");
	file_line(l2, "ronfix-core.dll", "ronfix-core.dll", "new core 4");
	file_line(l3, "dssl.dll", "dssl.dll", "sound v1");
	wsprintfA(m, "ronfix-manifest 1\nversion 4\nname 0.7-test\n%s%s%s", l1, l2, l3);

	/* bad signature first: nothing may change */
	publish(m, 0);
	int r = update_check();
	check(r == 2 && file_is(gdir, L"CrossplayNetLib.dll", "old loader") && file_is(gdir, L"ronfix\\version.txt", "3 0.6\r\n"),
	      "update: a manifest with a bad signature is refused, nothing changes");

	publish(m, 1);
	r = update_check();
	check(r == 0, "update: signed release 4 installs");
	check(file_is(gdir, L"CrossplayNetLib.dll", "new loader 4") && file_is(gdir, L"ronfix-core.dll", "new core 4"),
	      "update: the loader and the core are the new files");
	wjoin(q, MAX_PATH, gdir, L"ronfix\\previous\\ronfix-core.dll", 0);
	check(held_img != 0 && GetFileAttributesW(q) != INVALID_FILE_ATTRIBUTES &&
	          file_is(gdir, L"ronfix\\previous\\CrossplayNetLib.dll", "old loader"),
	      "update: the old files moved to ronfix\\previous, including a core that was loaded at the time");
	check(file_is(gdir, L"dssl.dll", "sound v1") && !file_is(gdir, L"ronfix\\previous\\dssl.dll", "sound v1"),
	      "update: an unchanged file is not downloaded or moved");
	check(file_is(gdir, L"ronfix\\version.txt", "4 0.7-test\r\n"), "update: version.txt says release 4");

	r = update_check();
	check(r == 0 && file_is(gdir, L"ronfix\\version.txt", "4 0.7-test\r\n"), "update: the same release again is 'up to date'");

	/* release 5 whose core does not match its hash */
	wjoin(p, MAX_PATH, serve, L"ronfix-core.dll", 0);
	write_file(p, "new core 5 TAMPERED", 19);
	file_line(l2, "ronfix-core.dll", "ronfix-core.dll", "new core 5");
	wsprintfA(m, "ronfix-manifest 1\nversion 5\nname 0.8\n%s%s%s", l1, l2, l3);
	publish(m, 1);
	r = update_check();
	wjoin(q, MAX_PATH, gdir, L"ronfix\\pending\\ronfix-core.dll.part", 0);
	check(r == 4 && file_is(gdir, L"ronfix-core.dll", "new core 4") && file_is(gdir, L"ronfix\\version.txt", "4 0.7-test\r\n") &&
	          GetFileAttributesW(q) == INVALID_FILE_ATTRIBUTES,
	      "update: a file that does not match its hash stops the update, nothing changes, no leftovers");

	/* release 5 for another game build */
	wsprintfA(m, "ronfix-manifest 1\nversion 5\nname 0.8\ngame 12345678 1000\n%s", l1);
	publish(m, 1);
	r = update_check();
	check(r == 3 && file_is(gdir, L"ronfix\\version.txt", "4 0.7-test\r\n"), "update: a release for another game build is not installed");

	/* the game has restarted since release 4: the core moved to ronfix\previous is no longer loaded */
	if (held_img)
		FreeLibrary(held_img);
	held_img = 0;

	/* release 6, which Windows refuses on this PC: tried once, then not again for a week (no notice at every start) */
	wjoin(p, MAX_PATH, serve, L"ronfix-core.dll", 0);
	write_file(p, "new core 6", 10);
	file_line(l2, "ronfix-core.dll", "ronfix-core.dll", "new core 6");
	wsprintfA(m, "ronfix-manifest 1\nversion 6\nname 0.9\n%s%s%s", l1, l2, l3);
	publish(m, 1);
	test_day = 1000, test_refuse_image = 1, test_image_checks = 0;
	r = update_check();
	int refused_checks = test_image_checks;
	check(r == 6 && refused_checks == 1 && file_is(gdir, L"ronfix-core.dll", "new core 4") && file_is(gdir, L"ronfix\\version.txt", "4 0.7-test\r\n"),
	      "update: a release Windows refuses is not installed, nothing changes");
	test_day = 1006;
	r = update_check();
	check(r == 6 && test_image_checks == refused_checks, "update: 6 days later the refused release is not tried again (no download, no test load)");
	test_day = 1007, test_refuse_image = 0;
	r = update_check();
	check(r == 0 && file_is(gdir, L"ronfix-core.dll", "new core 6") && file_is(gdir, L"ronfix\\version.txt", "6 0.9\r\n"),
	      "update: a week later it is tried again, and installs once Windows allows it");
	test_day = 0;

	/* feed offline */
	WritePrivateProfileStringW(L"update", L"Url", L"http://127.0.0.1:9/", ini);
	r = update_check();
	check(r == 1, "update: no feed (offline) is reported, nothing changes");

	if (held_img)
		FreeLibrary(held_img);
	CloseHandle(ulog);
	ulog = INVALID_HANDLE_VALUE;
}

/* ------------------------------------------------------------------ log upload, against the real server code
 * (logserver/test/serve.mjs: src/index.js with an in-memory database that it dumps to RONFIX_TEST_LOGDUMP) */
static int contains(const char *h, const char *n)
{
	for (; *h; h++) {
		const char *a = h, *b = n;
		while (*a && *b && *a == *b)
			a++, b++;
		if (!*b)
			return 1;
	}
	return 0;
}

static int count_of(const char *h, const char *n)
{
	int c = 0;
	for (; *h; h++)
		if (starts(h, n))
			c++;
	return c;
}

static void test_upload(void)
{
	static WCHAR logs[300], dumpw[MAX_PATH], p[MAX_PATH], prof[MAX_PATH];
	static char dump[400000], t[2000], gd8[MAX_PATH * 3], user[200];
	if (!GetEnvironmentVariableW(L"RONFIX_TEST_LOGS", logs, 300) || !GetEnvironmentVariableW(L"RONFIX_TEST_LOGDUMP", dumpw, MAX_PATH) ||
	    !gdir[0]) {
		check(0, "log upload: RONFIX_TEST_LOGS/LOGDUMP not set (the runner needs node)");
		return;
	}
	wjoin(p, MAX_PATH, gdir, L"ronfix-update.log", 0);
	ulog = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	/* the Windows user name: the last part of the profile folder */
	GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
	narrow(prof, user, sizeof user);
	const char *u = user;
	for (const char *c = user; *c; c++)
		if (*c == '\\')
			u = c + 1;
	narrow(gdir, gd8, sizeof gd8);
	WritePrivateProfileStringW(L"upload", L"Url", logs, ini);
	WritePrivateProfileStringW(L"upload", L"Consent", L"0", ini);
	wsprintfA(t, "12:00:00.000 [1] NET + player joined the network list (#1): PlayerOne | id 1A2B3C4D5E6F7081 | Steam | "
	             "76561198000000001 | flags 0\r\n12:00:01.000 [1] read %sronfix.ini\r\n12:00:02.000 [1] PF post: member "
	             "{name=PlayerTwo,versionId=2.0}\r\n", gd8);
	wjoin(p, MAX_PATH, gdir, L"ronfix-net.log", 0);
	write_file(p, t, (DWORD)lstrlenA(t));
	DWORD first_len = (DWORD)lstrlenA(t);
	wjoin(p, MAX_PATH, gdir, L"ronfix-ids.txt", 0);
	write_file(p, "PlayerOne\r\n1A2B3C4D5E6F7081\r\n76561198000000001\r\nPlayerTwo\r\n", 59);

	int r = upload_logs();
	DWORD n = read_file(dumpw, dump, sizeof dump - 1);
	dump[n == (DWORD)-1 ? 0 : n] = 0;
	check(r == 1 && streq(dump, "[]"), "log upload: without the player's yes nothing is sent");

	WritePrivateProfileStringW(L"upload", L"Consent", L"1", ini);
	r = upload_logs();
	n = read_file(dumpw, dump, sizeof dump - 1);
	dump[n == (DWORD)-1 ? 0 : n] = 0;
	check(r == 0 && contains(dump, "NET + player joined the network list") && contains(dump, "P-") && contains(dump, "versionId=2.0"),
	      "log upload: with the player's yes the new log text arrives");
	check(!contains(dump, "PlayerOne") && !contains(dump, "PlayerTwo") && !contains(dump, "7656119") && !contains(dump, "1A2B3C4D5E6F7081"),
	      "log upload: no player name or id is stored on the server");
	check(contains(dump, "<game>") && u[0] && !contains(dump, u), "log upload: the game folder and the Windows user name never left the PC");
	/* (the upload itself adds a "logs sent" line to ronfix-update.log, so that one is new each time) */
	int rows = count_of(dump, "\"file\":\"ronfix-net.log\"");

	r = upload_logs();
	n = read_file(dumpw, dump, sizeof dump - 1);
	dump[n == (DWORD)-1 ? 0 : n] = 0;
	check(r == 0 && count_of(dump, "\"file\":\"ronfix-net.log\"") == rows, "log upload: a log with nothing new is not sent again");

	static const char more[] = "12:05:00.000 [1] LOAD sync point 7 reached by this PC\r\n";
	wjoin(p, MAX_PATH, gdir, L"ronfix-net.log", 0);
	HANDLE f = CreateFileW(p, FILE_APPEND_DATA, 0, NULL, OPEN_EXISTING, 0, NULL);
	DWORD w;
	WriteFile(f, more, sizeof more - 1, &w, NULL);
	CloseHandle(f);
	r = upload_logs();
	n = read_file(dumpw, dump, sizeof dump - 1);
	dump[n == (DWORD)-1 ? 0 : n] = 0;
	char off[40];
	wsprintfA(off, "\"offset\":%lu", first_len);
	check(r == 0 && contains(dump, "LOAD sync point 7") && contains(dump, off), "log upload: next time only the new part is sent");

	WritePrivateProfileStringW(L"upload", L"Url", L"http://127.0.0.1:9/v1/upload", ini);
	wjoin(p, MAX_PATH, gdir, L"ronfix-net.log", 0);
	f = CreateFileW(p, FILE_APPEND_DATA, 0, NULL, OPEN_EXISTING, 0, NULL);
	WriteFile(f, more, sizeof more - 1, &w, NULL);
	CloseHandle(f);
	r = upload_logs();
	DWORD kept = (DWORD)GetPrivateProfileIntW(L"offsets", L"ronfix-net.log", 0, (wjoin(p, MAX_PATH, gdir, L"ronfix\\upload.ini", 0), p));
	check(r == 2 && kept == first_len + sizeof more - 1, "log upload: server unreachable, the logs wait for the next start");
	CloseHandle(ulog);
	ulog = INVALID_HANDLE_VALUE;
}

__declspec(dllexport) int __stdcall run_update_tests(char *buf, int len)
{
	out = buf;
	out_left = len;
	fails = passes = 0;
	buf[0] = 0;
	test_names();
	test_manifest();
	test_signature();
	test_update();
	test_upload();
	char b[80];
	wsprintfA(b, "updater: %d passed, %d failed\r\n", passes, fails);
	say(b);
	return fails;
}
