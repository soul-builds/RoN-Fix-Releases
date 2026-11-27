/* ------------------------------------------------------------------ quit-crash fix, in memory (core v16)
 * Included by netproxy.c. Until 0.14 this fix shipped as a stand-in dssl.dll (sndproxy.c) with the game's own file
 * kept as dssl_orig.dll. Windows' Smart App Control kept refusing that unsigned stand-in on PCs where it is on,
 * sometimes only after first allowing it, and the game then showed a "Bad Image" error at every start. Now the
 * game loads its own dssl.dll, which Windows knows, and the core applies the same fix to it in memory.
 *
 * The crash (reverse/crash-dssl.md, 5 dumps): at shutdown, DSSLSys slot 2 (dssl 0x10004f30) frees the three
 * channel pools; releasing each 0x30-byte slot "unlinks" the sound object that last played on it by writing
 * through slot+0x24. Those sound objects were already freed, so the write hits dead memory and the game crashes on
 * exit. Fix: just before the real slot 2 runs, clear every slot's owner pointer, so the unlink write is skipped.
 * Here: slot 2 of DSSLSys::vftable (dssl .rdata, RVA 0x6414c) points to our wrapper. That table is shared by every
 * DSSLSys object, so objects made before the patch are covered too.
 *
 * Safety: only the dssl.dll build this was written against (PE timestamp + image size), and only if slot 2 holds
 * the expected function. Otherwise nothing is touched and the reason is logged.
 *
 * Moving existing installs back: at start-up, before the game loads dssl.dll, a stand-in dssl.dll next to the
 * game's own dssl_orig.dll is replaced by that original (the stand-in goes to ronfix\previous). */

#define SND_TIMESTAMP 0x667485aeu
#define SND_IMAGESIZE 0x121000u
#define SND_VT_RVA 0x6414c       /* DSSLSys::vftable */
#define SND_SHUTDOWN_RVA 0x4f30  /* its slot 2: the sound system shutdown */
#define SND_SLOT_SIZE 0x30
#define SND_SLOT_OWNER_OFF 0x24
static const DWORD snd_pool_rva[3] = { 0x10ae9c, 0x10aebc, 0x10aedc }; /* channel pools: count +0x04, array +0x10 */

typedef int(THISCALL *snd_shutdown_fn)(void *);
static snd_shutdown_fn snd_orig_shutdown;
static unsigned char *snd_base;
static int snd_state; /* 0 not decided yet, 1 fix active, -1 not applied (reason logged) */

static int THISCALL snd_w_shutdown(void *self)
{
	int cleared = 0, slots = 0;
	for (int p = 0; p < 3; p++) {
		unsigned char *pool = snd_base + snd_pool_rva[p], *arr = 0;
		int count = 0;
		if (!safe_read(pool + 0x04, &count, 4) || !safe_read(pool + 0x10, &arr, 4) || !arr || count <= 0 || count > 4096)
			continue;
		for (int i = 0; i < count; i++) {
			void **owner = (void **)(arr + i * SND_SLOT_SIZE + SND_SLOT_OWNER_OFF);
			slots++;
			if (*owner) {
				*owner = 0;
				cleared++;
			}
		}
	}
	logline("SOUND shutdown: cleared %d stale owner pointers in %d channel slots (quit-crash fix)", cleared, slots);
	return snd_orig_shutdown(self);
}

/* patch the loaded dssl.dll at base: 1 done, -1 not applicable */
static int snd_patch(unsigned char *base)
{
	void **vt, *slot2 = 0;
	DWORD old;
	if (!build_is(base, SND_TIMESTAMP, SND_IMAGESIZE)) {
		logline("SOUND dssl.dll is not the build the quit-crash fix was made for: fix off");
		return -1;
	}
	vt = (void **)(base + SND_VT_RVA);
	if (!safe_read(vt + 2, &slot2, 4) || slot2 != (void *)(base + SND_SHUTDOWN_RVA)) {
		logline("SOUND dssl.dll's sound system table is not as expected (slot 2 = %p): quit-crash fix off", slot2);
		return -1;
	}
	if (!VirtualProtect(vt + 2, 4, PAGE_READWRITE, &old)) {
		logline("SOUND could not unprotect dssl.dll's table (error %lu): quit-crash fix off", GetLastError());
		return -1;
	}
	snd_base = base;
	snd_orig_shutdown = (snd_shutdown_fn)slot2;
	vt[2] = (void *)snd_w_shutdown;
	VirtualProtect(vt + 2, 4, old, &old);
	logline("SOUND quit-crash fix active (the game's own dssl.dll, patched in memory)");
	return 1;
}

/* every check_pulse until decided: the game loads dssl.dll at start-up, long before anyone quits */
static void snd_tick(void)
{
	if (snd_state)
		return;
	HMODULE m = GetModuleHandleA("dssl.dll");
	if (!m)
		return;
	if (GetModuleHandleA("dssl_orig.dll")) { /* an older RoN-Fix stand-in got loaded: it applies the fix itself */
		logline("SOUND the older RoN-Fix sound stand-in is in use (it applies the quit-crash fix itself)");
		snd_state = -1;
		return;
	}
	snd_state = snd_patch((unsigned char *)m);
}

/* PE timestamp + image size of a file on disk */
static int snd_file_build(const char *path, DWORD *stamp, DWORD *size)
{
	unsigned char h[0x400];
	DWORD got = 0;
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	ReadFile(f, h, sizeof h, &got, NULL);
	CloseHandle(f);
	DWORD pe = got >= 0x40 ? *(DWORD *)(h + 0x3c) : 0;
	if (pe < 0x40 || pe + 0x58 > got || h[pe] != 'P' || h[pe + 1] != 'E')
		return 0;
	*stamp = *(DWORD *)(h + pe + 8);
	*size = *(DWORD *)(h + pe + 0x50);
	return 1;
}

/* at start-up, before the game loads dssl.dll: stand-in dssl.dll + original dssl_orig.dll -> the original back */
static void snd_migrate(void)
{
	char cur[MAX_PATH], orig[MAX_PATH], keep[MAX_PATH];
	DWORD st = 0, sz = 0;
	if (GetModuleHandleA("dssl.dll"))
		return; /* loaded already: next start */
	lstrcpyA(cur, dir), lstrcatA(cur, "dssl.dll");
	lstrcpyA(orig, dir), lstrcatA(orig, "dssl_orig.dll");
	lstrcpyA(keep, dir), lstrcatA(keep, "ronfix\\previous\\dssl-standin.dll");
	if (GetFileAttributesA(orig) == INVALID_FILE_ATTRIBUTES)
		return; /* nothing to move back */
	if (!snd_file_build(orig, &st, &sz) || st != SND_TIMESTAMP || sz != SND_IMAGESIZE) {
		logline("SOUND dssl_orig.dll is not the game's dssl.dll build RoN-Fix knows: left as it is");
		return;
	}
	if (snd_file_build(cur, &st, &sz) && st == SND_TIMESTAMP && sz == SND_IMAGESIZE) {
		DeleteFileA(orig); /* the original is in place already; the spare copy is not needed */
		return;
	}
	char prev[MAX_PATH];
	lstrcpyA(prev, dir), lstrcatA(prev, "ronfix");
	CreateDirectoryA(prev, NULL);
	lstrcatA(prev, "\\previous");
	CreateDirectoryA(prev, NULL);
	if (GetFileAttributesA(cur) != INVALID_FILE_ATTRIBUTES && !MoveFileExA(cur, keep, MOVEFILE_REPLACE_EXISTING)) {
		logline("SOUND could not move the old sound stand-in aside (error %lu): it stays for now", GetLastError());
		return;
	}
	if (!MoveFileExA(orig, cur, MOVEFILE_REPLACE_EXISTING)) {
		DWORD e = GetLastError();
		MoveFileExA(keep, cur, MOVEFILE_REPLACE_EXISTING); /* put the stand-in back: the game needs a dssl.dll */
		logline("SOUND could not put the game's own dssl.dll back (error %lu): the old stand-in stays for now", e);
		return;
	}
	logline("SOUND the game's own dssl.dll is back (the old RoN-Fix stand-in is in ronfix\\previous); the quit-crash fix now runs in memory");
}

/* Windows' Smart App Control: 0 off, 1 on, 2 evaluation, -1 not readable (older Windows) */
static int sac_state(void)
{
	typedef LONG(WINAPI * reg_get_fn)(HKEY, const char *, const char *, DWORD, DWORD *, void *, DWORD *);
	HMODULE adv = GetModuleHandleA("advapi32.dll");
	reg_get_fn get = adv ? (reg_get_fn)GetProcAddress(adv, "RegGetValueA") : 0;
	DWORD v = 0, n = sizeof v;
	if (!get || get(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\CI\\Policy", "VerifiedAndReputablePolicyState",
	                0x00000010 /* RRF_RT_REG_DWORD */, NULL, &v, &n) != 0)
		return -1;
	return (int)v;
}

static void sac_log(void)
{
	int s = sac_state();
	logline("Windows Smart App Control: %s", s == 0 ? "off" : s == 1 ? "ON (it can refuse unsigned RoN-Fix files)"
	                                       : s == 2 ? "evaluation mode (it watches, does not refuse yet)"
	                                                : "not reported (older Windows)");
}
