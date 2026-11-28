/* ------------------------------------------------------------------ lobby lines (core v20): the lobby tags on screen
 * Included by netproxy.c after lobbytag.c. lobbytag.c works out who in the lobby has Accord; this puts those lines
 * into the lobby's chat list on THIS PC, with the game's own "add a chat line" method of the setup window, the one
 * the game uses for its local notices (a player joined, the chat rate limit, who the host still waits for):
 *     exe 0x005C8C40  void thiscall(SetupWin *this, const refstr *text, const refstr *aux, peer *sender), ret 0xC
 * called as (text, the game's empty string, NULL). It sends nothing (reverse/lobby-indicator.md: read in the code,
 * and measured live on 2026-11-28 with a Frida prototype: 0 chat packets, 0 network or PlayFab calls inside the call).
 * The text is a game string built by the game's own constructor (0x00A1F0D0) and released by its own release
 * (0x00A1CF40) right after the call; the list keeps its own reference and frees the text with the lobby window.
 * It is only ever called
 *   - on the game's main thread (lobby_tick's, noted in ls_thread): the chat list has no lock;
 *   - while the setup window exists (the method takes it as `this` and does not check it);
 *   - with at least 4 characters (shorter all-digit texts are taunt numbers to the method). The method swaps one
 *     internal token of the game for a notice of its own, but only when the whole text IS that token; every line
 *     from lobbytag.c carries more than a name, so none can be;
 *   - on the exact exe build, and only if all three functions start with the bytes they had when this was written.
 * Otherwise the lines stay in ronfix-net.log, as before. [lobby] TagLines=1 (default) / 0 = log only. */

#define LS_VA_ADD_LINE 0x5c8c40u /* SetupWin: add a chat line */
#define LS_VA_REF_CTOR 0xa1f0d0u /* game string from wchar_t *: refstr *thiscall(refstr *, const wchar_t *, int unused), ret 8 */
#define LS_VA_REF_FREE 0xa1cf40u /* void thiscall(refstr *) */
#define LS_VA_EMPTY 0xeb437cu    /* the game's empty string (data) */
#define LS_REF_SIZE 0x14

typedef void(THISCALL *ls_add_fn)(void *setup, const void *text, const void *aux, void *sender);
typedef void *(THISCALL *ls_ctor_fn)(void *ref, const wchar_t *text, int unused);
typedef void(THISCALL *ls_free_fn)(void *ref);
static ls_add_fn ls_add;
static ls_ctor_fn ls_ctor;
static ls_free_fn ls_free;
static const void *ls_empty;
static int ls_enabled = 1; /* [lobby] TagLines */
static DWORD ls_thread;    /* the thread lobby_tick runs on = the game's main thread */
static unsigned ls_shown;

/* the first 16 bytes of each function; mask bit i clear = byte i is relocated and not compared */
struct ls_site {
	unsigned char want[16];
	unsigned mask;
};
static const struct ls_site ls_sites[3] = {
	{ { 0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0, 0, 0, 0, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00 }, 0xfc3f }, /* add a chat line */
	{ { 0x55, 0x8b, 0xec, 0xa0, 0, 0, 0, 0, 0x53, 0x8b, 0x5d, 0x08, 0x57, 0x8b, 0xf9, 0x88 }, 0xff0f }, /* string from text */
	{ { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x51, 0x56, 0x8b, 0xf1, 0xf6, 0x46, 0x0a, 0x01, 0x74, 0x11 }, 0xffff }, /* release */
};
static int ls_site_ok(const unsigned char *at, const struct ls_site *s)
{
	unsigned char b[16];
	if (!safe_read(at, b, 16))
		return 0;
	for (int i = 0; i < 16; i++)
		if ((s->mask >> i & 1) && b[i] != s->want[i])
			return 0;
	return 1;
}

/* lt_show: one line into the lobby chat of this PC. Anything not as it must be: no call, the log has the line. */
static void ls_show(const char *line)
{
	void *setup = 0;
	wchar_t w[200];
	unsigned char ref[LS_REF_SIZE];
	if (!ls_add || !ls_ctor || !ls_free || !ls_thread || GetCurrentThreadId() != ls_thread)
		return;
	if (!exe_base || !safe_read(exe_base + RVA_SETUPWIN_PTR, &setup, 4) || !setup)
		return; /* no lobby window (menus, Game Browser, loading, in a match) */
	int n = MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 199);
	if (n < 5)
		return; /* n counts the terminator: fewer than 4 characters could be read as a taunt number */
	w[199] = 0;
	ls_ctor(ref, w, 0);              /* the game's pooled copy of the text: one reference, ours */
	ls_add(setup, ref, ls_empty, 0); /* the list entry takes its own reference; nothing is sent */
	ls_free(ref);                    /* drop ours: the game frees the text with the list */
	ls_shown++;
}

/* at start-up, on the exact exe build only: are the three functions the ones this was written against? */
static int ls_check(const unsigned char *add, const unsigned char *ctor, const unsigned char *fr)
{
	return ls_site_ok(add, &ls_sites[0]) && ls_site_ok(ctor, &ls_sites[1]) && ls_site_ok(fr, &ls_sites[2]);
}
static void ls_init(unsigned char *exe)
{
	unsigned char *add = exe + (LS_VA_ADD_LINE - 0x400000u), *ctor = exe + (LS_VA_REF_CTOR - 0x400000u), *fr = exe + (LS_VA_REF_FREE - 0x400000u);
	if (!ls_enabled) {
		logline("LOBBY tag lines: in the log only ([lobby] TagLines=0)");
		return;
	}
	if (!ls_check(add, ctor, fr)) {
		logline("LOBBY tag lines: in the log only: the game's chat line code is not what was expected");
		return;
	}
	ls_add = (ls_add_fn)add, ls_ctor = (ls_ctor_fn)ctor, ls_free = (ls_free_fn)fr;
	ls_empty = exe + (LS_VA_EMPTY - 0x400000u);
	lt_show = ls_show;
	logline("LOBBY tag lines: shown in the lobby's chat on this PC only (nothing is sent to other players)");
}
