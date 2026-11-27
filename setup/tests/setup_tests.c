/* Builds the setup's install / restore actions (setup.c) into a DLL so tools/test-setup.ps1 can run them on a
 * scratch game folder from PowerShell. The resources (setup.res) are linked into this DLL. */
#define SETUP_TEST
#include "../setup.c"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH)
		resmod = inst;
	return TRUE;
}

/* action 0 install, 1 install without updates, 2 restore, 3 install with logs yes, 4 install with logs no.
 * self = the file to copy as ronfix\RoN-Fix.exe. Returns the action's code (0 ok, 1 refused, 2 access denied);
 * the message goes to out. */
__declspec(dllexport) int __stdcall setup_test_run(int action, const WCHAR *dir, const WCHAR *self, WCHAR *out, int cap)
{
	no_shortcut = 1;
	lstrcpynW(selfpath, self, MAX_PATH);
	msg[0] = 0;
	int r = action == 2 ? do_restore(dir) : do_install(dir, action != 1, action == 3 ? 1 : action == 4 ? 0 : -1);
	lstrcpynW(out, msg, cap);
	return r;
}


/* the ban list as the "Banned players" window reads it: one line per ban, "name<TAB>date<TAB>ids<LF>"; returns the
 * count (-1 = too big) */
__declspec(dllexport) int __stdcall setup_test_bans_list(const WCHAR *dir, WCHAR *out, int cap)
{
	int n = bans_read(dir), len = 0;
	out[0] = 0;
	for (int k = 0; k < nbans && len < cap - 1; k++) {
		WCHAR line[200];
		_snwprintf(line, 200, L"%ls\t%ls\t%hs%hs%hs\n", bans[k].name, bans[k].date, bans[k].id[0], bans[k].nid > 1 ? " " : "",
		           bans[k].nid > 1 ? bans[k].id[1] : "");
		line[199] = 0;
		lstrcpynW(out + len, line, cap - len);
		len += lstrlenW(out + len);
	}
	return n;
}

/* unban the player in row `row` of that list, as the window's Unban button does (all of that row's ids) */
__declspec(dllexport) int __stdcall setup_test_bans_unban(const WCHAR *dir, int row)
{
	static char ids[4][24];
	int n = 0;
	if (bans_read(dir) < 0)
		return bans_unban(dir, ids, 0);
	if (row < 0 || row >= nbans)
		return -1;
	for (int i = 0; i < bans[row].nid; i++)
		lstrcpynA(ids[n++], bans[row].id[i], 24);
	return bans_unban(dir, ids, n);
}

/* used by the window code only, which this test build leaves out */
__attribute__((used)) static void *const window_only[] = { (void *)find_game, (void *)installed_release, (void *)consent_known, (void *)consent_now };
