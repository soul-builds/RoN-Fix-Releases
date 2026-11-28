/* Unit tests for gameswitch.c (the gameplay pack per match). Included by thunk_test.c after lobbytag_tests.c: it
 * reuses turndelay_tests.c's fake lobby (members with their ronfix_ver values), the fake lobby window pointer and the
 * log capture. The exe is a buffer laid out like the game's image from 0x582000 to 0xCC3000, so every patch site, hook
 * site, the settings pointer and the damage grid sit at their real addresses; the game's rules reload is two fakes
 * that "read" the XML files through the redirect and "build" the grid; the two stubs are entered as the game enters
 * them. The data files are small files in a scratch folder under %TEMP%. None of this runs the game. */

#define GS_T_LO 0x582000u
#define GS_T_HI 0xCC3000u
#define GS_T_CODE_END 0xA28000u /* up to here the image is code: must come back byte for byte */
static unsigned char gs_img[GS_T_HI - GS_T_LO];
static unsigned char gs_t_rec[0x40]; /* the game-settings record */
static unsigned char *gs_t_exe(void) { return gs_img - (GS_T_LO - 0x400000u); }
static unsigned char *gs_t(unsigned va) { return gs_img + (va - GS_T_LO); }

static void gs_t_put(unsigned va, const char *hex)
{
	volatile unsigned char *p = gs_t(va);
	for (; hex[0] && hex[1]; hex += 2, p++) {
		int hi = hex[0] <= '9' ? hex[0] - '0' : hex[0] - 'a' + 10, lo = hex[1] <= '9' ? hex[1] - '0' : hex[1] - 'a' + 10;
		*p = (unsigned char)(hi << 4 | lo);
	}
}
static void gs_t_abs(unsigned va, unsigned target) { *(unsigned *)gs_t(va) = (unsigned)(UINT_PTR)(gs_t_exe() + (target - 0x400000u)); }
/* the image as the game has it: the 8 fix sites, the two set-up calls, the XML loader's entry, the two reload functions */
static void gs_t_fill(void)
{
	volatile unsigned char *z = gs_img;
	for (unsigned i = 0; i < sizeof gs_img; i++)
		z[i] = 0xcc;
	for (int i = 0; i < GS_NSITES; i++) {
		gs_t_put(gs_sites[i].va, gs_sites[i].off);
		if (gs_sites[i].ctx)
			gs_t_put(gs_sites[i].ctx_va, gs_sites[i].ctx);
	}
	gs_t_put(0x584637, "6683b84405000000756ce8caf549008bf085f6");
	gs_t_put(0x5846AD, "6800000105b9"), gs_t_abs(0x5846B3, 0xEB4FD0), gs_t_put(0x5846B7, "e854d52300");
	gs_t_put(GS_VA_XML, "558bec6aff68"), gs_t_abs(GS_VA_XML + 6, 0xAB0243), gs_t_put(GS_VA_XML + 10, "64a10000000050");
	gs_t_put(GS_VA_UNLOAD, "a1"), gs_t_abs(GS_VA_UNLOAD + 1, 0xC061EC), gs_t_put(GS_VA_UNLOAD + 5, "8b15");
	gs_t_abs(GS_VA_UNLOAD + 7, 0xCAB394), gs_t_put(GS_VA_UNLOAD + 11, "56be01000000");
	gs_t_put(GS_VA_LOAD, "558bec64a1000000006aff68");
	*(unsigned char **)gs_t(GS_VA_SETTINGS) = gs_t_rec;
	*gs_t(GS_VA_STRTAG) = 0x5a;
}
static unsigned gs_t_code_hash(void)
{
	const unsigned char *p = gs_t(GS_T_LO);
	unsigned h = 2166136261u;
	for (unsigned i = 0; i < GS_T_CODE_END - GS_T_LO; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}
static void gs_t_grid(unsigned char b)
{
	volatile unsigned char *g = gs_t(GS_VA_GRID);
	for (int i = 0; i < GS_GRID_BYTES; i++)
		g[i] = b;
}
static unsigned gs_t_grid_of(unsigned char b)
{
	unsigned h = 2166136261u;
	for (int i = 0; i < GS_GRID_BYTES; i++)
		h = (h ^ b) * 16777619u;
	return h;
}

/* ---------------------------------------------------------------- game strings, as the XML loader gets them */
static unsigned short gs_t_w[40];
static const unsigned short *gs_t_wp;
static struct gs_str gs_t_s;
static struct gs_str *gs_t_str(const char *text, int start, int ptrform)
{
	int n = 0;
	for (; text[n]; n++)
		gs_t_w[n] = (unsigned char)text[n];
	gs_t_w[n] = 0;
	gs_t_wp = gs_t_w;
	gs_t_s.p = ptrform ? (const unsigned short *)(const void *)&gs_t_wp : gs_t_w;
	gs_t_s.cap = (unsigned short)n, gs_t_s.start = (unsigned short)start, gs_t_s.len = (unsigned short)(n - start);
	gs_t_s.flags = ptrform ? 0 : 1, gs_t_s.tag = 0x5a, gs_t_s.z0 = gs_t_s.z1 = 0;
	return &gs_t_s;
}
/* which of the 13 pack files the loader would open for that name; -1 = the name is left alone */
static int gs_t_maps(const char *text, int start, int ptrform)
{
	void *slot = gs_t_str(text, start, ptrform), *was = slot;
	gs_xml_name(&slot);
	return slot == was ? -1 : (int)((struct gs_str *)slot - gs_names);
}

/* ---------------------------------------------------------------- the game's rules reload, faked */
static int gs_t_unloads, gs_t_loads, gs_t_order, gs_t_flagsum, gs_t_rec_first, gs_t_redirect_seen, gs_t_from_pack;
static unsigned gs_t_f_seen;
static int gs_t_break_grid, gs_t_skip_file = -1;
static int gs_t_rec_is(int a, int b, int c, int d) { return gs_t_rec[0x2A] == a && gs_t_rec[0x34] == b && gs_t_rec[0x35] == c && gs_t_rec[0x36] == d; }
static void __fastcall gs_t_unload(int flag)
{
	gs_t_unloads++, gs_t_order = gs_t_order * 10 + 1, gs_t_flagsum += flag;
	gs_t_rec_first = gs_t_rec_is(0, 0, 0, 7);
}
static void __fastcall gs_t_load(int flag)
{
	gs_t_loads++, gs_t_order = gs_t_order * 10 + 2, gs_t_flagsum += flag;
	gs_t_rec_first &= gs_t_rec_is(0, 0, 0, 7);
	gs_t_redirect_seen = gs_redirect;
	gs_t_f_seen = *(unsigned *)gs_t(0x582B6B); /* fix F's operand while the damage grid is built */
	gs_t_from_pack = 0;
	for (int i = 0; i < GS_NFILES; i++) /* the loader is asked for every rules file by its bare name */
		if (i != gs_t_skip_file)
			gs_t_from_pack += gs_t_maps(gs_files[i], 0, i & 1) == i;
	gs_t_maps("techrules.xml", 0, 0);
	gs_t_grid(gs_t_break_grid && gs_redirect ? 0x33 : gs_t_from_pack && gs_t_f_seen == 0x4894 ? 0x22 : 0x11);
}

/* ---------------------------------------------------------------- the stubs, entered as the game enters them */
void *gs_t_stub;
int gs_t_reg[7], gs_t_esp0, gs_t_esp1, gs_t_esp_t, gs_t_ret, gs_t_tcalls, gs_t_after[3];
extern char gs_t_target[] __asm__("gs_t_target");
/* the game's `call 0xA23C10` with known register values; gs_t_target stands for 0xA23C10 */
__attribute__((naked, used)) static void gs_run_setup(void)
{
	__asm__ volatile(
		"pushl %ebx\n\t"
		"pushl %esi\n\t"
		"pushl %edi\n\t"
		"pushl %ebp\n\t"
		"movl $0x11111111, %eax\n\t"
		"movl $0x22222222, %ecx\n\t"
		"movl $0x33333333, %edx\n\t"
		"movl $0x44444444, %ebx\n\t"
		"movl $0x55555555, %esi\n\t"
		"movl $0x66666666, %edi\n\t"
		"movl $0x77777777, %ebp\n\t"
		"movl %esp, _gs_t_esp0\n\t"
		"call *_gs_t_stub\n\t"
		"movl %esp, _gs_t_esp1\n\t"
		"movl %eax, _gs_t_ret\n\t"
		"movl %ebx, _gs_t_after\n\t"
		"movl %esi, _gs_t_after+4\n\t"
		"movl %edi, _gs_t_after+8\n\t"
		"popl %ebp\n\t"
		"popl %edi\n\t"
		"popl %esi\n\t"
		"popl %ebx\n\t"
		"ret\n"
		".globl gs_t_target\n"
		"gs_t_target:\n\t"
		"movl %eax, _gs_t_reg\n\t"
		"movl %ecx, _gs_t_reg+4\n\t"
		"movl %edx, _gs_t_reg+8\n\t"
		"movl %ebx, _gs_t_reg+12\n\t"
		"movl %esi, _gs_t_reg+16\n\t"
		"movl %edi, _gs_t_reg+20\n\t"
		"movl %ebp, _gs_t_reg+24\n\t"
		"movl %esp, _gs_t_esp_t\n\t"
		"incl _gs_t_tcalls\n\t"
		"movl $0xABCD, %eax\n\t"
		"ret\n");
}
static int gs_t_setup(void *stub) /* 1 = the game's call arrived at its target exactly as it was made */
{
	gs_t_stub = stub, gs_t_tcalls = 0;
	gs_run_setup();
	return gs_t_tcalls == 1 && gs_t_reg[0] == 0x11111111 && gs_t_reg[1] == 0x22222222 && gs_t_reg[2] == 0x33333333 && gs_t_reg[3] == 0x44444444 &&
	       gs_t_reg[4] == 0x55555555 && gs_t_reg[5] == 0x66666666 && gs_t_reg[6] == 0x77777777 && gs_t_esp_t == gs_t_esp0 - 4 && gs_t_esp1 == gs_t_esp0 &&
	       gs_t_ret == 0xABCD && gs_t_after[0] == 0x44444444 && gs_t_after[1] == 0x55555555 && gs_t_after[2] == 0x66666666;
}

void *gs_t_xml;
int gs_t_x[6], gs_t_xafter[4];
extern char gs_t_xml_back[] __asm__("gs_t_xml_back");
/* a caller of the XML loader: this in ECX, three arguments on the stack, the loader takes them off (ret 12).
 * gs_t_xml_back stands for 0xA279E5, the loader's sixth byte. */
__attribute__((naked, used)) static int gs_run_xml(void *self, void *res, void *name, void *third)
{
	__asm__ volatile(
		"pushl %ebp\n\t"
		"movl %esp, %ebp\n\t"
		"pushl %ebx\n\t"
		"pushl %esi\n\t"
		"pushl %edi\n\t"
		"movl 8(%ebp), %ecx\n\t"
		"movl $0x44444444, %ebx\n\t"
		"movl $0x55555555, %esi\n\t"
		"movl $0x66666666, %edi\n\t"
		"pushl 20(%ebp)\n\t"
		"pushl 16(%ebp)\n\t"
		"pushl 12(%ebp)\n\t"
		"call *_gs_t_xml\n\t"
		"movl %ebx, _gs_t_xafter\n\t"
		"movl %esi, _gs_t_xafter+4\n\t"
		"movl %edi, _gs_t_xafter+8\n\t"
		"leal -12(%ebp), %edx\n\t"
		"subl %esp, %edx\n\t"
		"movl %edx, _gs_t_xafter+12\n\t" /* 0 = the three arguments are off the stack and nothing else moved */
		"popl %edi\n\t"
		"popl %esi\n\t"
		"popl %ebx\n\t"
		"popl %ebp\n\t"
		"ret\n"
		".globl gs_t_xml_back\n"
		"gs_t_xml_back:\n\t"
		"movl %ecx, _gs_t_x\n\t"
		"movl 8(%ebp), %eax\n\t"
		"movl %eax, _gs_t_x+4\n\t"
		"movl 12(%ebp), %eax\n\t"
		"movl %eax, _gs_t_x+8\n\t"
		"movl 16(%ebp), %eax\n\t"
		"movl %eax, _gs_t_x+12\n\t"
		"movl (%esp), %eax\n\t"
		"movl %eax, _gs_t_x+16\n\t"      /* the loader's own push -1 */
		"leal 4(%esp), %eax\n\t"
		"subl %ebp, %eax\n\t"
		"movl %eax, _gs_t_x+20\n\t"      /* 0 = ebp is the frame the loader's push ebp / mov ebp, esp makes */
		"addl $4, %esp\n\t"
		"popl %ebp\n\t"
		"movl $0x1234, %eax\n\t"
		"ret $12\n");
}

/* ---------------------------------------------------------------- the data files, in a scratch folder */
static char gs_t_dir[MAX_PATH];
static const char *const gs_t_dirs[5] = { "", "Data\\", "ronfix\\", "ronfix\\gameplay1\\", GS_DIR };
static void gs_t_path(char *out, const char *sub, const char *file)
{
	lstrcpyA(out, gs_t_dir);
	lstrcatA(out, sub);
	lstrcatA(out, file);
}
/* writes the 13 files and returns their fingerprint, computed here from what was written */
static unsigned gs_t_files(const char *sub, const char *tag, unsigned *two)
{
	unsigned h = 2166136261u;
	for (int i = 0; i < GS_NFILES; i++) {
		char p[MAX_PATH], text[80];
		DWORD w;
		int n = wsprintfA(text, "<%s file=\"%s\"/>\r\n", tag, gs_files[i]);
		gs_t_path(p, sub, gs_files[i]);
		HANDLE f = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (f != INVALID_HANDLE_VALUE) {
			WriteFile(f, text, (DWORD)n, &w, NULL);
			CloseHandle(f);
		}
		for (int k = 0; k < n; k++)
			h = (h ^ (unsigned char)text[k]) * 16777619u;
		if (i == 1)
			*two = h;
	}
	return h;
}
static void gs_t_cleanup(void)
{
	char p[MAX_PATH];
	for (int d = 1; d <= 4; d += 3)
		for (int i = 0; i < GS_NFILES; i++) {
			gs_t_path(p, gs_t_dirs[d], gs_files[i]);
			DeleteFileA(p);
		}
	for (int d = 4; d >= 0; d--) {
		gs_t_path(p, gs_t_dirs[d], "");
		RemoveDirectoryA(p);
	}
}

static unsigned char gs_t_peer[2][0x40]; /* network player objects: flags at PEER_FLAGS (4 = this PC) */
static void gs_t_lobby(const char *a, const char *b) /* A = this PC, B = a friend; what each says in ronfix_ver */
{
	td_lobby_ab("1", "1");
	td_lob[0].ver = a, td_lob[1].ver = b;
}
static void gs_t_ready(void) /* this PC takes part again (after a test made it step back) */
{
	gs_ready = 1, gs_loaded = 0, gs_redirect = 0;
	gs_tag();
}

static void test_gameswitch(void)
{
	char why[128], p[MAX_PATH];
	unsigned all = 0, two = 0, want_orig, want_pack, two_orig = 0, two_pack = 0;
	int bad = -1;
	exe_base = (unsigned char *)&lf_setup_ptr - RVA_SETUPWIN_PTR;
	lf_setup_ptr = (void *)0x1234;
	td_get_members = td_fake_members, td_get_member_prop = td_fake_member_prop, td_get_owner = td_fake_owner;
	lf_game_lobby = 0, td_lobby = (void *)0x9000;
	lt_enabled = 1, lt_show = 0;
	zero(seen, sizeof seen);
	gamefix_version = 0;

	/* 1. what this PC says about itself, and what it accepts from others */
	lt_set_release(19, "0.19");
	gs_ready = 1, gs_tag();
	int v1 = !lstrcmpA(lt_val, "19:0.19:g1");
	gs_tag();
	int v2 = !lstrcmpA(lt_val, "19:0.19:g1");
	gs_ready = 0, gs_tag();
	int v3 = !lstrcmpA(lt_val, "19:0.19");
	lt_set_release(19, "");
	gs_ready = 1, gs_tag();
	int v4 = !lstrcmpA(lt_val, "19::g1") && lt_parse(lt_val, why, 16) == 19 && gs_parse(lt_val) == 1;
	lt_val[0] = 0, gs_tag();
	check(v1 && v2 && v3 && v4 && !lt_val[0],
	      "switch: our lobby value gets a third part g1 while this PC takes part, loses it when not, and nothing is said without a release number");
	check(gs_parse("19:0.19:g1") == 1 && gs_parse("20:0.20:g2") == 2 && gs_parse("19:0.19:g1:later") == 1 && gs_parse("19:0.19:g12") == 12,
	      "switch: a member's value names the pack it has ready; a later release may add a fourth part");
	check(!gs_parse("19:0.19") && !gs_parse("19") && !gs_parse("") && !gs_parse("19:0.19:") && !gs_parse("19:0.19:g") && !gs_parse("19:0.19:g0") &&
	          !gs_parse("19:0.19:g1x") && !gs_parse("19:0.19:x1") && !gs_parse("19:0.19:g123") && !gs_parse("banana:x:g1") && !gs_parse("19:<b>:g1"),
	      "switch: no third part, junk in it, or a value that is no release at all means no pack");
	gs_ready = 0;

	/* 2. the fingerprint of the 13 data files */
	GetTempPathA(MAX_PATH - 80, gs_t_dir);
	wsprintfA(gs_t_dir + lstrlenA(gs_t_dir), "ronfix-gs-test-%lu\\", GetCurrentProcessId());
	for (int d = 0; d < 5; d++) {
		gs_t_path(p, gs_t_dirs[d], "");
		CreateDirectoryA(p, NULL);
	}
	want_orig = gs_t_files("Data\\", "original", &two_orig);
	want_pack = gs_t_files(GS_DIR, "pack", &two_pack);
	gs_t_path(p, GS_DIR, "");
	int f1 = gs_fingerprint(p, &all, &two) && all == want_pack && two == two_pack && want_pack != want_orig;
	gs_t_path(p, "Data\\", "");
	int f2 = gs_fingerprint(p, &all, &two) && all == want_orig && two == two_orig;
	check(f1 && f2, "switch: the fingerprint is FNV-1a 32 over the 13 files in a fixed order, balance.xml and unitrules.xml first");
	gs_t_path(p, "Data\\", "help.xml");
	DeleteFileA(p);
	gs_t_path(p, "Data\\", "");
	check(!gs_fingerprint(p, &all, &two), "switch: a missing file means no fingerprint");
	gs_t_files("Data\\", "original", &two_orig);
	gs_want_orig = want_orig, gs_want_pack = want_pack;

	/* 3. only a PC where everything is as expected takes part; any other PC changes nothing in the game */
	gs_t_fill();
	unsigned h0 = gs_t_code_hash();
	lt_set_release(19, "0.19");
	gs_switch = 0;
	gs_init(gs_t_exe(), gs_t_dir);
	check(!gs_ready && gs_t_code_hash() == h0 && !lstrcmpA(lt_val, "19:0.19"), "switch: with Switch=0 nothing is written and nothing is announced");
	gs_switch = 1, gamefix_version = 1;
	gs_init(gs_t_exe(), gs_t_dir);
	int c1 = !gs_ready && gs_t_code_hash() == h0 && has_ci(gs_not, "classic gameplay pack");
	gamefix_version = -1;
	gs_init(gs_t_exe(), gs_t_dir);
	check(c1 && !gs_ready && gs_t_code_hash() == h0, "switch: the classic pack ([gamefix] Version=1, also when its files went missing) wins: nothing written");
	gamefix_version = 0;
	int refused = 0;
	for (int i = 0; i < GS_NSITES; i++) { /* one byte off at each of the 8 fix sites in turn */
		*gs_t(gs_sites[i].va + gs_sites[i].n - 1) ^= 0x40;
		gs_init(gs_t_exe(), gs_t_dir);
		*gs_t(gs_sites[i].va + gs_sites[i].n - 1) ^= 0x40;
		refused += !gs_ready && gs_t_code_hash() == h0 && has_ci(gs_not, "unexpected code at a fix site");
	}
	*gs_t(0x6DC73C) ^= 1; /* G3: the instruction before the site */
	gs_init(gs_t_exe(), gs_t_dir);
	*gs_t(0x6DC73C) ^= 1;
	check(refused == GS_NSITES && !gs_ready && gs_t_code_hash() == h0,
	      "switch: an unexpected byte at any of the 8 fix sites, or in the code around one -> this PC does not take part, nothing written");
	static const unsigned odd_at[6] = { 0x584642, 0x5846B4, 0x5846B8, GS_VA_XML + 3, GS_VA_UNLOAD + 12, GS_VA_LOAD + 4 };
	refused = 0;
	for (int i = 0; i < 6; i++) {
		*gs_t(odd_at[i]) ^= 0x10;
		gs_init(gs_t_exe(), gs_t_dir);
		*gs_t(odd_at[i]) ^= 0x10;
		refused += !gs_ready && gs_t_code_hash() == h0 && has_ci(gs_not, "unexpected code in");
	}
	check(refused == 6, "switch: unexpected code at a set-up call, in the XML loader's entry or in a reload function -> does not take part, nothing written");
	gs_want_orig ^= 1;
	gs_init(gs_t_exe(), gs_t_dir);
	int d1 = !gs_ready && gs_t_code_hash() == h0 && has_ci(gs_not, "not the original ones");
	gs_want_orig ^= 1, gs_want_pack ^= 1;
	gs_init(gs_t_exe(), gs_t_dir);
	int d2 = !gs_ready && gs_t_code_hash() == h0 && has_ci(gs_not, "are not pack 1");
	gs_want_pack ^= 1;
	gs_t_path(p, GS_DIR, "soundtypes.xml");
	DeleteFileA(p);
	gs_init(gs_t_exe(), gs_t_dir);
	int d3 = !gs_ready && gs_t_code_hash() == h0 && has_ci(gs_not, "are not (all) in ronfix");
	gs_t_files(GS_DIR, "pack", &two_pack);
	check(d1 && d2 && d3 && !lstrcmpA(lt_val, "19:0.19"),
	      "switch: other data files in Data, other files in ronfix\\gameplay1, or one of the 13 missing there -> does not take part, nothing written");
	gs_init(gs_t_exe(), gs_t_dir);
	unsigned char *xa = gs_t(GS_VA_XML), *sa = gs_t(GS_VA_SETUP_A), *sb = gs_t(GS_VA_SETUP_B);
	check(gs_ready && !lstrcmpA(lt_val, "19:0.19:g1") && gs_sites_state(&bad) == 0 && !gs_loaded && !gs_redirect,
	      "switch: everything as expected -> this PC takes part and says g1; the rules and the 8 fix sites stay the original");
	check(xa[0] == 0xE9 && xa + 5 + *(int *)(xa + 1) == (unsigned char *)(void *)gs_xml_stub && gs_xml_back == xa + 5 && xa[5] == 0x68 &&
	          sa[0] == 0xE8 && sa + 5 + *(int *)(sa + 1) == (unsigned char *)(void *)gs_setup_a && gs_setup_a_orig == gs_t_exe() + (0xA23C10 - 0x400000) &&
	          sb[0] == 0xE8 && sb + 5 + *(int *)(sb + 1) == (unsigned char *)(void *)gs_setup_b && gs_setup_b_orig == gs_t_exe() + (0x7C1C10 - 0x400000) &&
	          gs_fn_unload == (gs_reload_fn)(void *)gs_t(GS_VA_UNLOAD) && gs_fn_load == (gs_reload_fn)(void *)gs_t(GS_VA_LOAD),
	      "switch: the three hooks: both set-up calls go through the stubs to their own targets, the XML loader's first five bytes are the jump");
	unsigned h1 = gs_t_code_hash(); /* the image with the hooks in: what it must be again after every switch back */

	/* 4. the code fixes: written for the pack, the game's own bytes restored after */
	int w1 = gs_write_sites(1) && gs_sites_state(&bad) == 1;
	unsigned char *g2 = gs_t(0x738F52), *g3 = gs_t(0x6DC743), *g4 = gs_t(0x737398), *g7 = gs_t(0x5AD603), *hh = gs_t(0x627DBF);
	check(w1 && *(unsigned *)gs_t(0x582B6B) == 0x4894 && *gs_t(0x582B6A) == 0x05 && *gs_t(0x609ABC) == 0xEB && *gs_t(0x624A9E) == 0xBC &&
	          *gs_t(0x624A9D) == 0x05 && *gs_t(0x624A9F) == 0x16,
	      "switch: pack code: fix F's operand 0x4894, G1 jmp, G5 notice 0x16BC, nothing beside them touched");
	check(g2[0] == 0xE9 && g2 + 5 + *(int *)(g2 + 1) == (unsigned char *)(void *)g2_thunk && g2[5] == 0x90 && g2[6] == 0x90 && g2[7] == 0xcc &&
	          g3[0] == 0xE9 && g3 + 5 + *(int *)(g3 + 1) == (unsigned char *)(void *)g3_thunk && g3[5] == 0x90 && g3[8] == 0x90 && g3[9] == 0xcc &&
	          g4[0] == 0xE9 && g4 + 5 + *(int *)(g4 + 1) == (unsigned char *)(void *)g4_thunk && g4[5] == 0xcc &&
	          g7[0] == 0xE9 && g7 + 5 + *(int *)(g7 + 1) == (unsigned char *)(void *)g7_thunk && g7[5] == 0xcc &&
	          hh[0] == 0xE9 && hh + 5 + *(int *)(hh + 1) == (unsigned char *)(void *)h1_thunk && hh[5] == 0x90 && hh[9] == 0x90 && hh[10] == 0xcc,
	      "switch: pack code: G2, G3, G4, G7 and H1 are jumps to their thunks, padded with NOPs to the end of the replaced instructions");
	check(rf_g2_back == gs_t(0x738F59) && rf_g3_wgloop == gs_t(0x6DC74C) && rf_g4_orig == gs_t(0x6DC490) && rf_g7_back == gs_t(0x5AD608) &&
	          rf_h1_back == gs_t(0x627DC9) && rf_h1_players == gs_t(0xE3A390),
	      "switch: the thunks return into the game at the addresses the classic pack uses");
	*gs_t(0x737398) = 0xE8;
	int mixed = gs_sites_state(&bad) == -1;
	check(gs_write_sites(0) && gs_sites_state(&bad) == 0 && gs_t_code_hash() == h1 && mixed,
	      "switch: restoring gives the image back byte for byte; a half-written state is recognised as neither");

	/* 5. the file name the XML loader gets */
	gs_redirect = 0;
	int m0 = gs_t_maps("unitrules.xml", 0, 0) == -1;
	gs_redirect = 1;
	void *slot = gs_t_str("unitrules.xml", 0, 0);
	gs_xml_name(&slot);
	const struct gs_str *r = slot;
	static const unsigned short wantw[] = L"ronfix\\gameplay1\\Data\\unitrules.xml";
	check(m0 && r == &gs_names[1] && r->len == 35 && r->cap == 35 && r->start == 0 && r->flags == 1 && r->tag == 0x5a && !r->z0 && !r->z1 &&
	          bytes_eq(r->p, wantw, sizeof wantw) && sizeof(struct gs_str) == 0x14,
	      "switch: while the pack is wanted, unitrules.xml becomes a game string naming ronfix\\gameplay1\\Data\\unitrules.xml; not otherwise");
	int all13 = 1;
	for (int i = 0; i < GS_NFILES; i++)
		all13 &= gs_t_maps(gs_files[i], 0, 0) == i;
	check(all13 && gs_t_maps("TYPENAMES.XML", 0, 0) == 11 && gs_t_maps("Balance.xml", 0, 1) == 0 && gs_t_maps("xx:rules.xml", 3, 1) == 9 &&
	          gs_t_maps("12345help.xml", 5, 0) == 5,
	      "switch: all 13 names map, in any letter case, for both forms of a game string and with a start offset");
	check(gs_t_maps("techrules.xml", 0, 0) == -1 && gs_t_maps("data\\unitrules.xml", 0, 0) == -1 && gs_t_maps("unitrules.xmlx", 0, 0) == -1 &&
	          gs_t_maps("unitrules.xm", 0, 0) == -1 && gs_t_maps("", 0, 0) == -1 && gs_t_maps("anim_graphics.xml", 0, 0) == -1 &&
	          gs_t_maps("a_file_name_that_is_far_too_long.xml", 0, 0) == -1,
	      "switch: files the pack does not carry, names with a folder, and near misses are left alone");

	/* 6. the XML loader's entry stub, entered as the game's callers enter the loader */
	gs_t_xml = (void *)gs_xml_stub, gs_xml_back = gs_t_xml_back;
	struct gs_str *nm = gs_t_str("rules.xml", 0, 0);
	int ret = gs_run_xml((void *)0xC0FFEE, (void *)0x1001, nm, (void *)0x3003);
	check(ret == 0x1234 && gs_t_x[0] == 0xC0FFEE && gs_t_x[1] == 0x1001 && gs_t_x[2] == (int)(UINT_PTR)&gs_names[9] && gs_t_x[3] == 0x3003 &&
	          gs_t_x[4] == -1 && gs_t_x[5] == 0 && gs_t_xafter[0] == 0x44444444 && gs_t_xafter[1] == 0x55555555 && gs_t_xafter[2] == 0x66666666 &&
	          gs_t_xafter[3] == 0,
	      "switch: XML stub: only the file name argument changes; this, the other arguments, registers and stack reach the loader as sent");
	gs_redirect = 0;
	nm = gs_t_str("rules.xml", 0, 0);
	ret = gs_run_xml((void *)0xC0FFEE, (void *)0x1001, nm, (void *)0x3003);
	check(ret == 0x1234 && gs_t_x[2] == (int)(UINT_PTR)nm && gs_t_x[0] == 0xC0FFEE && gs_t_x[4] == -1 && gs_t_xafter[3] == 0,
	      "switch: XML stub: with the original rules wanted the call passes through unchanged");
	gs_xml_back = gs_t(GS_VA_XML) + 5;

	/* 7. the decision, from data every client sees */
	static const char G1[] = "19:0.19:g1";
	gs_t_lobby(G1, G1);
	check(gs_decide((void *)0x9000, why, sizeof why) == 1, "switch: every lobby member says g1 -> the pack");
	gs_t_lobby(G1, "19:0.19");
	check(!gs_decide((void *)0x9000, why, sizeof why) && !lstrcmpA(why, "BBBB2222BBBB2222 has no gameplay pack 1 ready (or has not said so yet)"),
	      "switch: one player with Accord but without the pack -> the original game, and who");
	gs_t_lobby(G1, 0);
	int n1 = !gs_decide((void *)0x9000, why, sizeof why);
	gs_t_lobby(G1, "20:0.20:g2");
	int n2 = !gs_decide((void *)0x9000, why, sizeof why);
	gs_t_lobby(0, G1);
	check(n1 && n2 && !gs_decide((void *)0x9000, why, sizeof why),
	      "switch: a player without Accord, with another pack, or the host without it -> the original game (there is no host switch)");
	gs_t_lobby(G1, G1);
	lf_strcpy(slots[3].a, "CCCC3333CCCC3333", 48);
	int s1 = !gs_decide((void *)0x9000, why, sizeof why) && !lstrcmpA(why, "slot 3 (CCCC3333CCCC3333) is not a lobby member with gameplay pack 1");
	gs_t_lobby(G1, G1);
	lf_strcpy(slots[4].b, "76561198000000009", 48);
	check(s1 && !gs_decide((void *)0x9000, why, sizeof why), "switch: a human in a slot who is not a lobby member with the pack -> the original game");
	gs_t_lobby(G1, G1);
	int o1 = !gs_decide(0, why, sizeof why) && !lstrcmpA(why, "game lobby not known yet");
	slots_valid = 0;
	int o2 = !gs_decide((void *)0x9000, why, sizeof why);
	gs_t_lobby(G1, G1);
	td_nlob = 1;
	zero(&slots[1], sizeof slots[1]);
	int o3 = gs_decide((void *)0x9000, why, sizeof why) == 1;
	gs_ready = 0, lf_strcpy(gs_not, "for a test", sizeof gs_not);
	check(o1 && o2 && o3 && !gs_decide((void *)0x9000, why, sizeof why) && !lstrcmpA(why, "for a test"),
	      "switch: no lobby or slot table yet -> original; alone with AI players -> the pack; a PC that does not take part -> original, with its reason");
	gs_t_ready();

	/* 8. in the lobby window: decided at once, withdrawn when someone without it joins, frozen in the countdown */
	DWORD t = GetTickCount() - 20000;
	gs_fn_unload = gs_t_unload, gs_fn_load = gs_t_load;
	gs_setup_a_orig = gs_t_target, gs_setup_b_orig = gs_t_target;
	gs_grid_want[0] = gs_t_grid_of(0x11), gs_grid_want[1] = gs_t_grid_of(0x22);
	gs_t_grid(0x11);
	gs_t_rec[0x2A] = 5, gs_t_rec[0x34] = 1, gs_t_rec[0x35] = 2, gs_t_rec[0x36] = 0; /* as in the menu, not as at the first load */
	gs_t_peer[0][PEER_FLAGS] = 4, gs_t_peer[1][PEER_FLAGS] = 0;
	seen[0].ptr = gs_t_peer[0], seen[1].ptr = gs_t_peer[1];
	gs_t_lobby(G1, G1);
	cap_clear();
	gs_tick(1, 0, 1, t);
	gs_tick(1, 0, 0, t + 300);
	check(gs_decided == 1 && count_of("GAMEPLAY pack 1 ON for this lobby") == 1 && gs_sites_state(&bad) == 0 && !gs_loaded && !gs_t_loads,
	      "switch: agreed in the lobby -> logged; nothing is loaded or patched in the lobby");
	unsigned reads = td_reads;
	gs_tick(1, 0, 0, t + 350);
	gs_tick(1, 0, 0, t + 600);
	check(td_reads > reads && count_of("GAMEPLAY pack 1 ON for this lobby") == 1, "switch: read again every 250 ms, an unchanged decision is not logged again");
	td_lob[1].ver = "19:0.19";
	gs_tick(1, 0, 0, t + 900);
	int off1 = gs_decided == 0 && count_of("GAMEPLAY pack off for this lobby: BBBB2222BBBB2222 has no gameplay pack 1 ready") == 1;
	td_lob[1].ver = G1;
	gs_tick(1, 0, 0, t + 1200);
	check(off1 && gs_decided == 1 && count_of("GAMEPLAY pack 1 ON for this lobby") == 2, "switch: a player without it joins -> original, and who; he leaves -> the pack again");
	slots[0].v[10] = 2; /* the host starts the countdown */
	gs_tick(1, 0, 0, t + 1500);
	td_lob[1].ver = 0;
	reads = td_reads;
	gs_tick(1, 0, 0, t + 1800);
	check(gs_frozen && gs_pending && gs_decided == 1 && td_reads == reads && count_of("GAMEPLAY start countdown: the gameplay pack for this match (final)") == 1,
	      "switch: countdown -> the decision is final and logged; no more reads, a late change moves nothing");
	td_lob[1].ver = G1;

	/* 9. session set-up of that match: the stub loads the pack, exactly once, and the game's call goes on untouched */
	gs_t_unloads = gs_t_loads = gs_t_order = gs_t_flagsum = 0;
	int reg = gs_t_setup((void *)gs_setup_a);
	check(reg, "switch: set-up stub: the game's call reaches its target with every register and the stack as the game left them, and returns its result");
	check(gs_loaded == 1 && gs_t_order == 12 && gs_t_flagsum == 0 && gs_t_rec_first && gs_t_rec_is(5, 1, 2, 0),
	      "switch: the reload is the game's unload then load, both with flag 0, with the settings bytes 0,0,0,7 during it and put back after");
	check(gs_t_f_seen == 0x4894 && gs_t_redirect_seen == 1 && gs_t_from_pack == GS_NFILES && gs_sites_state(&bad) == 1 && gs_redirect == 1 &&
	          gs_grid_seen == gs_grid_want[1] && count_of("GAMEPLAY pack 1 loaded for this session") == 1,
	      "switch: the code fixes are in before the rules load, all 13 files come from the pack, the damage grid is the pack's: logged");
	gs_tick(0, 1, 0, t + 2100); /* the lobby window closes: the match loads */
	gs_tick(0, 0, 0, t + 30000);
	check(count_of("GAMEPLAY note") == 0 && !gs_await, "switch: a match whose set-up was seen leaves no warning behind");
	int again = gs_t_loads;
	reg = gs_t_setup((void *)gs_setup_a); /* a second set-up from the same lobby window is not that match */
	check(reg && gs_loaded == 0 && gs_t_loads == again + 1 && gs_redirect == 0 && gs_sites_state(&bad) == 0 && gs_t_code_hash() == h1 &&
	          gs_grid_seen == gs_grid_want[0] && gs_t_rec_is(5, 1, 2, 0) && count_of("other players are connected, but no start countdown was seen") == 1 &&
	          count_of("GAMEPLAY original rules loaded again") == 1,
	      "switch: a set-up with other players but no countdown -> the original: redirect off, the image byte for byte as before, the grid the original's");

	/* 10. the next lobby does not agree: the match starts with the original rules, nothing is reloaded */
	slots[0].v[10] = 0;
	gs_tick(1, 0, 1, t + 40000);
	td_lob[1].ver = "19:0.19";
	gs_tick(1, 0, 0, t + 40300);
	slots[0].v[10] = 5;
	gs_tick(1, 0, 0, t + 40600);
	again = gs_t_loads;
	cap_clear();
	gs_pending_at = GetTickCount();
	reg = gs_t_setup((void *)gs_setup_a);
	check(reg && gs_loaded == 0 && gs_t_loads == again && count_of("GAMEPLAY session set-up: the original game, already loaded (multiplayer: BBBB2222BBBB2222 has no") == 1,
	      "switch: a lobby that did not agree -> the original game stays loaded, no reload, and the log says why");
	slots[0].v[10] = 0;

	/* 11. a cancelled countdown switched nothing; a lobby that closes in its countdown without a set-up is noted */
	gs_tick(1, 0, 1, t + 50000);
	td_lob[1].ver = G1;
	gs_tick(1, 0, 0, t + 50300);
	slots[0].v[10] = 3;
	gs_tick(1, 0, 0, t + 50600);
	int pend = gs_pending;
	slots[0].v[10] = 1; /* the host cancels */
	gs_tick(1, 0, 0, t + 50900);
	check(pend && !gs_pending && !gs_frozen && gs_loaded == 0 && gs_t_loads == again, "switch: a cancelled countdown: nothing was switched, nothing is pending");
	slots[0].v[10] = 4;
	gs_tick(1, 0, 0, t + 51200);
	cap_clear();
	gs_tick(0, 1, 0, t + 51500);
	gs_tick(0, 0, 0, t + 60000);
	int early = count_of("GAMEPLAY note");
	gs_tick(0, 0, 0, t + 71600);
	check(count_of("GAMEPLAY for this match: gameplay pack 1") == 1 && !early && count_of("GAMEPLAY note: a lobby closed during its start countdown and no session set-up followed") == 1,
	      "switch: lobby closed in its countdown: the match's mode is logged; if no set-up follows within 20 s, that is logged once");
	gs_pending_at = GetTickCount() - 40000, gs_pending = 1;
	zero(seen, sizeof seen); /* back in the menu, alone */
	gs_solo = 0;
	reg = gs_t_setup((void *)gs_setup_a);
	check(reg && gs_loaded == 0 && gs_t_loads == again, "switch: a countdown seen long ago does not count for a later single-player session");
	slots[0].v[10] = 0;

	/* 12. alone: [gamefix] Solo decides; the saved-game path goes through the same switch */
	cap_clear();
	gs_solo = 1;
	reg = gs_t_setup((void *)gs_setup_a);
	int solo_on = reg && gs_loaded == 1 && gs_sites_state(&bad) == 1 && count_of("no other player: [gamefix] Solo=1") == 1;
	again = gs_t_loads;
	reg = gs_t_setup((void *)gs_setup_b);
	int same = reg && gs_loaded == 1 && gs_t_loads == again;
	gs_solo = 0;
	reg = gs_t_setup((void *)gs_setup_b);
	check(solo_on && same && reg && gs_loaded == 0 && gs_t_loads == again + 1 && gs_t_code_hash() == h1,
	      "switch: alone with Solo=1 -> the pack; the same mode again -> no reload; with Solo=0 the saved-game path loads the original back");

	/* 13. never play on in a half-switched state */
	gs_solo = 1, gs_t_break_grid = 1;
	cap_clear();
	again = gs_t_loads;
	reg = gs_t_setup((void *)gs_setup_a);
	check(reg && gs_loaded == 0 && !gs_ready && gs_t_loads == again + 2 && gs_redirect == 0 && gs_t_code_hash() == h1 && !lstrcmpA(lt_val, "19:0.19") &&
	          count_of("GAMEPLAY switch FAILED: pack 1 did not load as expected (13 of 13 data files") == 1 && count_of("The original rules are loaded again") == 1,
	      "switch: the damage grid is not the pack's after loading -> original loaded again, this PC stops announcing the pack, logged");
	gs_t_break_grid = 0;
	again = gs_t_loads;
	reg = gs_t_setup((void *)gs_setup_a);
	check(reg && gs_loaded == 0 && gs_t_loads == again, "switch: after a failed switch this PC no longer switches in this game start");
	gs_t_ready();
	gs_t_skip_file = 3; /* buildingrules.xml never comes through the loader */
	cap_clear();
	reg = gs_t_setup((void *)gs_setup_a);
	check(reg && gs_loaded == 0 && !gs_ready && gs_t_code_hash() == h1 && count_of("(12 of 13 data files read from the pack, mask 1ff7") == 1,
	      "switch: one of the 13 files was not read from the pack -> the same way back, and the log names which");
	gs_t_skip_file = -1;
	gs_t_ready();
	*gs_t(0x737398) = 0xE9; /* someone else changed a fix site since start-up */
	cap_clear();
	again = gs_t_loads;
	reg = gs_t_setup((void *)gs_setup_a);
	check(reg && gs_loaded == 0 && !gs_ready && gs_t_loads == again && *gs_t(0x737398) == 0xE9 && *gs_t(0x609ABC) == 0x74 &&
	          count_of("GAMEPLAY switch REFUSED: unexpected code at a fix site (G4 nomad attrition)") == 1,
	      "switch: unexpected code at a fix site when the pack is to be loaded -> refused, nothing written, nothing reloaded");
	gs_t_ready();
	gs_tick(1, 0, 1, t + 80000);
	check(!gs_ready && !lstrcmpA(lt_val, "19:0.19"), "switch: the fix sites are looked at again when a lobby opens: odd code there -> this PC steps back");
	*gs_t(0x737398) = 0xE8;
	check(gs_t_code_hash() == h1, "switch: after all of this the image is still the one the hooks left");

	/* 14. with Switch=0 the lobby tick does nothing at all */
	gs_t_ready();
	gs_switch = 0;
	cap_clear();
	reads = td_reads;
	gs_tick(1, 0, 1, t + 90000);
	gs_tick(1, 0, 0, t + 90300);
	check(td_reads == reads && count_of("GAMEPLAY") == 0, "switch: Switch=0: no reads, no lines");

	gs_t_cleanup();
	gs_switch = gs_solo = gs_ready = 0, gs_loaded = 0, gs_redirect = 0, gs_decided = -1, gs_frozen = 0, gs_pending = 0, gs_await = 0;
	gs_want_orig = GS_ORIG_HASH, gs_want_pack = GS_PACK_HASH, gs_grid_want[0] = GS_GRID_ORIG, gs_grid_want[1] = GS_GRID_PACK;
	lt_val[0] = 0, lf_game_lobby = 0, td_lobby = 0;
	for (int i = 0; i < 4; i++)
		td_lob[i].ver = 0;
	zero(seen, sizeof seen);
	zero(slots, sizeof slots);
	slots_valid = 0;
}
