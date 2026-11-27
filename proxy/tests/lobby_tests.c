/* Unit tests for netproxy v9 lobby logging (included by thunk_test.c after netproxy.c and check()).
 * Fake PlayFab state changes / PFLobbyPostUpdate arguments, a fake ConnectionData slot table, fake
 * CrossplayNetLibSys and players. Every log line is captured (RONFIX_TEST) and searched. */

static char cap[24576];
static int cap_n;
static void test_capture(const char *line, int n)
{
	volatile char *d = cap; /* volatile: no memcpy (no CRT) */
	for (int i = 0; i < n && cap_n < (int)sizeof cap - 1; i++) d[cap_n++] = line[i];
	cap[cap_n] = 0;
}
static void cap_clear(void) { cap_n = 0; cap[0] = 0; }
static int count_of(const char *needle)
{
	int c = 0;
	for (const char *h = cap; *h; h++) {
		const char *a = h, *b = needle;
		while (*a && *b && *a == *b) a++, b++;
		if (!*b) c++;
	}
	return c;
}
static void zero(void *p, int n) { volatile unsigned char *q = p; for (int i = 0; i < n; i++) q[i] = 0; }

/* MSVC std::wstring: <= 7 chars inline, else heap pointer; size +16, capacity +20 */
static void mk_wstr(unsigned char *at, const wchar_t *text, unsigned short *heap)
{
	unsigned n = 0, i;
	const volatile wchar_t *tv = text; /* volatile: no wcslen (no CRT) */
	while (tv[n]) n++;
	if (n <= 7) {
		unsigned short *w = (unsigned short *)at;
		for (i = 0; i < n; i++) w[i] = (unsigned short)tv[i];
		w[n] = 0;
		*(unsigned *)(at + 20) = 7;
	} else {
		for (i = 0; i < n; i++) heap[i] = (unsigned short)tv[i];
		heap[n] = 0;
		*(unsigned short **)at = heap;
		*(unsigned *)(at + 20) = n | 15;
	}
	*(unsigned *)(at + 16) = n;
}
/* the game's string type: direct (flags bit 0) or through a pointer to the buffer pointer */
static void mk_refstr(unsigned char *at, const wchar_t *text, const wchar_t **indirect)
{
	unsigned n = 0;
	while (text[n]) n++;
	if (indirect) { *indirect = text; *(const wchar_t ***)at = indirect; at[10] = 0; }
	else { *(const wchar_t **)at = text; at[10] = 1; }
	*(unsigned short *)(at + 6) = 0;
	*(unsigned short *)(at + 8) = (unsigned short)n;
}

static unsigned char ns[0x300], peerA[0xb0], peerB[0xb0];
static unsigned short idA[20], idB[20];
static const wchar_t *nameB_ptr;
static unsigned char tbl[8 * SLOT_SIZE];
static unsigned short sid0[20], sid1[20], sid2[20];

static void set_list(int n, unsigned char *p0, unsigned char *p1)
{
	*(int *)(ns + 4) = n;
	*(unsigned char **)(ns + 8) = p0;
	*(unsigned char **)(ns + 12) = p1;
}
static void set_slot(int i, const wchar_t *name, const wchar_t *id, unsigned short *heap, unsigned char ready)
{
	unsigned char *s = tbl + i * SLOT_SIZE;
	zero(s, SLOT_SIZE);
	if (name) {
		mk_wstr(s, name, 0);
		mk_wstr(s + 0x18, id, heap);
		s[0x34] = 1;
		s[SLOT_READY_OFF] = ready;
	} else
		s[0x34] = 4; /* delete_player: memset 0, [0x34] = 4 */
}

static void test_lobby_slots(void)
{
	DWORD t = 1000;
	lobby_log = 1;
	zero(seen, sizeof seen); zero(ghosts, sizeof ghosts); zero(slots, sizeof slots); slots_valid = 0;
	zero(ns, sizeof ns); zero(peerA, sizeof peerA); zero(peerB, sizeof peerB);
	mk_wstr(peerA + PEER_ID, L"AAAA1111AAAA1111", idA);
	mk_refstr(peerA + PEER_NAME, L"Hosty", 0);
	*(unsigned *)(peerA + PEER_FLAGS) = 5; /* host + this PC */
	mk_wstr(peerB + PEER_ID, L"BBBB2222BBBB2222", idB);
	mk_refstr(peerB + PEER_NAME, L"Bob", &nameB_ptr); /* indirect form */
	*(unsigned *)(peerB + PEER_FLAGS) = 2;
	*(void **)(peerB + PEER_LINK) = (void *)1;
	*(unsigned char *)(peerB + PEER_READY) = 1;
	*(unsigned char **)(ns + NETSYS_LOCAL) = peerA;
	set_list(2, peerA, peerB);
	cap_clear();
	peer_scan(ns, t);
	check(count_of("NET + player joined") == 2, "lobby: two players join the network list");
	check(count_of("Hosty | id AAAA1111AAAA1111") == 1 && count_of("(this PC) (host)") == 1, "lobby: name (direct string) and entity id read");
	check(count_of("Bob | id BBBB2222BBBB2222") == 1, "lobby: name read through the indirect string form");

	set_slot(0, L"Hosty", L"AAAA1111AAAA1111", sid0, 0);
	set_slot(1, L"Bob", L"BBBB2222BBBB2222", sid1, 0);
	for (int i = 2; i < 8; i++) set_slot(i, 0, 0, 0, 0);
	cap_clear();
	slot_scan(tbl, t);
	check(count_of("(as found)") == 2, "lobby: occupied slots logged once as found");
	cap_clear();
	slot_scan(tbl, t + 50);
	check(cap_n == 0, "lobby: nothing logged when nothing changed");

	/* 7 ready changes in 5 s: no flood; the 8th: one flood line; the 9th: none again */
	cap_clear();
	for (int k = 1; k <= 9; k++) {
		t += 300;
		tbl[1 * SLOT_SIZE + SLOT_READY_OFF] = (unsigned char)(k & 1);
		slot_scan(tbl, t);
		if (k == 7) check(count_of("FLOOD") == 0, "lobby: 7 ready changes in 5 s are not a flood");
	}
	check(count_of("LOBBY FLOOD slot 1 (Bob): ready flag changed 8 times") == 1 && count_of("FLOOD") == 1,
	      "lobby: 8 ready changes in 5 s = one flood line");
	check(count_of("[ready 0->1]") == 5 && count_of("[ready 1->0]") == 4, "lobby: each ready change logged");
	check(count_of("LOBBY slot 1: Bob ('Bob' 'BBBB2222BBBB2222')") == 9, "lobby: slot lines name the player");

	/* the host's start countdown (ready 2..12, here faster than the real 1/s) is not a flood */
	cap_clear();
	t += 20000;
	for (int k = 2; k <= 12; k++) { t += 300; tbl[1 * SLOT_SIZE + SLOT_READY_OFF] = (unsigned char)k; slot_scan(tbl, t); }
	check(count_of("[ready 2->3]") == 1 && count_of("FLOOD") == 0, "lobby: start countdown 2..12 is not counted as a flood");
	tbl[1 * SLOT_SIZE + SLOT_READY_OFF] = 1;
	slot_scan(tbl, t + 100);

	/* ready changes spaced 1 s apart never flood */
	cap_clear();
	t += 20000;
	for (int k = 0; k < 15; k++) { t += 750; tbl[1 * SLOT_SIZE + SLOT_READY_OFF] ^= 1; slot_scan(tbl, t); }
	check(count_of("FLOOD") == 0, "lobby: 7 ready changes per 5 s (one per 0.75 s) are not a flood");
	check(count_of("slot 1: 4 more changes in the last 10 s not logged") == 1, "lobby: change lines capped at 10 per 10 s");

	/* host boots Bob: slot cleared while he stays in the network list */
	cap_clear();
	t += 5000;
	set_slot(1, 0, 0, 0, 0);
	slot_scan(tbl, t);
	check(count_of("[cleared]") == 1 && count_of("slot 1 cleared (boot or leave) while Bob is still connected") == 1,
	      "lobby: cleared slot with the player still connected is watched");
	ghost_check(t + 9000);
	check(count_of("GHOST") == 0, "lobby: no ghost warning before 10 s");
	ghost_check(t + 10500);
	ghost_check(t + 12000);
	check(count_of("LOBBY GHOST? Bob is still connected 10 s after slot 1 was cleared") == 1, "lobby: one ghost warning after 10 s");
	set_list(1, peerA, 0);
	peer_scan(ns, t + 15000);
	check(count_of("NET - player left the network list: Bob") == 1 && count_of("LOBBY Bob left the network 15000 ms after slot 1 was cleared (was a ghost") == 1,
	      "lobby: ghost leaving is logged with the time since the boot");

	/* Bob rejoins, is cleared again and re-takes his slot (stock client re-asserting its keys) */
	cap_clear();
	t += 30000;
	set_list(2, peerA, peerB);
	peer_scan(ns, t);
	set_slot(1, L"Bob", L"BBBB2222BBBB2222", sid1, 1);
	slot_scan(tbl, t);
	set_slot(1, 0, 0, 0, 0);
	slot_scan(tbl, t + 100);
	set_slot(1, L"Bob", L"BBBB2222BBBB2222", sid1, 1);
	slot_scan(tbl, t + 400);
	check(count_of("LOBBY Bob is back in slot 1 300 ms after slot 1 was cleared") == 1, "lobby: player re-taking a cleared slot is logged");
	ghost_check(t + 20000);
	check(count_of("GHOST") == 0, "lobby: no ghost warning once he is back in a slot");

	/* a slot cleared for a player who already left */
	cap_clear();
	set_slot(2, L"Zed", L"ZZZZ3333ZZZZ3333", sid2, 0);
	slot_scan(tbl, t + 500);
	set_slot(2, 0, 0, 0, 0);
	slot_scan(tbl, t + 600);
	check(count_of("slot 2 cleared; Zed is not in the network list") == 1, "lobby: cleared slot of a player who is gone");

	cap_clear();
	*(unsigned *)(peerB + PEER_PULSE) = t - 2500;
	lobby_status(ns, 1, t, "test");
	check(count_of("[Hosty: this PC, host]") == 1 && count_of("[Bob: pulse 2500 ms ago, connected]") == 1, "lobby: status line with pulse age");
}

/* ---- PlayFab */
static const char *const k_ready3[] = {"steam_ready_3"};
static const char *const k_ready0[] = {"steam_ready_0", "team_0"};
static const char *const v_ready0[] = {"1", "2"};
struct pf_key { const char *id, *type; };
struct pf_summary { struct pf_key member; unsigned char conn, pad[3]; unsigned n; const char *const *keys; };
struct pf_updated { unsigned type; void *lobby; unsigned char owner, maxm, access, lock; unsigned nsearch; const char *const *skeys;
	unsigned nlobby; const char *const *lkeys; unsigned nmem; const struct pf_summary *mem; };
struct pf_added { unsigned type; void *lobby; struct pf_key member; };
struct pf_removed { unsigned type; void *lobby; struct pf_key member; unsigned reason; };
struct pf_postdone { unsigned type; long result; void *lobby; struct pf_key user; void *ctx; };
static struct pf_added ch_add = {2, (void *)0x5000, {"CCCC4444CCCC4444", "title_player_account"}};
static struct pf_removed ch_rem = {4, (void *)0x5000, {"CCCC4444CCCC4444", "title_player_account"}, 2};
static const void *ch_list[2] = {&ch_add, &ch_rem};
static unsigned fake_start_calls;
static long __stdcall fake_start(void *h, unsigned *count, const void *const **changes)
{
	fake_start_calls++;
	*count = h == (void *)0x77 ? 2 : 0;
	*changes = ch_list;
	return 0x0;
}
static void *post_args[5];
static long __stdcall fake_post(void *a, const void *b, const void *c, const void *d, void *e)
{
	post_args[0] = a; post_args[1] = (void *)b; post_args[2] = (void *)c; post_args[3] = (void *)d; post_args[4] = e;
	return 0x11;
}
static long __stdcall fake_remove(void *a, const void *b, int c, void *d) { (void)a; (void)b; (void)d; return c & 0xff ? 0 : -1; }
static DWORD __stdcall fake_tick(void) { return 12345; }

static void test_lobby_pf(void)
{
	DWORD t = 50000;
	zero(pf_h, sizeof pf_h); pf_next_id = 0; zero(pf_mem, sizeof pf_mem); pf_win = 0; pf_lines = pf_hidden = 0;
	cap_clear();
	pf_log_change((const unsigned char *)&ch_add, t);
	check(count_of("PF lobby #1: handle 00005000 (first seen in member added)") == 1 &&
	      count_of("PF lobby #1: member added CCCC4444CCCC4444 (title_player_account)") == 1, "pf: member added with entity key");
	pf_log_change((const unsigned char *)&ch_rem, t);
	check(count_of("member removed CCCC4444CCCC4444 (title_player_account), reason 2 = they left or were removed") == 1 &&
	      count_of("handle") == 1, "pf: member removed with reason, same lobby number");

	struct pf_summary sum = {{"DDDD5555DDDD5555", "title_player_account"}, 0, {0}, 1, k_ready3};
	struct pf_updated up = {7, (void *)0x5000, 0, 0, 0, 1, 0, 0, 0, 0, 1, &sum};
	cap_clear();
	for (int k = 1; k <= 9; k++) { t += 400; pf_log_change((const unsigned char *)&up, t); }
	check(count_of("PF lobby #1 updated: membership-lock | member DDDD5555DDDD5555: steam_ready_3") == 9, "pf: member update lists member and keys");
	check(count_of("PF FLOOD member DDDD5555DDDD5555: 8 updates in 5 s (last keys: steam_ready_3)") == 1 && count_of("FLOOD") == 1,
	      "pf: 8 member updates in 5 s = one flood line");
	sum.conn = 1;
	cap_clear();
	pf_log_change((const unsigned char *)&up, t + 100000);
	check(count_of("member DDDD5555DDDD5555 connection status changed") == 1, "pf: connection status change always logged");

	struct pf_postdone pd = {8, (long)0x80070005, (void *)0x6000, {"EEEE", "title_player_account"}, 0};
	cap_clear();
	pf_log_change((const unsigned char *)&pd, t);
	check(count_of("PF lobby #2: handle 00006000") == 1 && count_of("PF our update to lobby #2 FAILED: result 80070005") == 1,
	      "pf: failed update of ours logged, second lobby handle numbered");
	pd.result = 0;
	cap_clear();
	pf_log_change((const unsigned char *)&pd, t);
	check(cap_n == 0, "pf: successful update completions only counted");

	/* wrappers pass everything through */
	pf_start_orig = fake_start;
	unsigned cnt = 9;
	const void *const *arr = 0;
	cap_clear();
	long hr = w_pf_start((void *)0x77, &cnt, &arr);
	check(hr == 0 && cnt == 2 && arr == ch_list && count_of("member added") == 1 && count_of("member removed") == 1, "pf: state-change wrapper passes through and logs");
	pf_post_orig = fake_post;
	struct pf_key me = {"FFFF6666FFFF6666", "title_player_account"};
	struct { unsigned n; const char *const *k, *const *v; } mu = {2, k_ready0, v_ready0};
	cap_clear();
	hr = w_pf_post((void *)0x5000, &me, 0, &mu, (void *)0x99);
	check(hr == 0x11 && post_args[0] == (void *)0x5000 && post_args[1] == &me && post_args[2] == 0 && post_args[3] == &mu && post_args[4] == (void *)0x99,
	      "pf: post wrapper passes all 5 arguments and the result");
	check(count_of("PF local user: entity id FFFF6666FFFF6666, entity type 'title_player_account'") == 1 &&
	      count_of("PF post to lobby #1: lobby {} member {steam_ready_0=1,team_0=2} -> 00000011") == 1, "pf: post logs local user once and keys=values");
	w_pf_post((void *)0x5000, &me, 0, &mu, 0);
	check(count_of("PF local user") == 1, "pf: local user logged only when it changes");
	pf_remove_orig = fake_remove;
	cap_clear();
	hr = w_pf_remove((void *)0x5000, &me, 1, 0);
	check(hr == 0 && count_of("PF force-remove FFFF6666FFFF6666 (title_player_account) from lobby #1, prevent rejoin 1 -> 00000000") == 1,
	      "pf: force-remove wrapper passes the bool and logs");

	/* IAT hook on this test DLL's own import of GetTickCount */
	extern IMAGE_DOS_HEADER __ImageBase;
	HMODULE me_mod = (HMODULE)&__ImageBase;
	void *orig = 0, *dummy = 0;
	check(iat_slot(me_mod, "kernel32.dll", "NoSuchFunction") == 0 && iat_slot(me_mod, "nosuch.dll", "GetTickCount") == 0, "iat: unknown names not found");
	int ok = iat_hook(me_mod, "KERNEL32.dll", "GetTickCount", (void *)fake_tick, &orig);
	DWORD v = GetTickCount();
	int ok2 = iat_hook(me_mod, "KERNEL32.dll", "GetTickCount", orig, &dummy);
	check(ok && ok2 && v == 12345 && dummy == (void *)fake_tick && GetTickCount() != 12345, "iat: hook redirects the import and restores");
}

static int fake_pop_calls;
static __attribute__((thiscall)) int fake_pop(void *self, void *buf, void **sender, unsigned *len)
{
	(void)self;
	unsigned char *b = buf;
	fake_pop_calls++;
	b[0] = 0x0A; b[1] = 3; b[2] = b[3] = b[4] = 0;
	*sender = peerB;
	*len = 5;
	return 0x101; /* AL = 1 */
}
static void test_lobby_barriers(void)
{
	lobby_log = 1;
	bar_last = 0;
	cap_clear();
	bar_ours(100000);
	bar_arrival(2, 0, "Bob", 100400);
	bar_arrival(2, 0, "Bob", 101000);
	bar_ours(102000);
	check(count_of("LOAD sync point 1 reached by this PC") == 1 && count_of("LOAD sync point 1: player 2 (Bob) arrived 400 ms after this PC") == 1,
	      "load: arrival after this PC timed");
	check(count_of("LOAD sync point 2: player 2 (Bob) arrived before this PC") == 1 && count_of("sync point 2 reached by this PC, 2000 ms after sync point 1") == 1,
	      "load: arrival before this PC and our own interval");
	cap_clear();
	bar_ours(102000 + 400000);
	check(count_of("LOAD sync point 1 reached by this PC") == 1, "load: numbering restarts for a new loading sequence");
	orig_vt[23] = (void *)fake_pop;
	unsigned char buf[16];
	void *sender = 0;
	unsigned len = 0;
	cap_clear();
	int r = w_pop(ns, buf, &sender, &len);
	check((r & 0xff) == 1 && sender == peerB && len == 5 && count_of("player 3 (Bob) arrived") == 1, "load: slot-23 wrapper passes through and logs a sync packet");
}

/* telemetry against a fake TurnControl block: exe_base is pointed so that TC(0xE863D0) lands in tcmem */
static unsigned tcmem[0x200 / 4];
static void tc_set(unsigned va, unsigned v) { tcmem[(va - 0xE863D0) / 4] = v; }
static void test_telemetry(void)
{
	unsigned char *save_base = exe_base;
	int save_ok = exe_ok, save_int = tel_interval_ms;
	exe_base = (unsigned char *)tcmem - (0xE863D0 - 0x400000);
	exe_ok = 1; lobby_log = 1; tel_interval_ms = 10000; tel_next = 0; tel_have = 0; tel_frame_last = tel_tc_last = 0;
	tc_set(TC_TURN_LENGTH, 6); tc_set(TC_FRAME_MS, 67); tc_set(TC_SPEED, 2); tc_set(TC_FLOOR, 400);
	tc_set(TC_PING + 4, 180);
	cap_clear();
	for (unsigned f = 100; f <= 170; f++) {          /* 71 frames, turn_counter 0..6 as measured live */
		tc_set(TC_FRAME_COUNTER, f); tc_set(TC_TURN_COUNTER, f % 7);
		if (f == 170) tel_next = GetTickCount() - 1;
		telemetry_tick(ns);
		telemetry_tick(ns);                           /* same frame twice: counted once */
	}
	check(count_of("restarted") == 0, "telemetry: no restart lines while frames advance (v8 bug: one per turn)");
	check(count_of("NET frames 100-170 (10 turns): delay 402-402 ms (avg 402, floor 400)") == 1 && count_of("p1 180/") == 1,
	      "telemetry: frames, turns (turn_counter wraps) and delay");
	cap_clear();
	tc_set(TC_FRAME_COUNTER, 3); tc_set(TC_TURN_COUNTER, 3);
	telemetry_tick(ns);
	check(count_of("NET frame counter restarted (new game)") == 1, "telemetry: a new game is noticed once");
	exe_base = save_base; exe_ok = save_ok; tel_interval_ms = save_int;
}

static void test_lobby(void)
{
	test_lobby_slots();
	test_lobby_pf();
	test_lobby_barriers();
	test_telemetry();
}
