/* Unit tests for lobbyline.c (the lobby tags drawn in the lobby's chat list). Included by thunk_test.c after
 * lobbytag_tests.c: it reuses the fake lobby and the fake lobby window pointer. The three game functions are fakes
 * that record what they were called with and in which order. */

static int ll_order[12], ll_n;
static void *ll_setup, *ll_sender, *ll_ref_ctor, *ll_ref_free;
static const void *ll_ref_add, *ll_aux;
static wchar_t ll_text[200];
static void *THISCALL ll_fake_ctor(void *ref, const wchar_t *text, int unused)
{
	(void)unused;
	if (ll_n < 12)
		ll_order[ll_n++] = 1;
	ll_ref_ctor = ref;
	int i = 0;
	for (; text[i] && i < 199; i++)
		ll_text[i] = text[i];
	ll_text[i] = 0;
	return ref;
}
static void THISCALL ll_fake_add(void *setup, const void *text, const void *aux, void *sender)
{
	if (ll_n < 12)
		ll_order[ll_n++] = 2;
	ll_setup = setup, ll_ref_add = text, ll_aux = aux, ll_sender = sender;
}
static void THISCALL ll_fake_free(void *ref)
{
	if (ll_n < 12)
		ll_order[ll_n++] = 3;
	ll_ref_free = ref;
}
static int ll_weq(const wchar_t *a, const wchar_t *b)
{
	for (; *a && *a == *b; a++, b++)
		;
	return *a == *b;
}
static void ll_fill(unsigned char *b, int site, unsigned char reloc)
{
	volatile unsigned char *o = b;
	for (int i = 0; i < 16; i++)
		o[i] = (ls_sites[site].mask >> i & 1) ? ls_sites[site].want[i] : reloc;
}

static void test_lobbyline(void)
{
	static unsigned char s_add[16], s_ctor[16], s_free[16];
	static const char A[] = "AAAA1111AAAA1111", B[] = "BBBB2222BBBB2222";

	/* 1. only the functions this was written against */
	ll_fill(s_add, 0, 0x11), ll_fill(s_ctor, 1, 0x22), ll_fill(s_free, 2, 0);
	int ok = ls_check(s_add, s_ctor, s_free);
	ll_fill(s_add, 0, 0x99), ll_fill(s_ctor, 1, 0x77);
	int reloc = ls_check(s_add, s_ctor, s_free);
	s_add[0] = 0x56;
	int bad1 = ls_check(s_add, s_ctor, s_free);
	s_add[0] = 0x55, s_ctor[15] = 0x89;
	int bad2 = ls_check(s_add, s_ctor, s_free);
	s_ctor[15] = 0x88, s_free[14] = 0x75;
	int bad3 = ls_check(s_add, s_ctor, s_free);
	s_free[14] = 0x74;
	check(ok && reloc && !bad1 && !bad2 && !bad3 && ls_check(s_add, s_ctor, s_free) && !ls_check(0, s_ctor, s_free),
	      "lines: the three game functions must start with the expected bytes; relocated bytes are not compared");

	/* 2. the call: the game's string built, the line added with no sender, our reference released */
	exe_base = (unsigned char *)&lf_setup_ptr - RVA_SETUPWIN_PTR;
	lf_setup_ptr = (void *)0x1234;
	ls_add = ll_fake_add, ls_ctor = ll_fake_ctor, ls_free = ll_fake_free;
	ls_empty = (const void *)0x4321;
	ls_thread = GetCurrentThreadId();
	ls_shown = 0, ll_n = 0;
	ls_show("Bravo has Accord 0.16");
	check(ll_n == 3 && ll_order[0] == 1 && ll_order[1] == 2 && ll_order[2] == 3 && ll_setup == (void *)0x1234 && ll_aux == (const void *)0x4321 &&
	          ll_sender == 0 && ll_ref_ctor && ll_ref_add == ll_ref_ctor && ll_ref_free == ll_ref_ctor && ll_weq(ll_text, L"Bravo has Accord 0.16") &&
	          ls_shown == 1,
	      "lines: one line = game string built, added to the lobby window with the empty second text and no sender, released");
	ll_n = 0;
	ls_show("Zo\xc3\xab plays without Accord");
	check(ll_n == 3 && ll_text[2] == 0x00eb && ll_weq(ll_text + 3, L" plays without Accord"), "lines: a name outside ASCII arrives as the game's wide text");

	/* 3. when it must not be called */
	ll_n = 0;
	lf_setup_ptr = 0;
	ls_show("Bravo has Accord 0.16");
	lf_setup_ptr = (void *)0x1234;
	int no_window = ll_n;
	ls_thread = GetCurrentThreadId() + 1;
	ls_show("Bravo has Accord 0.16");
	int other_thread = ll_n;
	ls_thread = 0;
	ls_show("Bravo has Accord 0.16");
	int no_thread = ll_n;
	ls_thread = GetCurrentThreadId();
	check(no_window == 0 && other_thread == 0 && no_thread == 0, "lines: nothing is called without a lobby window or from another thread than the game's");
	ls_show("123");
	ls_show("");
	int refused = ll_n;
	ls_show("1234");
	check(refused == 0 && ll_n == 3, "lines: texts under 4 characters are never passed on");
	ll_n = 0;
	ls_add = 0;
	ls_show("Bravo has Accord 0.16");
	ls_add = ll_fake_add;
	check(ll_n == 0, "lines: without the game's functions (another build, unexpected code) nothing is called");

	/* 4. through the tags: a player's announcement lands in the lobby window */
	td_get_members = td_fake_members, td_get_member_prop = td_fake_member_prop, td_get_owner = td_fake_owner;
	lf_game_lobby = 0, td_lobby = (void *)0x9000;
	td_nlob = 2, td_owner = A;
	td_lob[0].id = A, td_lob[0].val = "1", td_lob[0].ver = "16:0.16";
	td_lob[1].id = B, td_lob[1].val = "1", td_lob[1].ver = "16:0.16";
	zero(slots, sizeof slots);
	lf_strcpy(slots[0].a, A, 48), lf_strcpy(slots[1].a, B, 48);
	slots_valid = 1;
	zero(seen, sizeof seen);
	lt_peer(0, "Alpha", A), lt_peer(1, "Bravo", B);
	lf_strcpy(lt_self, A, sizeof lt_self);
	lt_enabled = 1, lt_show = ls_show;
	ll_n = 0, ll_text[0] = 0;
	cap_clear();
	lt_tick(1, 1, 900000);
	check(ll_n == 3 && ll_weq(ll_text, L"Bravo has Accord 0.16") && count_of("LOBBY tag: Bravo has Accord 0.16") == 1,
	      "lines: a tag reaches both the log and the lobby window");

	ls_add = 0, ls_ctor = 0, ls_free = 0, ls_empty = 0, ls_thread = 0, lt_show = 0, lt_self[0] = 0;
	for (int i = 0; i < 4; i++)
		td_lob[i].ver = 0;
	zero(seen, sizeof seen);
	zero(ltm, sizeof ltm);
}
