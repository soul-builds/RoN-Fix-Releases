/* Unit tests for lobbyfix.c (host-only lobby fixes). Included by thunk_test.c after lobby_tests.c (reuses its
 * fake netsys / players / slot table helpers and the log capture). PlayFab's PFLobbyForceRemoveMember is a fake
 * that records its arguments. */

static void *rm_lobby;
static char rm_id[40], rm_type[40];
static int rm_prevent, rm_calls;
static long __stdcall lf_fake_remove(void *lobby, const void *key, int prevent, void *ctx)
{
	(void)ctx;
	rm_lobby = lobby;
	read_cstr(((const char *const *)key)[0], rm_id, sizeof rm_id);
	read_cstr(((const char *const *)key)[1], rm_type, sizeof rm_type);
	rm_prevent = prevent & 0xff;
	rm_calls++;
	return 0;
}

static void *lf_setup_ptr; /* the exe's "setup window" global */
static unsigned char lf_lu[0x28];
static unsigned short lfidA[20], lfidB[20], lfsB[24], lfslotB[24];
static struct pf_key lf_user = { "AAAA1111AAAA1111", "title_player_account" };

static void lf_reset(DWORD *t)
{
	zero(seen, sizeof seen);
	zero(ghosts, sizeof ghosts);
	zero(slots, sizeof slots);
	slots_valid = 0;
	zero(lf_q, sizeof lf_q);
	lf_nkicked = 0;
	lf_last_kick = 0;
	lf_game_lobby = 0;
	rm_calls = 0;
	*t += 60000;
}

/* a slot as the game fills it: +0x00 PlayFab entity id, +0x18 Steam ID (both longer than 7 chars: heap strings) */
static unsigned short lfh[8][24], lfhb[8][24];
static void lf_set_slot(int i, const wchar_t *ent, const wchar_t *steam, unsigned char ready)
{
	unsigned char *s = tbl + i * SLOT_SIZE;
	zero(s, SLOT_SIZE);
	if (ent) {
		mk_wstr(s, ent, lfh[i]);
		mk_wstr(s + 0x18, steam, lfhb[i]);
		s[0x34] = 1;
		s[SLOT_READY_OFF] = ready;
	} else
		s[0x34] = 4;
}

/* writes the ban file with a time stamp `bump` seconds ahead: what the core watches to see that it changed */
static void lf_write_bans(const char *path, const char *text, int bump)
{
	HANDLE w = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD x;
	FILETIME ft;
	ULARGE_INTEGER u;
	WriteFile(w, text, (DWORD)lstrlenA(text), &x, NULL);
	GetSystemTimeAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime, u.HighPart = ft.dwHighDateTime;
	for (int i = 0; i < bump; i++)
		u.QuadPart += 10000000ULL;
	ft.dwLowDateTime = u.LowPart, ft.dwHighDateTime = u.HighPart;
	SetFileTime(w, NULL, NULL, &ft);
	CloseHandle(w);
}

/* B leaves his slot 1 (cleared) while staying connected */
static void lf_clear_slot_b(DWORD t)
{
	lf_set_slot(1, 0, 0, 0);
	slot_scan(tbl, t);
}

static void test_lobbyfix(void)
{
	DWORD t = 900000;
	char path[MAX_PATH], buf[600];
	exe_base = (unsigned char *)&lf_setup_ptr - RVA_SETUPWIN_PTR; /* exe_base + RVA_SETUPWIN_PTR = &lf_setup_ptr */
	lf_setup_ptr = (void *)0x1234;
	pf_remove_orig = lf_fake_remove;
	pf_post_orig = fake_post;
	lf_enabled = 1, lf_bootkick = 1, lf_spamkick = 12, lf_test_shift = 0;
	GetTempPathA(MAX_PATH - 40, path);
	lstrcatA(path, "ronfix-test-bans.txt");
	DeleteFileA(path);
	lstrcpyA(lf_ban_path, path);
	lf_nbans = 0, lf_ban_check = 0;
	lstrcpyA(pf_user, "AAAA1111AAAA1111");

	/* host A (this PC), remote B with a Steam ID */
	zero(ns, sizeof ns);
	zero(peerA, sizeof peerA);
	zero(peerB, sizeof peerB);
	mk_wstr(peerA + PEER_ID, L"AAAA1111AAAA1111", lfidA);
	mk_refstr(peerA + PEER_NAME, L"Hosty", 0);
	*(unsigned *)(peerA + PEER_FLAGS) = 5;
	mk_wstr(peerB + PEER_ID, L"BBBB2222BBBB2222", lfidB);
	mk_refstr(peerB + PEER_NAME, L"Bob", 0);
	mk_wstr(peerB + 0x5c, L"76561198000000002", lfsB);
	*(unsigned *)(peerB + PEER_FLAGS) = 2;
	*(unsigned *)(ns + NETSYS_FLAGS) = 0x17; /* lobby, host */
	set_list(2, peerA, peerB);

	lf_reset(&t);
	peer_scan(ns, t);
	lf_set_slot(0, L"AAAA1111AAAA1111", L"76561198000000001", 0);
	lf_set_slot(1, L"BBBB2222BBBB2222", L"76561198000000002", 0);
	for (int i = 2; i < 8; i++)
		lf_set_slot(i, 0, 0, 0);
	slot_scan(tbl, t);
	cap_clear();

	/* 1. game lobby handle not known yet: a boot is not turned into a removal */
	lf_clear_slot_b(t + 100);
	lf_tick(ns, t + 2000);
	check(rm_calls == 0, "lobbyfix: nothing removed while the game lobby's handle is unknown");

	/* 2. the host posts lobby data -> that is the game lobby; the boot becomes a real kick */
	pf_log_post((void *)0x6000, &lf_user, lf_lu, 0, 0, t + 2100);
	ghosts[1].kick_queued = 0;
	lf_tick(ns, t + 2200);
	check(rm_calls == 1 && rm_lobby == (void *)0x6000 && !lstrcmpA(rm_id, "BBBB2222BBBB2222") && !lstrcmpA(rm_type, "title_player_account") && rm_prevent == 1,
	      "lobbyfix: boot -> PFLobbyForceRemoveMember(game lobby, Bob, title_player_account, prevent rejoin)");
	check(count_of("LOBBY KICK Bob (booted by the host): removed from the PlayFab lobby, rejoin blocked") == 1, "lobbyfix: the kick is logged");
	if (rm_calls != 1) { say("--- captured log:\r\n"); say(cap); }
	lf_tick(ns, t + 5000);
	check(rm_calls == 1, "lobbyfix: the same player is removed only once");

	/* 3. a slot cleared and taken again within 1.5 s (a move, not a boot): nothing */
	lf_reset(&t);
	peer_scan(ns, t);
	lf_set_slot(1, L"BBBB2222BBBB2222", L"76561198000000002", 0);
	slot_scan(tbl, t);
	pf_log_post((void *)0x6000, &lf_user, lf_lu, 0, 0, t);
	lf_clear_slot_b(t + 100);
	lf_set_slot(2, L"BBBB2222BBBB2222", L"76561198000000002", 0);
	slot_scan(tbl, t + 600);
	lf_tick(ns, t + 3000);
	check(rm_calls == 0, "lobbyfix: a player who is back in a slot within 1.5 s is not removed");
	lf_set_slot(2, 0, 0, 0);

	/* 4. Shift + boot = ban: removed and written to ronfix-bans.txt (entity id + Steam ID) */
	lf_reset(&t);
	peer_scan(ns, t);
	lf_set_slot(1, L"BBBB2222BBBB2222", L"76561198000000002", 0);
	slot_scan(tbl, t);
	pf_log_post((void *)0x6000, &lf_user, lf_lu, 0, 0, t);
	cap_clear();
	lf_test_shift = 1;
	lf_clear_slot_b(t + 100);
	lf_test_shift = 0;
	lf_tick(ns, t + 2000);
	DWORD got = 0;
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f != INVALID_HANDLE_VALUE) {
		ReadFile(f, buf, sizeof buf - 1, &got, NULL);
		CloseHandle(f);
	}
	buf[got] = 0;
	check(rm_calls == 1 && count_of("(booted with Shift: banned)") == 1 && count_of("LOBBY ban list: Bob added") == 1,
	      "lobbyfix: Shift + boot = kick and ban");
	int has = 0;
	for (char *p = buf; *p; p++)
		if (!has && p[0] == 'B' && p[1] == 'B' && p[2] == 'B' && p[3] == 'B')
			has = 1;
	check(has && got > 40, "lobbyfix: ronfix-bans.txt holds Bob's entity id and Steam ID");

	/* 5. ban list: a banned player is removed as soon as PlayFab reports him joining (new lobby) */
	lf_reset(&t);
	lf_nbans = 0, lf_ban_check = 0;
	lf_on_lobby_created((void *)0x7000);
	static struct pf_added add = { 2, (void *)0x7000, { "BBBB2222BBBB2222", "title_player_account" } };
	cap_clear();
	pf_log_change((const unsigned char *)&add, t);
	lf_tick(ns, t + 10);
	check(rm_calls == 1 && rm_lobby == (void *)0x7000 && count_of("LOBBY KICK BBBB2222BBBB2222 (on the ban list)") == 1 && count_of("LOBBY ban list: 2 entries") == 1,
	      "lobbyfix: a banned player joining is removed at once (ban list read from the file)");

	/* 6. ban by Steam ID alone, found in the network list */
	lf_reset(&t);
	{
		HANDLE w = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD x;
		static const char only_steam[] = "# test\r\n76561198000000002  # Bob by Steam ID\r\n";
		WriteFile(w, only_steam, sizeof only_steam - 1, &x, NULL);
		CloseHandle(w);
	}
	lf_nbans = 0, lf_ban_check = 0;
	lf_on_lobby_created((void *)0x7100);
	peer_scan(ns, t);
	lf_tick(ns, t + 10);
	check(rm_calls == 1 && !lstrcmpA(rm_id, "BBBB2222BBBB2222"), "lobbyfix: a player banned by Steam ID is removed from the network list");

	/* 6b. unban while the game runs: the RoN-Fix window moves a new ronfix-bans.txt over the old one (or deletes
	 * it when no ban is left). The core sees the new time stamp at its next 5 s check; no restart. */
	lf_reset(&t);
	lf_write_bans(path, "76561198000000002  # Bob, banned 2026-11-27\r\n", 1);
	lf_nbans = 0, lf_ban_check = 0;
	lf_on_lobby_created((void *)0x7300);
	peer_scan(ns, t);
	lf_tick(ns, t + 10);
	int banned = rm_calls;
	lf_write_bans(path, "CCCC3333CCCC3333  # Carl, banned 2026-11-27\r\n", 2); /* Bob unbanned, Carl stays */
	cap_clear();
	rm_calls = 0, lf_last_kick = 0;
	zero(lf_q, sizeof lf_q);
	lf_on_lobby_created((void *)0x7301);
	lf_tick(ns, t + 1000);
	int within_5s = rm_calls;
	rm_calls = 0, lf_last_kick = 0;
	zero(lf_q, sizeof lf_q);
	lf_on_lobby_created((void *)0x7302);
	lf_tick(ns, t + 5100);
	check(banned == 1 && within_5s == 1 && rm_calls == 0 && count_of("LOBBY ban list: 1 entry from") == 1,
	      "lobbyfix: unbanned in the file while the game runs -> let in from the next 5 s check on");
	lf_write_bans(path, "76561198000000002  # Bob, banned 2026-11-28\r\n", 3);
	rm_calls = 0, lf_last_kick = 0;
	zero(lf_q, sizeof lf_q);
	lf_on_lobby_created((void *)0x7303);
	lf_tick(ns, t + 10200);
	banned = rm_calls;
	DeleteFileA(path);
	rm_calls = 0, lf_last_kick = 0;
	zero(lf_q, sizeof lf_q);
	lf_on_lobby_created((void *)0x7304);
	lf_tick(ns, t + 15300);
	check(banned == 1 && rm_calls == 0 && lf_nbans == 0, "lobbyfix: ban file deleted (last ban unbanned) -> nobody banned");

	/* 7. ready spam: 11 toggles in 5 s no, 12 yes */
	lf_reset(&t);
	DeleteFileA(path);
	lf_nbans = 0, lf_ban_check = 0;
	peer_scan(ns, t);
	lf_set_slot(1, L"BBBB2222BBBB2222", L"76561198000000002", 0);
	slot_scan(tbl, t);
	lf_on_lobby_created((void *)0x7200);
	for (int k = 1; k <= 11; k++) {
		tbl[1 * SLOT_SIZE + SLOT_READY_OFF] = (unsigned char)(k & 1);
		slot_scan(tbl, t + k * 300);
	}
	lf_tick(ns, t + 3400);
	check(rm_calls == 0, "lobbyfix: 11 ready toggles in 5 s are not removed");
	tbl[1 * SLOT_SIZE + SLOT_READY_OFF] = 0;
	slot_scan(tbl, t + 3600);
	tbl[1 * SLOT_SIZE + SLOT_READY_OFF] = 0; /* (no change) */
	lf_tick(ns, t + 3700);
	check(rm_calls == 1 && count_of("(ready toggled 12 times in 5 s)") == 1, "lobbyfix: 12 ready toggles in 5 s remove the spammer");

	/* 8. never as non-host, never in a running game, never during the start countdown, never ourselves */
	lf_reset(&t);
	peer_scan(ns, t);
	lf_set_slot(1, L"BBBB2222BBBB2222", L"76561198000000002", 0);
	slot_scan(tbl, t);
	lf_on_lobby_created((void *)0x7300);
	*(unsigned *)(ns + NETSYS_FLAGS) = 0x16; /* not host */
	lf_clear_slot_b(t + 100);
	cap_clear();
	lf_tick(ns, t + 2000);
	check(rm_calls == 0, "lobbyfix: a non-host never removes anyone");
	lf_kick("BBBB2222BBBB2222", "Bob", "test", 0);
	lf_tick(ns, t + 2100);
	check(rm_calls == 0 && count_of("LOBBY not removing Bob (test): not the host") == 1, "lobbyfix: a queued removal is dropped (and said) when not the host");
	*(unsigned *)(ns + NETSYS_FLAGS) = 0x57; /* in game */
	lf_kick("BBBB2222BBBB2222", "Bob", "test", 0);
	lf_tick(ns, t + 2200);
	check(rm_calls == 0 && count_of("the game is running") == 1, "lobbyfix: nothing in a running game");
	*(unsigned *)(ns + NETSYS_FLAGS) = 0x17;
	zero(ghosts, sizeof ghosts); /* forget the boot made while not host */
	lf_set_slot(0, L"AAAA1111AAAA1111", L"76561198000000001", 5); /* countdown */
	slot_scan(tbl, t + 2300);
	lf_kick("BBBB2222BBBB2222", "Bob", "test", 0);
	lf_tick(ns, t + 2400);
	check(rm_calls == 0 && count_of("the game is starting") == 1, "lobbyfix: nothing during the start countdown");
	lf_set_slot(0, L"AAAA1111AAAA1111", L"76561198000000001", 0);
	slot_scan(tbl, t + 2500);
	lf_setup_ptr = 0;
	lf_kick("BBBB2222BBBB2222", "Bob", "test", 0);
	lf_tick(ns, t + 2600);
	check(rm_calls == 0 && count_of("no lobby open") == 1, "lobbyfix: nothing without an open lobby");
	lf_setup_ptr = (void *)0x1234;
	lf_kick("AAAA1111AAAA1111", "Hosty", "test", 0);
	lf_tick(ns, t + 2700);
	check(rm_calls == 0, "lobbyfix: never this PC's own player");

	/* 9. two removals queued: one now, the next after 500 ms */
	lf_reset(&t);
	lf_on_lobby_created((void *)0x7400);
	slot_scan(tbl, t);
	lf_kick("BBBB2222BBBB2222", "Bob", "one", 0);
	lf_kick("CCCC3333CCCC3333", "Cy", "two", 0);
	lf_tick(ns, t + 10);
	lf_tick(ns, t + 200);
	check(rm_calls == 1, "lobbyfix: at most one removal per 500 ms");
	lf_tick(ns, t + 600);
	check(rm_calls == 2 && !lstrcmpA(rm_id, "CCCC3333CCCC3333"), "lobbyfix: the next one follows");

	DeleteFileA(path);
	lf_ban_path[0] = 0;
	lf_enabled = 0;
}
