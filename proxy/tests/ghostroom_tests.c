/* Unit tests for ghostroom.c (leave the lobby a failed Create left behind). Included by thunk_test.c after
 * lobbyfix_tests.c: reuses the fake netsys (ns), the log capture and check(). PlayFab's PFLobbyLeave and
 * CrossplayProxy's service object (vtable slot 51 = P2PCloseAll) are fakes that record their calls. The PlayFab
 * events go through the real pf_log_change, so the wiring is tested too. */

static void *gr_lv_lobby, *gr_lv_ctx, *gr_close_this;
static char gr_lv_id[40], gr_lv_type[40];
static int gr_lv_calls, gr_close_calls, gr_seq, gr_lv_at, gr_close_at;
static long __stdcall gr_fake_leave(void *lobby, const void *user, void *ctx)
{
	gr_lv_lobby = lobby, gr_lv_ctx = ctx;
	read_cstr(((const char *const *)user)[0], gr_lv_id, sizeof gr_lv_id);
	read_cstr(((const char *const *)user)[1], gr_lv_type, sizeof gr_lv_type);
	gr_lv_calls++;
	gr_lv_at = ++gr_seq;
	return 0;
}
static void THISCALL gr_fake_close(void *self)
{
	gr_close_this = self;
	gr_close_calls++;
	gr_close_at = ++gr_seq;
}
static void *gr_vt[60], *gr_vt_other[60];
static void *gr_svc[1] = { gr_vt }, *gr_svc_other[1] = { gr_vt_other };

static void gr_reset(void)
{
	gr_lobby = gr_join_lobby = 0;
	gr_room_seen = gr_done = 0;
	gr_join_at = 0;
	gr_lv_calls = gr_close_calls = gr_seq = 0;
	gr_lv_lobby = gr_close_this = 0;
	gr_enabled = 1;
	*(void **)(ns + NETSYS_SERVICE) = gr_svc;
	cap_clear();
}
/* the PlayFab state changes as CrossplayProxy receives them */
static void gr_pf_created(void *lobby, long result, DWORD now)
{
	static unsigned char sc[0x10];
	zero(sc, sizeof sc);
	*(unsigned *)sc = 0, *(long *)(sc + 4) = result, *(void **)(sc + 0xc) = lobby;
	pf_log_change(sc, now);
}
static void gr_pf_joined(void *lobby, DWORD now)
{
	static unsigned char sc[0x18];
	static const char *const me[2] = { "AAAA1111AAAA1111", "title_player_account" };
	zero(sc, sizeof sc);
	*(unsigned *)sc = 1, *(long *)(sc + 4) = 0;
	*(const char **)(sc + 8) = me[0], *(const char **)(sc + 0xc) = me[1], *(void **)(sc + 0x14) = lobby;
	pf_log_change(sc, now);
}
static void gr_pf_gone(void *lobby, unsigned type, DWORD now) /* 6 = LeaveLobbyCompleted, 10 = Disconnected */
{
	static unsigned char sc[8];
	*(unsigned *)sc = type, *(void **)(sc + 4) = lobby;
	pf_log_change(sc, now);
}

static void test_ghostroom(void)
{
	DWORD t = 3000000;
	void *A = (void *)0x9100, *B = (void *)0x9200, *room = (void *)0x1234;
	pf_leave_orig = gr_fake_leave;
	lstrcpyA(pf_user, "AAAA1111AAAA1111");
	lstrcpyA(pf_user_type, "title_player_account");
	gr_vt[51] = (void *)gr_fake_close, gr_vt_other[51] = (void *)gr_fake_close;
	gr_vt_expect = gr_vt, gr_close_expect = (void *)gr_fake_close;
	zero(pf_h, sizeof pf_h);

	/* 0. once per start: is the service object CrossplayProxy's? */
	gr_reset();
	gr_checked = 0;
	gr_tick(ns, t, 0, 0);
	gr_tick(ns, t + 100, 0, 0);
	check(count_of("ghost room clean-up ready: CrossplayProxy's service object recognised") == 1, "ghost room: logs once that it recognises CrossplayProxy");

	/* 1. a normal Create: the room window opens 1.7 s later. A join afterwards (a match, then back to the
	 *    browser) changes nothing. */
	gr_reset();
	gr_pf_created(A, 0, t);
	gr_tick(ns, t + 1000, 0, 0);
	gr_tick(ns, t + 1700, room, 0x17);
	gr_tick(ns, t + 60000, 0, 0x77);
	gr_pf_joined(B, t + 61000);
	gr_tick(ns, t + 65000, 0, 0);
	check(gr_lv_calls == 0 && gr_close_calls == 0, "ghost room: a normal Create (room window opened) is never touched");

	/* 2. the ghost: created, the room window never opens, the game joins the browser's chat lobby 3 s later */
	gr_reset();
	t += 100000;
	gr_pf_created(A, 0, t);
	gr_tick(ns, t + 1000, 0, 0);
	gr_pf_joined(B, t + 3000);
	gr_tick(ns, t + 5900, 0, 0);
	int early = gr_lv_calls + gr_close_calls;
	gr_tick(ns, t + 6000, 0, 0);
	check(early == 0 && gr_lv_calls == 1 && gr_lv_lobby == A && gr_lv_ctx == NULL && !lstrcmpA(gr_lv_id, "AAAA1111AAAA1111") &&
	          !lstrcmpA(gr_lv_type, "title_player_account"),
	      "ghost room: 3 s after the game went back to the browser, PFLobbyLeave(created lobby, our entity key, ctx NULL)");
	check(gr_close_calls == 1 && gr_close_this == (void *)gr_svc && gr_lv_at < gr_close_at,
	      "ghost room: then P2PCloseAll on CrossplayProxy's service object, in the order of the game's own Leave");
	check(count_of("LOBBY ghost room:") == 2 && count_of("its room window never opened and the game went back to the browser") == 1,
	      "ghost room: what it does is logged");
	gr_tick(ns, t + 9000, 0, 0);
	gr_pf_joined(B, t + 9500);
	gr_tick(ns, t + 20000, 0, 0);
	check(gr_lv_calls == 1 && gr_close_calls == 1, "ghost room: done once per created lobby");

	/* 3. the game leaves (or loses) the created lobby itself before the 3 s are up: nothing to do */
	gr_reset();
	t += 100000;
	gr_pf_created(A, 0, t);
	gr_pf_joined(B, t + 3000);
	gr_pf_gone(A, 6, t + 4000);
	gr_tick(ns, t + 8000, 0, 0);
	gr_reset();
	gr_pf_created(A, 0, t + 10000);
	gr_pf_joined(B, t + 13000);
	gr_pf_gone(A, 10, t + 14000);
	gr_tick(ns, t + 18000, 0, 0);
	check(gr_lv_calls == 0 && gr_close_calls == 0, "ghost room: a lobby the game left or lost itself is not touched");

	/* 4. a failed create, joining the lobby we created, the setting off, a running match: nothing */
	gr_reset();
	t += 100000;
	gr_pf_created(A, (long)0x80004005, t);
	gr_pf_joined(B, t + 3000);
	gr_tick(ns, t + 8000, 0, 0);
	gr_reset();
	gr_pf_created(A, 0, t + 10000);
	gr_pf_joined(A, t + 13000);
	gr_tick(ns, t + 18000, 0, 0);
	gr_reset();
	gr_enabled = 0;
	gr_pf_created(A, 0, t + 20000);
	gr_pf_joined(B, t + 23000);
	gr_tick(ns, t + 28000, 0, 0);
	gr_reset();
	gr_pf_created(A, 0, t + 30000);
	gr_pf_joined(B, t + 33000);
	gr_tick(ns, t + 38000, 0, 0x77);
	check(gr_lv_calls == 0 && gr_close_calls == 0 && count_of("a match is running: left alone") == 1,
	      "ghost room: failed create / join of our own lobby / GhostRoom=0 / match running -> nothing");

	/* 5. another build's service object, or our PlayFab id unknown: logged, nothing called */
	gr_reset();
	t += 100000;
	*(void **)(ns + NETSYS_SERVICE) = gr_svc_other;
	gr_pf_created(A, 0, t);
	gr_pf_joined(B, t + 3000);
	gr_tick(ns, t + 7000, 0, 0);
	int other = gr_lv_calls + gr_close_calls, other_log = count_of("is NOT cleaned up: CrossplayProxy's service object is not the expected build");
	gr_reset();
	pf_user[0] = 0;
	gr_pf_created(A, 0, t + 10000);
	gr_pf_joined(B, t + 13000);
	gr_tick(ns, t + 17000, 0, 0);
	check(other == 0 && other_log == 1 && gr_lv_calls == 0 && gr_close_calls == 0 && count_of("our PlayFab id is not known yet") == 1,
	      "ghost room: unexpected service object or unknown PlayFab id -> logged, nothing called");

	lstrcpyA(pf_user, "AAAA1111AAAA1111");
	*(void **)(ns + NETSYS_SERVICE) = 0;
	gr_lobby = 0;
	gr_vt_expect = gr_close_expect = 0;
	pf_leave_orig = 0;
}
