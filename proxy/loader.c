/* RoN-Fix loader: the CrossplayNetLib.dll stand-in (loader v1 2026-11-26; v2 2026-11-27: core loaded after start-up;
 * v3: no "Bad Image" box when Windows refuses a file).
 *
 * The game imports CrossplayNetLib.dll statically: if Windows refuses this file, the game cannot start at all.
 * So it does as little as possible and almost never changes:
 *  1. makes the process's first BCryptGenRandom call at load: the startup-deadlock fix (netproxy.c DllMain has
 *     the story). This runs before any other thread of the game exists;
 *  2. loads ronfix-core.dll (the fixes, netproxy.c) from the game folder, unless RoN-Fix is off for this start:
 *     Shift held while the game starts, "-original" on the command line (Steam launch options) or ronfix.ini
 *     [ronfix] Off=1. If the core is missing or Windows refuses it (Smart App Control can block a new unsigned
 *     build), the game runs on CrossplayNetLib_orig.dll exactly as it would without RoN-Fix;
 *  3. some seconds after the game started, on a low-priority thread, loads ronfix-update.dll (update.c) and
 *     lets it look for a new RoN-Fix. If that file is missing or refused, there are simply no updates.
 * The other ten exports forward to CrossplayNetLib_orig.dll (loader.def). No CRT: build with -nostdlib.
 * Log: <game>\ronfix-update.log. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#define LOADER_VERSION 3

#ifndef ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION
#define ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION 4551L
#endif

typedef void *(__cdecl *get_obj_fn)(void *);
typedef int(__cdecl *update_fn)(const WCHAR *gamedir, HANDLE log);

static WCHAR gdir[MAX_PATH]; /* game folder, ends in '\' */
static WCHAR ini[MAX_PATH];
static HANDLE ulog = INVALID_HANDLE_VALUE;
static get_obj_fn core_get;
static DWORD core_err;   /* GetLastError of a failed core load */
static int core_present; /* ronfix-core.dll exists */
static int core_skipped; /* not loaded: an earlier start died loading this exact file (load_core) */
static int off_reason;   /* 0 on, 1 Shift, 2 -original, 3 ini */
static int warmed;
static long warm_status;

#include "ronfix_util.h"

static const char *refused(DWORD e)
{
	return e == ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION || e == ERROR_INVALID_IMAGE_HASH
	           ? ", blocked by Windows (Smart App Control or code integrity)" : "";
}

static HMODULE quiet_load(const WCHAR *p);

static DWORD WINAPI background(void *unused)
{
	char nb[600];
	WCHAR p[MAX_PATH];
	(void)unused;
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	ulog_open();
	ulogf("==== RoN-Fix loader v%d, folder %s", LOADER_VERSION, narrow(gdir, nb, sizeof nb));
	if (warmed)
		ulogf("startup: crypto warmed at load (status 0x%08lx)", (unsigned long)warm_status);
	if (off_reason)
		ulogf("RoN-Fix OFF for this start (%s): original game",
		      off_reason == 1 ? "Shift held" : off_reason == 2 ? "-original on the command line" : "ronfix.ini [ronfix] Off=1");
	else if (core_get)
		ulogf("fixes loaded (ronfix-core.dll)");
	else if (core_skipped)
		ulogf("ronfix-core.dll skipped: an earlier start stopped while loading this file (Windows refusing it, e.g. Smart "
		      "App Control); original game until an update brings a new one");
	else if (!core_present)
		ulogf("ronfix-core.dll is missing: original game (run RoN-Fix setup again)");
	else
		ulogf("ronfix-core.dll could NOT be loaded (error %lu%s): original game", core_err, refused(core_err));
	if (!GetPrivateProfileIntW(L"update", L"Enabled", 1, ini)) {
		ulogf("automatic updates off (ronfix.ini [update] Enabled=0)");
		return 0;
	}
	Sleep((DWORD)GetPrivateProfileIntW(L"update", L"DelaySeconds", 20, ini) * 1000);
	if (!wjoin(p, MAX_PATH, gdir, L"ronfix-update.dll", 0))
		return 0;
	HMODULE u = quiet_load(p);
	if (!u) {
		DWORD e = GetLastError();
		ulogf("no update check: ronfix-update.dll %s (error %lu%s)", e == ERROR_FILE_NOT_FOUND || e == ERROR_MOD_NOT_FOUND ? "is missing" : "could not be loaded", e, refused(e));
		return 0;
	}
	update_fn run = (update_fn)GetProcAddress(u, "ronfix_update");
	if (run)
		run(gdir, ulog);
	else
		ulogf("no update check: ronfix-update.dll has no ronfix_update export");
	FreeLibrary(u);
	return 0;
}

/* Loads ronfix-core.dll. Normally on the game's first call into the network DLL, NOT in DllMain: measured
 * 2026-11-27 in the test copy, when Smart App Control refuses the core while the process is still starting
 * (static imports, loader lock), the game hangs with the main thread in an LPC wait and never shows a window.
 * Refused after start-up, LoadLibrary just fails (error 4551) and the game runs on the original. */
/* The refusal does NOT come back in the game: it waits until the process is killed (81 ms in a plain process).
 * So a guard: ronfix\core-loading.txt exists only while the load runs. Found at the next start, it means that
 * start died loading this core: this exact file (size + time) goes into ronfix\core-skip.txt and is not loaded
 * again, the game starts as the original, and a new core from an update is tried afresh. */
/* LoadLibrary without Windows' "Bad Image" message box. When Smart App Control refuses a file, the loader raises a
 * hard error: a modal box (status 0xC0E90002) that the game's thread WAITS on, with no game window yet: that was
 * the "hang" (seen by the user 2026-11-27). With SEM_FAILCRITICALERRORS on this thread the load just fails. */
static HMODULE quiet_load(const WCHAR *p)
{
	DWORD old = 0;
	BOOL set = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old);
	HMODULE m = LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	DWORD e = GetLastError();
	if (set)
		SetThreadErrorMode(old, NULL);
	SetLastError(e);
	return m;
}

static void core_stamp(const WCHAR *p, char *out)
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a)) {
		out[0] = 0;
		return;
	}
	wsprintfA(out, "%lu %08lx%08lx", a.nFileSizeLow, a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime);
}

static int file_text(const WCHAR *p, char *buf, int cap)
{
	DWORD got = 0;
	HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	ReadFile(f, buf, (DWORD)cap - 1, &got, NULL);
	CloseHandle(f);
	buf[got] = 0;
	return 1;
}

static void put_text(const WCHAR *p, const char *s)
{
	DWORD w;
	HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f != INVALID_HANDLE_VALUE) {
		WriteFile(f, s, (DWORD)lstrlenA(s), &w, NULL);
		CloseHandle(f);
	}
}

static void load_core(void)
{
	WCHAR p[MAX_PATH], mark[MAX_PATH], skip[MAX_PATH], dirp[MAX_PATH];
	char stamp[48], prev[64];
	if (off_reason || core_get || !wjoin(p, MAX_PATH, gdir, L"ronfix-core.dll", 0) || GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES)
		return;
	core_present = 1;
	core_stamp(p, stamp);
	wjoin(mark, MAX_PATH, gdir, L"ronfix\\core-loading.txt", 0);
	wjoin(skip, MAX_PATH, gdir, L"ronfix\\core-skip.txt", 0);
	if (GetFileAttributesW(mark) != INVALID_FILE_ATTRIBUTES) {
		put_text(skip, stamp); /* the last start died loading this core */
		DeleteFileW(mark);
	}
	if (stamp[0] && file_text(skip, prev, sizeof prev) && streq(prev, stamp)) {
		core_skipped = 1;
		return;
	}
	wjoin(dirp, MAX_PATH, gdir, L"ronfix", 0);
	CreateDirectoryW(dirp, NULL);
	put_text(mark, stamp);
	HMODULE core = quiet_load(p);
	if (!core)
		core_err = GetLastError();
	else if (!(core_get = (get_obj_fn)GetProcAddress(core, "get_netsys_object_ptr")))
		core_err = ERROR_PROC_NOT_FOUND;
	DeleteFileW(mark);
}

__declspec(dllexport) void *__cdecl get_netsys_object_ptr(void *arg)
{
	static volatile LONG started;
	static get_obj_fn orig_get;
	if (!started && !InterlockedExchange(&started, 1)) {
		load_core();
		HANDLE t = CreateThread(NULL, 0, background, NULL, 0, NULL);
		if (t)
			CloseHandle(t);
	}
	if (core_get)
		return core_get(arg);
	if (!orig_get) {
		WCHAR p[MAX_PATH];
		HMODULE o = GetModuleHandleW(L"CrossplayNetLib_orig.dll");
		if (!o && wjoin(p, MAX_PATH, gdir, L"CrossplayNetLib_orig.dll", 0))
			o = LoadLibraryW(p);
		orig_get = o ? (get_obj_fn)GetProcAddress(o, "get_netsys_object_ptr") : 0;
	}
	return orig_get ? orig_get(arg) : 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	WCHAR p[MAX_PATH];
	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(inst);
	DWORD n = GetModuleFileNameW(inst, gdir, MAX_PATH);
	while (n > 0 && gdir[n - 1] != '\\')
		n--;
	if (n == 0 || n + 40 >= MAX_PATH)
		n = 0;
	gdir[n] = 0;
	if (!n || !wjoin(ini, MAX_PATH, gdir, L"ronfix.ini", 0))
		return TRUE;
	if (GetPrivateProfileIntW(L"startup", L"WarmCrypto", 1, ini)) {
		unsigned char rnd[16];
		warm_status = BCryptGenRandom(NULL, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
		warmed = 1;
	}
	off_reason = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1
	             : has_switch(GetCommandLineW(), L"-original") ? 2
	             : GetPrivateProfileIntW(L"ronfix", L"Off", 0, ini) ? 3 : 0;
	/* With the gameplay pack on, the core's DllMain must patch the exe before the game's own start-up code
	 * reads its data, so it loads here as in the one-DLL versions (and accepts the start-up risk above).
	 * Everyone else gets the core on the first network call (load_core). */
	(void)p;
	if (GetPrivateProfileIntW(L"gamefix", L"Version", 0, ini) > 0)
		load_core();
	return TRUE;
}
