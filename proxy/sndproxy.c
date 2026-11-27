/* Stand-in for dssl.dll, the DirectSound sound system (RoN-Fix). Fixes the crash on exit.
 *
 * The crash (reverse/crash-dssl.md, 5 dumps): at shutdown, DSSLSys slot 2 (FUN_10004f30)
 * frees the three channel pools; releasing each 0x30-byte slot "unlinks" the sound object
 * that last played on it by writing through slot+0x24 (`*(owner + 0x40) = 0`). Those sound
 * objects were already freed, so the write hits dead memory and the game crashes on exit,
 * then sits in Windows' crash handling for minutes.
 *
 * Fix: just before the real slot 2 runs, clear every slot's owner pointer, so the unlink
 * write is skipped. Nothing else changes; during play this DLL is a pure pass-through.
 *
 * Safety: the pool addresses are specific to one build of dssl.dll. If dssl_orig.dll is not
 * exactly that build (PE timestamp + image size), the fix switches itself off and the DLL is
 * only a pass-through. Log: ronfix-sound.log next to the DLL.
 */
#include <windows.h>

#define THISCALL __attribute__((thiscall))
#define VT_COPY 32 /* DSSLSys has 14 slots */

/* the dssl.dll this was written against (Steam build of 2024-06-20, still current 2026-11-25) */
#define EXPECT_TIMESTAMP 0x667485aeu
#define EXPECT_IMAGESIZE 0x121000u
static const DWORD pool_rva[3] = {0x10ae9c, 0x10aebc, 0x10aedc}; /* channel pools in .data */
#define POOL_COUNT_OFF 0x04
#define POOL_ARRAY_OFF 0x10
#define SLOT_SIZE 0x30
#define SLOT_OWNER_OFF 0x24
#define SHUTDOWN_SLOT 2

typedef void *(__cdecl *get_obj_fn)(void *, void *);
typedef int(THISCALL *m0)(void *);

static void *orig_vt[VT_COPY];
static void *our_vt[VT_COPY];
static void **orig_vt_addr;
static char logpath[MAX_PATH];
static unsigned char *orig_base;
static int build_ok;

static void logline(const char *fmt, ...)
{
	if (!logpath[0])
		return;
	HANDLE f = CreateFileA(logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
	                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	char buf[400];
	SYSTEMTIME t;
	GetLocalTime(&t);
	int n = wsprintfA(buf, "%04u-%02u-%02u %02u:%02u:%02u.%03u pid %lu: ", t.wYear, t.wMonth, t.wDay,
	                  t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
	va_list ap;
	va_start(ap, fmt);
	n += wvsprintfA(buf + n, fmt, ap);
	va_end(ap);
	buf[n++] = '\r';
	buf[n++] = '\n';
	DWORD w;
	WriteFile(f, buf, (DWORD)n, &w, NULL);
	CloseHandle(f);
}

static int check_build(unsigned char *base)
{
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
	IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
	return nt->FileHeader.TimeDateStamp == EXPECT_TIMESTAMP && nt->OptionalHeader.SizeOfImage == EXPECT_IMAGESIZE;
}

/* slot 2: shutdown. Clear the owner back-pointers, then run the real shutdown. */
static int THISCALL w_shutdown(void *self)
{
	if (build_ok && orig_base) {
		int cleared = 0, slots = 0, p;
		for (p = 0; p < 3; p++) {
			unsigned char *pool = orig_base + pool_rva[p];
			int count = *(int *)(pool + POOL_COUNT_OFF);
			unsigned char *arr = *(unsigned char **)(pool + POOL_ARRAY_OFF);
			if (!arr || count <= 0 || count > 4096)
				continue;
			int i;
			for (i = 0; i < count; i++) {
				void **owner = (void **)(arr + i * SLOT_SIZE + SLOT_OWNER_OFF);
				slots++;
				if (*owner) {
					*owner = 0;
					cleared++;
				}
			}
		}
		logline("sound shutdown: cleared %d stale owner pointers in %d channel slots", cleared, slots);
	}
	return ((m0)orig_vt[SHUTDOWN_SLOT])(self);
}

__declspec(dllexport) void *__cdecl get_soundsys_object_ptr(void *a, void *b)
{
	HMODULE orig = GetModuleHandleA("dssl_orig.dll");
	if (!orig)
		orig = LoadLibraryA("dssl_orig.dll");
	get_obj_fn real = orig ? (get_obj_fn)GetProcAddress(orig, "get_soundsys_object_ptr") : 0;
	if (!real) {
		logline("dssl_orig.dll missing: no sound system");
		return 0;
	}
	orig_base = (unsigned char *)orig;
	build_ok = check_build(orig_base);

	void **obj = (void **)real(a, b);
	if (!obj)
		return obj;
	if (!build_ok) {
		logline("dssl_orig.dll is not the build this fix was made for: fix OFF (pass-through only)");
		return obj;
	}
	void **vt = (void **)obj[0];
	if (!orig_vt_addr) {
		int i;
		for (i = 0; i < VT_COPY; i++)
			orig_vt[i] = our_vt[i] = vt[i];
		our_vt[SHUTDOWN_SLOT] = (void *)w_shutdown;
		orig_vt_addr = vt;
	}
	if (vt == orig_vt_addr) {
		obj[0] = (void *)our_vt;
		logline("RoN-Fix sound proxy v1: exit-crash fix active");
	}
	return obj;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(inst);
	DWORD n = GetModuleFileNameA(inst, logpath, MAX_PATH);
	while (n > 0 && logpath[n - 1] != '\\')
		n--;
	if (n == 0 || n + 18 >= MAX_PATH)
		logpath[0] = 0;
	else
		lstrcpyA(logpath + n, "ronfix-sound.log");
	return TRUE;
}
