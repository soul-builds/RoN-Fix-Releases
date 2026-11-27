/* ------------------------------------------------------------------ lobby fixes (core v12), HOST ONLY
 * Included by netproxy.c after the lobby logging (it reuses its slot table and peer list scans and its PlayFab
 * wrappers). reverse/lobby-boot-ban.md has the analysis: the game's "Boot Player" only clears the slot, the
 * booted player stays in the PlayFab lobby and the P2P network (the ghost), and nothing can ban anyone. The fix is
 * PlayFab's own PFLobbyForceRemoveMember(game lobby, member, preventRejoin = TRUE), which only the lobby owner (the
 * host) may call. The member-removed event then cleans the player out on every client through the game's
 * normal "member left" path, so only the host needs RoN-Fix for this.
 *   - Boot -> real kick: a slot cleared while that player is still connected, and not back in a slot 1.5 s later.
 *     Shift held while booting = ban as well (added to ronfix-bans.txt).
 *   - Ban list ronfix-bans.txt (game folder): PlayFab entity ids (16 hex) or Steam ID64s, '#' starts a comment.
 *     A listed player is removed as soon as PlayFab reports him joining (before he gets a slot), or when he shows
 *     up in the network list.
 *   - Ready spam: SpamKick ready toggles (0 <-> 1) within 5 s from one slot removes that player.
 * Acts only: as host, in the lobby (setup window open), never once the start countdown runs or the game started
 * (netsys flag 0x40), never on this PC's player, only through the game lobby's handle (the one we post lobby data
 * to as host / created), at most one removal per 500 ms. [lobby] Fixes=1 (0 = off), BootKick=1, SpamKick=12.
 * Calls happen on the main thread (check_pulse), where the game makes its own PlayFab calls. ctx = NULL: the
 * proxy's completion dispatch then finds no request of its own and skips it (CP 0x10031d.. case 5,
 * N_remove_list_node_locked returns 0). */

#define LF_BOOT_GRACE_MS 1500
#define LF_GAP_MS 500
#define LF_MAXBANS 256

static int lf_enabled = 1, lf_bootkick = 1, lf_spamkick = 12;
static void *lf_game_lobby;
static char lf_ban_path[MAX_PATH];
static char lf_bans[LF_MAXBANS][24];
static int lf_nbans;
static FILETIME lf_ban_time;
static DWORD lf_ban_check, lf_last_kick;

struct lf_kick {
	char id[24], name[48], why[64];
	int ban, done;
	DWORD at;
};
static struct lf_kick lf_q[8];
static char lf_kicked[16][24]; /* removed from this lobby already (PlayFab keeps them out) */
static int lf_nkicked;

static void lf_strcpy(char *d, const char *s, int cap)
{
	int i = 0;
	for (; s && s[i] && i < cap - 1; i++)
		d[i] = s[i];
	d[i] = 0;
}

static int lf_is_hex16(const char *s)
{
	int n = 0;
	for (; s[n]; n++)
		if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'A' && s[n] <= 'F') || (s[n] >= 'a' && s[n] <= 'f')))
			return 0;
	return n == 16;
}

static int lf_is_steamid(const char *s)
{
	int n = 0;
	for (; s[n]; n++)
		if (s[n] < '0' || s[n] > '9')
			return 0;
	return n == 17 && s[0] == '7' && s[1] == '6';
}

static void lf_bans_load(DWORD now)
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!lf_ban_path[0] || (lf_ban_check && now - lf_ban_check < 5000))
		return;
	lf_ban_check = now;
	if (!GetFileAttributesExA(lf_ban_path, GetFileExInfoStandard, &a)) {
		lf_nbans = 0;
		return;
	}
	if (lf_nbans && a.ftLastWriteTime.dwLowDateTime == lf_ban_time.dwLowDateTime && a.ftLastWriteTime.dwHighDateTime == lf_ban_time.dwHighDateTime)
		return;
	lf_ban_time = a.ftLastWriteTime;
	static char buf[16384];
	DWORD got = 0;
	HANDLE f = CreateFileA(lf_ban_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	ReadFile(f, buf, sizeof buf - 1, &got, NULL);
	CloseHandle(f);
	buf[got] = 0;
	lf_nbans = 0;
	/* every id on a line counts (a line may hold the entity id AND the Steam ID); '#' to the end of the line is a
	 * comment */
	for (char *p = buf; *p && lf_nbans < LF_MAXBANS;) {
		char w[32];
		int n = 0;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (*p == '#') {
			while (*p && *p != '\n')
				p++;
			continue;
		}
		while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '#') {
			if (n < 31)
				w[n++] = *p;
			p++;
		}
		w[n] = 0;
		if (n && (lf_is_hex16(w) || lf_is_steamid(w)))
			lf_strcpy(lf_bans[lf_nbans++], w, 24);
	}
	logline("LOBBY ban list: %d entr%s from ronfix-bans.txt", lf_nbans, lf_nbans == 1 ? "y" : "ies");
}

static int lf_banned(const char *id)
{
	for (int k = 0; id && id[0] && k < lf_nbans; k++)
		if (!lstrcmpiA(id, lf_bans[k]))
			return 1;
	return 0;
}

static void lf_ban_add(const char *entity, const char *steam, const char *name)
{
	char line[200];
	SYSTEMTIME t;
	DWORD w;
	GetLocalTime(&t);
	if (!lf_ban_path[0])
		return;
	int n = wsprintfA(line, "%s%s%s  # %s, banned %04u-%02u-%02u\r\n", entity, steam[0] ? " " : "", steam, name, t.wYear, t.wMonth, t.wDay);
	HANDLE f = CreateFileA(lf_ban_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	WriteFile(f, line, (DWORD)n, &w, NULL);
	CloseHandle(f);
	if (lf_nbans < LF_MAXBANS && entity[0])
		lf_strcpy(lf_bans[lf_nbans++], entity, 24);
	if (lf_nbans < LF_MAXBANS && steam[0])
		lf_strcpy(lf_bans[lf_nbans++], steam, 24);
	logline("LOBBY ban list: %s added", name);
}

static int lf_was_kicked(const char *id)
{
	for (int k = 0; k < lf_nkicked && k < 16; k++)
		if (!lstrcmpiA(lf_kicked[k], id))
			return 1;
	return 0;
}

/* queue a removal (entity id = 16 hex PlayFab id) */
static void lf_kick(const char *id, const char *name, const char *why, int ban)
{
	if (!lf_enabled || !id || !lf_is_hex16(id) || !lstrcmpiA(id, pf_user) || lf_was_kicked(id))
		return;
	for (int k = 0; k < 8; k++)
		if (lf_q[k].id[0] && !lf_q[k].done && !lstrcmpiA(lf_q[k].id, id))
			return;
	for (int k = 0; k < 8; k++)
		if (!lf_q[k].id[0] || lf_q[k].done) {
			lf_strcpy(lf_q[k].id, id, 24);
			lf_strcpy(lf_q[k].name, name && name[0] ? name : id, 48);
			lf_strcpy(lf_q[k].why, why, 64);
			lf_q[k].ban = ban;
			lf_q[k].done = 0;
			return;
		}
}

/* the state the fixes may act in: host, lobby open, game not starting or running */
static int lf_may_act(void *self, const char **reason)
{
	unsigned flags = 0;
	void *setup = 0;
	if (!safe_read((char *)self + NETSYS_FLAGS, &flags, 4) || !(flags & 1)) {
		*reason = "not the host";
		return 0;
	}
	if (flags & 0x40) {
		*reason = "the game is running";
		return 0;
	}
	if (!safe_read(exe_base + RVA_SETUPWIN_PTR, &setup, 4) || !setup) {
		*reason = "no lobby open";
		return 0;
	}
	for (int i = 0; i < 8; i++) /* the host's start countdown counts the ready byte 2..12 */
		if (slots[i].v[10] >= 2 && (slots[i].a[0] || slots[i].b[0])) {
			*reason = "the game is starting";
			return 0;
		}
	if (!lf_game_lobby) {
		*reason = "game lobby handle not known yet";
		return 0;
	}
	return 1;
}

/* PlayFab events (from pf_log_change / pf_log_post, main thread) */
static void lf_on_lobby_created(void *lobby) { lf_game_lobby = lobby, lf_nkicked = 0; }
static void lf_on_lobby_data_post(void *lobby) /* only the owner writes lobby data */
{
	if (lobby != lf_game_lobby) {
		lf_game_lobby = lobby;
		lf_nkicked = 0;
	}
}
static void lf_on_lobby_gone(void *lobby)
{
	if (lobby == lf_game_lobby) {
		lf_game_lobby = 0;
		lf_nkicked = 0;
		for (int k = 0; k < 8; k++)
			lf_q[k].id[0] = 0;
	}
}
static void lf_on_member_added(void *lobby, const char *id)
{
	lf_bans_load(GetTickCount());
	if (lobby == lf_game_lobby && lf_banned(id))
		lf_kick(id, id, "on the ban list", 0);
}

/* slot table events (from slot_scan) */
static void lf_on_spam(const char *entity, const char *name, int flips)
{
	char why[64];
	if (lf_spamkick <= 0 || flips < lf_spamkick)
		return;
	wsprintfA(why, "ready toggled %d times in %d s", flips, FLOOD_WINDOW_MS / 1000);
	lf_kick(entity, name, why, 0);
}

/* every lobby tick (main thread) */
static void lf_tick(void *self, DWORD now)
{
	const char *reason = "";
	if (!lf_enabled)
		return;
	lf_bans_load(now);
	if (!lf_may_act(self, &reason)) {
		for (int k = 0; k < 8; k++)
			if (lf_q[k].id[0] && !lf_q[k].done) {
				logline("LOBBY not removing %s (%s): %s", lf_q[k].name, lf_q[k].why, reason);
				lf_q[k].done = 1;
			}
		return;
	}
	/* boot -> real kick: cleared slot, player still connected and not back in a slot */
	for (int g = 0; lf_bootkick && g < 8; g++) {
		struct ghost_watch *w = &ghosts[g];
		if (!w->peer || w->kick_queued || now - w->since < LF_BOOT_GRACE_MS)
			continue;
		char id[40];
		unsigned flags = 0;
		w->kick_queued = 1;
		if (!safe_read(w->peer + PEER_FLAGS, &flags, 4) || (flags & 4))
			continue;
		read_wstring(w->peer + PEER_ID, id, sizeof id);
		if (w->ban) {
			char steam[40];
			read_wstring(w->peer + 0x5c, steam, sizeof steam);
			lf_ban_add(id, lf_is_steamid(steam) ? steam : "", w->name);
		}
		lf_kick(id, w->name, w->ban ? "booted with Shift: banned" : "booted by the host", w->ban);
	}
	/* banned players already in the network list */
	for (int k = 0; lf_nbans && k < 8; k++) {
		char steam[40];
		unsigned flags = 0;
		if (!seen[k].ptr || !safe_read(seen[k].ptr + PEER_FLAGS, &flags, 4) || (flags & 4))
			continue;
		read_wstring(seen[k].ptr + 0x5c, steam, sizeof steam);
		if (lf_banned(seen[k].id) || lf_banned(steam))
			lf_kick(seen[k].id, seen[k].name, "on the ban list", 0);
	}
	/* one removal per LF_GAP_MS */
	if (now - lf_last_kick < LF_GAP_MS)
		return;
	for (int k = 0; k < 8; k++) {
		struct lf_kick *q = &lf_q[k];
		if (!q->id[0] || q->done)
			continue;
		static char kid[24];
		struct {
			const char *id, *type;
		} key = { kid, "title_player_account" };
		lf_strcpy(kid, q->id, 24);
		long hr = pf_remove_orig ? pf_remove_orig(lf_game_lobby, &key, 1, NULL) : -1;
		logline("LOBBY KICK %s (%s): removed from the PlayFab lobby, rejoin blocked -> %08lx", q->name, q->why, hr);
		q->done = 1;
		lf_last_kick = now;
		if (hr >= 0 && lf_nkicked < 16)
			lf_strcpy(lf_kicked[lf_nkicked++], q->id, 24);
		break;
	}
}
