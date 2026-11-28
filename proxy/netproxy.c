/* RoN-Fix fixes. Up to v9 this WAS the CrossplayNetLib.dll stand-in; from v10 it is ronfix-core.dll (core.def),
 * loaded by the loader (loader.c, the new stand-in) so the fixes can change and update without touching the one
 * file the game cannot start without. Still builds as the stand-in with netproxy.def.
 *   v1: logging of the network calls that matter for freezes and drops.
 *   v2: fix A, "report a player lost after N seconds without his turn data".
 *   v4: fix B, no crash when the desync vote looks up a player who has left (slot 36);
 *       fix C, no crash when a lobby update finds a setup-window control missing (exe call patch).
 *   v5: fix D, no crash finishing the match recording when the game is already being torn down
 *       (exit() after a GPU reset); fix E, no crash when a path step leaves the map (tile lookup
 *       clamped to the map edge, deterministic; in-map lookups unchanged).
 *   v9: lobby logging (read only): PlayFab lobby events, lobby slots, ready flood, ghosts after a boot,
 *       players' liveness in the lobby, loading sync points. [lobby] Log=1.
 *   v10: same fixes, built as ronfix-core.dll behind the loader.
 *   v11: Steam link phase A (steamlink.c): RoN-Fix players ping each other over Steam's relay network and log it
 *        next to PlayFab; the game's traffic is unchanged. [steam] Shadow=1.
 *   v12: lobby fixes, host only (lobbyfix.c): boot = real kick through PlayFab, ban list, ready spam kick.
 *   v13: shorter command delay, all or nobody (turndelay.c): turn floor 200-300 ms instead of 400-600 when every
 *        player runs it and the host turns it on ([netfix] ShortDelay=1); agreed through PlayFab member data.
 *   v14: Steam link freeze watch (steamlink.c): every WAIT line says what the Steam link to the awaited player did
 *        meanwhile (last heard, pings answered, whether that player is itself waiting and for whom).
 *   v15: ghost room clean-up (ghostroom.c): when the game's Create gives up and goes back to the Game Browser without
 *        leaving the lobby it created, that lobby is left the way the game's own Leave would. [lobby] GhostRoom=1.
 *   v16: the quit-crash fix runs in memory on the game's own dssl.dll (soundfix.c) instead of a stand-in dssl.dll that
 *        Smart App Control kept refusing; old installs get their original dssl.dll back. Logs Smart App Control's state.
 *   v17: Steam mirror (steamlink.c + rfxstream.c), stage 1 of moving the game's traffic to Steam: every message this
 *        PC sends also travels, numbered, over Steam to players who have the mirror on, and is compared on arrival
 *        with what PlayFab delivered. The game still uses PlayFab's only. [steam] Mirror=1, off by default.
 *        Slot 21 (send to one player) is hooked for it, pass-through.
 *   v18: lobby tags (lobbytag.c): every RoN-Fix player posts its release as a PlayFab member key, and the log (and,
 *        once a way to draw it is proven, the lobby) says who has RoN-Fix and who plays without. [lobby] Tags=1.
 *   v19: gameplay switch (gameswitch.c): the gameplay pack loaded per match, at session set-up, when every player
 *        has it ready; the original game otherwise. [gamefix] Switch=1, off by default.
 *   v20: lobby lines (lobbyline.c): the lobby tags are drawn in the lobby's chat list on this PC, with the game's
 *        own method for local notices. [lobby] TagLines=1.
 *
 * How RoN drops a player (reverse/netcode-map.md, "How a player actually gets dropped"): every
 * client must report having lost them; when all have, the game drops them at the same turn on all
 * machines and opens the Multiplayer Drop vote. Each client normally reports only when ITS OWN
 * 20 s pulse timeout fires, so a player whose small pulses still get through but whose turn data
 * does not freezes everyone indefinitely.
 *
 * Fix A: the exe's turn-wait loop calls netsys slot 62 notify_waiting_on_player(player, ms) on
 * every pass after 1 s. Once ms passes WaitReportSeconds, we file the game's own "I lost this
 * player" report (NetDaemon callback slot 8 -> FUN_00959110), the same report a pulse timeout
 * leads to, but WITHOUT the forced local drop that a pulse timeout also does (callback slot 7,
 * which marks the player as lost by everyone on this machine). So:
 *   - everyone stuck on the same player and running this fix -> all report, consensus forms,
 *     the normal drop + vote happen, identically on every machine;
 *   - anyone still receiving that player's data does not report -> nothing is dropped, the game
 *     just lags as it does today (no desync, unlike a unilateral drop);
 *   - players without the fix in the lobby simply never report early: no harm, no help.
 * Residual risk (documented in netcode-map.md): clients stuck on the same player but at
 * different turns. Kept small by a long default (20 s without turn data).
 *
 * Every address and slot below is specific to the Steam builds of 2024-06-20. If the exe or
 * the original DLL is a different build, the hooks are not installed at all (pure forwarding).
 * Settings: ronfix.ini next to the DLL, [netfix] WaitReportSeconds=20 (0 = fix off).
 * Log: ronfix-net.log next to the DLL.
 *
 * No CRT (-nostdlib): kernel32/user32 only, small stack buffers, no memcpy.
 */
#include <windows.h>
#include <bcrypt.h>

#define THISCALL __attribute__((thiscall))
#define VT_COPY 128 /* more than the 65 real slots */

/* builds this was written against */
#define EXE_TIMESTAMP 0x6674863fu
#define EXE_IMAGESIZE 0xbb4000u
#define DLL_TIMESTAMP 0x667485bau
#define DLL_IMAGESIZE 0x133000u

/* exe data (RVA = VA - 0x400000; the exe is ASLR-relocated, so always add the real base) */
#define RVA_PLAYERS_PTR 0x8061ec   /* PTR_DAT_00c061ec -> player table */
#define RVA_GAMEINFO_PTR 0x806210  /* PTR_DAT_00c06210 -> game info, local player index at +0x2a0 */
#define GAMEINFO_LOCAL_OFF 0x2a0
#define RVA_ALT_DROP_FLAG 0x8ab3a0 /* DAT_00cab3a0: nonzero = the game uses another drop path */
#define PLAYER_STRIDE 0x8c
#define PLAYER_PEER_OFF 0xc0

/* CrossplayNetLibSys object */
#define NETSYS_CALLBACK_OFF 0x5c /* exe's NetDaemon callback object */
#define CALLBACK_REPORT_LOST 8   /* NetDaemon slot 8 = FUN_009515b0 -> FUN_00959110(peer), ret 4 */

typedef void *(__cdecl *get_obj_fn)(void *);
typedef int(THISCALL *m0)(void *);
typedef int(THISCALL *m1)(void *, int);
typedef int(THISCALL *m2)(void *, int, int);
typedef int(THISCALL *m3)(void *, int, int, int);

static void *orig_vt[VT_COPY];
static void *our_vt[VT_COPY];
static void **orig_vt_addr;
static int vt_ready;
static HANDLE logf = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION loglock;
static char dir[MAX_PATH]; /* folder of this DLL, with trailing backslash */
static unsigned char *exe_base;
static int exe_ok, report_ms;
static int warmed;
static int maskfix_status; /* fix F: 0 off, 1 applied, -1 code not as expected / unknown build */
static char cbpfix_report[320]; /* CBP exe bug fixes: what was applied (logged once the log is open) */
static char gamefix_data_note[120];
static int gamefix_version; /* [gamefix] Version: 0 = gameplay fixes not installed (sync-compatible with
                              plain RoN / RoN-Fix 0.5); N = gameplay pack N installed (data + fix F + guard) */
static LONG warm_status;
static int lobby_log; /* [lobby] Log: lobby logging (v9), read only */
static int lf_enabled;  /* [lobby] Fixes: host-only lobby fixes (lobbyfix.c, v12) */
static int td_capable, td_host_wants; /* shorter command delay (turndelay.c, v13) */
static unsigned td_min, td_max;
static void td_init(unsigned char *exe);
static int gs_switch, gs_solo, gs_ready; /* gameplay pack per match (gameswitch.c, v19) */
static void gs_init(unsigned char *exe, const char *gamedir);
static void gs_log_start(void);
static void bar_ours(DWORD now);

/* ------------------------------------------------------------------ logging */
#ifdef RONFIX_TEST
static void test_capture(const char *line, int n);
#endif
static void logline(const char *fmt, ...)
{
#ifndef RONFIX_TEST
	if (logf == INVALID_HANDLE_VALUE)
		return;
#endif
	char buf[1100]; /* wvsprintfA writes at most 1024 chars */
	SYSTEMTIME t;
	GetLocalTime(&t);
	int n = wsprintfA(buf, "%02u:%02u:%02u.%03u [%5lu] ", t.wHour, t.wMinute, t.wSecond,
	                  t.wMilliseconds, GetCurrentThreadId());
	va_list ap;
	va_start(ap, fmt);
	n += wvsprintfA(buf + n, fmt, ap);
	va_end(ap);
	buf[n++] = '\r';
	buf[n++] = '\n';
#ifdef RONFIX_TEST
	test_capture(buf, n);
	if (logf == INVALID_HANDLE_VALUE)
		return;
#endif
	EnterCriticalSection(&loglock);
	DWORD w;
	WriteFile(logf, buf, (DWORD)n, &w, NULL);
	LeaveCriticalSection(&loglock);
}

/* Identifiers for the log upload (update.c). Every player name and id this file writes into the log is also
 * noted in ronfix-ids.txt, so the uploader can send that list with the logs and RoN-Fix's server replaces each
 * of them with a code before anything is stored. A local file; nothing leaves the PC unless the player said
 * yes to sending logs in the setup. */
static unsigned id_hash[160];
static int id_count;
static void id_note(const char *s)
{
	unsigned h = 2166136261u;
	int len = 0;
	if (!s)
		return;
	for (const char *p = s; *p; p++, len++)
		h = (h ^ (unsigned char)*p) * 16777619u;
	if (len < 3 || (s[0] == '-' && !s[1]))
		return;
	for (int k = 0; k < id_count; k++)
		if (id_hash[k] == h)
			return;
	if (id_count < 160)
		id_hash[id_count++] = h;
#ifndef RONFIX_TEST
	char path[MAX_PATH], line[84];
	int n = 0;
	lstrcpyA(path, dir);
	lstrcatA(path, "ronfix-ids.txt");
	for (const char *p = s; *p && n < 80; p++)
		if (*p != '\r' && *p != '\n')
			line[n++] = *p;
	line[n++] = '\r';
	line[n++] = '\n';
	HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f != INVALID_HANDLE_VALUE) {
		DWORD w;
		WriteFile(f, line, (DWORD)n, &w, NULL);
		CloseHandle(f);
	}
#endif
}

/* a lobby/member property whose value names or identifies a player: name, platformId, entity/user/steam ids.
 * Not versionId and the like: those stay readable (version mismatches are worth seeing). */
static int has_ci(const char *s, const char *sub)
{
	for (; *s; s++) {
		const char *a = s, *b = sub;
		while (*a && *b && ((*a | 32) == *b))
			a++, b++;
		if (!*b)
			return 1;
	}
	return 0;
}
static int key_identifies(const char *k)
{
	return has_ci(k, "name") || has_ci(k, "platformid") || has_ci(k, "entityid") || has_ci(k, "userid") ||
	       has_ci(k, "steamid") || has_ci(k, "xuid");
}

static void hexbytes(char *out, const unsigned char *p, int len)
{
	static const char h[] = "0123456789abcdef";
	int i, k = 0;
	for (i = 0; i < len && i < 16; i++) {
		out[k++] = h[p[i] >> 4];
		out[k++] = h[p[i] & 15];
		out[k++] = ' ';
	}
	out[k] = 0;
}

static int build_is(const void *base, DWORD stamp, DWORD size)
{
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
	const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const unsigned char *)base + dos->e_lfanew);
	return nt->FileHeader.TimeDateStamp == stamp && nt->OptionalHeader.SizeOfImage == size;
}

/* ------------------------------------------------------------------ logging wrappers */
static int THISCALL w_send_desync(void *self, int frame)
{
	logline("send_desync frame=%d", frame);
	return ((m1)orig_vt[14])(self, frame);
}

static int THISCALL w_set_allow_timeout(void *self, int on)
{
	logline("set_allow_timeout %d", on & 0xff);
	return ((m1)orig_vt[18])(self, on);
}

/* mirror (steamlink.c, included further down): every message this PC sends also goes, numbered, over Steam */
static void sl_mirror_out(const unsigned char *peer, const void *data, int len, int direct);
static void sl_mirror_party(const unsigned char *sender, const unsigned char *buf, unsigned len);
static void sl_data_poll(void);

/* slot 22: broadcast(buf, len, flag), ret 0xC (0x10017910; the DLL ignores flag). With our vtable copy in place
 * the network DLL's own packets (pulse, ready flag, desync ...) come through here too. */
static unsigned long bc_count, bc_bytes, bc_last_report;
static int THISCALL w_broadcast(void *self, int data, int len, int flag)
{
	if (len == 5 && data && *(const unsigned char *)data == 0x0A)
		bar_ours(GetTickCount());
	if (len > 0 && len <= 16 && data) {
		char hx[64];
		hexbytes(hx, (const unsigned char *)data, len);
		logline("broadcast len=%d flag=%d: %s", len, flag, hx);
	} else {
		bc_count++;
		bc_bytes += (unsigned long)len;
		DWORD now = GetTickCount();
		if (now - bc_last_report > 60000) {
			logline("broadcast: %lu larger messages, %lu bytes in the last minute", bc_count, bc_bytes);
			bc_count = bc_bytes = 0;
			bc_last_report = now;
		}
	}
	int r = ((m3)orig_vt[22])(self, data, len, flag);
	sl_mirror_out(NULL, (const void *)(UINT_PTR)data, len, 0);
	return r;
}

/* slot 21: send_to_peer(buf, len, peer, flag), ret 0x10 (0x100177F0): the time sync to the host and its echo, file
 * transfer, and the DLL's goodbye to each player. Passed through unchanged; the mirror gets a copy. */
typedef int(THISCALL *m4)(void *, int, int, int, int);
static int THISCALL w_send_to_peer(void *self, int data, int len, int peer, int flag)
{
	int r = ((m4)orig_vt[21])(self, data, len, peer, flag);
	if (peer)
		sl_mirror_out((const unsigned char *)(UINT_PTR)peer, (const void *)(UINT_PTR)data, len, 1);
	return r;
}

static int THISCALL w_disconnect(void *self, int flag)
{
	logline("disconnect %d", flag & 0xff);
	return ((m1)orig_vt[31])(self, flag);
}

static int THISCALL w_drop_player(void *self, int player)
{
	logline("drop_player obj=%08x", player);
	return ((m1)orig_vt[41])(self, player);
}
static int THISCALL w_cancel_drop(void *self, int player)
{
	logline("cancel_drop obj=%08x", player);
	return ((m1)orig_vt[42])(self, player);
}

/* ------------------------------------------------------------------ fix B: desync-vote crash */
/* Slot 36 = find_player_by_name(std::wstring name BY VALUE) (CrossplayNetLib FUN_1001b000,
 * ret 0x18): searches the netsys player list (count at +4, pointers from +8), returns the player
 * object or NULL. desync_majority_vote (exe FUN_0093e9e0) looks up the majority group's player by
 * name and calls the result's vtable+0x14 (get name) unchecked: NULL when that player has left
 * -> the 2026-10-10 crash (reverse/crash-dssl.md). For THAT call site only, a NULL becomes the
 * first player still in the list: the out-of-sync message may name the wrong player, but the
 * game survives. Other callers get the original result; NULLs are logged with their call site. */
typedef struct { unsigned int w[6]; } wstr24; /* MSVC std::wstring: 16-byte buffer/ptr, size, capacity */
typedef void *(THISCALL *find_by_name_fn)(void *, wstr24);
#define RVA_DESYNC_VOTE_RET 0x53eb99 /* return address of the slot-36 call in FUN_0093e9e0 */

static void *THISCALL w_find_player_by_name(void *self, wstr24 name)
{
	void *ret_addr = __builtin_return_address(0);
	void *p = ((find_by_name_fn)orig_vt[36])(self, name); /* frees the string, as the caller expects */
	if (p)
		return p;
	unsigned rva = (unsigned)((unsigned char *)ret_addr - exe_base);
	if (exe_ok && rva == RVA_DESYNC_VOTE_RET) {
		int n = *(int *)((unsigned char *)self + 4), i;
		void **list = (void **)((unsigned char *)self + 8);
		for (i = 0; i < n && i < 16; i++) {
			if (list[i]) {
				logline("fix B: desync vote looked up a player who is gone: using player object %08x instead of crashing",
				        (unsigned)(UINT_PTR)list[i]);
				return list[i];
			}
		}
		logline("fix B: desync vote looked up a player who is gone and no players are left: cannot help");
		return 0;
	}
	/* Normal for some callers (the lobby sync FUN_005b44b0 looks up every slot, empty ones too, on
	 * each update), so log the first 10 per caller, then every 500th. */
	static unsigned seen_rva[8], seen_n[8];
	unsigned k = 0;
	while (k < 8 && seen_rva[k] && seen_rva[k] != rva)
		k++;
	unsigned c = k < 8 ? (seen_rva[k] = rva, ++seen_n[k]) : 1;
	if (c <= 10 || c % 500 == 0)
		logline("find_player_by_name: not found (caller rva %06x, %u times)%s", rva, c,
		        c == 10 ? "; now logging every 500th" : "");
	return 0;
}

/* ------------------------------------------------------------------ version guard
 * The game passes adler32(build version, display version) to netsys init (slot 1: thiscall, 6 args:
 * callbacks, service, version hash, 0, ?, ?; see reverse/gameplay-fixes.md). CrossplayNetLib copies it
 * into every join request and the host refuses a different one ("Your game version does not match the
 * session's version"). With the gameplay pack installed we fold a RoN-Fix tag into it, so players with a
 * different gameplay version cannot join each other: a clear message at join instead of an out-of-sync
 * mid-game. Nothing else uses this hash (saves, replays and settings use the display version). */
#define RONFIX_GUARD_TAG 0x52460000u /* 'RF' << 16 | gameplay pack version */
typedef int(THISCALL *init_fn)(void *, void *, void *, unsigned, int, int, int);
static int THISCALL w_init(void *self, void *callbacks, void *service, unsigned version_hash, int a4, int a5, int a6)
{
	unsigned h = version_hash;
	if (gamefix_version > 0) {
		h ^= RONFIX_GUARD_TAG | (unsigned)gamefix_version;
		logline("version guard: gameplay pack %d installed, session version %08x -> %08x (only players with the same pack can join each other)",
		        gamefix_version, version_hash, h);
	}
	return ((init_fn)orig_vt[1])(self, callbacks, service, h, a4, a5, a6);
}

/* ------------------------------------------------------------------ exe call-site patches */
/* Redirect one CALL rel32 instruction in the exe to our function. Applied only when the exe is
 * the known build AND the bytes there are exactly "E8 <rel32 to the expected target>", so a
 * different build or someone else's patch is never overwritten. The exe file is not changed. */
static int patch_call(unsigned rva_call, unsigned rva_target, void *fn, void **orig_out)
{
	unsigned char *p = exe_base + rva_call;
	if (p[0] != 0xE8 || p + 5 + *(int *)(p + 1) != exe_base + rva_target)
		return 0;
	DWORD old;
	if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old))
		return 0;
	*orig_out = exe_base + rva_target;
	*(int *)(p + 1) = (int)((unsigned char *)fn - (p + 5));
	VirtualProtect(p, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), p, 5);
	return 1;
}

/* fix C: lobby crash of 2026-10-28 (reverse/crash-dssl.md). In the lobby_type handler
 * (exe FUN_005bc400) the setup window looks a control up by name (vtable+0x424) and, without
 * checking, assigns a string to the control's text at +0x2c4 via FUN_00a1eeb0 (thiscall, 1 arg,
 * ret 4; returns this). When the control is missing the "this" is NULL+0x2c4 -> crash. The call
 * at 0x005bc615 now goes through here: a near-null this means "no control": skip the text. */
#define RVA_LOBBYTYPE_SETTEXT_CALL 0x1bc615
#define RVA_WSTR_ASSIGN 0x61eeb0
typedef void *(THISCALL *assign_fn)(void *, void *);
static void *wstr_assign_orig;
static void *THISCALL c_lobby_set_text(void *field, void *src)
{
	if ((UINT_PTR)field < 0x10000) {
		logline("fix C: lobby window control missing (lobby_type update): text skipped instead of crashing");
		return field;
	}
	return ((assign_fn)wstr_assign_orig)(field, src);
}

/* Replace a whole exe function with ours: write "JMP fn" over its first 5 bytes, only if its
 * first bytes are exactly `expect` (the known build, nobody else's patch). No trampoline: the
 * replacement reimplements the function completely. */
static int patch_jmp(unsigned rva, const unsigned char *expect, unsigned n, void *fn)
{
	unsigned char *p = exe_base + rva;
	for (unsigned i = 0; i < n; i++)
		if (p[i] != expect[i])
			return 0;
	DWORD old;
	if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old))
		return 0;
	p[0] = 0xE9;
	*(int *)(p + 1) = (int)((unsigned char *)fn - (p + 5));
	VirtualProtect(p, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), p, 5);
	return 1;
}

/* fix D: recording crash of 2026-10-12 (reverse/crash-dssl.md). The GPU driver reset
 * (DXGI_ERROR_DEVICE_REMOVED, Windows LiveKernelEvent 141), d3dgl's next CreateBuffer failed and
 * its error handler called exit(). During exit, game_reset_state (FUN_00953990) finishes the
 * match recording (FUN_00952b40, thiscall + 1 unused stack arg, ret 4), which builds a path via
 * FUN_00a491d0 from the global folder table DAT_00e33340 -- already gone (NULL) -> read of 4 ->
 * crash. The call at 0x00953994 now goes through here: with no folder table, skip finishing the
 * recording (it cannot work); otherwise call the original unchanged. */
#define RVA_RESET_FINISH_REC_CALL 0x553994
#define RVA_FINISH_RECORDING 0x552b40
#define RVA_FOLDER_TABLE_PTR 0xa33340
typedef void(THISCALL *finish_rec_fn)(void *, int);
static void *finish_rec_orig;
static void THISCALL c_finish_recording(void *game, int arg)
{
	if (*(void **)(exe_base + RVA_FOLDER_TABLE_PTR) == 0) {
		logline("fix D: game folder table already gone (game being torn down): match recording not finished, instead of crashing");
		return;
	}
	((finish_rec_fn)finish_rec_orig)(game, arg);
}

/* fix E: simulation crash of 2026-10-13 (reverse/crash-dssl.md). A group move order's path
 * walker (FUN_00688fc0) steps from a waypoint toward the target without bounds checks and asked
 * get_tile_data_value (FUN_006b52e0, thiscall map, int *x, int *y, ret 8) about a point 2 cells
 * off the map (x 34, y -2): it indexes the tile arrays with no check -> read before the array ->
 * crash (or, on luckier machines, garbage). Its other callers' sibling evaluator already treats
 * off-map points as blocked. Replacement: identical arithmetic for every in-map point (so patched
 * and unpatched players compute the same in normal play); an off-map point is clamped to the
 * nearest edge cell, so every patched player gets the same answer. Integer only, deterministic.
 * Map fields (from the game's own bounds check FUN_0046f760): +0x18 width, +0x1c height of this
 * grid; +0x00 width of the 4x coarser grid; +0x134 coarse records (0x1c bytes); +0x138 cells. */
#define RVA_TILE_DATA_VALUE 0x2b52e0
static const unsigned char tile_value_head[8] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x53, 0x56};
static int tile_clamp_logged;
/* [exefix] SelfTest=1 (test copies only): every in-map lookup is also run through the untouched
 * original (its first 6 bytes = 3 whole instructions copied to a trampoline, then JMP back into
 * it) and any difference is logged, to prove the replacement is exact on real maps. */
typedef int(THISCALL *tile_fn)(int *, int *, int *);
static tile_fn tile_orig;
static unsigned tile_checked, tile_mismatch;
static int THISCALL c_tile_data_value_impl(int *map, int *px, int *py);
static int THISCALL c_tile_data_value_selftest(int *map, int *px, int *py)
{
	int x = *px, y = *py;
	static int offmap_done;
	if (!offmap_done && map[6] > 0 && map[7] > 0) {
		/* Off-map points, the 2026-10-13 crash's (34,-2) among them: ours must not crash and must
		 * equal the original's answer for the clamped (edge) cell. */
		offmap_done = 1;
		int w = map[6], h = map[7];
		int pts[6][4] = {{34, -2, 34, 0}, {-3, 5, 0, 5}, {w + 1, 7, w - 1, 7},
		                 {9, h + 4, 9, h - 1}, {-6, -6, 0, 0}, {w + 9, h + 9, w - 1, h - 1}};
		for (int i = 0; i < 6; i++) {
			int ox = pts[i][0], oy = pts[i][1], cx = pts[i][2], cy = pts[i][3];
			int mine_off = c_tile_data_value_impl(map, &ox, &oy), theirs_edge = tile_orig(map, &cx, &cy);
			logline("fix E selftest off-map (%d,%d) -> ours %d, original at edge (%d,%d) %d: %s", pts[i][0],
			        pts[i][1], mine_off, cx, cy, theirs_edge, mine_off == theirs_edge ? "same" : "DIFFERENT");
		}
	}
	int mine = c_tile_data_value_impl(map, px, py);
	if (x >= 0 && y >= 0 && x < map[6] && y < map[7]) {
		int theirs = tile_orig(map, px, py);
		tile_checked++;
		if (theirs != mine && tile_mismatch++ < 20)
			logline("fix E SELFTEST MISMATCH at (%d,%d): original %d, ours %d", x, y, theirs, mine);
		if ((tile_checked & (tile_checked - 1)) == 0 && tile_checked >= 1024)
			logline("fix E selftest: %u in-map lookups compared, %u mismatches", tile_checked, tile_mismatch);
	}
	return mine;
}
static int THISCALL c_tile_data_value_impl(int *map, int *px, int *py)
{
	int x = *px, y = *py, w = map[6], h = map[7];
	if ((x < 0 || y < 0 || x >= w || y >= h) && w > 0 && h > 0) {
		if (tile_clamp_logged < 20)
			logline("fix E: path point off the map (%d,%d; map %dx%d): clamped to the edge instead of reading outside the map%s",
			        x, y, w, h, ++tile_clamp_logged == 20 ? " (further ones not logged)" : "");
		x = x < 0 ? 0 : x >= w ? w - 1 : x;
		y = y < 0 ? 0 : y >= h ? h - 1 : y;
	}
	unsigned short *t = (unsigned short *)(map[0x4d] + ((y >> 2) * map[0] + (x >> 2)) * 0x1c);
	if ((*t & 0x100) != 0 && (*(unsigned char *)(map[0x4e] + (map[6] * y + x) * 2) & 0x30) == 0x20)
		return (short)t[3];
	return (short)t[2];
}

static void apply_exe_patches(int selftest)
{
	static int done; /* once per process: a second pass would copy already-patched bytes */
	if (!exe_ok || done)
		return;
	done = 1;
	void *tile_fn_new = (void *)c_tile_data_value_impl;
	if (selftest) {
		unsigned char *src = exe_base + RVA_TILE_DATA_VALUE;
		int head_ok = 1;
		for (unsigned i = 0; i < sizeof tile_value_head; i++)
			head_ok &= src[i] == tile_value_head[i];
		unsigned char *tr = head_ok ? VirtualAlloc(0, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE) : 0;
		if (tr) {
			for (int i = 0; i < 6; i++)
				tr[i] = src[i];
			tr[6] = 0xE9;
			*(int *)(tr + 7) = (int)((src + 6) - (tr + 11));
			tile_orig = (tile_fn)tr;
			tile_fn_new = (void *)c_tile_data_value_selftest;
			logline("fix E selftest ON (test copies only): replacement compared with the original on every in-map lookup");
		}
	}
	if (patch_call(RVA_LOBBYTYPE_SETTEXT_CALL, RVA_WSTR_ASSIGN, (void *)c_lobby_set_text, &wstr_assign_orig))
		logline("fix C (lobby crash) applied");
	else
		logline("fix C (lobby crash) NOT applied: code at the call site is not what was expected");
	if (patch_call(RVA_RESET_FINISH_REC_CALL, RVA_FINISH_RECORDING, (void *)c_finish_recording, &finish_rec_orig))
		logline("fix D (recording crash during teardown) applied");
	else
		logline("fix D (recording crash during teardown) NOT applied: code at the call site is not what was expected");
	if (patch_jmp(RVA_TILE_DATA_VALUE, tile_value_head, sizeof tile_value_head, tile_fn_new))
		logline("fix E (off-map path point crash) applied");
	else
		logline("fix E (off-map path point crash) NOT applied: code is not what was expected");
}

/* ------------------------------------------------------------------ fix A */
static int wait_player = -1;
static DWORD wait_last_log, wait_last_call;
static unsigned char reported[8];

static void report_lost(void *self, int player, int ms)
{
	if (!exe_ok || report_ms <= 0 || player < 0 || player > 7 || reported[player])
		return;
	reported[player] = 1; /* one attempt per wait episode, whatever happens below */

	unsigned char *players = *(unsigned char **)(exe_base + RVA_PLAYERS_PTR);
	unsigned char *info = *(unsigned char **)(exe_base + RVA_GAMEINFO_PTR);
	int alt = *(int *)(exe_base + RVA_ALT_DROP_FLAG);
	if (!players || !info) {
		logline("fix A: game tables not set up, not reporting");
		return;
	}
	int local = *(int *)(info + GAMEINFO_LOCAL_OFF);
	void *peer = *(void **)(players + player * PLAYER_STRIDE + PLAYER_PEER_OFF);
	void **cb = *(void ***)((unsigned char *)self + NETSYS_CALLBACK_OFF);
	if (player == local || !peer || !cb || alt != 0) {
		logline("fix A: not reporting player %d (local=%d peer=%08x callback=%08x alt_drop_flag=%d)",
		        player, local, (unsigned)(UINT_PTR)peer, (unsigned)(UINT_PTR)cb, alt);
		return;
	}
	logline("fix A: no turn data from player %d for %d ms (limit %d): reporting them lost", player, ms, report_ms);
	((m1)((void **)cb[0])[CALLBACK_REPORT_LOST])(cb, (int)(UINT_PTR)peer);
	logline("fix A: report filed for player %d", player);
}

/* freeze watch (steamlink.c, included further down): what the Steam link to the awaited player does meanwhile */
static void sl_wait_pass(int player, unsigned ms);
static void sl_wait_text(char *out, int final);
static void sl_wait_over(void);
static void sl_player_label(int player, char *out);

/* slot 62: notify_waiting_on_player(player, ms_waited), every pass of the wait loop after 1 s */
static int THISCALL w_waiting(void *self, int player, int ms)
{
	DWORD now = GetTickCount();
	char st[512], who[80];
	sl_wait_pass(player, (unsigned)ms);
	if (player != wait_player || now - wait_last_call > 2000) {
		sl_player_label(player, who);
		sl_wait_text(st, 0);
		logline("WAIT START player=%d waited=%d ms: %s%s", player, ms, who, st);
		wait_player = player;
		wait_last_log = now;
	} else if (now - wait_last_log >= 1000) {
		sl_wait_text(st, 0);
		logline("  still waiting player=%d waited=%d ms%s", player, ms, st);
		wait_last_log = now;
	}
	wait_last_call = now;
	int r = ((m2)orig_vt[62])(self, player, ms);
	if (report_ms > 0 && ms >= report_ms)
		report_lost(self, player, ms);
	return r;
}

/* slot 15 check_pulse, every tick: notices that a wait has ended */
static void telemetry_tick(void *self);
static void snd_tick(void); /* quit-crash fix in memory (soundfix.c) */
static void lobby_tick(void *self);
static void steam_tick(void *self);
static int THISCALL w_check_pulse(void *self)
{
	snd_tick();
	telemetry_tick(self);
	if (lobby_log || lf_enabled || td_capable || gs_ready) /* the lobby fixes and the negotiations reuse these scans */
		lobby_tick(self);
	if (exe_ok) {
		steam_tick(self);
		sl_data_poll(); /* mirror: Steam's frames of this pass, before the game takes PlayFab's (w_pop) */
	}
	if (wait_player >= 0 && GetTickCount() - wait_last_call > 1500) {
		char st[512];
		sl_wait_text(st, 1);
		logline("WAIT END player=%d (no wait call for 1.5 s)%s", wait_player, st);
		sl_wait_over();
		if (wait_player >= 0 && wait_player < 8)
			reported[wait_player] = 0;
		wait_player = -1;
	}
	return ((m0)orig_vt[15])(self);
}

/* ------------------------------------------------------------------ turn telemetry (read only)
 * Logs, every [telemetry] Interval seconds (default 10, 0 = off), what the game's own turn scheduler
 * (TurnControl, global at VA 0x00E863D0; field names from its state dump 0x956CC0) used: command delay
 * (turn_length frames x target_frame_time ms), and per player the ping, wait, lag, average frame time and
 * diff it reported, as the maximum over the interval. Reads memory only: no effect on the game, so every
 * player can run it with anyone. Also logs the network DLL's player list (count at netsys+4, pointers from
 * netsys+8, CrossplayNetLibPlayer: flags +0x20 (4 = local), last pulse +0xA0, three std::wstring at +0x2C,
 * +0x44, +0x5C: PlayFab entity id, platform, platform id) once per game, to map numbers to players.
 * frame_counter (0xE863E8) counts game frames; turn_counter (0xE86410) is the frame index INSIDE the current
 * turn (0..turn_length, measured 2026-11-26), so a turn ends when it wraps. (v8 took turn_counter for a turn
 * number and logged "turn counter restarted" every turn.) */
#define TC(x) (*(volatile unsigned *)(exe_base + ((x) - 0x400000u)))
#define TC_ARR(base, p) (*(volatile unsigned *)(exe_base + ((base) - 0x400000u) + 4 * (p)))
enum { TC_FRAME_COUNTER = 0xE863E8, TC_FRAME_MS = 0xE86408, TC_TURN_LENGTH = 0xE8640C, TC_TURN_COUNTER = 0xE86410, TC_SPEED = 0xE86400,
       TC_PKG = 0xE86424, TC_WAIT = 0xE86444, TC_LAG = 0xE86464, TC_AVGF = 0xE86484, TC_PING = 0xE864A4,
       TC_DIFF = 0xE864C4, TC_FLAGS = 0xE86538, TC_FLOOR = 0xE86540 /* min turn ms, set at match start */ };
static int tel_interval_ms;
static unsigned tel_frame_last, tel_tc_last, tel_first_frame, tel_have, tel_turns, tel_ms_min, tel_ms_max, tel_ms_sum, tel_next;
static unsigned tel_ping[8], tel_wait[8], tel_lag[8], tel_frame[8], tel_diff[8], tel_pkg[8];
static int tel_peer_count = -1;

/* Copies n bytes only if every page is committed and readable. VirtualQuery, not ReadProcessMemory: a DLL
 * that patches code AND reads its own process through ReadProcessMemory was quarantined by McAfee as soon as
 * the game loaded it (2026-11-26 15:47). Called on the game's main thread, where these objects live. */
static int readable(const void *p, int n)
{
	const unsigned char *a = p, *end = a + n;
	while (a < end) {
		MEMORY_BASIC_INFORMATION mi;
		if (!VirtualQuery(a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
		if (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
		if (!(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
		                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
		a = (const unsigned char *)mi.BaseAddress + mi.RegionSize;
	}
	return 1;
}
static int safe_read(const void *p, void *out, int n)
{
	if (!p || n <= 0 || !readable(p, n)) return 0;
	volatile const unsigned char *s = p;
	unsigned char *d = out;
	for (int i = 0; i < n; i++) d[i] = s[i];   /* volatile source: the compiler keeps the loop (no memcpy) */
	return 1;
}
/* MSVC std::wstring at p: 16-byte buffer/pointer, size, capacity */
static void read_wstring(const unsigned char *p, char *out, int outlen)
{
	unsigned char s[24];
	wchar_t w[32];
	out[0] = 0;
	if (!safe_read(p, s, 24)) return;
	unsigned size = *(unsigned *)(s + 16), cap = *(unsigned *)(s + 20);
	if (size > 31) size = 31;
	if (cap <= 7 && size > 7) size = 7;
	/* short strings live in the object itself (no copy loop: there is no memcpy without the CRT) */
	if (size && !safe_read(cap > 7 ? *(void **)s : (const void *)p, w, (int)size * 2)) return;
	w[size] = 0;
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outlen, NULL, NULL);
	if (n <= 0) out[0] = 0;
	for (char *c = out; *c; c++) if (*c == '|' || (unsigned char)*c < 32) *c = '?';
}
static void log_peers(void *self)
{
	int count;
	if (!safe_read((char *)self + 4, &count, 4) || count < 0 || count > 16) return;
	for (int i = 0; i < count && i < 8; i++) {
		unsigned char *peer; unsigned flags;
		char a[96], b[96], c[96];
		if (!safe_read((char *)self + 8 + 4 * i, &peer, 4) || !peer) { logline("NET player %d: (none)", i); continue; }
		if (!safe_read(peer + 0x20, &flags, 4)) continue;
		read_wstring(peer + 0x2C, a, sizeof a);
		read_wstring(peer + 0x44, b, sizeof b);
		read_wstring(peer + 0x5C, c, sizeof c);
		logline("NET player %d:%s %s | %s | %s", i, (flags & 4) ? " (this PC)" : "", a, b, c);
	}
}
/* Probe ([probe] SteamNet=1, test copies only): can code in this process get Steam's modern networking
 * interfaces (Steam Datagram Relay) although the game ships an old steam_api.dll (SteamClient017, no
 * ISteamNetworkingSockets exports)? Asks the installed Steam client through GetISteamGenericInterface. */
static int probe_steamnet;
static void *probe_utils;
static DWORD probe_until;
static void steamnet_probe(void)
{
	HMODULE sa = GetModuleHandleA("steam_api.dll");
	if (!sa) { logline("PROBE steam_api.dll not loaded"); return; }
	typedef void *(__cdecl *get_client_fn)(void);
	typedef int (__cdecl *get_handle_fn)(void);
	typedef void *(__cdecl *generic_fn)(void *client, int user, int pipe, const char *version);
	get_client_fn steam_client = (get_client_fn)GetProcAddress(sa, "SteamClient");
	get_handle_fn get_user = (get_handle_fn)GetProcAddress(sa, "SteamAPI_GetHSteamUser");
	get_handle_fn get_pipe = (get_handle_fn)GetProcAddress(sa, "SteamAPI_GetHSteamPipe");
	generic_fn generic = (generic_fn)GetProcAddress(sa, "SteamAPI_ISteamClient_GetISteamGenericInterface");
	if (!steam_client || !get_user || !get_pipe || !generic) {
		logline("PROBE missing exports: SteamClient %p user %p pipe %p generic %p", steam_client, get_user, get_pipe, generic);
		return;
	}
	void *client = steam_client();
	int user = get_user(), pipe = get_pipe();
	logline("PROBE SteamClient %p user %d pipe %d", client, user, pipe);
	if (!client || !user || !pipe) return;
	static const char *const want[] = {
		"SteamNetworkingSockets012", "SteamNetworkingSockets011", "SteamNetworkingSockets009", "SteamNetworkingSockets008",
		"SteamNetworkingMessages002", "SteamNetworkingUtils004", "SteamNetworkingUtils003", "SteamMatchMaking009", "SteamUser023" };
	for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++)
		logline("PROBE %s -> %p", want[i], generic(client, user, pipe, want[i]));
	/* ISteamNetworkingUtils004 vtable (GameNetworkingSockets include/steam/isteamnetworkingutils.h, verified in
	 * the game with Frida 2026-11-26): 0 AllocateMessage, 1 GetRelayNetworkStatus, 2 GetLocalPingLocation,
	 * 3 EstimatePingTimeBetweenTwoLocations, 4 EstimatePingTimeFromLocalHost, 5 ConvertPingLocationToString,
	 * 6 ParsePingLocationString, 7 CheckPingDataUpToDate(float). InitRelayNetworkAccess() is NOT virtual: an
	 * inline for CheckPingDataUpToDate(1e10f). (Calling slot 1 as "init" crashed: it takes an argument.) */
	probe_utils = generic(client, user, pipe, "SteamNetworkingUtils004");
	if (probe_utils) {
		typedef int (__attribute__((thiscall)) *check_fn)(void *, float);
		((check_fn)(*(void ***)probe_utils)[7])(probe_utils, 1e10f);
		logline("PROBE relay network access requested (CheckPingDataUpToDate)");
		probe_until = GetTickCount() + 40000;
	}
}
/* polled from telemetry_tick until relay status is final or 40 s passed */
static void steamnet_poll(void)
{
	static int last = -999; static DWORD next;
	DWORD now = GetTickCount();
	if (!probe_utils || !probe_until || (int)(now - next) < 0) return;
	next = now + 500;
	typedef int (__attribute__((thiscall)) *status_fn)(void *, void *);
	int st = ((status_fn)(*(void ***)probe_utils)[1])(probe_utils, NULL);
	if (st != last) { logline("PROBE relay network status %d (100 = current, 3 = attempting, <0 = failed)", st); last = st; }
	if (st == 100 || st < 0 || (int)(now - probe_until) >= 0) {
		static unsigned char loc[512]; char buf[1024];
		typedef float (__attribute__((thiscall)) *loc_fn)(void *, void *);
		typedef void (__attribute__((thiscall)) *str_fn)(void *, const void *, char *, int);
		float age = ((loc_fn)(*(void ***)probe_utils)[2])(probe_utils, loc);
		if (age >= 0) {
			((str_fn)(*(void ***)probe_utils)[5])(probe_utils, loc, buf, sizeof buf);
			buf[300] = 0;
			logline("PROBE ping location ready (age %d s): %s", (int)age, buf);
		} else logline("PROBE ping location not available yet");
		probe_until = 0;
	}
}

static void telemetry_tick(void *self)
{
	static int probed;
	if (probe_steamnet && !probed) { probed = 1; steamnet_probe(); }
	if (probe_steamnet) steamnet_poll();
	if (!exe_ok || tel_interval_ms <= 0) return;
	int count;
	if (!lobby_log && safe_read((char *)self + 4, &count, 4) && count != tel_peer_count) {
		logline("NET %d players in the network DLL's list", count);
		tel_peer_count = count;
		log_peers(self);
	}
	unsigned frame = TC(TC_FRAME_COUNTER), tc = TC(TC_TURN_COUNTER), len = TC(TC_TURN_LENGTH), fms = TC(TC_FRAME_MS);
	if (frame != tel_frame_last && len >= 1 && len <= 1000 && fms >= 1 && fms <= 1000) {
		if (frame < tel_frame_last) {
			logline("NET frame counter restarted (new game)");
			log_peers(self);
			tel_have = 0;
		}
		if (!tel_have) {
			tel_have = 1;
			tel_first_frame = frame;
			tel_turns = 0;
			tel_ms_min = ~0u; tel_ms_max = tel_ms_sum = 0;
			for (int p = 0; p < 8; p++) tel_ping[p] = tel_wait[p] = tel_lag[p] = tel_frame[p] = tel_diff[p] = tel_pkg[p] = 0;
			if (!tel_next) tel_next = GetTickCount() + (DWORD)tel_interval_ms;
		}
		if (tc < tel_tc_last) { /* turn_counter wrapped: a turn ended */
			unsigned ms = len * fms;
			if (ms < tel_ms_min) tel_ms_min = ms;
			if (ms > tel_ms_max) tel_ms_max = ms;
			tel_ms_sum += ms; tel_turns++;
		}
		for (int p = 0; p < 8; p++) {
			unsigned v;
			if ((v = TC_ARR(TC_PING, p)) > tel_ping[p]) tel_ping[p] = v;
			if ((v = TC_ARR(TC_WAIT, p)) > tel_wait[p]) tel_wait[p] = v;
			if ((v = TC_ARR(TC_LAG, p)) > tel_lag[p]) tel_lag[p] = v;
			if ((v = TC_ARR(TC_AVGF, p)) > tel_frame[p]) tel_frame[p] = v;
			if ((v = TC_ARR(TC_DIFF, p)) > tel_diff[p]) tel_diff[p] = v;
			if ((v = TC_ARR(TC_PKG, p)) > tel_pkg[p]) tel_pkg[p] = v;
		}
		tel_tc_last = tc;
		tel_frame_last = frame;
	}
	DWORD now = GetTickCount();
	if (tel_have && tel_turns && (int)(now - tel_next) >= 0) {
		char line[400]; int n = 0;
		for (int p = 0; p < 8; p++) {
			if (!tel_ping[p] && !tel_frame[p] && !tel_wait[p] && !tel_lag[p]) continue;
			n += wsprintfA(line + n, " p%d %u/%u/%u/%u/%u/%u", p, tel_ping[p], tel_wait[p], tel_lag[p], tel_frame[p], tel_diff[p], tel_pkg[p]);
			if (n > 340) break;
		}
		line[n] = 0;
		unsigned nflags = 0;
		safe_read((char *)self + 0x58, &nflags, 4); /* 0x40 = pulse timeouts armed (reverse/lobby-start-network.md 2.6) */
		logline("NET frames %u-%u (%u turns): delay %u-%u ms (avg %u, floor %u), frame %u ms, speed %u, flags %x, netsys %x | ping/wait/lag/frame/diff/pkg:%s",
		        tel_first_frame, tel_frame_last, tel_turns, tel_ms_min, tel_ms_max, tel_ms_sum / tel_turns, TC(TC_FLOOR), fms, TC(TC_SPEED), TC(TC_FLAGS), nflags,
		        line);
		tel_have = 0;
		tel_next = now + (DWORD)tel_interval_ms;
	}
}

/* ------------------------------------------------------------------ lobby logging (v9, read only)
 * Groundwork for the lobby fixes (reverse/lobby-boot-ban.md, reverse/lobby-start-network.md): log what the
 * lobby really does, so kick / ban / flood handling is built on observed facts. Nothing here changes the
 * game: memory is only read (VirtualQuery-checked) and calls are passed through unchanged.
 * ronfix.ini [lobby] Log=1 (default) / 0 = all of this off.
 *  - PlayFab lobby layer: CrossplayProxy.dll's imports of PlayFabMultiplayerWin.dll are pointed (IAT) at
 *    pass-through wrappers that log each lobby state change (member added/removed and why, which member
 *    updated which keys, results of our own updates), the lobby handles, and the local PFEntityKey (its
 *    type string is what a kick call will need). Struct layouts: Microsoft's PFLobby.h reference, checked
 *    against CrossplayProxy's own use (stdcall, offsets of asyncContext in state changes 0/1/3).
 *  - exe lobby slots (ConnectionData, 8 x 0x3b bytes at *(exe+0x80622c), std::wstring +0x00 and +0x18,
 *    int +0x30, bytes +0x34..+0x3a, +0x3a = ready): changes, ready-flag flood, cleared slots whose player
 *    stays connected (the "ghost after a boot").
 *  - network player list (netsys+4 count, +8 pointers): joins/leaves with name and entity id; in the lobby
 *    a status line every 30 s with each player's pulse age (liveness), ready flag, data channel.
 *  - loading: when each player reaches each sync point (0x0A packets popped through netsys slot 23). */
#define RVA_SLOT_TABLE_PTR 0x80622c /* PTR_DAT_00c0622c -> ConnectionData slot table */
#define RVA_SETUPWIN_PTR 0x8ab3a0   /* DAT_00cab3a0: setup window (lobby) object; same global as RVA_ALT_DROP_FLAG */
#define SLOT_SIZE 0x3b
#define SLOT_READY_OFF 0x3a
#define NETSYS_FLAGS 0x58 /* bit 0 host, 0x40 pulse timeouts armed, 0x80 joining */
#define NETSYS_LOCAL 0x28
#define PEER_FLAGS 0x20   /* 1 host, 4 this PC */
#define PEER_LINK 0x24    /* transport handle, NULL = no data channel yet */
#define PEER_READY 0x28   /* byte, IPT_READYFLAG (data connection formed), not the check mark */
#define PEER_ID 0x2c      /* std::wstring PlayFab entity id */
#define PEER_NAME 0x74    /* the game's own string type (see read_refstr), display name */
#define PEER_PULSE 0xa0   /* low 32 bits of GetTickCount64 when his last pulse was processed */
#define FLOOD_WINDOW_MS 5000
#define FLOOD_COUNT 8     /* ready changes / member updates within the window that count as a flood */
#define GHOST_MS 10000
#define CP_TIMESTAMP 0x667484ceu
#define CP_IMAGESIZE 0xcf000u

static int bytes_eq(const void *a, const void *b, int n)
{
	const unsigned char *x = a, *y = b;
	for (int i = 0; i < n; i++) if (x[i] != y[i]) return 0;
	return 1;
}
/* readable for a forward scan: checks a page only when the scan enters it */
static int scan_ok(const void *p, int n, UINT_PTR *ok_end)
{
	UINT_PTR a = (UINT_PTR)p;
	if (a + n <= *ok_end) return 1;
	if (!readable(p, n)) return 0;
	*ok_end = ((a + n - 1) | 0xfff) + 1;
	return 1;
}
static void read_cstr(const char *p, char *out, int outlen)
{
	UINT_PTR ok = 0;
	int i = 0;
	if (p)
		for (; i < outlen - 1 && scan_ok(p + i, 1, &ok); i++) {
			char c = ((volatile const char *)p)[i];
			if (!c) break;
			out[i] = ((unsigned char)c < 32 || c == '|') ? '?' : c;
		}
	out[i] = 0;
}
static void read_wide(const unsigned short *p, int maxchars, char *out, int outlen)
{
	wchar_t w[48];
	UINT_PTR ok = 0;
	int i = 0;
	out[0] = 0;
	if (maxchars > 47) maxchars = 47;
	if (p)
		for (; i < maxchars && scan_ok(p + i, 2, &ok); i++)
			if (!(w[i] = ((volatile const unsigned short *)p)[i])) break;
	w[i] = 0;
	if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outlen, NULL, NULL) <= 0) out[0] = 0;
	for (char *c = out; *c; c++) if (*c == '|' || (unsigned char)*c < 32) *c = '?';
}
/* The game's string type (CrossplayNetLib OnPlayerJoined fills peer+0x74 with it and logs it as
 * flags(+0xa) & 1 ? ptr + off*2 : *ptr + off*2): +0 ptr, +6 u16 offset (chars), +8 u16 length, +0xa flags. */
static void read_refstr(const unsigned char *p, char *out, int outlen)
{
	unsigned char s[12];
	out[0] = 0;
	if (!safe_read(p, s, 12)) return;
	const unsigned char *base = *(const unsigned char *const *)s;
	unsigned off = *(const unsigned short *)(s + 6), len = *(const unsigned short *)(s + 8);
	if (base && !(s[10] & 1) && !safe_read(base, &base, 4)) return;
	if (base) read_wide((const unsigned short *)(base + off * 2), len && len < 47 ? (int)len : 47, out, outlen);
}

/* ---- network player list */
struct peer_seen { unsigned char *ptr; DWORD since; char name[48], id[40]; };
static struct peer_seen seen[8];
struct ghost_watch { unsigned char *peer; DWORD since; int warned, kick_queued, ban; char a[48], b[48], name[48]; };
static struct ghost_watch ghosts[8]; /* by slot */
/* lobby fixes (lobbyfix.c, included further down) */
static void lf_on_spam(const char *entity, const char *name, int flips);
static void lf_on_lobby_created(void *lobby);
static void lf_on_lobby_data_post(void *lobby);
static void lf_on_lobby_gone(void *lobby);
static void lf_on_member_added(void *lobby, const char *id);
/* ghost room clean-up (ghostroom.c, included after lobbyfix.c) */
static void gr_on_created(void *lobby, DWORD now);
static void gr_on_joined(void *lobby, DWORD now);
static void gr_on_gone(void *lobby);
static void gr_tick(void *self, DWORD now, void *setup, unsigned flags);
/* shorter command delay (turndelay.c, included at the end) */
static void td_rewrite(void *lobby, const void **mu);
static void td_on_lobby_gone(void *lobby);
static void td_tick(int setup_open, int setup_closed_now, int setup_opened_now);
/* lobby tags (lobbytag.c, included at the end): who in the lobby has RoN-Fix */
static int lt_enabled;
static void lt_set_release(unsigned release, const char *name);
static void lt_rewrite(const void *user, const void **mu);
static void lt_tick(int setup_open, int setup_opened_now, DWORD now);
/* gameplay switch (gameswitch.c, included at the end): the gameplay pack per match, agreed through the same key */
static void gs_tick(int setup_open, int setup_closed_now, int setup_opened_now, DWORD now);
/* lobby lines (lobbyline.c): the lobby tags in the lobby's own chat list, on this PC only */
static int ls_enabled;
static DWORD ls_thread;
static void ls_init(unsigned char *exe);
#ifdef RONFIX_TEST
static int lf_test_shift; /* tests: "Shift held" at boot time */
#define LF_SHIFT_HELD() lf_test_shift
#else
#define LF_SHIFT_HELD() ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
#endif

static void peer_scan(void *self, DWORD now)
{
	unsigned char *cur[8];
	int count = 0, i, k;
	for (i = 0; i < 8; i++) cur[i] = 0;
	if (!safe_read((char *)self + 4, &count, 4) || count < 0 || count > 8) count = 0;
	if (count && !safe_read((char *)self + 8, cur, 4 * count)) count = 0;
	for (k = 0; k < 8; k++) {
		if (!seen[k].ptr) continue;
		for (i = 0; i < count && cur[i] != seen[k].ptr; i++) ;
		if (i < count) continue;
		logline("NET - player left the network list: %s (id %s), %lu s after joining", seen[k].name, seen[k].id,
		        (unsigned long)((now - seen[k].since) / 1000));
		for (int g = 0; g < 8; g++)
			if (ghosts[g].peer == seen[k].ptr) {
				logline("LOBBY %s left the network %lu ms after slot %d was cleared%s", ghosts[g].name,
				        (unsigned long)(now - ghosts[g].since), g, ghosts[g].warned ? " (was a ghost until now)" : "");
				ghosts[g].peer = 0;
			}
		seen[k].ptr = 0;
	}
	for (i = 0; i < count; i++) {
		if (!cur[i]) continue;
		for (k = 0; k < 8 && seen[k].ptr != cur[i]; k++) ;
		if (k < 8) continue;
		for (k = 0; k < 8 && seen[k].ptr; k++) ;
		if (k == 8) break;
		struct peer_seen *s = &seen[k];
		char b[96], c[96];
		unsigned flags = 0;
		s->ptr = cur[i];
		s->since = now;
		read_refstr(cur[i] + PEER_NAME, s->name, sizeof s->name);
		read_wstring(cur[i] + PEER_ID, s->id, sizeof s->id);
		read_wstring(cur[i] + 0x44, b, sizeof b);
		read_wstring(cur[i] + 0x5c, c, sizeof c);
		safe_read(cur[i] + PEER_FLAGS, &flags, 4);
		id_note(s->name);
		id_note(s->id);
		id_note(c);
		logline("NET + player joined the network list (#%d): %s | id %s | %s | %s | flags %x%s%s", i, s->name, s->id, b, c, flags,
		        (flags & 4) ? " (this PC)" : "", (flags & 1) ? " (host)" : "");
	}
}
static unsigned char *peer_matching(const char *a, const char *b)
{
	for (int k = 0; k < 8; k++) {
		const struct peer_seen *s = &seen[k];
		if (!s->ptr) continue;
		if (a[0] && (!lstrcmpA(a, s->name) || !lstrcmpA(a, s->id))) return s->ptr;
		if (b[0] && (!lstrcmpA(b, s->name) || !lstrcmpA(b, s->id))) return s->ptr;
	}
	return 0;
}
static void ghost_check(DWORD now)
{
	for (int g = 0; g < 8; g++)
		if (ghosts[g].peer && !ghosts[g].warned && now - ghosts[g].since >= GHOST_MS) {
			logline("LOBBY GHOST? %s is still connected %lu s after slot %d was cleared, without a slot",
			        ghosts[g].name, (unsigned long)((now - ghosts[g].since) / 1000), g);
			ghosts[g].warned = 1;
		}
}

/* display name of the player whose entity id / platform id is a or b (the slot strings), else a or b */
static const char *slot_who(const char *a, const char *b)
{
	for (int k = 0; k < 8; k++)
		if (seen[k].ptr && seen[k].name[0] && ((a[0] && !lstrcmpA(a, seen[k].id)) || (b[0] && !lstrcmpA(b, seen[k].id))))
			return seen[k].name;
	return a[0] ? a : b;
}

#include "steamlink.c" /* Steam link, phase A: measures only */

/* ---- exe lobby slots */
struct slot_seen { unsigned char v[11]; char a[48], b[48]; DWORD flips[16], win, flood_at; unsigned nflips; int lines, hidden, flooded; };
static struct slot_seen slots[8];
static int slots_valid;

static void slot_line(int i, struct slot_seen *s, DWORD now, const char *what)
{
	if (now - s->win >= 10000) {
		if (s->hidden) logline("LOBBY slot %d: %d more changes in the last 10 s not logged", i, s->hidden);
		s->win = now;
		s->lines = s->hidden = 0;
	}
	if (++s->lines > 10) { s->hidden++; return; }
	logline("LOBBY slot %d: %s ('%s' '%s') v30=%d 34-3a: %u %u %u %u %u %u %u%s", i, (s->a[0] || s->b[0]) ? slot_who(s->a, s->b) : "-",
	        s->a, s->b, *(int *)s->v, s->v[4], s->v[5], s->v[6], s->v[7], s->v[8], s->v[9], s->v[10], what);
}
static void slot_flip(int i, struct slot_seen *s, DWORD now)
{
	int n = 0;
	s->flips[s->nflips++ & 15] = now;
	for (unsigned j = 0; j < 16 && j < s->nflips; j++) if (now - s->flips[j] < FLOOD_WINDOW_MS) n++;
	if (n >= FLOOD_COUNT && (!s->flooded || now - s->flood_at >= 10000)) {
		logline("LOBBY FLOOD slot %d (%s): ready flag changed %d times in %d s", i, slot_who(s->a, s->b), n, FLOOD_WINDOW_MS / 1000);
		s->flooded = 1;
		s->flood_at = now;
	}
	lf_on_spam(s->a, slot_who(s->a, s->b), n); /* slot +0x00 = PlayFab entity id */
}
static void slot_scan(const unsigned char *table, DWORD now)
{
	for (int i = 0; i < 8; i++) {
		const unsigned char *p = table + i * SLOT_SIZE;
		struct slot_seen *s = &slots[i];
		unsigned char v[11];
		char a[48], b[48], olda[48], oldb[48], what[96];
		if (!safe_read(p + 0x30, v, 11)) return;
		read_wstring(p, a, sizeof a);
		read_wstring(p + 0x18, b, sizeof b);
		int changed_player = lstrcmpA(a, s->a) || lstrcmpA(b, s->b);
		if (slots_valid && !changed_player && bytes_eq(v, s->v, 11)) continue;
		int was = s->a[0] || s->b[0], now_taken = a[0] || b[0], ready_changed = v[10] != s->v[10];
		unsigned old_ready = s->v[10];
		lstrcpyA(olda, s->a); lstrcpyA(oldb, s->b);
		for (int j = 0; j < 11; j++) s->v[j] = v[j];
		lstrcpyA(s->a, a); lstrcpyA(s->b, b);
		if (!slots_valid) {
			if (now_taken) slot_line(i, s, now, " (as found)");
			continue;
		}
		what[0] = 0;
		if (ready_changed) wsprintfA(what, " [ready %u->%u]", old_ready, v[10]);
		if (was && !now_taken) lstrcatA(what, " [cleared]");
		else if (!was && now_taken) lstrcatA(what, " [taken]");
		else if (changed_player) lstrcatA(what, " [player changed]");
		slot_line(i, s, now, what);
		if (ready_changed && now_taken && old_ready <= 1 && v[10] <= 1) slot_flip(i, s, now); /* 2..12 = start countdown */
		if (was && !now_taken) {
			unsigned char *peer = peer_matching(olda, oldb);
			if (peer) {
				struct ghost_watch *g = &ghosts[i];
				g->peer = peer; g->since = now; g->warned = 0; g->kick_queued = 0;
				g->ban = LF_SHIFT_HELD(); /* the host booted with Shift held: ban (lobbyfix.c) */
				lstrcpyA(g->a, olda); lstrcpyA(g->b, oldb);
				lstrcpynA(g->name, slot_who(olda, oldb), sizeof g->name);
				logline("LOBBY slot %d cleared (boot or leave) while %s is still connected: watching", i, g->name);
			} else
				logline("LOBBY slot %d cleared; %s is not in the network list", i, olda[0] ? olda : oldb);
		}
		if (now_taken && changed_player)
			for (int g = 0; g < 8; g++)
				if (ghosts[g].peer && ((a[0] && (!lstrcmpA(a, ghosts[g].a) || !lstrcmpA(a, ghosts[g].b))) ||
				                       (b[0] && (!lstrcmpA(b, ghosts[g].a) || !lstrcmpA(b, ghosts[g].b))))) {
					logline("LOBBY %s is back in slot %d %lu ms after slot %d was cleared", ghosts[g].name, i,
					        (unsigned long)(now - ghosts[g].since), g);
					ghosts[g].peer = 0;
				}
	}
	slots_valid = 1;
}

/* ---- PlayFab lobby layer (CrossplayProxy -> PlayFabMultiplayerWin, IAT wrappers) */
static struct { void *h; int id; } pf_h[4];
static int pf_next_id;
static volatile LONG pf_changes, pf_member_updates, pf_posts, pf_post_fail, pf_added, pf_removed;
static char pf_user[40], pf_user_type[40];
static DWORD pf_win;
static int pf_lines, pf_hidden;

static int pf_lobby_no(void *h, const char *how)
{
	int k;
	if (!h) return 0;
	for (k = 0; k < 4; k++) if (pf_h[k].h == h) return pf_h[k].id;
	k = pf_next_id % 4;
	pf_h[k].h = h;
	pf_h[k].id = ++pf_next_id;
	logline("PF lobby #%d: handle %p (first seen in %s)", pf_h[k].id, h, how);
	return pf_h[k].id;
}
static int pf_budget(DWORD now)
{
	if (now - pf_win >= 10000) {
		if (pf_hidden) logline("PF %d more lobby update lines in the last 10 s not logged", pf_hidden);
		pf_win = now;
		pf_lines = pf_hidden = 0;
	}
	if (++pf_lines > 30) { pf_hidden++; return 0; }
	return 1;
}
static void key_strings(const void *key, char *id, char *ty)
{
	const char *k[2] = {0, 0};
	id[0] = ty[0] = 0;
	if (key && safe_read(key, k, 8)) { read_cstr(k[0], id, 40); read_cstr(k[1], ty, 40); }
}
/* "k=v,k=v" (vals NULL: "k,k"), at most `room` chars including the terminator */
static int fmt_kv(char *o, int room, const char *const *keys, const char *const *vals, unsigned n)
{
	int len = 0;
	unsigned i;
	o[0] = 0;
	if (!keys || room < 100) return 0;
	if (n > 24) n = 24;
	for (i = 0; i < n && room - len > 90; i++) {
		const char *kp = 0, *vp = 0;
		char k[32], v[28];
		if (!safe_read(keys + i, &kp, 4)) break;
		read_cstr(kp, k, sizeof k);
		if (vals && safe_read(vals + i, &vp, 4)) {
			read_cstr(vp, v, sizeof v);
			if (key_identifies(k))
				id_note(v);
			len += wsprintfA(o + len, "%s%s=%s", i ? "," : "", k, v);
		} else
			len += wsprintfA(o + len, "%s%s", i ? "," : "", k);
	}
	if (i < n) len += wsprintfA(o + len, ",+%u more", n - i);
	return len;
}
struct pf_member { char id[24]; DWORD t[16], flood_at; unsigned n; int flooded; };
static struct pf_member pf_mem[16];
static int pf_mem_next;
static void pf_member_update(const char *id, const char *keys, DWORD now)
{
	struct pf_member *m = 0;
	int k, cnt = 0;
	for (k = 0; k < 16; k++) if (pf_mem[k].id[0] && !lstrcmpA(pf_mem[k].id, id)) { m = &pf_mem[k]; break; }
	if (!m) {
		m = &pf_mem[pf_mem_next++ % 16];
		lstrcpynA(m->id, id, sizeof m->id);
		m->n = 0;
		m->flooded = 0;
	}
	m->t[m->n++ & 15] = now;
	for (unsigned j = 0; j < 16 && j < m->n; j++) if (now - m->t[j] < FLOOD_WINDOW_MS) cnt++;
	if (cnt >= FLOOD_COUNT && (!m->flooded || now - m->flood_at >= 10000)) {
		logline("PF FLOOD member %s: %d updates in %d s (last keys: %s)", m->id, cnt, FLOOD_WINDOW_MS / 1000, keys);
		m->flooded = 1;
		m->flood_at = now;
	}
}
static void pf_log_updated(const unsigned char *sc, DWORD now)
{
	unsigned char u[0x24];
	char line[1000];
	int n;
	if (!safe_read(sc, u, 0x24)) return;
	int no = pf_lobby_no(*(void *const *)(u + 4), "an update");
	unsigned nlk = *(const unsigned *)(u + 0x14), nm = *(const unsigned *)(u + 0x1c);
	const unsigned char *mu = *(const unsigned char *const *)(u + 0x20);
	n = wsprintfA(line, "PF lobby #%d updated:%s%s%s%s", no, u[8] ? " owner" : "", u[9] ? " max-members" : "", u[10] ? " access-policy" : "",
	              u[11] ? " membership-lock" : "");
	if (nlk) {
		n += wsprintfA(line + n, " lobby keys ");
		n += fmt_kv(line + n, 400, *(const char *const *const *)(u + 0x18), 0, nlk);
	}
	for (unsigned m = 0; m < nm && m < 8; m++) {
		unsigned char s[0x14]; /* PFLobbyMemberUpdateSummary: PFEntityKey, bool connectionStatusUpdated, u32 count, keys */
		char id[40], ty[40], keys[200];
		if (!safe_read(mu + m * 0x14, s, 0x14)) break;
		key_strings(s, id, ty);
		fmt_kv(keys, sizeof keys, *(const char *const *const *)(s + 0x10), 0, *(const unsigned *)(s + 0xc));
		InterlockedIncrement(&pf_member_updates);
		if (*(const unsigned *)(s + 0xc)) pf_member_update(id, keys, now);
		if (s[8]) logline("PF lobby #%d: member %s connection status changed", no, id);
		if (n < 700) n += wsprintfA(line + n, " | member %s%s: %s", id, s[8] ? " (connection)" : "", keys);
	}
	if (nm > 8 && n < 900) n += wsprintfA(line + n, " | +%u more members", nm - 8);
	if (pf_budget(now)) logline("%s", line);
}
static void pf_log_change(const unsigned char *sc, DWORD now)
{
	static const char *const removed_why[] = {"we left", "we were removed by the owner", "they left or were removed"};
	unsigned char c[0x18];
	char id[40], ty[40];
	if (!safe_read(sc, c, 4)) return;
	unsigned type = *(const unsigned *)c;
	InterlockedIncrement(&pf_changes);
	switch (type) {
	case 0: /* CreateAndJoinLobbyCompleted: result, asyncContext, lobby */
		if (safe_read(sc, c, 0x10)) {
			logline("PF created lobby #%d: result %08lx", pf_lobby_no(*(void **)(c + 0xc), "create"), *(long *)(c + 4));
			if (*(long *)(c + 4) >= 0) {
				lf_on_lobby_created(*(void **)(c + 0xc));
				gr_on_created(*(void **)(c + 0xc), now);
			}
		}
		break;
	case 1: /* JoinLobbyCompleted: result, PFEntityKey newMember, asyncContext, lobby */
		if (safe_read(sc, c, 0x18)) {
			key_strings(c + 8, id, ty);
			logline("PF joined lobby #%d as %s (%s): result %08lx", pf_lobby_no(*(void **)(c + 0x14), "join"), id, ty, *(long *)(c + 4));
			if (*(long *)(c + 4) >= 0)
				gr_on_joined(*(void **)(c + 0x14), now);
		}
		break;
	case 2: /* MemberAdded: lobby, PFEntityKey member */
		if (safe_read(sc, c, 0x10)) {
			key_strings(c + 8, id, ty);
			InterlockedIncrement(&pf_added);
			logline("PF lobby #%d: member added %s (%s)", pf_lobby_no(*(void **)(c + 4), "member added"), id, ty);
			lf_on_member_added(*(void **)(c + 4), id);
		}
		break;
	case 4: /* MemberRemoved: lobby, PFEntityKey member, reason */
		if (safe_read(sc, c, 0x14)) {
			unsigned r = *(unsigned *)(c + 0x10);
			key_strings(c + 8, id, ty);
			InterlockedIncrement(&pf_removed);
			logline("PF lobby #%d: member removed %s (%s), reason %u = %s", pf_lobby_no(*(void **)(c + 4), "member removed"), id, ty, r,
			        r < 3 ? removed_why[r] : "?");
		}
		break;
	case 5: /* ForceRemoveMemberCompleted: result, lobby, PFEntityKey target, asyncContext */
		if (safe_read(sc, c, 0x14)) {
			key_strings(c + 0xc, id, ty);
			logline("PF lobby #%d: force-remove of %s finished, result %08lx", pf_lobby_no(*(void **)(c + 8), "force-remove"), id, *(long *)(c + 4));
		}
		break;
	case 6: /* LeaveLobbyCompleted: lobby, ... (live: lobby handle first) */
		if (safe_read(sc, c, 8)) {
			logline("PF left lobby #%d", pf_lobby_no(*(void **)(c + 4), "leave completed"));
			lf_on_lobby_gone(*(void **)(c + 4));
			gr_on_gone(*(void **)(c + 4));
			td_on_lobby_gone(*(void **)(c + 4));
		}
		break;
	case 7:
		pf_log_updated(sc, now);
		break;
	case 8: /* PostUpdateCompleted: result, lobby, PFEntityKey localUser, asyncContext */
		if (safe_read(sc, c, 0xc) && *(long *)(c + 4) < 0) {
			InterlockedIncrement(&pf_post_fail);
			logline("PF our update to lobby #%d FAILED: result %08lx", pf_lobby_no(*(void **)(c + 8), "update result"), *(long *)(c + 4));
		}
		break;
	case 9: /* Disconnecting / Disconnected: lobby */
	case 10:
		if (safe_read(sc, c, 8)) {
			logline("PF lobby #%d: %s", pf_lobby_no(*(void **)(c + 4), "disconnect"), type == 9 ? "disconnecting" : "DISCONNECTED from the lobby service");
			if (type == 10) {
				lf_on_lobby_gone(*(void **)(c + 4));
				gr_on_gone(*(void **)(c + 4));
			}
			td_on_lobby_gone(*(void **)(c + 4)); /* the short-delay reads stop already while it disconnects */
		}
		break;
	case 12: /* FindLobbiesCompleted (lobby browser): frequent, not interesting here */
		break;
	default:
		if (safe_read(sc, c, 0x10) && pf_budget(now))
			logline("PF lobby state change type %u: %08x %08x %08x", type, *(unsigned *)(c + 4), *(unsigned *)(c + 8), *(unsigned *)(c + 0xc));
	}
}

typedef long(__stdcall *pf_start_fn)(void *, unsigned *, const void *const **);
typedef long(__stdcall *pf_post_fn)(void *, const void *, const void *, const void *, void *);
typedef long(__stdcall *pf_remove_fn)(void *, const void *, int, void *);
typedef long(__stdcall *pf_leave_fn)(void *, const void *, void *);
static pf_start_fn pf_start_orig;
static pf_post_fn pf_post_orig;
static pf_remove_fn pf_remove_orig;
static pf_leave_fn pf_leave_orig;

#include "lobbyfix.c" /* host-only lobby fixes: real kick on boot, ban list, ready spam */
#include "ghostroom.c" /* leave the lobby a failed Create left behind */
#include "soundfix.c" /* quit-crash fix on the game's own dssl.dll, in memory */

static long __stdcall w_pf_start(void *h, unsigned *count, const void *const **changes)
{
	long hr = pf_start_orig(h, count, changes);
	if (hr >= 0 && count && changes) {
		unsigned n = *count;
		const void *const *arr = *changes;
		DWORD now = GetTickCount();
		if (n > 64) n = 64;
		for (unsigned i = 0; i < n && arr; i++) {
			const void *sc;
			if (!safe_read(arr + i, &sc, 4)) break;
			pf_log_change(sc, now);
		}
	}
	return hr;
}
static void pf_log_post(void *lobby, const void *user, const void *lu, const void *mu, long hr, DWORD now)
{
	char id[40], ty[40], lob[420], mem[420];
	key_strings(user, id, ty);
	if (lstrcmpA(id, pf_user) || lstrcmpA(ty, pf_user_type)) {
		lstrcpyA(pf_user, id);
		lstrcpyA(pf_user_type, ty);
		id_note(id);
		logline("PF local user: entity id %s, entity type '%s'", id, ty);
	}
	InterlockedIncrement(&pf_posts);
	if (lu && hr >= 0)
		lf_on_lobby_data_post(lobby); /* only the lobby owner (host) writes lobby data: this is the game lobby */
	lob[0] = mem[0] = 0;
	unsigned char d[0x28]; /* PFLobbyDataUpdate (fields up to lobbyPropertyValues) */
	if (lu && safe_read(lu, d, 0x28)) {
		int n = 0;
		if (*(void **)d) {
			char oid[40], oty[40];
			key_strings(*(void **)d, oid, oty);
			n += wsprintfA(lob + n, "NEW OWNER %s ", oid);
		}
		unsigned lockv;
		if (*(void **)(d + 0xc) && safe_read(*(void **)(d + 0xc), &lockv, 4)) n += wsprintfA(lob + n, "membership-lock=%u ", lockv);
		n += fmt_kv(lob + n, 300, *(const char *const **)(d + 0x20), *(const char *const **)(d + 0x24), *(unsigned *)(d + 0x1c));
		if (*(unsigned *)(d + 0x10) && n < 300) {
			n += wsprintfA(lob + n, " search: ");
			fmt_kv(lob + n, 410 - n, *(const char *const **)(d + 0x14), *(const char *const **)(d + 0x18), *(unsigned *)(d + 0x10));
		}
	}
	unsigned char md[12]; /* PFLobbyMemberDataUpdate: count, keys, values */
	if (mu && safe_read(mu, md, 12)) fmt_kv(mem, sizeof mem, *(const char *const **)(md + 4), *(const char *const **)(md + 8), *(unsigned *)md);
	if (hr < 0) InterlockedIncrement(&pf_post_fail);
	if (hr < 0 || pf_budget(now))
		logline("PF post to lobby #%d: lobby {%s} member {%s} -> %08lx", pf_lobby_no(lobby, "our post"), lob, mem, hr);
}
static long __stdcall w_pf_post(void *lobby, const void *user, const void *lu, const void *mu, void *ctx)
{
	td_rewrite(lobby, &mu); /* shorter command delay: our member key rides along (turndelay.c) */
	lt_rewrite(user, &mu);  /* lobby tags: so does our release (lobbytag.c) */
	long hr = pf_post_orig(lobby, user, lu, mu, ctx);
	pf_log_post(lobby, user, lu, mu, hr, GetTickCount());
	return hr;
}
static long __stdcall w_pf_remove(void *lobby, const void *target, int prevent_rejoin, void *ctx)
{
	long hr = pf_remove_orig(lobby, target, prevent_rejoin, ctx);
	char id[40], ty[40];
	key_strings(target, id, ty);
	logline("PF force-remove %s (%s) from lobby #%d, prevent rejoin %d -> %08lx", id, ty, pf_lobby_no(lobby, "force-remove"), prevent_rejoin & 0xff, hr);
	return hr;
}
static long __stdcall w_pf_leave(void *lobby, const void *user, void *ctx)
{
	long hr = pf_leave_orig(lobby, user, ctx);
	logline("PF leaving lobby #%d -> %08lx", pf_lobby_no(lobby, "leave"), hr);
	return hr;
}

/* address of `mod`'s import-table entry for dll!fn (by name), or NULL */
static void **iat_slot(HMODULE mod, const char *dll, const char *fn)
{
	unsigned char *b = (unsigned char *)mod;
	const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
	const IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!d->VirtualAddress) return 0;
	for (const IMAGE_IMPORT_DESCRIPTOR *imp = (const void *)(b + d->VirtualAddress); imp->Name; imp++) {
		if (lstrcmpiA((const char *)(b + imp->Name), dll) || !imp->OriginalFirstThunk) continue;
		const IMAGE_THUNK_DATA32 *names = (const void *)(b + imp->OriginalFirstThunk);
		IMAGE_THUNK_DATA32 *iat = (void *)(b + imp->FirstThunk);
		for (; names->u1.AddressOfData; names++, iat++)
			if (!(names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) &&
			    !lstrcmpA((const char *)((const IMAGE_IMPORT_BY_NAME *)(b + names->u1.AddressOfData))->Name, fn))
				return (void **)&iat->u1.Function;
	}
	return 0;
}
static int iat_hook(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig)
{
	void **slot = iat_slot(mod, dll, fn);
	MEMORY_BASIC_INFORMATION mi;
	DWORD old;
	if (!slot || !VirtualQuery(slot, &mi, sizeof mi)) return 0;
	/* keep execute permission if the page had it (another thread may be running code on it) */
	if (!VirtualProtect(slot, 4, (mi.Protect & 0xf0) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old)) return 0;
	*orig = *slot; /* first, so a call that already lands in the hook can go on */
	*slot = hook;
	VirtualProtect(slot, 4, old, &old);
	return 1;
}
static void pf_hooks_install(void)
{
	static const char pf[] = "PlayFabMultiplayerWin.dll";
	HMODULE cp = GetModuleHandleA("CrossplayProxy.dll");
	if (!cp) { logline("lobby logging: CrossplayProxy.dll not loaded, PlayFab lobby calls not logged"); return; }
	if (!build_is(cp, CP_TIMESTAMP, CP_IMAGESIZE)) { logline("lobby logging: CrossplayProxy.dll is an unknown build, PlayFab lobby calls not logged"); return; }
	int a = iat_hook(cp, pf, "PFMultiplayerStartProcessingLobbyStateChanges", (void *)w_pf_start, (void **)&pf_start_orig);
	int b = iat_hook(cp, pf, "PFLobbyPostUpdate", (void *)w_pf_post, (void **)&pf_post_orig);
	int c = iat_hook(cp, pf, "PFLobbyForceRemoveMember", (void *)w_pf_remove, (void **)&pf_remove_orig);
	int d = iat_hook(cp, pf, "PFLobbyLeave", (void *)w_pf_leave, (void **)&pf_leave_orig);
	logline("lobby logging: PlayFab lobby calls logged: state changes %s, updates %s, force-remove %s, leave %s", a ? "ok" : "NO",
	        b ? "ok" : "NO", c ? "ok" : "NO", d ? "ok" : "NO");
}

/* ---- loading: sync points (reverse/lobby-start-network.md 2.4). Every client broadcasts {0x0A, u32 player}
 * when it reaches a sync point and waits until all others have; the arrival times say who is slow. */
static int bar_n, bar_seen[16];
static DWORD bar_t[16], bar_last;
static void bar_fresh(DWORD now)
{
	if (!bar_last || now - bar_last > 300000) { /* a new loading sequence */
		bar_n = 0;
		for (int p = 0; p < 16; p++) bar_seen[p] = 0;
	}
	bar_last = now;
}
static void bar_ours(DWORD now)
{
	if (!lobby_log) return;
	bar_fresh(now);
	if (bar_n < 16) bar_t[bar_n] = now;
	bar_n++;
	if (bar_n > 1 && bar_n <= 16) logline("LOAD sync point %d reached by this PC, %lu ms after sync point %d", bar_n, (unsigned long)(now - bar_t[bar_n - 2]), bar_n - 1);
	else logline("LOAD sync point %d reached by this PC", bar_n);
}
static void bar_arrival(int idx, int is_local, const char *name, DWORD now)
{
	bar_fresh(now);
	if (idx < 0 || idx > 15) { logline("LOAD sync packet with player index %d from %s", idx, name); return; }
	int k = ++bar_seen[idx];
	if (k <= bar_n && k <= 16)
		logline("LOAD sync point %d: player %d (%s)%s %lu ms after this PC", k, idx, name, is_local ? " = this PC, own packet back" : " arrived",
		        (unsigned long)(now - bar_t[k - 1]));
	else
		logline("LOAD sync point %d: player %d (%s) arrived before this PC", k, idx, name);
}
/* slot 23: pop one received message: char pop(this, void *buf, peer **sender, unsigned *len), ret 0xc */
typedef int(THISCALL *pop_fn)(void *, void *, void **, unsigned *);
static int THISCALL w_pop(void *self, void *buf, void **sender, unsigned *len)
{
	int r = ((pop_fn)orig_vt[23])(self, buf, sender, len);
	if ((r & 0xff) && buf && len && sender && *sender)
		sl_mirror_party((const unsigned char *)*sender, (const unsigned char *)buf, *len);
	if ((r & 0xff) && lobby_log && buf && len && *len == 5 && *(unsigned char *)buf == 0x0A) {
		unsigned char *peer = sender ? (unsigned char *)*sender : 0, *local = 0;
		char name[48];
		name[0] = 0;
		safe_read((char *)self + NETSYS_LOCAL, &local, 4);
		if (peer) read_refstr(peer + PEER_NAME, name, sizeof name);
		bar_arrival(*(int *)((unsigned char *)buf + 1), peer && peer == local, name[0] ? name : "?", GetTickCount());
	}
	return r;
}

/* ---- the lobby tick (slot 15 check_pulse, main thread) */
static void lobby_status(void *self, unsigned flags, DWORD now, const char *why)
{
	char line[700];
	int n = 0, handles = 0;
	line[0] = 0;
	for (int k = 0; k < 8 && n < 560; k++) {
		unsigned char *p = seen[k].ptr, ready = 0;
		unsigned pulse = 0, pfl = 0;
		void *link = 0;
		if (!p) continue;
		safe_read(p + PEER_PULSE, &pulse, 4);
		safe_read(p + PEER_FLAGS, &pfl, 4);
		safe_read(p + PEER_LINK, &link, 4);
		safe_read(p + PEER_READY, &ready, 1);
		if (pfl & 4) n += wsprintfA(line + n, " [%s: this PC%s]", seen[k].name, (pfl & 1) ? ", host" : "");
		else n += wsprintfA(line + n, " [%s: pulse %lu ms ago%s%s%s]", seen[k].name, (unsigned long)(now - pulse), ready ? ", connected" : "",
		                    link ? "" : ", NO data channel", (pfl & 1) ? ", host" : "");
	}
	for (int k = 0; k < 4; k++) if (pf_h[k].h) handles++;
	logline("LOBBY status (%s): this PC is %s, netsys flags %x, players:%s | PlayFab: %d lobby handle(s), %ld state changes, %ld member updates, "
	        "%ld posts by us (%ld failed), %ld members added, %ld removed", why, (flags & 1) ? "host" : "not host", flags, line, handles,
	        pf_changes, pf_member_updates, pf_posts, pf_post_fail, pf_added, pf_removed);
}
static void lobby_tick(void *self)
{
	static int started;
	static DWORD last, last_status;
	static void *last_setup;
	DWORD now = GetTickCount();
	unsigned flags = 0;
	void *setup = 0;
	unsigned char *table = 0;
	if (!exe_ok) return;
	if (started && now - last < 100) return;
	last = now;
	safe_read((char *)self + NETSYS_FLAGS, &flags, 4);
	safe_read(exe_base + RVA_SETUPWIN_PTR, &setup, 4);
	if (!started) {
		started = 1;
		logline("LOBBY logging on: check_pulse runs on this thread; netsys flags %x, setup window %p", flags, setup);
		ls_thread = GetCurrentThreadId(); /* the game's main thread: the only one lobby lines are drawn from */
		last_setup = setup;
	}
	peer_scan(self, now);
	int opened_now = setup && !last_setup, closed_now = !setup && last_setup;
	if ((setup != 0) != (last_setup != 0)) {
		logline("LOBBY setup window %s (%p), netsys flags %x", setup ? "opened" : "closed", setup ? setup : last_setup, flags);
		slots_valid = 0;
		lobby_status(self, flags, now, setup ? "lobby opened" : "lobby closed");
		last_status = now;
	}
	last_setup = setup;
	if (setup && safe_read(exe_base + RVA_SLOT_TABLE_PTR, &table, 4) && table) slot_scan(table, now);
	ghost_check(now);
	if (setup && now - last_status >= 30000) {
		lobby_status(self, flags, now, "every 30 s");
		last_status = now;
	}
	gr_tick(self, now, setup, flags);
	lf_tick(self, now);
	td_tick(setup != 0, closed_now, opened_now);
	lt_tick(setup != 0, opened_now, now);
	gs_tick(setup != 0, closed_now, opened_now, now);
}

/* ------------------------------------------------------------------ the one real export */
__declspec(dllexport) void *__cdecl get_netsys_object_ptr(void *arg)
{
	HMODULE orig = GetModuleHandleA("CrossplayNetLib_orig.dll");
	if (!orig)
		orig = LoadLibraryA("CrossplayNetLib_orig.dll");
	get_obj_fn real = orig ? (get_obj_fn)GetProcAddress(orig, "get_netsys_object_ptr") : 0;
	if (!real)
		return 0;

	if (logf == INVALID_HANDLE_VALUE) {
		char path[MAX_PATH], ini[MAX_PATH];
		lstrcpyA(path, dir);
		lstrcatA(path, "ronfix-net.log");
		lstrcpyA(ini, dir);
		lstrcatA(ini, "ronfix.ini");
		logf = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		exe_base = (unsigned char *)GetModuleHandleA(NULL);
		exe_ok = build_is(exe_base, EXE_TIMESTAMP, EXE_IMAGESIZE);
		report_ms = (int)GetPrivateProfileIntA("netfix", "WaitReportSeconds", 20, ini) * 1000;
		tel_interval_ms = (int)GetPrivateProfileIntA("telemetry", "Interval", 10, ini) * 1000;
		probe_steamnet = (int)GetPrivateProfileIntA("probe", "SteamNet", 0, ini);
		lobby_log = (int)GetPrivateProfileIntA("lobby", "Log", 1, ini);
		lf_enabled = (int)GetPrivateProfileIntA("lobby", "Fixes", 1, ini);
		lf_bootkick = (int)GetPrivateProfileIntA("lobby", "BootKick", 1, ini);
		lf_spamkick = (int)GetPrivateProfileIntA("lobby", "SpamKick", 12, ini);
		gr_enabled = (int)GetPrivateProfileIntA("lobby", "GhostRoom", 1, ini);
		lt_enabled = (int)GetPrivateProfileIntA("lobby", "Tags", 1, ini);
		ls_enabled = (int)GetPrivateProfileIntA("lobby", "TagLines", 1, ini);
		td_host_wants = (int)GetPrivateProfileIntA("netfix", "ShortDelay", 0, ini);
		td_min = (unsigned)GetPrivateProfileIntA("netfix", "TurnFloorMin", 200, ini);
		td_max = (unsigned)GetPrivateProfileIntA("netfix", "TurnFloorMax", 300, ini);
		gs_switch = (int)GetPrivateProfileIntA("gamefix", "Switch", 0, ini);
		gs_solo = (int)GetPrivateProfileIntA("gamefix", "Solo", 0, ini);
		lstrcpyA(lf_ban_path, dir);
		lstrcatA(lf_ban_path, "ronfix-bans.txt");
		sl_enabled = (int)GetPrivateProfileIntA("steam", "Shadow", 1, ini);
		sl_mirror = (int)GetPrivateProfileIntA("steam", "Mirror", 0, ini);
		sl_selftest = sl_mirror && (int)GetPrivateProfileIntA("steam", "SelfTest", 0, ini); /* one-PC testing only */
		{ /* our release number for the Steam link's hello: ronfix\version.txt, first number */
			char vp[MAX_PATH], vb[16];
			DWORD got = 0;
			lstrcpyA(vp, dir);
			lstrcatA(vp, "ronfix\\version.txt");
			HANDLE vf = CreateFileA(vp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
			if (vf != INVALID_HANDLE_VALUE) {
				ReadFile(vf, vb, sizeof vb - 1, &got, NULL);
				CloseHandle(vf);
			}
			DWORD k = 0;
			for (; k < got && vb[k] >= '0' && vb[k] <= '9'; k++)
				sl_release_no = sl_release_no * 10 + (unsigned)(vb[k] - '0');
			/* the second word is the name players know the release by ("16 0.16"): for the lobby tags */
			vb[got < sizeof vb ? got : sizeof vb - 1] = 0;
			while (k < got && vb[k] == ' ')
				k++;
			for (DWORD e = k; e < got; e++)
				if ((unsigned char)vb[e] <= ' ')
					vb[e] = 0;
			lt_set_release(sl_release_no, k < got ? vb + k : "");
		}
		logline("==== RoN-Fix core v20, pid %lu; exe build %s; fix A %s (report after %d s without turn data); fix B (desync-vote crash) %s",
		        GetCurrentProcessId(), exe_ok ? "ok" : "UNKNOWN", exe_ok && report_ms > 0 ? "ON" : "OFF",
		        report_ms / 1000, exe_ok ? "ON" : "OFF");
		sac_log();
		snd_migrate(); /* before the game loads dssl.dll */
		if (warmed)
			logline("startup: crypto warmed at load (BCryptGenRandom status 0x%08lx)", (unsigned long)warm_status);
		else
			logline("startup: crypto warm-up off (ronfix.ini [startup] WarmCrypto=0)");
		logline(maskfix_status > 0 ? "fix F (object-mask damage bug) applied at load: balance.xml mask modifiers active (changes combat: everyone needs it)"
		        : maskfix_status < 0 ? "fix F (object-mask damage bug) NOT applied: code is not what was expected"
		                             : gamefix_version > 0 ? "fix F (object-mask damage bug) off (ronfix.ini [gamefix] MaskDamageFix=0)"
		                             : gamefix_version < 0 ? "gameplay pack switched off (see next line): fix F, gameplay code fixes and version guard off"
		                                                   : "gameplay fixes not installed ([gamefix] Version=0): fix F and version guard off");
		if (gamefix_version < 0) {
			logline("GAMEPLAY PACK %d SWITCHED OFF: %s. Run INSTALL.bat again (Steam probably restored the original files).", -gamefix_version, gamefix_data_note);
			/* Once per start, main thread, outside the loader lock (this export runs at start-up). Without it the
			 * player only finds out when the group's lobby refuses them with "version does not match". */
			MessageBoxA(NULL,
			            "The Accord gameplay pack is switched OFF: the game's data files are no longer the ones the pack "
			            "installed (Steam probably restored the originals after an update or \"Verify integrity\").\n\n"
			            "You can still play with people who do NOT have the gameplay pack.\n"
			            "To play with your group again, close the game and run INSTALL.bat from the gameplay pack's folder.",
			            "Accord", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
		}
		if (gamefix_version > 0)
			logline("gameplay code fixes (pack %d): CBP %s", gamefix_version, cbpfix_report);
		if (GetPrivateProfileIntA("exefix", "Enabled", 1, ini))
			apply_exe_patches(GetPrivateProfileIntA("exefix", "SelfTest", 0, ini));
		else
			logline("exe crash fixes off (ronfix.ini [exefix] Enabled=0)");
		if (exe_ok)
			td_init(exe_base);
		logline("short command delay: %s; %s, turn floor %u-%u ms", td_capable ? "this PC can take part" : "NOT possible (unexpected code at turn start)",
		        td_host_wants ? "on when hosting ([netfix] ShortDelay=1)" : "off when hosting ([netfix] ShortDelay=0)", td_min, td_max);
		if (exe_ok && lt_enabled)
			ls_init(exe_base); /* the lobby tags on screen, if the game's chat line code is as expected */
		if (exe_ok)
			gs_init(exe_base, dir); /* after lt_set_release: it adds its part to our lobby tag */
		gs_log_start();
		if ((lobby_log || lf_enabled || td_capable || gs_ready) && exe_ok)
			pf_hooks_install();
		else
			logline("lobby logging and fixes off (%s)", exe_ok ? "ronfix.ini [lobby] Log=0, Fixes=0" : "unknown exe build");
		if (exe_ok && lf_enabled)
			logline("lobby fixes on (as host): boot = real kick%s, ban list ronfix-bans.txt (Shift + boot adds), ready spam kick at %d toggles in 5 s",
			        lf_bootkick ? "" : " OFF", lf_spamkick);
		if (exe_ok)
			logline("ghost room clean-up %s", gr_enabled ? "on: a room that Create left behind is left again ([lobby] GhostRoom=1)" : "off ([lobby] GhostRoom=0)");
	}

	void **obj = (void **)real(arg);
	if (!obj)
		return obj;
	if (!build_is(orig, DLL_TIMESTAMP, DLL_IMAGESIZE)) {
		logline("CrossplayNetLib_orig.dll is an unknown build: no hooks, plain forwarding");
		return obj;
	}
	void **vt = (void **)obj[0];
	if (!vt_ready) {
		int i;
		for (i = 0; i < VT_COPY; i++)
			orig_vt[i] = our_vt[i] = vt[i];
		our_vt[1] = (void *)w_init;
		our_vt[14] = (void *)w_send_desync;
		our_vt[15] = (void *)w_check_pulse;
		our_vt[18] = (void *)w_set_allow_timeout;
		our_vt[21] = (void *)w_send_to_peer;
		our_vt[22] = (void *)w_broadcast;
		our_vt[23] = (void *)w_pop;
		our_vt[31] = (void *)w_disconnect;
		our_vt[36] = (void *)w_find_player_by_name;
		our_vt[41] = (void *)w_drop_player;
		our_vt[42] = (void *)w_cancel_drop;
		our_vt[62] = (void *)w_waiting;
		orig_vt_addr = vt;
		vt_ready = 1;
	}
	if (vt == orig_vt_addr) {
		obj[0] = (void *)our_vt;
		logline("netsys object %08x hooked", (unsigned)(UINT_PTR)obj);
	} else {
		logline("netsys object %08x has another vtable %08x: left alone", (unsigned)(UINT_PTR)obj, (unsigned)(UINT_PTR)vt);
	}
	return obj;
}

/* ------------------------------------------------------------------ fix F: object-mask damage bug
 * balance.xml's 32 object-mask entries (Flag_A_OBJMASK_ARMORED ... Flag_6_OBJMASK_ANTI_AIR) have no
 * effect in EE, so almost every unit matchup deals the wrong damage (CBP: ~28,700 modifiers).
 * The damage-grid builder (C_load_xml_value_table FUN_005823f0 -> N_compute_type_damage
 * FUN_00581cc0) combines unit, type, age AND mask entries exactly like Thrones & Patriots, but it
 * finds each entry by name, and the mask names are built (FUN_005829b0) as
 *     pool"Flag " + letter + pool[+0x154] + "OBJMASK_..."   (spaces -> '_')
 * In EE the pool string at +0x154 is never set (empty), so the game looks for
 * "Flag_EOBJMASK_ELEPHANT" and every mask row/column falls back to 100%. The pool already holds the
 * '_' separator the game uses elsewhere (+0x4894: conquest rules, terrain text). Fix: the
 * `add eax, 0x154` at 0x00582b6a becomes `add eax, 0x4894`. Applied here, at load, because the
 * grid is built once during start-up (C_init_type_tables FUN_00669cc0). Changes combat: every player
 * in a game must have the same setting. reverse/crash-dssl.md has the verification. */
#define RVA_MASKNAME_SEP 0x182b67 /* 8b 40 10 | 05 <imm32 0x154> | 50 | e8 */
static const unsigned char maskname_sep_code[10] = {0x8b, 0x40, 0x10, 0x05, 0x54, 0x01, 0x00, 0x00, 0x50, 0xe8};
static void apply_mask_fix(void)
{
	unsigned char *base = (unsigned char *)GetModuleHandleA(NULL), *p = base + RVA_MASKNAME_SEP;
	maskfix_status = -1;
	if (!build_is(base, EXE_TIMESTAMP, EXE_IMAGESIZE))
		return;
	for (unsigned i = 0; i < sizeof maskname_sep_code; i++)
		if (p[i] != maskname_sep_code[i])
			return;
	DWORD old;
	if (!VirtualProtect(p + 4, 4, PAGE_EXECUTE_READWRITE, &old))
		return;
	*(DWORD *)(p + 4) = 0x4894;
	VirtualProtect(p + 4, 4, old, &old);
	FlushInstructionCache(GetCurrentProcess(), p + 4, 4);
	maskfix_status = 1;
}

/* ------------------------------------------------------------------ CBP's exe bug fixes (gameplay pack)
 * Re-implemented from CBP Alpha 11's exe patch (cbp/decisions/exe-sites.md has the analysis of all 47 CBP
 * sites); only its plain bug fixes, none of its balance or RNG changes. VA = address in a 0x400000-based
 * image. Each patch checks the original bytes first. Applied at load when [gamefix] Version > 0. */
void *rf_g2_back __asm__("rf_g2_back");
void *rf_g2_skip __asm__("rf_g2_skip");
void *rf_g4_orig __asm__("rf_g4_orig");
void *rf_g4_back __asm__("rf_g4_back");
unsigned char *rf_g4_flags __asm__("rf_g4_flags");
void *rf_g7_orig __asm__("rf_g7_orig");
void *rf_g7_back __asm__("rf_g7_back");
unsigned char **rf_g7_ptr __asm__("rf_g7_ptr");
void *rf_g3_owns __asm__("rf_g3_owns");
void *rf_g3_power __asm__("rf_g3_power");
void *rf_g3_find __asm__("rf_g3_find");
void *rf_g3_keep __asm__("rf_g3_keep");
void *rf_g3_wgloop __asm__("rf_g3_wgloop");
unsigned char *rf_h1_players __asm__("rf_h1_players");
void *rf_h1_team __asm__("rf_h1_team");
void *rf_h1_xfer __asm__("rf_h1_xfer");
void *rf_h1_back __asm__("rf_h1_back");

/* G2 Chinese assimilation (RoN/T&P bugfix): a captured City became a Large City whatever its size (also the
 * Forbidden City). At VA 0x738F52 the game pushes (0, 0x19F = Large City) for the Build's set-type virtual,
 * EAX = Build. Only do it when the Build's type is Small City (0x19E, field +0x6C as in CBP). */
__attribute__((naked)) static void g2_thunk(void)
{
	__asm__ volatile(
		"cmpl $0x19E, 0x6C(%eax)\n\t"
		"jne 1f\n\t"
		"pushl $0\n\t"
		"pushl $0x19F\n\t"
		"jmp *rf_g2_back\n"
		"1:\n\t"
		"jmp *rf_g2_skip\n");
}
/* G4 Russian attrition in nomad (RoN bugfix): creating the first (nomad) City did not raise the owner's
 * "recompute attrition" flag (0x4000000 in the per-player word at 0xE3A390 + player*0x6EEC). After the
 * pop-cap update at VA 0x737398 (call 0x6DC490; ESI = city record, owner at +0x5E) set it. */
__attribute__((naked)) static void g4_thunk(void)
{
	__asm__ volatile(
		"call *rf_g4_orig\n\t"
		"movsbl 0x5E(%esi), %eax\n\t"
		"imull $0x6EEC, %eax, %ecx\n\t"
		"addl rf_g4_flags, %ecx\n\t"
		"orl $0x4000000, (%ecx)\n\t"
		"jmp *rf_g4_back\n");
}
/* G7 Borders at game start (RoN bugfix, CBP Alpha 11): after the start territory computation at VA 0x5AD603
 * (call 0x6B5700) set the border-dirty byte [[0xC0620C]+0x22A] so the border lines are drawn. */
__attribute__((naked)) static void g7_thunk(void)
{
	__asm__ volatile(
		"call *rf_g7_orig\n\t"
		"pushl %eax\n\t"
		"movl rf_g7_ptr, %eax\n\t"
		"movl (%eax), %eax\n\t"
		"testl %eax, %eax\n\t"
		"jz 1f\n\t"
		"movb $1, 0x22A(%eax)\n"
		"1:\n\t"
		"popl %eax\n\t"
		"jmp *rf_g7_back\n");
}

/* G3 Persians vs World Government (T&P bugfix, CBP Alpha 11; RoN wiki "World Government": capturing either
 * of a Persian's two capitals eliminated them at once when the capturer had World Government).
 * 0x6DC6D0 (this = ESI = player who lost a capital, EDI = capturer) in the capital-timer mode
 * ([[0xC061EC]+0x37] == 1) skips the World Government instant elimination only for the Forbidden City owner
 * (call 0x6EBC10 with 0x213 already pushed). The Sudden Death Capital branch (0x6DC7D0) also exempts a
 * Persian (power 0x17) who still has a second capital (0x6EB930 finds a city flagged capital, owner = this
 * player); this branch lacked that. Same sequence as the game's own at 0x6DC7D0-0x6DC800 (and as CBP's cave).
 * Entered by jmp, so the stack is the call site's: 0x6EB930 writes its two outputs into this function's
 * argument slots [esp+0x14]/[esp+0x1C], exactly as the original branch does (nothing reads them later on
 * either path; EDI/EBX stay in registers). Both targets are the original jump targets. */
__attribute__((naked)) static void g3_thunk(void)
{
	__asm__ volatile(
		"call *rf_g3_owns\n\t"          /* original: owns Forbidden City? (0x213 pushed by the game) */
		"testl %eax, %eax\n\t"
		"jnz 2f\n\t"
		"movl %esi, %ecx\n\t"
		"pushl $0x17\n\t"
		"call *rf_g3_power\n\t"         /* Persian power? */
		"testl %eax, %eax\n\t"
		"jz 1f\n\t"
		"pushl $-1\n\t"
		"pushl $-1\n\t"
		"leal 0x24(%esp), %eax\n\t"
		"movl %esi, %ecx\n\t"
		"pushl %eax\n\t"
		"leal 0x20(%esp), %eax\n\t"
		"pushl %eax\n\t"
		"call *rf_g3_find\n\t"          /* another capital: idx -> [esp+0x14], owner -> [esp+0x1C] */
		"cmpl $0, 0x14(%esp)\n\t"
		"jl 1f\n\t"
		"movl 0x1C(%esp), %eax\n\t"
		"cmpl 0x8(%esi), %eax\n\t"
		"jne 1f\n"
		"2:\n\t"
		"jmp *rf_g3_keep\n"             /* 0x6DC797: no instant elimination (timer path as normal) */
		"1:\n\t"
		"jmp *rf_g3_wgloop\n");         /* 0x6DC74C: World Government check as before */
}

/* H1 (RoN-Fix's own, user report 2026-11-26): an ally could not give a player his capital back after the
 * captor had moved his own capital into it. Analysis: reverse/capital-recapture.md.
 * When a city falls (0x6276A0), the game gives it to an ally of the capturing team if that ally is its
 * original owner (+0x60) or rightful owner (+0x5F). Building a Senate in a captured capital sets both to the
 * captor, so B's ally D kept B's capital, and B's capital countdown never stopped (it only stops when the
 * player HIMSELF receives a city whose "captured capital of" mask (+0x68) has his bit). The mask still has
 * B's bit, so: at the transfer call (0x627DBF; EDI = current owner, EBX = chosen new owner, [ebp+8] = best
 * player of the capturing team, [ebp+0xC] = city object, [ebp-0x54] = city index), when no original owner was
 * found (EBX == best), give the city to the lowest-numbered player p whose bit is in the mask, who is not the
 * owner or the capturer, is in the game (flag 2) and is a mutual ally of the capturer (0x6EDB50, the same
 * tests as the game's own rule). Also set +0x60 = p on the old object so the transfer (0x733380 ->
 * 0x736C40) restores the capital flag, the Persian marker, and STOPs p's countdown, and the captor's own
 * countdown starts (his capital was taken). Cities with an empty mask, and every case the game's rule already
 * handles, take the original path unchanged. Deterministic: synced state only, players 0..7 in order. */
__attribute__((naked)) static void h1_thunk(void)
{
	__asm__ volatile(
		"pushl %eax\n\t"
		"pushl %ecx\n\t"
		"pushl %edx\n\t"
		"pushl %esi\n\t"
		"cmpl 8(%ebp), %ebx\n\t"         /* the game already found an original owner? */
		"jne 9f\n\t"
		"movl 0xC(%ebp), %eax\n\t"
		"movzbl 0x68(%eax), %esi\n\t"    /* captured-capital-of mask */
		"testl %esi, %esi\n\t"
		"jz 9f\n\t"
		"pushl $0\n"                     /* [esp] = p */
		"1:\n\t"
		"movl (%esp), %edx\n\t"
		"btl %edx, %esi\n\t"
		"jnc 3f\n\t"
		"cmpl %edi, %edx\n\t"            /* not the current owner */
		"je 3f\n\t"
		"cmpl %ebx, %edx\n\t"            /* not the capturer */
		"je 3f\n\t"
		"imull $0x6EEC, %edx, %eax\n\t"
		"addl rf_h1_players, %eax\n\t"
		"testb $2, (%eax)\n\t"           /* p in the game */
		"jz 3f\n\t"
		"imull $0x6EEC, %ebx, %ecx\n\t"
		"addl rf_h1_players, %ecx\n\t"
		"pushl %edx\n\t"
		"call *rf_h1_team\n\t"           /* mutual allies (thiscall, pops its arg; EAX/ECX/EDX only) */
		"testl %eax, %eax\n\t"
		"jz 3f\n\t"
		"movl (%esp), %ebx\n\t"          /* new owner = p */
		"addl $4, %esp\n\t"
		"movl 0xC(%ebp), %eax\n\t"
		"movb %bl, 0x60(%eax)\n\t"
		"jmp 9f\n"
		"3:\n\t"
		"incl (%esp)\n\t"
		"cmpl $8, (%esp)\n\t"
		"jl 1b\n\t"
		"addl $4, %esp\n"
		"9:\n\t"
		"popl %esi\n\t"
		"popl %edx\n\t"
		"popl %ecx\n\t"
		"popl %eax\n\t"
		"pushl %edi\n\t"                 /* original: push owner; push idx; push new owner; call 0x733380 */
		"pushl -0x54(%ebp)\n\t"
		"pushl %ebx\n\t"
		"call *rf_h1_xfer\n\t"
		"jmp *rf_h1_back\n");
}

#define VA(x) (exe + ((x) - 0x400000u))
static int bytes_are(const unsigned char *p, const char *hex)
{
	for (; hex[0] && hex[1]; hex += 2, p++) {
		int hi = hex[0] <= '9' ? hex[0] - '0' : (hex[0] | 32) - 'a' + 10;
		int lo = hex[1] <= '9' ? hex[1] - '0' : (hex[1] | 32) - 'a' + 10;
		if (*p != (unsigned char)(hi << 4 | lo)) return 0;
	}
	return 1;
}
static int write_code(unsigned char *p, const unsigned char *src, int n)
{
	DWORD old;
	if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
	for (int i = 0; i < n; i++) p[i] = src[i];
	VirtualProtect(p, n, old, &old);
	FlushInstructionCache(GetCurrentProcess(), p, n);
	return 1;
}
static int write_jmp(unsigned char *p, void *to, int total)
{
	unsigned char b[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
	*(int *)(b + 1) = (int)((unsigned char *)to - (p + 5));
	return write_code(p, b, total);
}
static void apply_cbp_exe_fixes(void)
{
	unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
	int g1 = 0, g2 = 0, g3 = 0, g4 = 0, g5 = 0, g7 = 0, h1 = 0;
	if (!build_is(exe, EXE_TIMESTAMP, EXE_IMAGESIZE)) { lstrcpyA(cbpfix_report, "unknown exe build: none applied"); return; }
	/* G1 Forage/Supply/Logistics raised Supply Wagon (and red Patriot) craft x(techs+1): je -> jmp */
	if (bytes_are(VA(0x609AB1), "8b81b802000083e04085c07419")) { unsigned char j = 0xEB; g1 = write_code(VA(0x609ABC), &j, 1); }
	/* G2 */
	if (bytes_are(VA(0x738F52), "6a00689f010000")) {
		rf_g2_back = VA(0x738F59); rf_g2_skip = VA(0x738F65);
		g2 = write_jmp(VA(0x738F52), (void *)g2_thunk, 7);
	}
	/* G3: the 4 bytes after the jmp (test/jne) become NOPs; nothing jumps into them */
	if (bytes_are(VA(0x6DC73C), "68130200008bcee8c8f4000085c0754b")) {
		rf_g3_owns = VA(0x6EBC10); rf_g3_power = VA(0x6E1370); rf_g3_find = VA(0x6EB930);
		rf_g3_keep = VA(0x6DC797); rf_g3_wgloop = VA(0x6DC74C);
		g3 = write_jmp(VA(0x6DC743), (void *)g3_thunk, 8);
		if (g3) { unsigned char nop = 0x90; g3 = write_code(VA(0x6DC74B), &nop, 1); }
	}
	/* G4 */
	if (bytes_are(VA(0x737398), "e8f350faff")) {
		rf_g4_orig = VA(0x6DC490); rf_g4_back = VA(0x73739D); rf_g4_flags = VA(0xE3A390);
		g4 = write_jmp(VA(0x737398), (void *)g4_thunk, 5);
	}
	/* G5 other players moving a capital used the skull notice (0x16D0) instead of capital_moved (0x16BC) */
	if (bytes_are(VA(0x624A9D), "05d0160000")) { unsigned char b = 0xBC; g5 = write_code(VA(0x624A9E), &b, 1); }
	/* G7 */
	if (bytes_are(VA(0x5AD603), "e8f8801000")) {
		rf_g7_orig = VA(0x6B5700); rf_g7_back = VA(0x5AD608); rf_g7_ptr = (unsigned char **)VA(0xC0620C);
		g7 = write_jmp(VA(0x5AD603), (void *)g7_thunk, 5);
	}
	/* H1: jmp + NOPs over the whole 10-byte push/push/push/call; only 0x627DBF is a jump target */
	if (bytes_are(VA(0x627DBF), "57ff75ac53e8b7b51000")) {
		rf_h1_players = VA(0xE3A390); rf_h1_team = VA(0x6EDB50); rf_h1_xfer = VA(0x733380); rf_h1_back = VA(0x627DC9);
		h1 = write_jmp(VA(0x627DBF), (void *)h1_thunk, 8);
		if (h1) { unsigned char nops[2] = {0x90, 0x90}; h1 = write_code(VA(0x627DC7), nops, 2); }
	}
	wsprintfA(cbpfix_report, "supply-wagon craft %s, Chinese assimilation %s, Persian capitals vs World Government %s, nomad attrition %s, "
	          "capital notice %s, start borders %s; RoN-Fix: ally returns a moved capital %s",
	          g1 ? "ok" : "NO", g2 ? "ok" : "NO", g3 ? "ok" : "NO", g4 ? "ok" : "NO", g5 ? "ok" : "NO", g7 ? "ok" : "NO", h1 ? "ok" : "NO");
}
#undef VA

/* ------------------------------------------------------------------ gameplay pack data check
 * The gameplay pack is only consistent when the data files the installer wrote are still there (Steam's
 * "Verify integrity" or an update puts the originals back). The installer records a fingerprint of
 * Data\balance.xml + Data\unitrules.xml in ronfix.ini ([gamefix] DataHash, FNV-1a 32-bit, hex); if the files
 * no longer match, every gameplay change is switched off (Version treated as 0) so this PC stays compatible
 * with plain RoN-Fix / RoN instead of announcing a pack it no longer has. */
static unsigned fnv1a_file(const char *path, unsigned h, int *ok)
{
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE) { *ok = 0; return h; }
	static unsigned char buf[2048]; DWORD n; /* static: no stack probe without the CRT; only called from DllMain */
	while (ReadFile(f, buf, sizeof buf, &n, NULL) && n)
		for (DWORD i = 0; i < n; i++) h = (h ^ buf[i]) * 16777619u;
	CloseHandle(f);
	return h;
}
static int gameplay_data_ok(const char *ini)
{
	char want[16], exe[MAX_PATH], path[MAX_PATH];
	GetPrivateProfileStringA("gamefix", "DataHash", "", want, sizeof want, ini);
	DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
	while (n > 0 && exe[n - 1] != '\\') n--;
	exe[n] = 0;
	unsigned h = 2166136261u; int ok = 1;
	lstrcpyA(path, exe); lstrcatA(path, "Data\\balance.xml"); h = fnv1a_file(path, h, &ok);
	lstrcpyA(path, exe); lstrcatA(path, "Data\\unitrules.xml"); h = fnv1a_file(path, h, &ok);
	char have[16]; wsprintfA(have, "%08x", h);
	if (ok && want[0] && lstrcmpiA(want, have) == 0) return 1;
	wsprintfA(gamefix_data_note, "data files do not match the gameplay pack (have %s, installer recorded %s)", have, want[0] ? want : "nothing");
	return 0;
}

/* ------------------------------------------------------------------ DllMain */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(inst);
	InitializeCriticalSection(&loglock);
	DWORD n = GetModuleFileNameA(inst, dir, MAX_PATH);
	while (n > 0 && dir[n - 1] != '\\')
		n--;
	if (n == 0 || n + 20 >= MAX_PATH)
		n = 0;
	dir[n] = 0;

	/* Startup-deadlock fix (diagnostics: deadlock dumps; reverse/netcode-map.md "Startup
	 * deadlock"). The FIRST BCryptGenRandom in a process sets up the RNG provider, loading a DLL
	 * while holding bcrypt's lock. PlayFab Party (PartyWin) makes that first call on a worker
	 * thread right as the main thread, holding the loader lock inside SteamAPI_Init's
	 * LoadLibrary(steamclient), makes its own: lock-order deadlock, the game never shows a window.
	 * We are a static import of the exe, so this runs during process start-up before any other
	 * thread can run: make that first call here, and every later caller finds it done. */
	if (n) {
		char ini[MAX_PATH];
		lstrcpyA(ini, dir);
		lstrcatA(ini, "ronfix.ini");
		if (GetPrivateProfileIntA("startup", "WarmCrypto", 1, ini)) {
			unsigned char rnd[16];
			warm_status = BCryptGenRandom(NULL, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
			warmed = 1;
		}
		/* Everything that changes the simulation hangs on one switch the installer sets together with the
		 * gameplay data, so a DLL alone never desyncs against players without the pack. */
		gamefix_version = GetPrivateProfileIntA("gamefix", "Version", 0, ini);
		if (gamefix_version > 0 && !gameplay_data_ok(ini))
			gamefix_version = -gamefix_version; /* remembered for the log, treated as off */
		if (gamefix_version > 0 && GetPrivateProfileIntA("gamefix", "MaskDamageFix", 1, ini))
			apply_mask_fix();
		if (gamefix_version > 0)
			apply_cbp_exe_fixes();
	}
	return TRUE; /* files are opened later, outside the loader lock */
}

#include "turndelay.c" /* shorter command delay: all players agree through PlayFab member data */
#include "lobbytag.c"  /* who in the lobby has RoN-Fix, from the same member data */
#include "lobbyline.c" /* ... and those lines in the lobby's chat list, on this PC only */
#include "gameswitch.c" /* the gameplay pack per match: loaded at session set-up when every player has it */
