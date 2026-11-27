/* RoN-Fix setup: one file, double-click, Install (setup v1, 2026-11-26).
 *
 * Carries every RoN-Fix file as a resource (setup.rc). Finds the game through Steam, checks that the game files
 * are the builds RoN-Fix was written for, keeps the originals (CrossplayNetLib_orig.dll, dssl_orig.dll) and puts
 * RoN-Fix's files in. "Restore original game" puts the originals back. Copies itself to <game>\ronfix\RoN-Fix.exe
 * with a Start menu entry, so the restore button is always one click away, and the updater can keep this copy
 * current too. The same window lists the players banned from rooms this player hosts and unbans them (bans.c).
 *
 * Command line (used by the tests, and by the elevated copy when the game folder needs admin rights):
 *   RoN-Fix-Setup.exe --install "<game folder>" [--no-updates] [--logs yes|no] [--no-shortcut] [--quiet]
 *                     [--result <file>]
 *   (--graphics <anything> is still accepted and ignored: 0.13 dropped better graphics, every install removes 0.12's)
 *   RoN-Fix-Setup.exe --restore "<game folder>" [--quiet] [--result <file>]
 * Exit code 0 = done, 1 = refused (message says why), 2 = needs admin rights, 3 = bad arguments. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <bcrypt.h>
#include <imagehlp.h>
#include <shellapi.h>
#include <urlmon.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if __has_include("version.h")
#include "version.h" /* written by tools/build-setup.ps1: RONFIX_RELEASE, RONFIX_NAME */
#endif
#ifndef RONFIX_RELEASE
#define RONFIX_RELEASE 0
#endif
#ifndef RONFIX_NAME
#define RONFIX_NAME L"dev"
#endif

/* builds RoN-Fix was written for (Steam build of 2024-06-20) */
#define EXE_STAMP 0x6674863fu
#define EXE_SIZE 0xbb4000u
#define NET_STAMP 0x667485bau
#define NET_SIZE 0x133000u
#define SND_STAMP 0x667485aeu
#define SND_SIZE 0x121000u

enum { R_LOADER = 101, R_CORE, R_UPDATE, R_SOUND, R_INI };

static WCHAR msg[2048]; /* result of the last action, shown to the player */
static int no_shortcut;  /* --no-shortcut (tests): no Start menu entry */
static HMODULE resmod;   /* where the embedded files are: NULL = this exe (a test DLL sets its own) */
static WCHAR selfpath[MAX_PATH]; /* this setup's file, copied to ronfix\RoN-Fix.exe */

static void say(const WCHAR *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf(msg, 2047, fmt, ap);
	msg[2047] = 0;
	va_end(ap);
}

static int path(WCHAR *out, const WCHAR *dir, const WCHAR *name)
{
	int n = _snwprintf(out, MAX_PATH, L"%ls\\%ls", dir, name);
	return n > 0 && n < MAX_PATH;
}

static int exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

/* PE TimeDateStamp + SizeOfImage */
static int pe_build(const WCHAR *p, DWORD *stamp, DWORD *size)
{
	unsigned char h[1024];
	DWORD got = 0;
	HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
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

static int is_build(const WCHAR *p, DWORD stamp, DWORD size)
{
	DWORD s, z;
	return pe_build(p, &s, &z) && s == stamp && z == size;
}

static int game_running(void)
{
	PROCESSENTRY32W e;
	int found = 0;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return 0;
	e.dwSize = sizeof e;
	for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
		if (!_wcsicmp(e.szExeFile, L"riseofnations.exe") || !_wcsicmp(e.szExeFile, L"patriots.exe"))
			found = 1;
	CloseHandle(snap);
	return found;
}

/* ------------------------------------------------------------------ finding the game */
static int find_game(WCHAR *out)
{
	WCHAR steam[MAX_PATH], vdf[MAX_PATH], cand[MAX_PATH];
	DWORD sz = sizeof steam;
	const WCHAR *def = L"C:\\Program Files (x86)\\Steam";
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, steam, &sz) != ERROR_SUCCESS)
		lstrcpyW(steam, def);
	for (WCHAR *c = steam; *c; c++)
		if (*c == '/')
			*c = '\\';
	_snwprintf(cand, MAX_PATH, L"%ls\\steamapps\\common\\Rise of Nations", steam);
	cand[MAX_PATH - 1] = 0;
	if (path(vdf, cand, L"riseofnations.exe") && exists(vdf)) {
		lstrcpyW(out, cand);
		return 1;
	}
	/* other Steam libraries: "path"  "D:\\SteamLibrary" lines in libraryfolders.vdf */
	if (!path(vdf, steam, L"steamapps\\libraryfolders.vdf"))
		return 0;
	HANDLE f = CreateFileW(vdf, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	static char buf[65536];
	DWORD got = 0;
	ReadFile(f, buf, sizeof buf - 1, &got, NULL);
	CloseHandle(f);
	buf[got] = 0;
	for (char *p = buf; (p = strstr(p, "\"path\"")) != NULL;) {
		p += 6;
		char *q = strchr(p, '"');
		if (!q)
			break;
		char *e = strchr(q + 1, '"');
		if (!e)
			break;
		char lib[MAX_PATH];
		int n = 0;
		for (char *s = q + 1; s < e && n < MAX_PATH - 1; s++) {
			if (s[0] == '\\' && s[1] == '\\')
				s++;
			lib[n++] = *s;
		}
		lib[n] = 0;
		WCHAR wl[MAX_PATH];
		MultiByteToWideChar(CP_UTF8, 0, lib, -1, wl, MAX_PATH);
		_snwprintf(cand, MAX_PATH, L"%ls\\steamapps\\common\\Rise of Nations", wl);
		cand[MAX_PATH - 1] = 0;
		if (path(vdf, cand, L"riseofnations.exe") && exists(vdf)) {
			lstrcpyW(out, cand);
			return 1;
		}
		p = e + 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ state */
enum state { S_NO_GAME, S_WRONG_BUILD, S_ORIGINAL, S_INSTALLED, S_OLD_RONFIX };

static int installed_release(const WCHAR *dir, WCHAR *name, int cap)
{
	WCHAR p[MAX_PATH];
	char b[64] = { 0 };
	DWORD got = 0;
	if (name)
		name[0] = 0;
	if (!path(p, dir, L"ronfix\\version.txt"))
		return 0;
	HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	ReadFile(f, b, sizeof b - 1, &got, NULL);
	CloseHandle(f);
	int v = atoi(b);
	char *sp = strchr(b, ' ');
	if (sp && name) {
		char *e = sp + 1;
		while (*e && *e != '\r' && *e != '\n')
			e++;
		*e = 0;
		MultiByteToWideChar(CP_UTF8, 0, sp + 1, -1, name, cap);
	}
	return v;
}

static enum state game_state(const WCHAR *dir)
{
	WCHAR p[MAX_PATH], o[MAX_PATH];
	if (!dir[0] || !path(p, dir, L"riseofnations.exe") || !exists(p))
		return S_NO_GAME;
	if (!is_build(p, EXE_STAMP, EXE_SIZE))
		return S_WRONG_BUILD;
	path(p, dir, L"CrossplayNetLib.dll");
	path(o, dir, L"CrossplayNetLib_orig.dll");
	if (is_build(p, NET_STAMP, NET_SIZE) || !exists(o))
		return S_ORIGINAL;
	path(p, dir, L"ronfix-core.dll");
	return exists(p) ? S_INSTALLED : S_OLD_RONFIX;
}

/* ------------------------------------------------------------------ files */
static int write_all(const WCHAR *p, const void *data, DWORD n)
{
	WCHAR tmp[MAX_PATH];
	DWORD w;
	if (_snwprintf(tmp, MAX_PATH, L"%ls.ronfix-new", p) <= 0)
		return 0;
	tmp[MAX_PATH - 1] = 0;
	HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	BOOL ok = WriteFile(f, data, n, &w, NULL) && w == n;
	CloseHandle(f);
	if (!ok || !MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING)) {
		DWORD e = GetLastError();
		DeleteFileW(tmp);
		SetLastError(e ? e : ERROR_WRITE_FAULT);
		return 0;
	}
	return 1;
}

static const void *res(int id, DWORD *n)
{
	HRSRC r = FindResourceW(resmod, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
	HGLOBAL g = r ? LoadResource(resmod, r) : NULL;
	*n = r ? SizeofResource(resmod, r) : 0;
	return g ? LockResource(g) : NULL;
}

/* 1 when the file at p holds exactly these n bytes */
static int file_equals(const WCHAR *p, const void *data, DWORD n)
{
	unsigned char buf[65536];
	DWORD got, at = 0;
	HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	int same = GetFileSize(f, NULL) == n;
	while (same && at < n && ReadFile(f, buf, sizeof buf, &got, NULL) && got) {
		same = at + got <= n && !memcmp(buf, (const unsigned char *)data + at, got);
		at += got;
	}
	CloseHandle(f);
	return same && at == n;
}

/* A file that already holds exactly these bytes is not written again. Windows' Smart App Control keeps its verdict
 * on a DLL with the file: a rewritten copy of the very same DLL is judged afresh, and on 2026-11-27 that turned an
 * allowed dssl.dll into a blocked one (reverse/reshade-steam-overlay.md, "Side effect"). */
static int put_res(const WCHAR *dir, const WCHAR *name, int id)
{
	WCHAR p[MAX_PATH];
	DWORD n;
	const void *d = res(id, &n);
	if (!d || !n) {
		SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return 0;
	}
	if (!path(p, dir, name))
		return 0;
	return file_equals(p, d, n) || write_all(p, d, n);
}

/* the same for copying a whole file (this setup as ronfix\RoN-Fix.exe) */
static int files_equal(const WCHAR *a, const WCHAR *b)
{
	HANDLE f = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	DWORD n = GetFileSize(f, NULL), got = 0;
	void *d = n && n != INVALID_FILE_SIZE ? HeapAlloc(GetProcessHeap(), 0, n) : NULL;
	int ok = d && ReadFile(f, d, n, &got, NULL) && got == n;
	CloseHandle(f);
	int same = ok && file_equals(b, d, n);
	if (d)
		HeapFree(GetProcessHeap(), 0, d);
	return same;
}

/* 4 GB flag (IMAGE_FILE_LARGE_ADDRESS_AWARE) with a correct header checksum. The old checksum is kept in
 * ronfix\laa.txt so "Restore original game" can put the file back byte for byte. */
static int set_laa(const WCHAR *dir, const WCHAR *exe, int on)
{
	WCHAR p[MAX_PATH], rec[MAX_PATH];
	if (!path(p, dir, exe) || !exists(p))
		return 1;
	path(rec, dir, L"ronfix\\laa.txt");
	HANDLE f = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	DWORD size = GetFileSize(f, NULL), got = 0;
	unsigned char *b = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
	int ok = b && ReadFile(f, b, size, &got, NULL) && got == size && size > 0x400;
	if (ok) {
		DWORD e = *(DWORD *)(b + 0x3c);
		WORD *ch = (WORD *)(b + e + 22);
		DWORD *sum = (DWORD *)(b + e + 24 + 64);
		WCHAR key[64], val[64];
		_snwprintf(key, 64, L"%ls", exe);
		if (on && !(*ch & IMAGE_FILE_LARGE_ADDRESS_AWARE)) {
			_snwprintf(val, 64, L"%08lx %08lx", *(DWORD *)(b + e + 8), *sum);
			WritePrivateProfileStringW(L"laa", key, val, rec);
			*ch |= IMAGE_FILE_LARGE_ADDRESS_AWARE;
			DWORD hs, cs;
			if (CheckSumMappedFile(b, size, &hs, &cs))
				*sum = cs;
		} else if (!on && (*ch & IMAGE_FILE_LARGE_ADDRESS_AWARE)) {
			DWORD stamp = 0, old = 0;
			GetPrivateProfileStringW(L"laa", key, L"", val, 64, rec);
			if (val[0] && swscanf(val, L"%lx %lx", &stamp, &old) == 2 && stamp == *(DWORD *)(b + e + 8)) {
				*ch &= ~IMAGE_FILE_LARGE_ADDRESS_AWARE; /* RoN-Fix set it: undo, old checksum back */
				*sum = old;
				WritePrivateProfileStringW(L"laa", key, NULL, rec);
			} else {
				HeapFree(GetProcessHeap(), 0, b);
				CloseHandle(f);
				return 1; /* was set before RoN-Fix (another 4GB patch): leave it */
			}
		} else {
			HeapFree(GetProcessHeap(), 0, b);
			CloseHandle(f);
			return 1;
		}
		DWORD w;
		ok = SetFilePointer(f, 0, NULL, FILE_BEGIN) == 0 && WriteFile(f, b, size, &w, NULL) && w == size;
	}
	if (b)
		HeapFree(GetProcessHeap(), 0, b);
	CloseHandle(f);
	return ok;
}

/* adds the keys of the default ronfix.ini that the player's ini does not have yet, keeps everything else */
static void merge_ini(const WCHAR *ini)
{
	DWORD n;
	const char *d = res(R_INI, &n);
	char line[512], sec[64] = "";
	for (DWORD i = 0; d && i < n;) {
		int k = 0;
		while (i < n && d[i] != '\n' && k < 511)
			line[k++] = d[i++];
		i++;
		line[k] = 0;
		while (k && (line[k - 1] == '\r' || line[k - 1] == ' '))
			line[--k] = 0;
		if (line[0] == '[') {
			char *e = strchr(line, ']');
			if (e && e - line < 63) {
				*e = 0;
				lstrcpyA(sec, line + 1);
			}
		} else if (line[0] && line[0] != ';' && sec[0]) {
			char *eq = strchr(line, '=');
			if (!eq)
				continue;
			*eq = 0;
			WCHAR ws[64], wk[128], wv[256], cur[16];
			MultiByteToWideChar(CP_UTF8, 0, sec, -1, ws, 64);
			MultiByteToWideChar(CP_UTF8, 0, line, -1, wk, 128);
			MultiByteToWideChar(CP_UTF8, 0, eq + 1, -1, wv, 256);
			GetPrivateProfileStringW(ws, wk, L"\x01", cur, 16, ini);
			if (cur[0] == 1)
				WritePrivateProfileStringW(ws, wk, wv, ini);
		}
	}
}

static void shortcut(const WCHAR *target)
{
	PWSTR progs = NULL;
	WCHAR lnk[MAX_PATH];
	IShellLinkW *sl = NULL;
	IPersistFile *pf = NULL;
	if (FAILED(SHGetKnownFolderPath(&FOLDERID_Programs, 0, NULL, &progs)))
		return;
	_snwprintf(lnk, MAX_PATH, L"%ls\\RoN-Fix.lnk", progs);
	lnk[MAX_PATH - 1] = 0;
	CoTaskMemFree(progs);
	if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) {
		IShellLinkW_SetPath(sl, target);
		IShellLinkW_SetDescription(sl, L"RoN-Fix: install, update or restore the original Rise of Nations");
		if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
			IPersistFile_Save(pf, lnk, TRUE);
			IPersistFile_Release(pf);
		}
		IShellLinkW_Release(sl);
	}
}

/* ------------------------------------------------------------------ the two actions. 0 ok, 1 refused, 2 admin */
static int fail_io(const WCHAR *what)
{
	DWORD e = GetLastError();
	if (e == ERROR_ACCESS_DENIED)
		return 2;
	say(L"Could not %ls (Windows error %lu). Nothing else was changed.", what, e);
	return 1;
}

/* The privacy notice, shown before the player decides (PRIVACY.md has the full text). */
static const WCHAR logs_short[] =
	L"RoN-Fix can send its own log files to its developer, to find and fix lag, disconnects and crashes.";
static const WCHAR logs_details[] =
	L"What is sent, at each game start: the new parts of RoN-Fix's own logs (lobby joins and leaves, ready clicks, "
	L"match timing such as ping and lag per player, loading times, problems RoN-Fix caught, update results), "
	L"RoN-Fix's version and a random install code.\n\n"
	L"Not sent: your files, saved games, chat, passwords, or anything outside RoN-Fix's logs.\n\n"
	L"Player names and Steam/PlayFab IDs (yours and other players') are replaced with codes before anything is "
	L"stored; your Windows user name never leaves your PC. Only the RoN-Fix developer sees the logs; they are "
	L"deleted after 180 days.\n\nYou can change this any time: open RoN-Fix from the Start menu.";

static int consent_known(const WCHAR *dir)
{
	WCHAR p[MAX_PATH], v[8];
	if (!path(p, dir, L"ronfix.ini"))
		return 0;
	GetPrivateProfileStringW(L"upload", L"Consent", L"\x01", v, 8, p);
	return v[0] != 1;
}

static int consent_now(const WCHAR *dir)
{
	WCHAR p[MAX_PATH];
	return path(p, dir, L"ronfix.ini") && GetPrivateProfileIntW(L"upload", L"Consent", 0, p) == 1;
}

#include "graphics.c"
#include "bans.c" /* the ban list: what the "Banned players" window shows and edits */

/* logs: 1 send, 0 don't, -1 leave as it is */
static int do_install(const WCHAR *dir, int updates, int logs)
{
	WCHAR p[MAX_PATH], o[MAX_PATH], copy_to[MAX_PATH];
	static const WCHAR *const names[2] = { L"CrossplayNetLib", L"dssl" };
	static const DWORD stamps[2] = { NET_STAMP, SND_STAMP }, sizes[2] = { NET_SIZE, SND_SIZE };
	int move_orig[2];

	if (game_running()) {
		say(L"Rise of Nations is running. Close the game, then press Install again.");
		return 1;
	}
	switch (game_state(dir)) {
	case S_NO_GAME:
		say(L"There is no Rise of Nations in that folder. Use \"Change...\" to pick the folder that has riseofnations.exe.");
		return 1;
	case S_WRONG_BUILD:
		say(L"This Rise of Nations is a different version from the one RoN-Fix %ls was made for, so nothing was "
		    L"changed. Steam may have updated the game: a new RoN-Fix will follow.", RONFIX_NAME);
		return 1;
	default:
		break;
	}
	/* 1. check both originals before touching anything */
	for (int k = 0; k < 2; k++) {
		WCHAR n1[64], n2[64];
		_snwprintf(n1, 64, L"%ls.dll", names[k]);
		_snwprintf(n2, 64, L"%ls_orig.dll", names[k]);
		path(p, dir, n1);
		path(o, dir, n2);
		if (is_build(p, stamps[k], sizes[k]))
			move_orig[k] = 1; /* the game's own file: becomes _orig (also after Steam restored it) */
		else if (is_build(o, stamps[k], sizes[k]))
			move_orig[k] = 0; /* RoN-Fix is in already, original kept */
		else {
			say(L"%ls is not the version RoN-Fix was made for, so nothing was changed. In Steam: right-click Rise of "
			    L"Nations > Properties > Installed Files > Verify integrity of game files, then try again.", n1);
			return 1;
		}
	}
	path(p, dir, L"ronfix");
	if (!CreateDirectoryW(p, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
		return fail_io(L"create the ronfix folder");
	/* 2. RoN-Fix's own new files first (harmless alone), then each stand-in: original aside, stand-in in, and
	 *    the original straight back if that fails, so the game is never left without a file it needs.
	 *    The loader goes last: it is the file that switches RoN-Fix on. */
	if (!put_res(dir, L"ronfix-core.dll", R_CORE) || !put_res(dir, L"ronfix-update.dll", R_UPDATE))
		return fail_io(L"copy RoN-Fix's files into the game folder");
	static const int ids[2] = { R_LOADER, R_SOUND };
	for (int k = 1; k >= 0; k--) {
		WCHAR n1[64], n2[64];
		_snwprintf(n1, 64, L"%ls.dll", names[k]);
		_snwprintf(n2, 64, L"%ls_orig.dll", names[k]);
		path(p, dir, n1);
		path(o, dir, n2);
		if (move_orig[k] && !MoveFileExW(p, o, MOVEFILE_REPLACE_EXISTING))
			return fail_io(L"keep the original game file aside");
		if (!put_res(dir, n1, ids[k])) {
			DWORD e = GetLastError();
			if (move_orig[k])
				MoveFileExW(o, p, MOVEFILE_REPLACE_EXISTING);
			SetLastError(e);
			return fail_io(L"copy RoN-Fix's files into the game folder");
		}
	}
	/* 3. settings */
	path(p, dir, L"ronfix.ini");
	if (!exists(p) && !put_res(dir, L"ronfix.ini", R_INI))
		return fail_io(L"write ronfix.ini");
	merge_ini(p);
	WritePrivateProfileStringW(L"update", L"Enabled", updates ? L"1" : L"0", p);
	WritePrivateProfileStringW(L"ronfix", L"Off", L"0", p);
	if (logs >= 0)
		WritePrivateProfileStringW(L"upload", L"Consent", logs ? L"1" : L"0", p);
	/* 4. memory: 4 GB */
	int laa = set_laa(dir, L"riseofnations.exe", 1) && set_laa(dir, L"patriots.exe", 1);
	/* 5. this setup as <game>\ronfix\RoN-Fix.exe + Start menu entry; the release number for the updater */
	if (!selfpath[0])
		GetModuleFileNameW(NULL, selfpath, MAX_PATH);
	path(copy_to, dir, L"ronfix\\RoN-Fix.exe");
	if (_wcsicmp(selfpath, copy_to) && !files_equal(selfpath, copy_to))
		CopyFileW(selfpath, copy_to, FALSE);
	if (!no_shortcut)
		shortcut(copy_to);
	path(p, dir, L"ronfix\\version.txt");
	char v[64];
	int vn = _snprintf(v, sizeof v, "%d %ls\r\n", RONFIX_RELEASE, RONFIX_NAME);
	if (!file_equals(p, v, (DWORD)vn))
		write_all(p, v, (DWORD)vn);
	/* 6. 0.12's better graphics (ReShade) is taken out: with the Steam overlay on it could stop the game at start */
	const WCHAR *gfx_note = graphics_remove(dir) ? L"\n\nBetter graphics (ReShade) from RoN-Fix 0.12 was removed: with Steam's "
	                                               L"overlay on it could stop the game from starting." : L"";
	say(L"RoN-Fix %ls is installed. Start Rise of Nations from Steam as usual.%ls%ls%ls", RONFIX_NAME,
	    updates ? L" New versions download by themselves while you play and are used from the next start." : L" Automatic updates are off.",
	    laa ? L"" : L"\n\n(The 4 GB memory setting could not be applied; everything else is in.)", gfx_note);
	return 0;
}

static int do_restore(const WCHAR *dir)
{
	WCHAR p[MAX_PATH], o[MAX_PATH];
	static const WCHAR *const names[2] = { L"CrossplayNetLib", L"dssl" };
	if (game_running()) {
		say(L"Rise of Nations is running. Close the game, then press Restore again.");
		return 1;
	}
	if (game_state(dir) == S_NO_GAME) {
		say(L"There is no Rise of Nations in that folder.");
		return 1;
	}
	graphics_remove(dir);
	for (int k = 0; k < 2; k++) {
		WCHAR n1[64], n2[64];
		_snwprintf(n1, 64, L"%ls.dll", names[k]);
		_snwprintf(n2, 64, L"%ls_orig.dll", names[k]);
		path(p, dir, n1);
		path(o, dir, n2);
		if (exists(o) && !MoveFileExW(o, p, MOVEFILE_REPLACE_EXISTING))
			return fail_io(L"put the original game file back");
	}
	path(p, dir, L"ronfix-core.dll");
	DeleteFileW(p);
	path(p, dir, L"ronfix-update.dll");
	DeleteFileW(p);
	path(p, dir, L"ronfix\\version.txt");
	DeleteFileW(p);
	int laa = set_laa(dir, L"riseofnations.exe", 0) && set_laa(dir, L"patriots.exe", 0);
	path(p, dir, L"ronfix.ini");
	int pack = GetPrivateProfileIntW(L"gamefix", L"Version", 0, p);
	if (exists(p))
		WritePrivateProfileStringW(L"gamefix", L"Version", L"0", p);
	say(L"The original game is back: Rise of Nations runs exactly as Steam installed it.%ls%ls\n\nRoN-Fix stays in the "
	    L"Start menu if you want it again.",
	    laa ? L"" : L"\n\n(The 4 GB memory setting could not be undone; Steam's \"Verify integrity of game files\" does it.)",
	    pack > 0 ? L"\n\nThe gameplay pack from the old zip version changed game data files: use Steam's \"Verify "
	               L"integrity of game files\" to put those back too."
	             : L"");
	return 0;
}

#ifndef SETUP_TEST /* tests/setup_tests.c builds the actions above into a DLL */
/* run the action, in an elevated copy of this exe if the folder needs admin rights */
static int run_action(HWND owner, int restore, const WCHAR *dir, int updates, int logs)
{
	int r = restore ? do_restore(dir) : do_install(dir, updates, logs);
	if (r != 2)
		return r;
	WCHAR self[MAX_PATH], args[2 * MAX_PATH + 128], res_file[MAX_PATH];
	GetModuleFileNameW(NULL, self, MAX_PATH);
	GetTempPathW(MAX_PATH - 40, res_file);
	lstrcatW(res_file, L"ronfix-setup-result.txt");
	DeleteFileW(res_file);
	_snwprintf(args, 2 * MAX_PATH + 127, L"%ls \"%ls\" --quiet%ls%ls --result \"%ls\"", restore ? L"--restore" : L"--install", dir,
	           updates ? L"" : L" --no-updates", logs < 0 ? L"" : logs ? L" --logs yes" : L" --logs no", res_file);
	args[2 * MAX_PATH + 127] = 0;
	SHELLEXECUTEINFOW se = { sizeof se };
	se.fMask = SEE_MASK_NOCLOSEPROCESS;
	se.hwnd = owner;
	se.lpVerb = L"runas";
	se.lpFile = self;
	se.lpParameters = args;
	se.nShow = SW_HIDE;
	if (!ShellExecuteExW(&se) || !se.hProcess) {
		say(L"Windows needs your permission to change the game folder, and it was not given. Nothing was changed.");
		return 1;
	}
	WaitForSingleObject(se.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(se.hProcess, &code);
	CloseHandle(se.hProcess);
	HANDLE f = CreateFileW(res_file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f != INVALID_HANDLE_VALUE) {
		DWORD got = 0;
		ReadFile(f, msg, sizeof msg - sizeof(WCHAR), &got, NULL);
		msg[got / sizeof(WCHAR)] = 0;
		CloseHandle(f);
		DeleteFileW(res_file);
	}
	return (int)code;
}

/* ------------------------------------------------------------------ window */
enum { ID_CHANGE = 10, ID_INSTALL, ID_RESTORE, ID_CLOSE, ID_UPDATES, ID_STATUS, ID_DIR, ID_LOGS, ID_LOGS_INFO, ID_BANS, ID_BAN_LIST,
       ID_BAN_UNBAN };
static WCHAR gamedir[MAX_PATH];
static HWND h_dir, h_status, h_install, h_restore, h_updates, h_logs, h_bans_text, h_bans;

/* asks once, before the first install: 1 send, 0 don't, -1 the player closed the question (install cancelled) */
static int ask_logs(HWND w)
{
	TASKDIALOG_BUTTON b[2] = { { 100, L"Send logs\nHelps find and fix lag, disconnects and crashes" },
		                       { 101, L"Don't send" } };
	TASKDIALOGCONFIG c = { sizeof c };
	int pressed = 0;
	c.hwndParent = w;
	c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_EXPAND_FOOTER_AREA;
	c.pszWindowTitle = L"RoN-Fix";
	c.pszMainInstruction = L"Help fix Rise of Nations?";
	c.pszContent = logs_short;
	c.pszExpandedInformation = logs_details;
	c.pszExpandedControlText = L"Hide what is sent";
	c.pszCollapsedControlText = L"What is sent?";
	c.pButtons = b;
	c.cButtons = 2;
	c.nDefaultButton = 100;
	if (FAILED(TaskDialogIndirect(&c, &pressed, NULL, NULL)) || pressed == IDCANCEL)
		return -1;
	return pressed == 100;
}
static HFONT font, bold;
static int dpi = 96;

static int S(int v) { return MulDiv(v, dpi, 96); }

/* the main window's ban line: how many players are banned from rooms this player hosts */
static void bans_line(void)
{
	WCHAR t[160];
	int n = bans_read(gamedir);
	if (n < 0)
		lstrcpyW(t, L"Banned players: ronfix-bans.txt is too big for this window, edit it in Notepad.");
	else if (!n)
		lstrcpyW(t, L"Nobody is banned. When you host, Shift + Boot Player bans someone.");
	else
		_snwprintf(t, 160, L"Players banned from the rooms you host: %d", n);
	t[159] = 0;
	SetWindowTextW(h_bans_text, t);
	EnableWindow(h_bans, n > 0);
}

static void refresh(void)
{
	WCHAR t[512], name[32];
	SetWindowTextW(h_dir, gamedir[0] ? gamedir : L"(not found: press Change...)");
	enum state s = game_state(gamedir);
	int rel = installed_release(gamedir, name, 32);
	switch (s) {
	case S_NO_GAME:
		lstrcpyW(t, L"Rise of Nations was not found. Press \"Change...\" and pick the game's folder.");
		break;
	case S_WRONG_BUILD:
		_snwprintf(t, 512, L"This is a different version of Rise of Nations from the one RoN-Fix %ls was made for.", RONFIX_NAME);
		break;
	case S_ORIGINAL:
		lstrcpyW(t, L"Status: original game, RoN-Fix is not installed.");
		break;
	case S_INSTALLED:
		if (rel > RONFIX_RELEASE)
			_snwprintf(t, 512, L"Status: RoN-Fix %ls is installed (newer than this setup, %ls).", name, RONFIX_NAME);
		else if (rel == RONFIX_RELEASE)
			_snwprintf(t, 512, L"Status: RoN-Fix %ls is installed.", RONFIX_NAME);
		else
			_snwprintf(t, 512, L"Status: RoN-Fix %ls is installed; Install updates it to %ls.", name[0] ? name : L"(older)", RONFIX_NAME);
		break;
	case S_OLD_RONFIX:
		_snwprintf(t, 512, L"Status: an older RoN-Fix (zip version) is installed; Install updates it to %ls.", RONFIX_NAME);
		break;
	}
	t[511] = 0;
	SetWindowTextW(h_status, t);
	EnableWindow(h_install, s != S_NO_GAME && s != S_WRONG_BUILD);
	EnableWindow(h_restore, s == S_INSTALLED || s == S_OLD_RONFIX);
	SendMessageW(h_logs, BM_SETCHECK, consent_now(gamedir) ? BST_CHECKED : BST_UNCHECKED, 0);
	SetWindowTextW(h_install, s == S_INSTALLED || s == S_OLD_RONFIX ? (rel >= RONFIX_RELEASE ? L"Reinstall" : L"Update") : L"Install");
	bans_line();
}

static void pick_folder(HWND w)
{
	IFileOpenDialog *d = NULL;
	if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
		return;
	FILEOPENDIALOGOPTIONS o;
	IFileOpenDialog_GetOptions(d, &o);
	IFileOpenDialog_SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
	IFileOpenDialog_SetTitle(d, L"Pick the Rise of Nations folder (the one with riseofnations.exe)");
	if (SUCCEEDED(IFileOpenDialog_Show(d, w))) {
		IShellItem *it;
		if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
			PWSTR p;
			if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
				lstrcpynW(gamedir, p, MAX_PATH);
				CoTaskMemFree(p);
			}
			IShellItem_Release(it);
		}
	}
	IFileOpenDialog_Release(d);
	refresh();
}

static HWND ctl(HWND p, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id, HFONT f)
{
	HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), p, (HMENU)(INT_PTR)id, NULL, NULL);
	SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
	return c;
}

/* ------------------------------------------------------------------ "Banned players" window */
static HWND h_ban_list, h_ban_unban, h_ban_info;

static void ban_col(int i, const WCHAR *title, int width)
{
	LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH };
	c.pszText = (WCHAR *)title;
	c.cx = S(width);
	SendMessageW(h_ban_list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
}

static void ban_fill(void)
{
	int n = bans_read(gamedir);
	SendMessageW(h_ban_list, LVM_DELETEALLITEMS, 0, 0);
	for (int k = 0; k < nbans; k++) {
		WCHAR ids[64];
		LVITEMW it = { LVIF_TEXT | LVIF_PARAM };
		it.iItem = k;
		it.pszText = bans[k].name;
		it.lParam = k;
		int row = (int)SendMessageW(h_ban_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
		LVITEMW sub = { 0 };
		sub.iSubItem = 1;
		sub.pszText = bans[k].date[0] ? bans[k].date : L"-";
		SendMessageW(h_ban_list, LVM_SETITEMTEXTW, row, (LPARAM)&sub);
		_snwprintf(ids, 64, L"%hs", bans[k].id[0]); /* the first id: two do not fit the column */
		ids[63] = 0;
		sub.iSubItem = 2;
		sub.pszText = ids;
		SendMessageW(h_ban_list, LVM_SETITEMTEXTW, row, (LPARAM)&sub);
	}
	SetWindowTextW(h_ban_info, n < 0 ? L"ronfix-bans.txt (in the game folder) is too big for this window: edit it in Notepad."
	                           : n == 0 ? L"Nobody is banned. When you host, hold Shift and press Boot Player to ban a player."
	                                    : L"These players are removed from every room you host. Select a player and press Unban "
	                                      L"to let them in again.");
	EnableWindow(h_ban_unban, FALSE);
}

static void ban_unban_selected(HWND d)
{
	static char ids[BAN_MAX * 4][24]; /* copied out: bans_unban reads the file again into bans[] */
	WCHAR q[200];
	int nids = 0, picked = 0, first = -1;
	for (int row = -1; (row = (int)SendMessageW(h_ban_list, LVM_GETNEXTITEM, row, LVNI_SELECTED)) >= 0;) {
		LVITEMW it = { LVIF_PARAM };
		it.iItem = row;
		SendMessageW(h_ban_list, LVM_GETITEMW, 0, (LPARAM)&it);
		int k = (int)it.lParam;
		if (k < 0 || k >= nbans)
			continue;
		if (first < 0)
			first = k;
		picked++;
		for (int i = 0; i < bans[k].nid; i++)
			lstrcpynA(ids[nids++], bans[k].id[i], 24);
	}
	if (!picked)
		return;
	if (picked == 1)
		_snwprintf(q, 200, L"Unban %ls? They can join the rooms you host again.", bans[first].name);
	else
		_snwprintf(q, 200, L"Unban these %d players? They can join the rooms you host again.", picked);
	q[199] = 0;
	if (MessageBoxW(d, q, L"RoN-Fix", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
		return;
	int r = bans_unban(gamedir, ids, nids);
	if (r)
		MessageBoxW(d, r == 1 ? L"ronfix-bans.txt (in the game folder) is too big for this window: edit it in Notepad."
		                      : L"Windows did not let RoN-Fix change ronfix-bans.txt in the game folder. Nobody was unbanned.",
		            L"RoN-Fix", MB_OK | MB_ICONWARNING);
	ban_fill();
}

static INT_PTR CALLBACK bans_dlgproc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
	switch (m) {
	case WM_INITDIALOG: {
		SetWindowTextW(d, L"RoN-Fix: banned players");
		RECT r = { 0, 0, S(480), S(344) }, o;
		AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(d, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(d, GWL_EXSTYLE));
		GetWindowRect(GetParent(d), &o);
		int cw = r.right - r.left, ch = r.bottom - r.top;
		SetWindowPos(d, NULL, (o.left + o.right - cw) / 2, (o.top + o.bottom - ch) / 2, cw, ch, SWP_NOZORDER);
		h_ban_info = ctl(d, L"STATIC", L"", 0, 16, 14, 448, 36, 0, font);
		h_ban_list = ctl(d, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 16, 54, 448, 190,
		                 ID_BAN_LIST, font);
		SendMessageW(h_ban_list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
		ban_col(0, L"Player", 190);
		ban_col(1, L"Banned on", 90);
		ban_col(2, L"ID", 144);
		h_ban_unban = ctl(d, L"BUTTON", L"Unban", BS_PUSHBUTTON | WS_TABSTOP, 16, 256, 120, 30, ID_BAN_UNBAN, font);
		ctl(d, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 364, 256, 100, 30, IDCANCEL, font);
		ctl(d, L"STATIC", L"An unban works within a few seconds, also while the game is running. Someone you removed from "
		                  L"the room that is open now can join again once you open a new room.",
		    0, 16, 296, 448, 40, 0, font);
		ban_fill();
		SetFocus(h_ban_list);
		return FALSE;
	}
	case WM_NOTIFY: {
		NMHDR *h = (NMHDR *)lp;
		if (h->idFrom == ID_BAN_LIST && h->code == LVN_ITEMCHANGED)
			EnableWindow(h_ban_unban, SendMessageW(h_ban_list, LVM_GETSELECTEDCOUNT, 0, 0) > 0);
		else if (h->idFrom == ID_BAN_LIST && h->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)lp)->wVKey == VK_DELETE)
			ban_unban_selected(d);
		return FALSE;
	}
	case WM_COMMAND:
		if (LOWORD(wp) == ID_BAN_UNBAN)
			ban_unban_selected(d);
		else if (LOWORD(wp) == IDCANCEL)
			EndDialog(d, 0);
		return TRUE;
	case WM_CTLCOLORDLG:
		return (INT_PTR)GetSysColorBrush(COLOR_WINDOW);
	case WM_CTLCOLORSTATIC:
		SetBkMode((HDC)wp, TRANSPARENT);
		return (INT_PTR)GetSysColorBrush(COLOR_WINDOW);
	}
	return FALSE;
}

static void bans_window(HWND owner)
{
	/* an empty dialog template: the controls are made in WM_INITDIALOG like the main window's */
	static struct {
		DLGTEMPLATE t;
		WORD menu, cls, title;
	} __attribute__((aligned(4))) tpl;
	tpl.t.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
	tpl.t.cx = 300;
	tpl.t.cy = 200;
	DialogBoxIndirectParamW(GetModuleHandleW(NULL), &tpl.t, owner, bans_dlgproc, 0);
	bans_line();
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	switch (m) {
	case WM_CREATE: {
		WCHAR title[64];
		_snwprintf(title, 64, L"RoN-Fix %ls", RONFIX_NAME);
		ctl(w, L"STATIC", title, 0, 20, 16, 500, 26, 0, bold);
		ctl(w, L"STATIC", L"Unofficial fixes for Rise of Nations: Extended Edition on Steam: the freeze at start-up, "
		                  L"crashes, frozen players. You can still play with people who don't have it.",
		    0, 20, 46, 520, 36, 0, font);
		ctl(w, L"STATIC", L"Game folder:", 0, 20, 92, 90, 20, 0, font);
		h_dir = ctl(w, L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_BORDER, 20, 112, 420, 24, ID_DIR, font);
		ctl(w, L"BUTTON", L"Change...", BS_PUSHBUTTON | WS_TABSTOP, 450, 111, 90, 26, ID_CHANGE, font);
		h_status = ctl(w, L"STATIC", L"", 0, 20, 146, 520, 36, ID_STATUS, font);
		h_updates = ctl(w, L"BUTTON", L"Keep RoN-Fix up to date automatically (recommended)", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 186,
		                 520, 22, ID_UPDATES, font);
		SendMessageW(h_updates, BM_SETCHECK, BST_CHECKED, 0);
		h_logs = ctl(w, L"BUTTON", L"Send RoN-Fix's logs to help fix the game", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 212, 400, 22,
		             ID_LOGS, font);
		ctl(w, L"BUTTON", L"What is sent?", BS_PUSHBUTTON | WS_TABSTOP, 430, 210, 110, 26, ID_LOGS_INFO, font);
		h_install = ctl(w, L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 20, 252, 150, 32, ID_INSTALL, font);
		h_restore = ctl(w, L"BUTTON", L"Restore original game", BS_PUSHBUTTON | WS_TABSTOP, 180, 252, 180, 32, ID_RESTORE, font);
		ctl(w, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 440, 252, 100, 32, ID_CLOSE, font);
		h_bans_text = ctl(w, L"STATIC", L"", 0, 20, 302, 410, 20, 0, font);
		h_bans = ctl(w, L"BUTTON", L"Unban...", BS_PUSHBUTTON | WS_TABSTOP, 440, 298, 100, 26, ID_BANS, font);
		ctl(w, L"STATIC", L"To play the original game just once: hold Shift while the game starts, or add -original to "
		                  L"its launch options in Steam.",
		    0, 20, 336, 520, 36, 0, font);
		refresh();
		return 0;
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case ID_CHANGE:
			pick_folder(w);
			return 0;
		case ID_INSTALL:
		case ID_RESTORE: {
			int restore = LOWORD(wp) == ID_RESTORE;
			if (restore && MessageBoxW(w, L"Put the original game files back and switch RoN-Fix off?", L"RoN-Fix",
			                           MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
				return 0;
			int logs = -1;
			if (!restore) {
				/* the first install asks (and shows what is sent); after that the checkbox is the answer */
				logs = consent_known(gamedir) ? SendMessageW(h_logs, BM_GETCHECK, 0, 0) == BST_CHECKED : ask_logs(w);
				if (logs < 0)
					return 0;
			}
			HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
			int r = run_action(w, restore, gamedir, SendMessageW(h_updates, BM_GETCHECK, 0, 0) == BST_CHECKED, logs);
			SetCursor(old);
			refresh();
			MessageBoxW(w, msg, L"RoN-Fix", MB_OK | (r == 0 ? MB_ICONINFORMATION : MB_ICONWARNING));
			return 0;
		}
		case ID_LOGS_INFO: {
			static WCHAR t[2048];
			_snwprintf(t, 2047, L"%ls\n\n%ls\n\nFull text: PRIVACY.md at github.com/soul-builds/RoN-Fix", logs_short, logs_details);
			t[2047] = 0;
			MessageBoxW(w, t, L"RoN-Fix: what is sent", MB_OK | MB_ICONINFORMATION);
			return 0;
		}
		case ID_BANS:
			bans_window(w);
			return 0;
		case ID_CLOSE:
			DestroyWindow(w);
			return 0;
		}
		break;
	case WM_ACTIVATE: /* the game may have banned someone while this window was open */
		if (LOWORD(wp) != WA_INACTIVE && h_bans_text)
			bans_line();
		break;
	case WM_CTLCOLORSTATIC:
		if ((HWND)lp != h_dir) {
			SetBkMode((HDC)wp, TRANSPARENT);
			return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
		}
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(w, m, wp, lp);
}

static void write_result(const WCHAR *file)
{
	if (!file)
		return;
	HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	DWORD w;
	WriteFile(f, msg, (DWORD)(lstrlenW(msg) * sizeof(WCHAR)), &w, NULL);
	CloseHandle(f);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
	int argc;
	(void)prev, (void)cmd;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

	/* command line mode */
	if (argc >= 3 && (!wcscmp(argv[1], L"--install") || !wcscmp(argv[1], L"--restore"))) {
		int updates = 1, quiet = 0, logs = -1;
		const WCHAR *result = NULL;
		for (int i = 3; i < argc; i++) {
			if (!wcscmp(argv[i], L"--no-updates"))
				updates = 0;
			else if (!wcscmp(argv[i], L"--quiet"))
				quiet = 1;
			else if (!wcscmp(argv[i], L"--logs") && i + 1 < argc)
				logs = !wcscmp(argv[++i], L"yes");
			else if (!wcscmp(argv[i], L"--graphics") && i + 1 < argc)
				i++; /* 0.12's option: ignored, every install removes 0.12's better graphics */
			else if (!wcscmp(argv[i], L"--no-shortcut"))
				no_shortcut = 1;
			else if (!wcscmp(argv[i], L"--result") && i + 1 < argc)
				result = argv[++i];
			else
				return 3;
		}
		int r = !wcscmp(argv[1], L"--restore") ? do_restore(argv[2]) : do_install(argv[2], updates, logs);
		if (r == 2)
			say(L"Windows did not allow RoN-Fix to change the game folder (access denied).");
		write_result(result);
		if (!quiet)
			MessageBoxW(NULL, msg, L"RoN-Fix", MB_OK | (r == 0 ? MB_ICONINFORMATION : MB_ICONWARNING));
		return r;
	}

	/* window */
	INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };
	InitCommonControlsEx(&icc);
	HDC dc = GetDC(NULL);
	dpi = GetDeviceCaps(dc, LOGPIXELSY);
	ReleaseDC(NULL, dc);
	NONCLIENTMETRICSW ncm = { sizeof ncm };
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
	font = CreateFontIndirectW(&ncm.lfMessageFont);
	ncm.lfMessageFont.lfWeight = FW_BOLD;
	ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 3, 2);
	bold = CreateFontIndirectW(&ncm.lfMessageFont);
	find_game(gamedir);

	WNDCLASSW wc = { 0 };
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
	wc.lpszClassName = L"RoNFixSetup";
	RegisterClassW(&wc);
	RECT r = { 0, 0, S(560), S(384) };
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	AdjustWindowRect(&r, style, FALSE);
	WCHAR title[64];
	_snwprintf(title, 64, L"RoN-Fix %ls setup", RONFIX_NAME);
	HWND w = CreateWindowW(L"RoNFixSetup", title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
	                       NULL, NULL, inst, NULL);
	ShowWindow(w, show);
	MSG mm;
	while (GetMessageW(&mm, NULL, 0, 0) > 0)
		if (!IsDialogMessageW(w, &mm)) {
			TranslateMessage(&mm);
			DispatchMessageW(&mm);
		}
	CoUninitialize();
	return 0;
}
#endif /* SETUP_TEST */
