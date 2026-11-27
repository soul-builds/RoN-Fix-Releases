/* ------------------------------------------------------------------ shorter command delay (core v13), ALL OR NOBODY
 * Included at the end of netproxy.c. The game's lockstep turn length is recomputed by EVERY client, once per turn,
 * from the per-turn reports all of them receive (tune_turn_length exe 0x955880):
 *     worst = max(200, ping of every player who reported this turn)
 *     turn ms = min(max(2 x worst, floor), 1000)
 * with floor = clamp(80 x human players, 400, 600) set at match start (turncontrol_start 0x957e20). The 200 ms
 * starting value alone keeps every turn at 400 ms or more, so 2 players never get under 400 ms and 8 players never
 * under 600 ms, even on a LAN (measured 2026-11-27: floor patched to 200 alone still gave 402 ms turns). Both must
 * come down: the floor to TurnFloorMin..TurnFloorMax and the starting ping to TurnFloorMin / 2, so the floor is the
 * real limit; 2 x the worst REAL ping still lengthens turns for whoever has a slow link.
 * All clients must use the same floor, or their turn lengths (and so lockstep) diverge. Negotiation, all in PlayFab
 * MEMBER data (each player writes only its own, every member reads everyone's):
 *   1. every RoN-Fix player (v13+, patch site as expected) adds ronfix_turn to its own member posts in the game
 *      lobby: "2" if its ronfix.ini [netfix] ShortDelay=1, else "1";
 *   2. every client decides on its own, from data all of them see: ON if the lobby owner (the host) says "2" and
 *      every lobby member AND every human in the game's slot table says "1" or "2". A player without RoN-Fix 0.11
 *      never says anything, so nobody turns it on;
 *   3. the decision is re-taken every 250 ms while the lobby window is open and applied at once (the floor is only
 *      read when the match starts). It is frozen once the host's start countdown runs (membership is locked then),
 *      so a late change can't split the lobby. It is undone when the next lobby window opens.
 * Why member data and not lobby data: PlayFab caps all lobby data at 4096 bytes and fails the whole update if a
 * post goes over, the game already packs its settings into that data, and CrossplayProxy loads EVERY lobby key into
 * the map it rebuilds the settings from. Member keys are read by name, plus keys starting with '_' (CrossplayProxy,
 * reverse/out/CrossplayProxy.named.c around line 12843), so "ronfix_turn" is invisible to the game on every PC,
 * with or without RoN-Fix. The game uses 14 of the 30 member properties PlayFab allows per player.
 * Patch: the immediates of `mov dword [esp+0x1c], 200` (0x955896, tune_turn_length's starting ping) and of
 * `mov ecx, 400` (0x958175) / `mov eax, 600` (0x9581c7) in turncontrol_start. Nothing else in the simulation changes. */

#define TD_KEY "ronfix_turn"
#define TD_VA_MIN 0x958175 /* b9 90 01 00 00   mov ecx, 400 */
#define TD_VA_MAX 0x9581c7 /* b8 58 02 00 00   mov eax, 600 */
#define TD_VA_PING 0x955896 /* c7 44 24 1c c8 00 00 00   mov dword [esp+0x1c], 200 */
#define TD_MAX_GAME_KEYS 24 /* never push a member post towards PlayFab's 30-property limit */

static int td_host_wants;                   /* [netfix] ShortDelay: we post "2" (0 = we post "1") */
static unsigned td_min = 200, td_max = 300; /* [netfix] TurnFloorMin / TurnFloorMax */
static int td_active;                       /* the floor is patched right now */
static int td_decided = -1;                 /* decision in this lobby window: 1 on, 0 off, -1 not taken yet */
static void *td_lobby;                      /* the game lobby: where our member posts with slot keys went */
static unsigned char *td_code;              /* exe base for the patch (tests point it at a buffer) */
static int td_capable;                      /* the patch site holds the expected code: only then do we take part */
static int td_frozen;                       /* start countdown running: the decision no longer changes */
static DWORD td_last_read;
static char td_why[128];                    /* why the current decision is off */

/* at start-up: is turncontrol_start the code we expect? Only then may this PC take part. */
static void td_init(unsigned char *exe)
{
	td_code = exe;
	unsigned char *a = exe + (TD_VA_MIN - 0x400000u), *b = exe + (TD_VA_MAX - 0x400000u), *c = exe + (TD_VA_PING - 0x400000u);
	unsigned char ca[5], cb[5], cc[8];
	static const unsigned char wa[5] = { 0xb9, 0x90, 0x01, 0x00, 0x00 }, wb[5] = { 0xb8, 0x58, 0x02, 0x00, 0x00 },
	                           wc[8] = { 0xc7, 0x44, 0x24, 0x1c, 0xc8, 0x00, 0x00, 0x00 };
	td_capable = safe_read(a, ca, 5) && safe_read(b, cb, 5) && safe_read(c, cc, 8) && bytes_eq(ca, wa, 5) && bytes_eq(cb, wb, 5) &&
	             bytes_eq(cc, wc, 8);
	if (td_min < 100 || td_min > 400)
		td_min = 200;
	if (td_max < td_min || td_max > 600)
		td_max = td_min > 300 ? td_min : 300;
}

typedef long(__stdcall *pf_get_member_prop_fn)(void *, const void *, const char *, const char **);
typedef long(__stdcall *pf_get_members_fn)(void *, unsigned *, const void **);
typedef long(__stdcall *pf_get_owner_fn)(void *, const void **);
static pf_get_member_prop_fn td_get_member_prop;
static pf_get_members_fn td_get_members;
static pf_get_owner_fn td_get_owner;

static int td_api(void)
{
#ifndef RONFIX_TEST
	if (!td_get_owner) {
		HMODULE pf = GetModuleHandleA("PlayFabMultiplayerWin.dll");
		if (!pf)
			return 0;
		td_get_member_prop = (pf_get_member_prop_fn)GetProcAddress(pf, "PFLobbyGetMemberProperty");
		td_get_members = (pf_get_members_fn)GetProcAddress(pf, "PFLobbyGetMembers");
		td_get_owner = (pf_get_owner_fn)GetProcAddress(pf, "PFLobbyGetOwner");
	}
#endif
	return td_get_member_prop && td_get_members && td_get_owner;
}

static int td_setup_open(void)
{
	void *setup = 0;
	return exe_base && safe_read(exe_base + RVA_SETUPWIN_PTR, &setup, 4) && setup;
}

/* PFLobbyPostUpdate's member data, rewritten in place of the game's own (w_pf_post). Static copies: PlayFab copies
 * everything during the call. The lobby's own data is never touched. */
static const char *td_mkeys[64], *td_mvals[64];
static unsigned td_mu[3];

static void td_rewrite(void *lobby, const void **mu)
{
	unsigned n;
	if (!td_capable || !*mu || !td_setup_open() || !safe_read(*mu, td_mu, 12) || (n = td_mu[0]) == 0 || n > TD_MAX_GAME_KEYS)
		return;
	const char *const *k = (const char *const *)td_mu[1], *const *v = (const char *const *)td_mu[2];
	int has = 0, game = 0;
	for (unsigned i = 0; i < n; i++) {
		const char *kp = 0, *vp = 0;
		char kk[24];
		if (!safe_read(k + i, &kp, 4) || !safe_read(v + i, &vp, 4))
			return;
		td_mkeys[i] = kp, td_mvals[i] = vp;
		read_cstr(kp, kk, sizeof kk);
		has |= !lstrcmpA(kk, TD_KEY);
		game |= kk[0] == '_'; /* slot keys (_steam_ready_0, _team_0, ...): the game lobby, not the browser's chat */
	}
	if (!game)
		return;
	td_lobby = lobby;
	if (has)
		return;
	td_mkeys[n] = TD_KEY, td_mvals[n] = td_host_wants ? "2" : "1";
	td_mu[0] = n + 1, td_mu[1] = (unsigned)td_mkeys, td_mu[2] = (unsigned)td_mvals;
	*mu = td_mu;
}

/* PlayFab told us this lobby is gone (left / disconnected): its handle must never be used again */
static void td_on_lobby_gone(void *lobby)
{
	if (lobby == td_lobby)
		td_lobby = 0;
}

/* short = 1: floor td_min..td_max and starting ping td_min / 2; 0: the game's own 400..600 and 200 */
static int td_write_floor(int short_delay)
{
	unsigned lo = short_delay ? td_min : 400, hi = short_delay ? td_max : 600, ping = short_delay ? td_min / 2 : 200;
	if (!td_code)
		return 0;
	unsigned char *a = td_code + (TD_VA_MIN - 0x400000u), *b = td_code + (TD_VA_MAX - 0x400000u), *c = td_code + (TD_VA_PING - 0x400000u);
	if (a[0] != 0xb9 || b[0] != 0xb8 || c[0] != 0xc7 || c[3] != 0x1c)
		return 0; /* not the instructions we expect: leave the game alone */
	return write_code(c + 4, (const unsigned char *)&ping, 4) && write_code(a + 1, (const unsigned char *)&lo, 4) &&
	       write_code(b + 1, (const unsigned char *)&hi, 4);
}

/* a member's ronfix_turn: '1' / '2', or 0 = none (not a member, no RoN-Fix 0.11, or nothing posted yet) */
static char td_member_value(void *lobby, const void *key)
{
	const char *v = 0;
	char b[4];
	if (td_get_member_prop(lobby, key, TD_KEY, &v) < 0 || !v)
		return 0;
	read_cstr(v, b, sizeof b);
	return (b[0] == '1' || b[0] == '2') && !b[1] ? b[0] : 0;
}

/* the decision every client takes on its own, from data all of them see. why: at least 128 bytes. */
static int td_decide(void *lobby, char *why, int cap)
{
	unsigned n = 0;
	const unsigned char *members = 0, *owner = 0; /* PFEntityKey { const char *id, *type } */
	const char *p = 0;
	char id[40];
	why[0] = 0;
	if (!lobby || !td_api()) {
		lf_strcpy(why, "game lobby not known yet", cap);
		return 0;
	}
	if (!slots_valid) {
		lf_strcpy(why, "slot table not read yet", cap);
		return 0;
	}
	/* the host first: in a normal lobby that is the whole answer, two PlayFab reads */
	if (td_get_owner(lobby, (const void **)&owner) < 0 || !owner) {
		lf_strcpy(why, "the lobby has no owner right now", cap);
		return 0;
	}
	if (td_member_value(lobby, owner) != '2') {
		lf_strcpy(why, "the host has not turned it on", cap);
		return 0;
	}
	if (td_get_members(lobby, &n, (const void **)&members) < 0 || !members || n == 0 || n > 16) {
		lf_strcpy(why, "member list not readable", cap);
		return 0;
	}
	for (unsigned i = 0; i < n; i++) {
		const unsigned char *key = members + 8 * i;
		p = 0;
		safe_read(key, &p, 4);
		read_cstr(p, id, sizeof id);
		if (!td_member_value(lobby, key)) {
			wsprintfA(why, "%s has no RoN-Fix 0.11 (or has not said so yet)", id);
			return 0;
		}
	}
	/* every human in the game's own slot table must be one of those members (AI and open slots are empty) */
	for (int s = 0; s < 8; s++) {
		if (!slots[s].a[0] && !slots[s].b[0])
			continue;
		struct {
			const char *id, *type;
		} k = { slots[s].a, "title_player_account" };
		if (!slots[s].a[0] || !td_member_value(lobby, &k)) {
			wsprintfA(why, "slot %d (%s) is not a lobby member with RoN-Fix 0.11", s, slots[s].a[0] ? slots[s].a : slots[s].b);
			return 0;
		}
	}
	return 1;
}

/* every lobby tick (lobby_tick, after slot_scan) */
static void td_tick(int setup_open, int setup_closed_now, int setup_opened_now)
{
	if (setup_opened_now) {
		td_lobby = 0, td_decided = -1, td_frozen = 0, td_why[0] = 0;
		td_last_read = GetTickCount() - 1000;
	}
	if (td_active && setup_opened_now) {
		if (td_write_floor(0)) {
			td_active = 0;
			logline("NET command delay back to the game's own floor (400-600 ms)");
		} else
			logline("NET command delay: could NOT restore the game's own floor");
	}
	if (setup_closed_now) {
		td_frozen = 0;
		if (td_active)
			logline("NET command delay for this match: SHORT, turn floor %u-%u ms (the game's own: 400-600)", td_min, td_max);
		else if (td_decided >= 0)
			logline("NET command delay for this match: the game's own (400-600 ms): %s", td_why);
	}
	if (!setup_open)
		return;
	/* the host's start countdown runs the ready byte 2..12: membership is locked, the decision stays */
	int countdown = 0;
	for (int i = 0; i < 8; i++)
		countdown |= slots[i].v[10] >= 2 && (slots[i].a[0] || slots[i].b[0]);
	if (countdown != td_frozen) {
		td_frozen = countdown;
		if (countdown && td_decided >= 0)
			logline("NET start countdown: command delay %s for this match (final)", td_active ? "SHORT" : "the game's own");
	}
	if (!td_capable || (td_frozen && td_decided >= 0))
		return;
	DWORD now = GetTickCount();
	if (now - td_last_read < 250)
		return;
	td_last_read = now;
	char why[128];
	int want = td_decide(td_lobby ? td_lobby : lf_game_lobby, why, sizeof why);
	if (want != td_active) {
		if (td_write_floor(want))
			td_active = want;
		else
			logline("NET command delay: could NOT write the turn floor (%s)", want ? "short" : "game's own");
	}
	if (want != td_decided || (!want && lstrcmpA(why, td_why))) {
		if (want)
			logline("NET short command delay ON for this lobby: the host offers it and every player has RoN-Fix 0.11 "
			        "(turn floor %u-%u ms)", td_min, td_max);
		else
			logline("NET short command delay off for this lobby: %s", why);
		td_decided = want;
		lf_strcpy(td_why, why, sizeof td_why);
	}
	if (td_frozen && td_decided >= 0)
		logline("NET start countdown: command delay %s for this match (final)", td_active ? "SHORT" : "the game's own");
}
