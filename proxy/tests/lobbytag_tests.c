/* Unit tests for lobbytag.c (who in the lobby has RoN-Fix, from PlayFab member data). Included by thunk_test.c after
 * turndelay_tests.c: it reuses that file's fake lobby (members with their ronfix_turn and ronfix_ver values), the fake
 * lobby window pointer and the log capture. Time is passed in, so the grace period needs no waiting. */

static char lt_shown[8][160];
static int lt_nshown;
static void lt_fake_show(const char *line)
{
	if (lt_nshown < 8)
		lf_strcpy(lt_shown[lt_nshown], line, 160);
	lt_nshown++;
}
static void lt_peer(int k, const char *name, const char *id)
{
	static unsigned char obj[8][4];
	seen[k].ptr = obj[k];
	lf_strcpy(seen[k].name, name, 48);
	lf_strcpy(seen[k].id, id, 40);
}
static const char *const lt_k27[27] = { "_a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n",
	                                    "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "zz" };

static void test_lobbytag(void)
{
	static const char A[] = "AAAA1111AAAA1111", B[] = "BBBB2222BBBB2222", C[] = "CCCC3333CCCC3333", D[] = "DDDD4444DDDD4444";
	static struct pf_key me = { "AAAA1111AAAA1111", "title_player_account" };
	static unsigned mu3[3], mu27[3], muc[3];
	char name[16];
	const void *mu;
	exe_base = (unsigned char *)&lf_setup_ptr - RVA_SETUPWIN_PTR;
	lf_setup_ptr = (void *)0x1234;
	td_get_members = td_fake_members, td_get_member_prop = td_fake_member_prop, td_get_owner = td_fake_owner;
	lf_game_lobby = 0, td_lobby = (void *)0x9000;
	lt_enabled = 1, lt_show = 0, lt_self[0] = 0;

	/* 1. what this PC says about itself, and what it accepts from others */
	lt_set_release(16, "0.16");
	int v1 = !lstrcmpA(lt_val, "16:0.16");
	lt_set_release(16, "");
	int v2 = !lstrcmpA(lt_val, "16");
	lt_set_release(16, "0.16 beta!");
	int v3 = !lstrcmpA(lt_val, "16:0.16beta");
	lt_set_release(0, "0.16");
	check(v1 && v2 && v3 && !lt_val[0], "tags: our value is <release>:<name>, plain characters only, nothing without a release number");
	check(lt_parse("16:0.16", name, 16) == 16 && !lstrcmpA(name, "0.16") && lt_parse("17", name, 16) == 17 && !name[0] &&
	          lt_parse("18:0.18:g1", name, 16) == 18 && !lstrcmpA(name, "0.18"),
	      "tags: a member's value gives release and name; a later release may add a third part");
	check(!lt_parse("", name, 16) && !lt_parse("abc", name, 16) && !lt_parse("16x", name, 16) && !lt_parse("12345", name, 16) &&
	          !lt_parse("16:<b>", name, 16) && !lt_parse("0", name, 16),
	      "tags: junk in a member's value is not taken for a release");

	/* 2. the key rides on our own member posts in the game lobby */
	lt_set_release(16, "0.16");
	td_capable = 1, td_host_wants = 0;
	mu3[0] = 3, mu3[1] = (unsigned)tdk, mu3[2] = (unsigned)tdv;
	mu = mu3;
	td_rewrite((void *)0x9000, &mu);
	lt_rewrite(&me, &mu);
	const unsigned *m = mu;
	const char *const *mk = (const char *const *)m[1], *const *mv = (const char *const *)m[2];
	check(m[0] == 5 && !lstrcmpA(mk[1], "_team_0") && !lstrcmpA(mv[1], "8") && !lstrcmpA(mk[3], "ronfix_turn") && !lstrcmpA(mk[4], "ronfix_ver") &&
	          !lstrcmpA(mv[4], "16:0.16") && !lstrcmpA(lt_self, A),
	      "tags: a game-lobby member post gets ronfix_ver after ronfix_turn, the game's keys kept, our own id noted");
	const void *again = mu;
	lt_rewrite(&me, &again);
	muc[0] = 2, muc[1] = (unsigned)tdck, muc[2] = (unsigned)tdcv;
	const void *chat = muc;
	lt_rewrite(&me, &chat);
	mu27[0] = 27, mu27[1] = (unsigned)lt_k27, mu27[2] = (unsigned)lt_k27;
	const void *big = mu27;
	lt_rewrite(&me, &big);
	check(again == mu && chat == muc && big == mu27,
	      "tags: untouched are a post that has the key, the Game Browser's chat lobby, and a post with more than 26 keys");
	lt_enabled = 0;
	const void *off = mu3;
	lt_rewrite(&me, &off);
	lt_enabled = 1, lt_val[0] = 0;
	const void *norel = mu3;
	lt_rewrite(&me, &norel);
	lt_set_release(16, "0.16");
	lf_setup_ptr = 0;
	const void *nowin = mu3;
	lt_rewrite(&me, &nowin);
	lf_setup_ptr = (void *)0x1234;
	check(off == mu3 && norel == mu3 && nowin == mu3, "tags: nothing is added with Tags=0, without a release number, or outside a lobby window");
	td_capable = 0;
	mu = mu3;
	td_rewrite((void *)0x9000, &mu);
	lt_rewrite(&me, &mu);
	td_capable = 1;
	m = mu;
	check(m[0] == 4 && !lstrcmpA(((const char *const *)m[1])[3], "ronfix_ver"), "tags: the key does not depend on the shorter command delay being possible");

	/* 3. who has it: A = this PC and host, B and C other players */
	td_nlob = 3, td_owner = A;
	td_lob[0].id = A, td_lob[0].val = "1", td_lob[0].ver = "16:0.16";
	td_lob[1].id = B, td_lob[1].val = 0, td_lob[1].ver = 0;
	td_lob[2].id = C, td_lob[2].val = 0, td_lob[2].ver = 0;
	zero(slots, sizeof slots);
	lf_strcpy(slots[0].a, A, 48), lf_strcpy(slots[1].a, B, 48), lf_strcpy(slots[2].a, C, 48);
	slots_valid = 1;
	zero(seen, sizeof seen);
	lt_peer(0, "Alpha", A), lt_peer(1, "Bravo", B);
	lt_show = lt_fake_show, lt_nshown = 0;
	cap_clear();
	DWORD t = 500000;
	lt_tick(1, 1, t);
	lt_tick(1, 0, t + 500); /* inside the one-second rhythm: no read */
	check(count_of("LOBBY tag:") == 0 && ltm[0].id[0] && lt_nshown == 0, "tags: a player who has just joined is not judged yet");
	td_lob[1].val = "1", td_lob[1].ver = "16:0.16";
	lt_tick(1, 0, t + 1000);
	lt_tick(1, 0, t + 2000);
	check(count_of("LOBBY tag: Bravo has Accord 0.16") == 1 && lt_nshown == 1 && !lstrcmpA(lt_shown[0], "Bravo has Accord 0.16"),
	      "tags: a player whose member data names a release is announced once, in the log and on screen");
	lt_tick(1, 0, t + 8000);
	int before_name = count_of("without");
	lt_peer(2, "Charlie", C);
	lt_tick(1, 0, t + 9000);
	lt_tick(1, 0, t + 10000);
	check(before_name == 0 && count_of("LOBBY tag: Charlie plays without Accord") == 1,
	      "tags: after the grace period, and once the name is known, a player without the keys is said to play without it, once");
	td_lob[2].val = "1";
	lt_tick(1, 0, t + 11000);
	td_lob[2].ver = "banana";
	lt_tick(1, 0, t + 12000);
	check(count_of("LOBBY tag: Charlie has an older version (RoN-Fix 0.11 to 0.15)") == 1 && count_of("Charlie") == 2,
	      "tags: only ronfix_turn = an older release; an unreadable ronfix_ver still counts as having it, nothing said twice");
	td_lob[2].ver = "17:0.17";
	lt_tick(1, 0, t + 13000);
	check(count_of("LOBBY tag: Charlie has Accord 0.17") == 1, "tags: a newer value for the same player is announced");

	/* 4. the summary when the host's countdown starts */
	td_nlob = 4;
	td_lob[3].id = D, td_lob[3].val = 0, td_lob[3].ver = 0;
	lf_strcpy(slots[3].a, D, 48);
	lt_peer(3, "Delta", D);
	lt_tick(1, 0, t + 20000);
	lt_tick(1, 0, t + 27000);
	slots[0].v[10] = 2;
	lt_tick(1, 0, t + 28000);
	lt_tick(1, 0, t + 29000);
	check(count_of("LOBBY tag: Delta plays without Accord") == 1 && count_of("LOBBY tag: Accord: 3 of 4 players have it; without: Delta") == 1,
	      "tags: at the start countdown one line sums the lobby up and names who plays without it");
	slots[0].v[10] = 1;
	lt_tick(1, 0, t + 30000);
	td_lob[3].val = "2", td_lob[3].ver = "16:0.16";
	slots[0].v[10] = 2;
	lt_tick(1, 0, t + 31000);
	check(count_of("LOBBY tag: Delta has Accord 0.16") == 1 && count_of("LOBBY tag: Accord: every player in this lobby has it") == 1,
	      "tags: a cancelled countdown that starts again is summed up again; everybody having it is said so");
	slots[0].v[10] = 1;

	/* 5. leaving, coming back, a new lobby, switched off, alone */
	td_nlob = 3;
	td_lob[1] = td_lob[2], td_lob[2] = td_lob[3]; /* B has left */
	lt_tick(1, 0, t + 40000);
	td_nlob = 4;
	td_lob[3].id = B, td_lob[3].val = "1", td_lob[3].ver = "16:0.16";
	lt_tick(1, 0, t + 41000);
	check(count_of("LOBBY tag: Bravo has Accord 0.16") == 2, "tags: a player who left and came back is announced again");
	cap_clear();
	lt_tick(1, 1, t + 50000);
	check(count_of("has Accord") == 3 && count_of("Alpha") == 0, "tags: a new lobby window starts over; this PC is never announced to itself");
	cap_clear();
	lt_enabled = 0;
	lt_tick(1, 1, t + 60000);
	lt_tick(1, 0, t + 70000);
	lt_enabled = 1;
	td_nlob = 1;
	slots[0].v[10] = 2;
	lt_tick(1, 1, t + 80000);
	lt_tick(1, 0, t + 81000);
	check(count_of("LOBBY tag:") == 0, "tags: Tags=0 reads and says nothing; alone in a lobby there is no summary");

	slots[0].v[10] = 0;
	lt_show = 0, lt_val[0] = 0, lt_self[0] = 0;
	for (int i = 0; i < 4; i++)
		td_lob[i].ver = 0;
	zero(seen, sizeof seen);
	zero(ltm, sizeof ltm);
}
