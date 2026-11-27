/* Unit tests for turndelay.c (shorter command delay, all or nobody, agreed through PlayFab member data). Included by
 * thunk_test.c after lobbyfix_tests.c (reuses its fake lobby window pointer, fake_post and the log capture). PlayFab's
 * readers are fakes over a small member table; the two patched instructions live in a buffer laid out like the exe. */

/* laid out like the exe from 0x955896: +0 mov [esp+0x1c],200 / TD_OFF_MIN mov ecx,400 / TD_OFF_MAX mov eax,600 */
#define TD_OFF_MIN (TD_VA_MIN - TD_VA_PING)
#define TD_OFF_MAX (TD_VA_MAX - TD_VA_PING)
static unsigned char td_buf[TD_OFF_MAX + 0x10];

/* the fake lobby: members with their ronfix_turn value (0 = never posted it), and the owner */
struct td_fake_member { const char *id, *val; };
static struct td_fake_member td_lob[4];
static unsigned td_nlob;
static struct pf_key td_keys[4], td_owner_key;
static const char *td_owner;
static unsigned td_reads; /* PlayFab calls, to check the 250 ms throttle */

static long __stdcall td_fake_members(void *lobby, unsigned *n, const void **m)
{
	(void)lobby;
	td_reads++;
	for (unsigned i = 0; i < td_nlob; i++)
		td_keys[i].id = td_lob[i].id, td_keys[i].type = "title_player_account";
	*n = td_nlob;
	*m = td_keys;
	return 0;
}
static long __stdcall td_fake_member_prop(void *lobby, const void *member, const char *key, const char **v)
{
	(void)lobby;
	td_reads++;
	const char *id = ((const struct pf_key *)member)->id;
	for (unsigned i = 0; i < td_nlob; i++)
		if (!lstrcmpA(id, td_lob[i].id)) {
			*v = !lstrcmpA(key, "ronfix_turn") ? td_lob[i].val : 0;
			return 0;
		}
	return (long)0x80070057; /* not a member */
}
static long __stdcall td_fake_owner(void *lobby, const void **o)
{
	(void)lobby;
	td_reads++;
	if (!td_owner)
		return (long)0x80004005;
	td_owner_key.id = td_owner, td_owner_key.type = "title_player_account";
	*o = &td_owner_key;
	return 0;
}

static void td_fill_code(int wrong) /* 0 = as the exe has it, 1 = floor site differs, 2 = ping site differs */
{
	static const unsigned char a[5] = { 0xb9, 0x90, 0x01, 0x00, 0x00 }, b[5] = { 0xb8, 0x58, 0x02, 0x00, 0x00 },
	                           c[8] = { 0xc7, 0x44, 0x24, 0x1c, 0xc8, 0x00, 0x00, 0x00 };
	volatile unsigned char *z = td_buf;
	for (unsigned i = 0; i < sizeof td_buf; i++)
		z[i] = 0xcc;
	for (int i = 0; i < 8; i++)
		z[i] = c[i];
	for (int i = 0; i < 5; i++)
		z[TD_OFF_MIN + i] = a[i], z[TD_OFF_MAX + i] = b[i];
	if (wrong == 1)
		z[TD_OFF_MIN + 1] = 0x91;
	if (wrong == 2)
		z[4] = 0x64;
}
static unsigned td_ping(void) { return *(unsigned *)(td_buf + 4); }
static unsigned td_lo(void) { return *(unsigned *)(td_buf + TD_OFF_MIN + 1); }
static unsigned td_hi(void) { return *(unsigned *)(td_buf + TD_OFF_MAX + 1); }
static unsigned char *td_exe(void) { return td_buf - (TD_VA_PING - 0x400000u); }

/* the lobby as the tests need it: A = us and the host, B = a friend, both human slots; slot 2 AI */
static void td_lobby_ab(const char *a, const char *b)
{
	td_nlob = 2;
	td_lob[0].id = "AAAA1111AAAA1111", td_lob[0].val = a;
	td_lob[1].id = "BBBB2222BBBB2222", td_lob[1].val = b;
	td_owner = "AAAA1111AAAA1111";
	zero(slots, sizeof slots);
	lf_strcpy(slots[0].a, "AAAA1111AAAA1111", 48), lf_strcpy(slots[0].b, "76561198000000001", 48);
	lf_strcpy(slots[1].a, "BBBB2222BBBB2222", 48), lf_strcpy(slots[1].b, "76561198000000002", 48);
	slots_valid = 1;
}
static void td_tick_now(int open, int closed, int opened) /* one lobby tick, past the 250 ms throttle */
{
	td_last_read = GetTickCount() - 1000;
	td_tick(open, closed, opened);
}
static int td_is_short(void) { return td_lo() == 200 && td_hi() == 300 && td_ping() == 100; }
static int td_is_own(void) { return td_lo() == 400 && td_hi() == 600 && td_ping() == 200; }

static const char *const tdk[3] = { "name", "_team_0", "versionId" }, *const tdv[3] = { "Bob", "8", "7" };
static const char *const tdck[2] = { "name", "platform" }, *const tdcv[2] = { "Bob", "Steam" };
static unsigned tdmu[3];
static const char *td_many_k[25], *td_many_v[25];

static void test_turndelay(void)
{
	char why[128];
	exe_base = (unsigned char *)&lf_setup_ptr - RVA_SETUPWIN_PTR;
	lf_setup_ptr = (void *)0x1234;
	td_get_members = td_fake_members, td_get_member_prop = td_fake_member_prop, td_get_owner = td_fake_owner;
	lf_game_lobby = 0, td_lobby = 0, td_active = 0, td_decided = -1, td_frozen = 0;

	/* 1. only with the expected code at turn start may this PC take part */
	td_fill_code(1);
	td_init(td_exe());
	check(!td_capable, "delay: unexpected code at turn start -> this PC does not take part");
	td_fill_code(2);
	td_init(td_exe());
	check(!td_capable, "delay: unexpected code in the turn tuner (starting ping) -> this PC does not take part");
	td_fill_code(0);
	td_min = 200, td_max = 300;
	td_init(td_exe());
	check(td_capable, "delay: expected code at all three sites -> this PC can take part");
	td_min = 50, td_max = 900;
	td_init(td_exe());
	check(td_min == 200 && td_max == 300, "delay: TurnFloorMin/Max outside 100..400 / min..600 fall back to 200-300");

	/* 2. every RoN-Fix player says "1" (or "2" with ShortDelay=1) in its own member posts in the game lobby */
	const void *mu;
	tdmu[0] = 3, tdmu[1] = (unsigned)tdk, tdmu[2] = (unsigned)tdv;
	td_host_wants = 0, mu = tdmu;
	td_rewrite((void *)0x9000, &mu);
	const unsigned *m2 = mu;
	check(mu != tdmu && m2[0] == 4 && !lstrcmpA(((const char *const *)m2[1])[3], "ronfix_turn") && !lstrcmpA(((const char *const *)m2[2])[3], "1") &&
	          !lstrcmpA(((const char *const *)m2[1])[1], "_team_0") && !lstrcmpA(((const char *const *)m2[2])[1], "8") && td_lobby == (void *)0x9000,
	      "delay: game-lobby member post gets ronfix_turn=1, the game's own keys kept, game lobby noted");
	td_host_wants = 1, mu = tdmu;
	td_rewrite((void *)0x9000, &mu);
	check(!lstrcmpA(((const char *const *)((const unsigned *)mu)[2])[3], "2"), "delay: with ShortDelay=1 our member key says 2");
	const void *mu2 = mu;
	td_rewrite((void *)0x9000, &mu2);
	check(mu2 == mu, "delay: a post that already has the key is left as it is");
	static unsigned tdcmu[3];
	tdcmu[0] = 2, tdcmu[1] = (unsigned)tdck, tdcmu[2] = (unsigned)tdcv;
	mu = tdcmu;
	td_rewrite((void *)0x9100, &mu);
	check(mu == tdcmu && td_lobby == (void *)0x9000, "delay: a post without slot keys (the Game Browser's chat lobby) is untouched");
	lf_setup_ptr = 0, mu = tdmu;
	td_rewrite((void *)0x9000, &mu);
	check(mu == tdmu, "delay: posts while no lobby window is open are untouched");
	lf_setup_ptr = (void *)0x1234;
	for (int i = 0; i < 25; i++)
		td_many_k[i] = "_x", td_many_v[i] = "0";
	static unsigned tdbig[3];
	tdbig[0] = 25, tdbig[1] = (unsigned)td_many_k, tdbig[2] = (unsigned)td_many_v;
	mu = tdbig;
	td_rewrite((void *)0x9000, &mu);
	check(mu == tdbig, "delay: a post with more than 24 keys is untouched (PlayFab allows 30 per player)");
	td_capable = 0, mu = tdmu;
	td_rewrite((void *)0x9000, &mu);
	check(mu == tdmu, "delay: a PC that cannot take part adds nothing");
	td_capable = 1;
	/* through the real post wrapper: the lobby's own data is passed on untouched */
	pf_post_orig = fake_post;
	static unsigned char lu_game[0x28];
	static struct pf_key td_me = { "AAAA1111AAAA1111", "title_player_account" };
	w_pf_post((void *)0x9000, &td_me, lu_game, tdmu, 0);
	check(post_args[2] == lu_game && post_args[3] != tdmu && ((const unsigned *)post_args[3])[0] == 4,
	      "delay: w_pf_post: lobby data passed on as it is, member data carries the key");
	lf_game_lobby = 0; /* that lobby-data post made us "host" of 0x9000 for the lobby fixes: undo for what follows */

	/* 3. the decision, from data every client sees */
	td_lobby_ab("2", "1");
	check(td_decide((void *)0x9000, why, sizeof why) == 1, "delay: host says 2, everyone has it -> on");
	td_lobby_ab("1", "1");
	check(!td_decide((void *)0x9000, why, sizeof why) && !lstrcmpA(why, "the host has not turned it on"), "delay: host says 1 -> off");
	td_lobby_ab(0, "2");
	check(!td_decide((void *)0x9000, why, sizeof why) && !lstrcmpA(why, "the host has not turned it on"),
	      "delay: host without RoN-Fix 0.11 -> off, whatever a joiner wants");
	td_lobby_ab("2", 0);
	check(!td_decide((void *)0x9000, why, sizeof why) && !lstrcmpA(why, "BBBB2222BBBB2222 has no RoN-Fix 0.11 (or has not said so yet)"),
	      "delay: one player without it -> off, and who");
	td_lobby_ab("2", "12");
	check(!td_decide((void *)0x9000, why, sizeof why), "delay: a garbled value counts as no");
	td_lobby_ab("2", "1");
	lf_strcpy(slots[3].a, "CCCC3333CCCC3333", 48);
	check(!td_decide((void *)0x9000, why, sizeof why) && !lstrcmpA(why, "slot 3 (CCCC3333CCCC3333) is not a lobby member with RoN-Fix 0.11"),
	      "delay: a human in a slot who is not a lobby member -> off");
	td_lobby_ab("2", "1");
	lf_strcpy(slots[4].b, "76561198000000009", 48);
	check(!td_decide((void *)0x9000, why, sizeof why), "delay: a slot with a Steam ID but no PlayFab ID -> off");
	td_lobby_ab("2", "1");
	td_owner = 0;
	check(!td_decide((void *)0x9000, why, sizeof why) && !lstrcmpA(why, "the lobby has no owner right now"), "delay: no owner -> off");
	td_lobby_ab("2", "1");
	check(!td_decide(0, why, sizeof why) && !lstrcmpA(why, "game lobby not known yet"), "delay: no game lobby -> off");
	slots_valid = 0;
	check(!td_decide((void *)0x9000, why, sizeof why), "delay: slot table not read yet -> off");
	td_lobby_ab("2", "1");
	td_nlob = 1;
	zero(&slots[1], sizeof slots[1]);
	check(td_decide((void *)0x9000, why, sizeof why) == 1, "delay: host alone with AI players -> on (the setup tested live)");

	/* 4. in the lobby window: applied at once, withdrawn when someone without it joins, frozen in the countdown */
	td_fill_code(0);
	td_active = 0;
	td_lobby_ab("2", "1");
	cap_clear();
	td_tick_now(1, 0, 1); /* the lobby window opens: forget the old lobby */
	check(td_lobby == 0 && td_decided == 0 && td_is_own(), "delay: a new lobby window starts with no lobby known and the game's floor");
	td_lobby = (void *)0x9000;
	cap_clear();
	td_tick_now(1, 0, 0);
	check(td_active && td_is_short() && count_of("short command delay ON for this lobby") == 1,
	      "delay: agreed -> floor 200-300 and starting ping 100 written at once");
	td_tick_now(1, 0, 0);
	check(count_of("short command delay ON for this lobby") == 1, "delay: an unchanged decision is not logged again");
	td_nlob = 3, td_lob[2].id = "CCCC3333CCCC3333", td_lob[2].val = 0;
	cap_clear();
	td_tick_now(1, 0, 0);
	check(!td_active && td_is_own() && count_of("short command delay off for this lobby: CCCC3333CCCC3333 has no RoN-Fix 0.11") == 1,
	      "delay: a player without it joins -> back to 400-600 at once, and who is logged");
	td_nlob = 2;
	td_tick_now(1, 0, 0);
	check(td_active && td_is_short(), "delay: he leaves again -> on again");
	unsigned before = td_reads;
	td_tick(1, 0, 0);
	td_tick(1, 0, 0);
	check(td_reads == before, "delay: PlayFab is read at most every 250 ms");
	slots[0].v[10] = 2; /* the host starts the countdown */
	cap_clear();
	td_tick_now(1, 0, 0);
	check(td_frozen && count_of("start countdown: command delay SHORT for this match (final)") == 1, "delay: countdown -> decision final, logged");
	td_nlob = 3; /* a change now (it can't be a join: membership is locked) must not move anything */
	before = td_reads;
	td_tick_now(1, 0, 0);
	check(td_active && td_is_short() && td_reads == before, "delay: frozen in the countdown: no reads, floor unchanged");
	cap_clear();
	td_tick_now(0, 1, 0); /* the lobby window closes: the match loads */
	check(td_active && td_is_short() && !td_frozen && count_of("command delay for this match: SHORT, turn floor 200-300 ms") == 1,
	      "delay: lobby closes -> stays short for the match, logged");
	cap_clear();
	td_tick_now(1, 0, 1); /* back in a lobby after the match */
	check(!td_active && td_is_own() && td_lobby == 0 && count_of("back to the game's own floor (400-600 ms)") == 1,
	      "delay: the next lobby restores 400-600 and forgets the old lobby");

	/* 5. off at the end: the match uses the game's floor and says why */
	td_lobby_ab("1", "1");
	td_lobby = (void *)0x9000;
	td_tick_now(1, 0, 0);
	cap_clear();
	td_tick_now(0, 1, 0);
	check(!td_active && td_is_own() && count_of("command delay for this match: the game's own (400-600 ms): the host has not turned it on") == 1,
	      "delay: host did not turn it on -> the game's own floor, reason logged");

	/* 6. the countdown starts before any decision was taken (joined just in time): decide once, then frozen */
	td_tick_now(1, 0, 1);
	td_lobby_ab("2", "1");
	td_lobby = (void *)0x9000;
	slots[0].v[10] = 3;
	td_decided = -1, td_frozen = 0;
	td_tick(1, 0, 0); /* countdown seen; read throttled */
	td_tick_now(1, 0, 0);
	check(td_frozen && td_decided == 1 && td_is_short(), "delay: countdown seen before the first read -> decided once, then frozen");
	td_tick_now(1, 0, 1);

	/* 7. PlayFab says the lobby is gone: its handle is never used again */
	td_lobby_ab("2", "1");
	td_lobby = (void *)0x9000;
	td_tick_now(1, 0, 0);
	td_on_lobby_gone((void *)0x9000);
	cap_clear();
	td_tick_now(1, 0, 0);
	check(td_lobby == 0 && !td_active && td_is_own() && count_of("off for this lobby: game lobby not known yet") == 1,
	      "delay: lobby disconnected -> handle forgotten, back to the game's floor");

	/* 8. a PC that cannot take part never patches */
	td_tick_now(1, 0, 1);
	td_capable = 0;
	td_lobby = (void *)0x9000;
	td_tick_now(1, 0, 0);
	check(!td_active && td_is_own(), "delay: a PC that cannot take part never patches");

	td_capable = 0, td_host_wants = 0, lf_game_lobby = 0, td_lobby = 0, td_decided = -1;
	zero(slots, sizeof slots);
	slots_valid = 0;
}
