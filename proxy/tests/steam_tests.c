/* Unit tests for steamlink.c (Steam link phase A, shadow mode). Included by thunk_test.c after lobby_tests.c
 * (uses its ns/peerA/peerB fakes and log capture). A fake ISteamNetworkingMessages records what is sent and hands
 * out messages laid out like SteamNetworkingMessage_t. */

static unsigned long long fk_to[64];
static unsigned char fk_buf[64][32];
static int fk_len[64], fk_flags[64], fk_n, fk_closed, fk_released;
static void *fk_queue[8];
static int fk_qn;
static int THISCALL fk_send(void *s, const struct sl_ident *id, const void *d, unsigned n, int fl, int ch)
{
	(void)s;
	if (fk_n < 64 && ch == SL_CHANNEL && id->type == 16 && id->size == 8) {
		volatile unsigned char *o = fk_buf[fk_n]; /* volatile: no memcpy (no CRT) */
		fk_to[fk_n] = id->u.sid;
		for (unsigned i = 0; i < n && i < 32; i++)
			o[i] = ((const unsigned char *)d)[i];
		fk_len[fk_n] = (int)n;
		fk_flags[fk_n++] = fl;
	}
	return 1;
}
static int THISCALL fk_recv(void *s, int ch, void **out, int max)
{
	(void)s;
	int n = 0;
	void *volatile *o = out;
	if (ch != SL_CHANNEL)
		return 0;
	for (; n < fk_qn && n < max; n++)
		o[n] = fk_queue[n];
	fk_qn = 0;
	return n;
}
static int THISCALL fk_close(void *s, const struct sl_ident *id)
{
	(void)s, (void)id;
	fk_closed++;
	return 1;
}
static int THISCALL fk_info(void *s, const struct sl_ident *id, void *info, void *st)
{
	(void)s, (void)id, (void)info;
	*(int *)st = 3;
	*(int *)((char *)st + 4) = 42;
	*(float *)((char *)st + 8) = 0.98f;
	*(float *)((char *)st + 12) = 1.0f;
	return 3;
}
static void __cdecl fk_release(void *m)
{
	(void)m;
	fk_released++;
}
static void *fk_vt[6] = { (void *)fk_send, (void *)fk_recv, (void *)fk_close, (void *)fk_close, (void *)fk_close, (void *)fk_info };
static void *fk_obj[1] = { fk_vt };
static unsigned char fk_msg[8][208], fk_data[8][32];

/* a message from Steam ID `from`: "RFX" 1 type + payload */
static void fk_push(unsigned long long from, int type, unsigned a, unsigned b, int ident_type)
{
	int k = fk_qn;
	unsigned char *m = fk_msg[k], *d = fk_data[k];
	zero(m, 208);
	d[0] = 'R', d[1] = 'F', d[2] = 'X', d[3] = 1, d[4] = (unsigned char)type;
	*(unsigned *)(d + 5) = a;
	*(unsigned *)(d + 9) = b;
	*(unsigned char **)(m + SLM_DATA) = d;
	*(int *)(m + SLM_SIZE) = 13;
	*(int *)(m + SLM_IDENT) = ident_type;
	*(int *)(m + SLM_IDENT + 4) = 8;
	*(unsigned long long *)(m + SLM_IDENT + 8) = from;
	*(void **)(m + SLM_RELEASE) = (void *)fk_release;
	*(int *)(m + SLM_CHANNEL) = SL_CHANNEL;
	fk_queue[fk_qn++] = m;
}

/* a pong as core v14 sends it: seq, time, then the player the sender waits for (-1 none) and for how long */
static void fk_push_pong4(unsigned long long from, unsigned seq, unsigned t, int wait, unsigned ms)
{
	int k = fk_qn;
	fk_push(from, SL_PONG, seq, t, 16);
	*(int *)(fk_data[k] + 13) = wait;
	*(unsigned *)(fk_data[k] + 17) = ms;
	*(int *)(fk_msg[k] + SLM_SIZE) = 21;
}
static int has(const char *h, const char *n)
{
	for (; *h; h++) {
		int i = 0;
		while (n[i] && h[i] == n[i])
			i++;
		if (!n[i])
			return 1;
	}
	return 0;
}

static int fk_last_type(void) { return fk_n ? fk_buf[fk_n - 1][4] : -1; }
static void tick_at(DWORD t)
{
	sl_test_now = t;
	sl_next = 0;
	steam_tick(ns);
}

static unsigned short sidB[24], sidA[24];
static const unsigned long long SID_B = 76561198000000002ull;

static void test_steam(void)
{
	unsigned long long v;
	check(sl_parse_sid("76561198000000002", &v) && v == SID_B && !sl_parse_sid("12345", &v) && !sl_parse_sid("7656119800000000x", &v) &&
	          !sl_parse_sid("", &v),
	      "steam: Steam ID64 parsed from the player list, junk refused");

	/* list: A = this PC, B = a remote player with a Steam ID */
	zero(ns, sizeof ns);
	zero(peerA, sizeof peerA);
	zero(peerB, sizeof peerB);
	*(unsigned *)(peerA + PEER_FLAGS) = 4;
	mk_wstr(peerA + 0x5c, L"76561198000000001", sidA);
	mk_refstr(peerB + PEER_NAME, L"Bravo", 0);
	mk_wstr(peerB + 0x5c, L"76561198000000002", sidB);
	set_list(1, peerA, 0);
	sl_msgs = fk_obj;
	sl_enabled = 1, sl_broken = 0, sl_release_no = 8;
	fk_n = fk_qn = fk_closed = fk_released = 0;
	for (int k = 0; k < SL_PEERS; k++)
		slp[k].ptr = 0;
	cap_clear();

	tick_at(1000);
	check(sl_started == 0 && fk_n == 0, "steam: nothing starts while this PC is alone in the list");
	set_list(2, peerA, peerB);
	sl_started = 1; /* the real start asks Steam; the fake interface is already in place */
	tick_at(1100);
	check(fk_n == 1 && fk_to[0] == SID_B && fk_last_type() == SL_HELLO && (fk_flags[0] & SL_SEND_RELIABLE) && *(unsigned *)(fk_buf[0] + 5) == 8,
	      "steam: a new player gets a reliable hello with our release number");

	fk_push(SID_B, SL_HELLO, 9, 0, 16);
	tick_at(1300);
	check(fk_released == 1 && fk_last_type() == SL_ACK && count_of("STEAM link to Bravo ready (RoN-Fix release 9)") == 1,
	      "steam: his hello is answered and the link is ready");

	fk_push(76561198000000099ull, SL_HELLO, 9, 0, 16);
	int before = fk_n;
	tick_at(1400);
	check(fk_released == 2 && fk_n == before, "steam: a message from a Steam ID not in the player list is thrown away unanswered");

	tick_at(3400);
	check(fk_last_type() == SL_PING && !(fk_flags[fk_n - 1] & SL_SEND_RELIABLE), "steam: every 2 s an unreliable ping");
	unsigned seq = *(unsigned *)(fk_buf[fk_n - 1] + 5), t = *(unsigned *)(fk_buf[fk_n - 1] + 9);
	fk_push(SID_B, SL_PONG, seq, t, 16);
	tick_at(3450);
	check(slp[0].got == 1 && slp[0].rtt_min == 50, "steam: his pong gives the round trip (50 ms)");

	fk_push(SID_B, SL_PING, 77, 123456, 16);
	tick_at(3500);
	check(fk_last_type() == SL_PONG && *(unsigned *)(fk_buf[fk_n - 1] + 5) == 77 && *(unsigned *)(fk_buf[fk_n - 1] + 9) == 123456,
	      "steam: his ping is echoed back as a pong");

	tick_at(31200);
	check(count_of("STEAM Bravo: rtt avg 50 min 50 max 50 ms over 1 pings") == 1 && count_of("Steam: ping 42 ms, quality 98%/100%") == 1,
	      "steam: every 30 s a line with our round trips and Steam's own ping and quality");

	/* freeze watch (v14): the game waits for B = player 1. What does the Steam link to B do meanwhile? */
	static unsigned char fw_players[8 * PLAYER_STRIDE], fw_peerC[0xb0];
	static unsigned char *fw_players_ptr;
	unsigned char *save_base = exe_base;
	char st[300], who[80];
	zero(fw_players, sizeof fw_players);
	zero(fw_peerC, sizeof fw_peerC);
	mk_refstr(fw_peerC + PEER_NAME, L"Charlie", 0);
	*(unsigned char **)(fw_players + 1 * PLAYER_STRIDE + PLAYER_PEER_OFF) = peerB;
	*(unsigned char **)(fw_players + 2 * PLAYER_STRIDE + PLAYER_PEER_OFF) = fw_peerC;
	fw_players_ptr = fw_players;
	exe_base = (unsigned char *)&fw_players_ptr - RVA_PLAYERS_PTR;
	sl_player_label(1, who);
	check(!lstrcmpA(who, "player 1 (Bravo)"), "freeze watch: the awaited player number is named from the game's player table");
	sl_test_now = 32000;
	sl_wait_pass(1, 1000);
	check(sl_wpeer == &slp[0], "freeze watch: the awaited player is found in the Steam link's list (same player object)");
	tick_at(32000);
	check(fk_last_type() == SL_PING && slp[0].w_sent == 1, "freeze watch: a wait starts -> the awaited player is pinged at once");
	tick_at(32250);
	tick_at(32510);
	check(slp[0].w_sent == 2, "freeze watch: while waiting, the awaited player is pinged every 0.5 s (not 2 s)");
	seq = *(unsigned *)(fk_buf[fk_n - 1] + 5), t = *(unsigned *)(fk_buf[fk_n - 1] + 9);
	fk_push_pong4(SID_B, seq, t, 2, 3000); /* B answers, and says B itself waits for player 2 */
	tick_at(32550);
	sl_wait_text(st, 0);
	check(has(st, "; Steam: last heard 0 ms ago, 1 of 2 pings answered during this wait (worst 40 ms)") &&
	          has(st, "; that player is waiting for player 2 (Charlie) too (3000 ms)"),
	      "freeze watch: WAIT line text: last heard, pings answered, and who the awaited player waits for");
	sl_wait_text(st, 1);
	check(has(st, "-> Steam link UP during the freeze"), "freeze watch: WAIT END verdict: Steam link up");
	/* our own pong says whom WE wait for (player 1), for anyone who asks */
	fk_push(SID_B, SL_PING, 5, 777, 16);
	tick_at(32600);
	check(fk_last_type() == SL_PONG && fk_len[fk_n - 1] == 21 && *(unsigned *)(fk_buf[fk_n - 1] + 5) == 5 &&
	          *(int *)(fk_buf[fk_n - 1] + 13) == 1 && *(unsigned *)(fk_buf[fk_n - 1] + 17) == 1000,
	      "freeze watch: our pong carries seq/time as before, then the player we wait for and how long");
	sl_wait_over();
	check(sl_wpeer == 0, "freeze watch: wait over -> nobody awaited");
	sl_test_now = 35000;
	fk_push(SID_B, SL_PING, 6, 778, 16);
	tick_at(35000);
	int lp = fk_n - 1;
	while (lp > 0 && fk_buf[lp][4] != SL_PONG)
		lp--; /* a regular 2 s ping may have gone out after the pong */
	check(fk_buf[lp][4] == SL_PONG && *(unsigned *)(fk_buf[lp] + 5) == 6 && *(int *)(fk_buf[lp] + 13) == -1,
	      "freeze watch: not waiting -> our pong says -1");
	/* a second freeze in which B's Steam link stays silent */
	sl_wait_pass(1, 1000);
	tick_at(35010);
	tick_at(35520);
	tick_at(36030);
	sl_wait_text(st, 1);
	check(has(st, "0 of 3 pings answered") && has(st, "-> Steam link SILENT too"), "freeze watch: no answers during the wait -> SILENT verdict");
	sl_wait_over();
	/* waiting for a player the Steam link has no entry for (no RoN-Fix 0.8+ there) */
	sl_wait_pass(2, 1000);
	sl_wait_text(st, 0);
	check(has(st, "no link to this player"), "freeze watch: a player without the Steam link is said so");
	sl_wait_over();
	exe_base = save_base;

	set_list(1, peerA, 0);
	tick_at(37000);
	check(fk_closed == 1 && count_of("(left)") == 1 && slp[0].ptr == 0, "steam: a player who left gets a last line, his session is closed");

	/* a player without RoN-Fix never answers: 8 hellos, then one line */
	set_list(2, peerA, peerB);
	for (DWORD t2 = 40000; t2 < 40000 + 12 * SL_HELLO_EVERY; t2 += 500)
		tick_at(t2);
	check(slp[0].hellos == SL_HELLO_TRIES && count_of("STEAM link to Bravo: no answer") == 1, "steam: no answer after 8 hellos, said once");

	/* a message laid out differently than expected: the link switches itself off instead of guessing */
	fk_push(SID_B, SL_HELLO, 1, 0, 0);
	tick_at(90000);
	int sent = fk_n;
	tick_at(92500);
	check(sl_broken == 1 && fk_n == sent && count_of("STEAM link off: unexpected message layout") == 1,
	      "steam: unexpected message layout -> link off, nothing more sent");
	sl_msgs = 0;
	sl_broken = 0;
	sl_started = 0;
}
