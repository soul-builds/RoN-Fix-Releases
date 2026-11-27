/* Unit tests for steamlink.c (Steam link phase A, shadow mode). Included by thunk_test.c after lobby_tests.c
 * (uses its ns/peerA/peerB fakes and log capture). A fake ISteamNetworkingMessages records what is sent and hands
 * out messages laid out like SteamNetworkingMessage_t. */

static unsigned long long fk_to[64];
static unsigned char fk_buf[64][32];
static int fk_len[64], fk_flags[64], fk_n, fk_closed, fk_released;
static void *fk_queue[8];
static int fk_qn;
/* the data channel (numbered frames, rfxstream.c): what was sent on it, and what is waiting to be received */
#define FD_MAX (RFX_HDR + 64)
static unsigned long long fd_to[32];
static unsigned char fd_buf[32][FD_MAX];
static int fd_len[32], fd_flags[32], fd_n;
static void *fd_queue[8];
static int fd_qn;
static int THISCALL fk_send(void *s, const struct sl_ident *id, const void *d, unsigned n, int fl, int ch)
{
	(void)s;
	if (fd_n < 32 && ch == SL_DATA_CHANNEL && id->type == 16 && id->size == 8) {
		volatile unsigned char *o = fd_buf[fd_n];
		fd_to[fd_n] = id->u.sid;
		for (unsigned i = 0; i < n && i < FD_MAX; i++)
			o[i] = ((const unsigned char *)d)[i];
		fd_len[fd_n] = (int)n;
		fd_flags[fd_n++] = fl;
	}
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
	if (ch == SL_DATA_CHANNEL) {
		for (; n < fd_qn && n < max; n++)
			o[n] = fd_queue[n];
		fd_qn = 0;
		return n;
	}
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

/* ---------------------------------------------------------------- mirror (core v17) */
/* a hello or ack as release 0.16 sends it: release, capabilities, the name of the sender's list entry */
static void fk_push_hello(unsigned long long from, int type, unsigned release, unsigned caps, unsigned epoch)
{
	int k = fk_qn;
	fk_push(from, type, release, caps, 16);
	*(unsigned *)(fk_data[k] + 13) = epoch;
	*(int *)(fk_msg[k] + SLM_SIZE) = 17;
}
static unsigned char fd_msg[8][208], fd_data[8][FD_MAX];
/* n bytes on the data channel from Steam ID `from`; `channel` is what the message itself claims */
static void fd_push_raw(unsigned long long from, const unsigned char *d, unsigned n, int channel)
{
	int k = fd_qn;
	unsigned char *m = fd_msg[k];
	volatile unsigned char *o = fd_data[k];
	zero(m, 208);
	for (unsigned i = 0; i < n && i < FD_MAX; i++)
		o[i] = d[i];
	*(unsigned char **)(m + SLM_DATA) = fd_data[k];
	*(int *)(m + SLM_SIZE) = (int)n;
	*(int *)(m + SLM_IDENT) = 16;
	*(int *)(m + SLM_IDENT + 4) = 8;
	*(unsigned long long *)(m + SLM_IDENT + 8) = from;
	*(void **)(m + SLM_RELEASE) = (void *)fk_release;
	*(int *)(m + SLM_CHANNEL) = channel;
	fd_queue[fd_qn++] = m;
}
/* a whole data frame: header and message. out: RFX_HDR + len bytes. (The DLL itself copies the message once and
 * writes a header per receiver, rfx_put_data_head.) */
static unsigned rfx_put_data(unsigned char *out, unsigned my_epoch, unsigned your_epoch, unsigned seq, unsigned ack, int flags,
                             const unsigned char *payload, unsigned len)
{
	unsigned n = rfx_put_data_head(out, my_epoch, your_epoch, seq, ack, flags, len);
	volatile unsigned char *o = out + RFX_HDR;
	for (unsigned i = 0; n && i < len; i++)
		o[i] = payload[i];
	return n;
}
static void fd_push_data(unsigned long long from, unsigned my_epoch, unsigned your_epoch, unsigned seq, unsigned ack, int flags,
                         const unsigned char *msg, unsigned len)
{
	unsigned char f[FD_MAX];
	fd_push_raw(from, f, rfx_put_data(f, my_epoch, your_epoch, seq, ack, flags, msg, len), SL_DATA_CHANNEL);
}
/* one pass of the game's network pump at time t */
static void poll_at(DWORD t)
{
	sl_test_now = t;
	sl_data_last = t - 1;
	sl_data_poll();
}

static void test_mirror(void)
{
	static unsigned char x[30], y[12], z[20], big[RFX_MAX_MSG + 1], junk[25] = { 'j', 'u', 'n', 'k' }, pulse[1] = { 0x83 };
	static const unsigned long long SID_A = 76561198000000001ull;
	const unsigned EB = 0xB0B0B0B0u, EB2 = 0xC0C0C0C0u; /* names of B's list entries for us */
	struct rfx_frame f;
	unsigned char a[RFX_ACK_LEN];
	char st[512];
	for (int i = 0; i < 30; i++)
		x[i] = (unsigned char)(i + 7); /* x[0] = 7: a turn's commands */
	for (int i = 0; i < 12; i++)
		y[i] = (unsigned char)(i * 3 + 5);
	for (int i = 0; i < 20; i++)
		z[i] = (unsigned char)(0x0A + i);
	big[0] = 0x1E;

	zero(ns, sizeof ns);
	zero(peerA, sizeof peerA);
	zero(peerB, sizeof peerB);
	*(unsigned *)(peerA + PEER_FLAGS) = 4;
	mk_wstr(peerA + 0x5c, L"76561198000000001", sidA);
	mk_refstr(peerB + PEER_NAME, L"Bravo", 0);
	mk_wstr(peerB + 0x5c, L"76561198000000002", sidB);
	set_list(2, peerA, peerB);
	sl_msgs = fk_obj;
	sl_enabled = 1, sl_broken = 0, sl_release_no = 16, sl_started = 1, sl_mirror = 1, sl_selftest = 0;
	fk_n = fk_qn = fd_n = fd_qn = fk_closed = fk_released = 0;
	for (int k = 0; k < SL_PEERS; k++)
		slp[k].ptr = 0;
	sl_wpeer = 0;
	cap_clear();

	tick_at(1000);
	struct sl_peer *p = &slp[0];
	unsigned me = p->my_epoch;
	check(fk_n == 1 && fk_last_type() == SL_HELLO && fk_len[0] == 17 && *(unsigned *)(fk_buf[0] + 5) == 16 &&
	          *(unsigned *)(fk_buf[0] + 9) == SL_CAP_MIRROR && me && *(unsigned *)(fk_buf[0] + 13) == me,
	      "mirror: our hello carries the release, the mirror capability and the name of our list entry");

	fk_push(SID_B, SL_HELLO, 9, 0, 16); /* an older release: the release number and nothing else */
	*(int *)(fk_msg[0] + SLM_SIZE) = 9;
	tick_at(1100);
	sl_mirror_out(NULL, x, 30, 0);
	check(p->ready && fd_n == 0 && count_of("STEAM link to Bravo ready (RoN-Fix release 9); no mirror with this player") == 1,
	      "mirror: a player on an older release never gets a frame");

	fk_push_hello(SID_B, SL_HELLO, 16, SL_CAP_MIRROR, EB);
	tick_at(1200);
	sl_test_now = 1250;
	sl_mirror_out(NULL, x, 30, 0);
	check(fd_n == 1 && fd_to[0] == SID_B && fd_len[0] == RFX_HDR + 30 && rfx_parse(fd_buf[0], (unsigned)fd_len[0], &f) && f.seq == 1 && f.ack == 0 &&
	          f.my_epoch == me && f.your_epoch == EB && f.flags == RFX_F_MIRROR && rfx_hash(f.payload, 30) == rfx_hash(x, 30) &&
	          (fd_flags[0] & SL_SEND_RELIABLE) && (fd_flags[0] & SL_SEND_NONAGLE),
	      "mirror: a broadcast goes to a player who announced the capability as frame 1, reliable, the message unchanged");

	sl_mirror_out(peerB, y, 12, 1);
	sl_mirror_out(peerA, y, 12, 1);                /* to somebody without a link entry */
	sl_mirror_out(NULL, big, RFX_MAX_MSG + 1, 0);  /* longer than the game's own receive buffer */
	check(fd_n == 2 && rfx_parse(fd_buf[1], (unsigned)fd_len[1], &f) && f.seq == 2 && f.flags == (RFX_F_MIRROR | RFX_F_DIRECT) && f.len == 12 &&
	          p->tx.refused == 1,
	      "mirror: a message to one player goes to that player only, numbered on; an over-long one is not sent");

	sl_mirror = 0;
	sl_mirror_out(NULL, x, 30, 0);
	sl_mirror_party(peerB, x, 30);
	fd_push_data(SID_B, EB, me, 1, 0, RFX_F_MIRROR, x, 30);
	poll_at(1300);
	check(fd_n == 2 && fd_qn == 1 && !p->mir.party.n && p->rx.next == 1, "mirror: with Mirror=0 nothing is sent, read or compared");
	sl_mirror = 1;
	fd_qn = 0;

	fd_push_data(SID_B, EB, me, 1, 2, RFX_F_MIRROR, x, 30); /* B's frame 1; it also confirms our 1 and 2 */
	poll_at(2000);
	check(p->rx.next == 2 && p->mir.steam.n == 1 && p->tx.acked == 2 && fd_n == 3 && rfx_parse(fd_buf[2], (unsigned)fd_len[2], &f) &&
	          f.type == RFX_ACK && f.ack == 1 && f.seen == 1 && f.my_epoch == me && f.your_epoch == EB && !(fd_flags[2] & SL_SEND_RELIABLE),
	      "mirror: a frame arrives: taken in order, kept for pairing, our own frames confirmed, an unreliable ack sent back");
	sl_test_now = 2040;
	sl_mirror_party(peerB, x, 30);
	check(p->mir.matched == 1 && p->mir.steam_first == 1 && p->mir.sf_max == 40 && !p->mir.steam.n && !p->mir.party.n,
	      "mirror: PlayFab hands the game the same message 40 ms later: paired, Steam first by 40 ms");

	sl_test_now = 2200;
	sl_mirror_party(peerB, y, 12);
	fd_push_data(SID_B, EB, me, 2, 2, RFX_F_MIRROR | RFX_F_DIRECT, y, 12);
	poll_at(2260);
	check(p->mir.matched == 2 && p->mir.party_first == 1 && p->mir.pf_max == 60, "mirror: the next one PlayFab delivers first, by 60 ms");

	sl_mirror_party(peerA, x, 30);                                           /* our own echo: not a remote player's message */
	fd_push_data(SID_B, EB, 0xDEAD0001u, 3, 2, RFX_F_MIRROR, z, 20);         /* for an earlier entry of ours */
	fd_push_data(76561198000000099ull, EB, me, 3, 2, RFX_F_MIRROR, z, 20);   /* a Steam ID that is not in the player list */
	fd_push_raw(SID_B, junk, sizeof junk, SL_DATA_CHANNEL);
	fd_push_data(SID_B, EB, me, 3, 2, 0, z, 20);                             /* a frame meant for the game: a later stage */
	poll_at(2400);
	check(p->stale == 1 && p->bad == 2 && p->rx.next == 3 && !p->mir.steam.n && !p->mir.party.n && !sl_broken,
	      "mirror: a frame for an earlier entry, one from a stranger, junk, and a frame meant for the game are all left out");

	/* the game waits for B: B's next message is here over Steam, PlayFab brings it 700 ms later */
	fd_push_data(SID_B, EB, me, 3, 2, RFX_F_MIRROR, z, 20);
	poll_at(3000);
	sl_wpeer = p;
	p->mir.w_matched = p->mir.w_party_late = p->mir.w_steam_late = 0;
	sl_test_now = 3300;
	sl_wait_text(st, 0);
	check(has(st, "; mirror: 1 messages from that player are here over Steam, not yet over PlayFab (oldest 300 ms)"),
	      "mirror: a WAIT line says what is already here over Steam and not yet over PlayFab");
	sl_test_now = 3700;
	sl_mirror_party(peerB, z, 20);
	sl_wait_text(st, 1);
	check(has(st, "; mirror: nothing from that player waits on one path only; in this wait PlayFab delivered up to 700 ms after Steam"),
	      "mirror: WAIT END says how far PlayFab was behind Steam during the wait");
	sl_wpeer = 0;

	tick_at(31000);
	check(count_of("MIRROR Bravo: sent 2 (42 bytes, 1 refused), confirmed after avg 750 max 750 ms, 0 over 1.5 s; "
	               "got 3 (0 early, 0 late or repeated, 0 lost, 1 stale, 2 bad); "
	               "paired 3: Steam first 2 (avg 370 max 700 ms), PlayFab first 1 (avg 60 max 60 ms), same pass 0; "
	               "no twin: 0 on Steam only, 0 on PlayFab only; 0 DLL packets (every 30 s)") == 1 &&
	          !p->tx.sent && !p->mir.matched,
	      "mirror: every 30 s one line with what was sent, confirmed, received and paired");

	fd_push_data(SID_B, EB, me, 4, 2, RFX_F_MIRROR, pulse, 1);
	poll_at(31100);
	check(p->rx.next == 5 && p->mir.dll == 1 && !p->mir.steam.n, "mirror: the other side's pulse (0x83) arrives as a frame and is only counted");

	fk_push_hello(SID_B, SL_HELLO, 16, SL_CAP_MIRROR, EB2); /* B left and came back: a new entry over there */
	tick_at(31200);
	sl_test_now = 31250;
	sl_mirror_out(NULL, x, 30, 0);
	int newest = fd_n - 1;
	fd_push_data(SID_B, EB, me, 5, 2, RFX_F_MIRROR, y, 12); /* still under way from the old entry */
	poll_at(31300);
	check(rfx_parse(fd_buf[newest], (unsigned)fd_len[newest], &f) && f.seq == 1 && f.your_epoch == EB2 && p->rx.next == 1 && p->stale == 1,
	      "mirror: a hello with a new entry name restarts both numberings; frames of the old entry are stale");

	poll_at(31250 + SL_MIRROR_QUIET);
	int not_yet = p->quiet;
	poll_at(31251 + SL_MIRROR_QUIET);
	int before = fd_n;
	sl_test_now = 31260 + SL_MIRROR_QUIET;
	sl_mirror_out(NULL, x, 30, 0);
	int while_quiet = fd_n - before;
	fd_push_raw(SID_B, a, rfx_put_ack(a, EB2, me, 1, 1), SL_DATA_CHANNEL);
	poll_at(31300 + SL_MIRROR_QUIET);
	sl_mirror_out(NULL, x, 30, 0);
	check(!not_yet && while_quiet == 0 && fd_n == before + 1 && !p->quiet && p->tx.stalls == 1 && count_of("STEAM mirror to Bravo paused: nothing confirmed for 30 s") == 1 &&
	          count_of("STEAM mirror to Bravo resumed") == 1,
	      "mirror: nothing confirmed for 30 s -> frames to that player stop, and start again once everything is confirmed");

	fd_push_raw(SID_B, a, RFX_ACK_LEN, SL_CHANNEL); /* a message that says it is on another channel than we asked for */
	poll_at(31400 + SL_MIRROR_QUIET);
	before = fd_n;
	sl_mirror_out(NULL, x, 30, 0);
	check(sl_broken == 1 && fd_n == before && count_of("STEAM link off: unexpected message layout on the data channel") == 1,
	      "mirror: unexpected message layout on the data channel -> link off, nothing more sent");

	/* SelfTest (one-PC testing): this PC is its own peer */
	sl_broken = 0, sl_started = 0, sl_selftest = 1;
	fk_n = fk_qn = fd_n = fd_qn = 0;
	for (int k = 0; k < SL_PEERS; k++)
		slp[k].ptr = 0;
	mk_refstr(peerA + PEER_NAME, L"Alpha", 0);
	set_list(1, peerA, 0);
	tick_at(100000);
	me = p->my_epoch;
	fk_push_hello(SID_A, SL_HELLO, 16, SL_CAP_MIRROR, me); /* Steam delivers our hello to ourselves */
	tick_at(100100);
	sl_test_now = 100200;
	sl_mirror_out(NULL, x, 30, 0);
	sl_mirror_party(peerA, x, 30); /* CrossplayProxy's echo of our broadcast */
	int looped = fd_n == 1 && fd_to[0] == SID_A;
	if (looped)
		fd_push_raw(SID_A, fd_buf[0], (unsigned)fd_len[0], SL_DATA_CHANNEL);
	poll_at(100230);
	check(fk_to[0] == SID_A && p->self && p->ready && looped && p->mir.matched == 1 && p->mir.party_first == 1 && p->mir.pf_max == 30,
	      "mirror self test: this PC as its own peer: its broadcast loops through Steam and pairs with the echo");

	sl_mirror = sl_selftest = 0;
	sl_msgs = 0;
	sl_broken = 0;
	sl_started = 0;
	for (int k = 0; k < SL_PEERS; k++)
		slp[k].ptr = 0;
	zero(peerA + PEER_NAME, 12);
	set_list(2, peerA, peerB);
}
