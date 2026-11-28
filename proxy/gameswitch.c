/* ------------------------------------------------------------------ gameplay switch (core v19): the gameplay pack PER MATCH
 * Included at the end of netproxy.c, after lobbytag.c (it uses turndelay's PlayFab readers, lobbytag's member key and
 * the gameplay thunks of netproxy.c). Specification and the live runs it rests on: reverse/gameplay-switch.md.
 * NOTHING in this file has run inside the game yet (2026-11-28): it was written from a Frida prototype that did the
 * same steps in four single-player matches, and is covered by unit tests only.
 *
 * The classic gameplay pack ([gamefix] Version=1) replaces the game's data files and patches the code at start, and
 * its version guard keeps its players apart from everybody else. This is the other way: the game starts as the
 * original, the pack's 13 data files sit side by side in <game>\ronfix\gameplay1\Data, and the pack is loaded into
 * memory only for a match in which EVERY player has it. Nobody is kept out; a lobby with one player without it
 * simply plays the original game. The classic pack, when installed, wins: this file then does nothing.
 *   1. [gamefix] Switch=1, the classic pack not installed, the game's own 13 data files and the staged 13 both
 *      with the expected fingerprint (FNV-1a 32 as [gamefix] DataHash, here over all 13 files), all 8 code-fix
 *      sites original and the hook sites as expected: only then does this PC take part. It says so with a third
 *      part in the member key lobbytag.c posts: ronfix_ver = "<release>:<name>:g1" (g + pack number).
 *   2. Every client decides on its own, from data all of them see, as turndelay.c does: the pack is used when every
 *      lobby member and every human in the game's slot table says the same g<N>. Re-taken every 250 ms while the
 *      lobby window is open, frozen while the host's start countdown runs. No host switch, no version guard.
 *   3. The switch itself happens where the game loads a scenario mod's rules itself: at session set-up
 *      (C_loading_window_setup 0x584590), on the game's main thread, before the map exists. Two call sites there
 *      are redirected through a stub: the new-game path (call 0xA23C10 at 0x584641, the site the prototype used)
 *      and the path taken when a saved game or a recording is loaded (call 0x7C1C10 at 0x5846B7), which skips the
 *      first. The stub compares "rules loaded" with "rules this session needs" and, when they differ:
 *        - the four game-settings bytes [[0xC061E8] + 0x2A, 0x34, 0x35, 0x36] become 0, 0, 0, 7 as at the game's
 *          first load (the loaders read them; without this a reload never equals a fresh start),
 *        - fix F and the seven code fixes are written (pack) or the original bytes restored,
 *        - the game's own rules reload runs: 0x5AA170 then 0x5AA3E0, both fastcall with flag 0,
 *        - the four bytes are put back, and the damage grid (0xC12BF4, 486,098 bytes) must have the fingerprint
 *          known for that mode, with all 13 files read from the pack (or none). If not: back to the original,
 *          this PC stops announcing the pack for the rest of the game start, and the log says so.
 *      Nothing happens when a match ends or a countdown is cancelled: the next set-up loads what it needs.
 *   4. While the pack is being loaded and for as long as it stays loaded, the game's XML loader
 *      (C_xml_document_load_com 0xA279E0, thiscall: result, file name, a third string) gets another file name for
 *      the 13 staged files: "ronfix\gameplay1\Data\<name>" in place of the bare "<name>" it would look up in
 *      Data\. A name with a folder part is used as given, relative to the game folder. The originals are never
 *      touched, so "back" is a reload without the redirect.
 *   5. Which rules a session needs: a multiplayer start (countdown seen in this lobby window) = the frozen lobby
 *      decision; anything else with other players connected = the original; alone (single-player, also a saved
 *      game or a recording opened from the menu) = the pack when [gamefix] Solo=1, else the original.
 * Not covered, see the write-up: saved games and recordings do not remember their mode; a scenario mod reloading
 * the rules while the pack is loaded; the pack's two animation edits (anim_graphics.xml is never read again). */

#define GS_PACK 1                 /* the pack this core carries the code fixes for */
#define GS_DIR "ronfix\\gameplay1\\Data\\"
#define GS_NFILES 13
#define GS_PACK_HASH2 0xe5b67340u /* pack 1: after balance.xml + unitrules.xml (the classic pack's DataHash) */
#define GS_PACK_HASH 0xbd25664au  /* pack 1: all 13 files */
#define GS_ORIG_HASH 0xc2aa5921u  /* the game's own 13 files (Steam build of 2024-06-20) */
#define GS_GRID_ORIG 0x1e664726u  /* damage grid, FNV-1a 32: a fresh original start (sha256 501b47ed...) */
#define GS_GRID_PACK 0x42d62a29u  /* ... a start with pack 1 and fix F (sha256 bc3a34a4...) */
#define GS_VA_GRID 0xC12BF4u
#define GS_GRID_BYTES 486098
#define GS_VA_SETTINGS 0xC061E8u  /* -> the game-settings record */
#define GS_VA_STRTAG 0xCC2311u    /* the byte every game string carries at +11 */
#define GS_VA_UNLOAD 0x5AA170u    /* C_shutdown_global_state, fastcall (flag) */
#define GS_VA_LOAD 0x5AA3E0u      /* C_loading_screen_init_systems, fastcall (flag) */
#define GS_VA_XML 0xA279E0u       /* C_xml_document_load_com */
#define GS_VA_SETUP_A 0x584641u   /* e8 ca f5 49 00   call 0xA23C10 (new game) */
#define GS_VA_SETUP_B 0x5846B7u   /* e8 54 d5 23 00   call 0x7C1C10 (saved game / recording) */
#define GS_PENDING_MS 30000       /* a session set-up this long after the last countdown tick is not that match */

static int gs_switch, gs_solo;        /* [gamefix] Switch / Solo */
static int gs_ready;                  /* this PC takes part: files, code and hooks are as they must be */
static char gs_not[160];              /* why not */
static volatile int gs_loaded;        /* the rules in memory: 0 the original game, 1 the pack */
static volatile int gs_redirect;      /* the XML loader gets the pack's file names */
static int gs_decided = -1;           /* decision in this lobby window: 1 pack, 0 original, -1 not taken yet */
static int gs_frozen;                 /* start countdown running: the decision no longer changes */
static char gs_why[128];              /* why the current decision is the original game */
static DWORD gs_last_read;
static volatile int gs_pending;       /* a countdown is running in this lobby window: a match may be starting */
static volatile DWORD gs_pending_at;
static int gs_gen, gs_used_gen = -1;  /* lobby windows counted; the one whose decision a set-up already used */
static int gs_await;                  /* the lobby closed during a countdown and no set-up has followed yet */
static DWORD gs_await_at;
static int gs_setups, gs_reloads;     /* set-ups seen, rule reloads done (for the log and the tests) */
static unsigned gs_swapped;           /* bit i: file i was read from the pack during the last reload */
static unsigned gs_grid_seen;         /* the damage grid's fingerprint after the last reload */
static unsigned gs_want_pack = GS_PACK_HASH, gs_want_orig = GS_ORIG_HASH; /* tests use their own files */
static unsigned gs_grid_want[2] = { GS_GRID_ORIG, GS_GRID_PACK };
static unsigned char *gs_exe;         /* exe base (tests point it at a buffer laid out like the exe) */

typedef void(__fastcall *gs_reload_fn)(int flag);
static gs_reload_fn gs_fn_unload, gs_fn_load;

/* the stubs' exits and the two functions they call, by name for the assembler */
void *gs_setup_a_orig __asm__("gs_setup_a_orig");
void *gs_setup_b_orig __asm__("gs_setup_b_orig");
void *gs_xml_back __asm__("gs_xml_back");
void gs_at_setup(void) __asm__("gs_at_setup");
void gs_xml_name(void **slot) __asm__("gs_xml_name");

static unsigned char *gs_at(unsigned va) { return gs_exe + (va - 0x400000u); }

/* ---------------------------------------------------------------- the 13 files */
static const char *const gs_files[GS_NFILES] = { /* the first two in the order of [gamefix] DataHash */
	"balance.xml", "unitrules.xml", "building_graphics.xml", "buildingrules.xml", "craftrules.xml", "help.xml", "icon_texs.xml",
	"interface.xml", "resourcerules.xml", "rules.xml", "soundtypes.xml", "typenames.xml", "unit_graphics.xml"
};
/* A game string as the game builds its own constants (reverse/gameplay-switch.md section 10): 0x14 bytes. */
struct gs_str {
	const unsigned short *p;        /* +0  the text (flag bit 0) or a pointer to it */
	unsigned short cap, start, len; /* +4 characters, +6 first character, +8 characters in use */
	unsigned char flags, tag;       /* +10 bit 0: p is the text itself; +11 copy of the byte at 0xCC2311 */
	unsigned z0, z1;                /* +12, +16 */
};
static unsigned short gs_wpath[GS_NFILES][48]; /* "ronfix\gameplay1\Data\<name>", zero-terminated */
static struct gs_str gs_names[GS_NFILES];

static void gs_build_names(void)
{
	for (int i = 0; i < GS_NFILES; i++) {
		int n = 0;
		for (const char *s = GS_DIR; *s; s++)
			gs_wpath[i][n++] = (unsigned char)*s;
		for (const char *s = gs_files[i]; *s; s++)
			gs_wpath[i][n++] = (unsigned char)*s;
		gs_wpath[i][n] = 0;
		gs_names[i].p = gs_wpath[i];
		gs_names[i].cap = gs_names[i].len = (unsigned short)n;
		gs_names[i].start = 0, gs_names[i].flags = 1, gs_names[i].z0 = gs_names[i].z1 = 0;
		gs_names[i].tag = 0;
		safe_read(gs_at(GS_VA_STRTAG), &gs_names[i].tag, 1);
	}
}

/* The XML loader's file name argument (slot = where that argument sits on the stack). While the pack's rules
 * are wanted, the bare name of one of the 13 files is replaced by the string naming the pack's copy. */
void gs_xml_name(void **slot)
{
	struct gs_str s;
	const unsigned short *t = 0;
	unsigned short w[24];
	if (!gs_redirect || !safe_read(*slot, &s, sizeof s) || !s.p || s.len < 5 || s.len > 23)
		return;
	t = s.p;
	if (!(s.flags & 1) && (!safe_read(s.p, &t, 4) || !t))
		return;
	if (!safe_read(t + s.start, w, s.len * 2))
		return;
	for (int i = 0; i < GS_NFILES; i++) {
		const char *f = gs_files[i];
		int k = 0;
		for (; k < s.len && f[k] && (w[k] | (w[k] >= 'A' && w[k] <= 'Z' ? 32 : 0)) == (unsigned char)f[k]; k++)
			;
		if (k == s.len && !f[k]) {
			*slot = &gs_names[i];
			gs_swapped |= 1u << i;
			return;
		}
	}
}

/* FNV-1a 32 over the 13 files in folder (with trailing backslash); *two = the value after the first two */
static int gs_fingerprint(const char *folder, unsigned *all, unsigned *two)
{
	char path[MAX_PATH];
	unsigned h = 2166136261u;
	int ok = 1;
	*two = 0;
	if (lstrlenA(folder) + 24 >= MAX_PATH)
		return 0;
	for (int i = 0; i < GS_NFILES && ok; i++) {
		lstrcpyA(path, folder);
		lstrcatA(path, gs_files[i]);
		h = fnv1a_file(path, h, &ok);
		if (i == 1)
			*two = h;
	}
	*all = h;
	return ok;
}

/* ---------------------------------------------------------------- the code fixes: written for the pack, restored after */
enum { GS_F, GS_G1, GS_G2, GS_G3, GS_G4, GS_G5, GS_G7, GS_H1, GS_NSITES };
struct gs_site {
	unsigned va;
	int n;
	const char *off;      /* the game's own bytes */
	unsigned ctx_va;
	const char *ctx;      /* the instructions around them, as apply_cbp_exe_fixes() checks them (0 = none) */
	const char *name;
};
static const struct gs_site gs_sites[GS_NSITES] = {
	{ 0x582B6B, 4, "54010000", 0x582B67, "8b4010055401000050e8", "F object masks in the damage table" },
	{ 0x609ABC, 1, "74", 0x609AB1, "8b81b802000083e04085c07419", "G1 supply-wagon craft" },
	{ 0x738F52, 7, "6a00689f010000", 0, 0, "G2 Chinese assimilation" },
	{ 0x6DC743, 9, "e8c8f4000085c0754b", 0x6DC73C, "68130200008bcee8c8f4000085c0754b", "G3 Persian capitals" },
	{ 0x737398, 5, "e8f350faff", 0, 0, "G4 nomad attrition" },
	{ 0x624A9E, 1, "d0", 0x624A9D, "05d0160000", "G5 capital notice" },
	{ 0x5AD603, 5, "e8f8801000", 0, 0, "G7 start borders" },
	{ 0x627DBF, 10, "57ff75ac53e8b7b51000", 0, 0, "H1 ally returns a moved capital" },
};
static unsigned char gs_offb[GS_NSITES][10], gs_onb[GS_NSITES][10];

static void gs_site_bytes(void)
{
	void *thunk[GS_NSITES] = { 0, 0, (void *)g2_thunk, (void *)g3_thunk, (void *)g4_thunk, 0, (void *)g7_thunk, (void *)h1_thunk };
	for (int i = 0; i < GS_NSITES; i++) {
		const char *h = gs_sites[i].off;
		for (int k = 0; k < gs_sites[i].n; k++, h += 2) {
			int hi = h[0] <= '9' ? h[0] - '0' : h[0] - 'a' + 10, lo = h[1] <= '9' ? h[1] - '0' : h[1] - 'a' + 10;
			gs_offb[i][k] = (unsigned char)(hi << 4 | lo);
			gs_onb[i][k] = 0x90; /* a jump to the thunk, the rest of the replaced instructions as NOPs */
		}
		if (thunk[i]) {
			gs_onb[i][0] = 0xE9;
			*(int *)(gs_onb[i] + 1) = (int)((unsigned char *)thunk[i] - (gs_at(gs_sites[i].va) + 5));
		}
	}
	gs_onb[GS_F][0] = 0x94, gs_onb[GS_F][1] = 0x48, gs_onb[GS_F][2] = 0, gs_onb[GS_F][3] = 0; /* add eax, 0x4894 */
	gs_onb[GS_G1][0] = 0xEB;                                                                  /* je -> jmp */
	gs_onb[GS_G5][0] = 0xBC;                                                                  /* notice 0x16BC */
	/* what the thunks jump back to and call: the same addresses apply_cbp_exe_fixes() uses */
	rf_g2_back = gs_at(0x738F59), rf_g2_skip = gs_at(0x738F65);
	rf_g3_owns = gs_at(0x6EBC10), rf_g3_power = gs_at(0x6E1370), rf_g3_find = gs_at(0x6EB930);
	rf_g3_keep = gs_at(0x6DC797), rf_g3_wgloop = gs_at(0x6DC74C);
	rf_g4_orig = gs_at(0x6DC490), rf_g4_back = gs_at(0x73739D), rf_g4_flags = gs_at(0xE3A390);
	rf_g7_orig = gs_at(0x6B5700), rf_g7_back = gs_at(0x5AD608), rf_g7_ptr = (unsigned char **)gs_at(0xC0620C);
	rf_h1_players = gs_at(0xE3A390), rf_h1_team = gs_at(0x6EDB50), rf_h1_xfer = gs_at(0x733380), rf_h1_back = gs_at(0x627DC9);
}

/* 0 = all 8 sites hold the game's own code, 1 = all hold the pack's, -1 = anything else (*bad: the first odd site) */
static int gs_sites_state(int *bad)
{
	int on = 0, off = 0;
	*bad = -1;
	for (int i = 0; i < GS_NSITES; i++) {
		const struct gs_site *s = &gs_sites[i];
		unsigned char *p = gs_at(s->va);
		if (!readable(s->ctx ? gs_at(s->ctx_va) : p, 16)) {
			*bad = i;
			return -1;
		}
		if (bytes_eq(p, gs_offb[i], s->n) && (!s->ctx || bytes_are(gs_at(s->ctx_va), s->ctx)))
			off++;
		else if (bytes_eq(p, gs_onb[i], s->n))
			on++;
		else {
			*bad = i;
			return -1;
		}
	}
	return off == GS_NSITES ? 0 : on == GS_NSITES ? 1 : -1;
}

static int gs_write_sites(int pack)
{
	int ok = 1;
	for (int i = 0; i < GS_NSITES; i++)
		ok &= write_code(gs_at(gs_sites[i].va), pack ? gs_onb[i] : gs_offb[i], gs_sites[i].n);
	return ok;
}

/* ---------------------------------------------------------------- the hooks */
/* a 32-bit address inside an instruction: the exe is relocated, so it is the real base plus the offset */
static int gs_abs(unsigned va, unsigned target)
{
	unsigned v = 0;
	return safe_read(gs_at(va), &v, 4) && v == (unsigned)(UINT_PTR)gs_at(target);
}

/* the code this file calls or redirects is the code it was written against; "" = yes, else what is not */
static const char *gs_code_odd(void)
{
	if (!readable(gs_at(0x584637), 0x90) || !bytes_are(gs_at(0x584637), "6683b84405000000756ce8caf549008bf085f6"))
		return "session set-up (new game)";
	if (!bytes_are(gs_at(0x5846AD), "6800000105b9") || !gs_abs(0x5846B3, 0xEB4FD0) || !bytes_are(gs_at(GS_VA_SETUP_B), "e854d52300"))
		return "session set-up (saved game)";
	if (!readable(gs_at(GS_VA_XML), 17) || !bytes_are(gs_at(GS_VA_XML), "558bec6aff68") || !gs_abs(GS_VA_XML + 6, 0xAB0243) ||
	    !bytes_are(gs_at(GS_VA_XML + 10), "64a10000000050"))
		return "the XML loader";
	if (!readable(gs_at(GS_VA_UNLOAD), 17) || !bytes_are(gs_at(GS_VA_UNLOAD), "a1") || !gs_abs(GS_VA_UNLOAD + 1, 0xC061EC) ||
	    !bytes_are(gs_at(GS_VA_UNLOAD + 5), "8b15") || !gs_abs(GS_VA_UNLOAD + 7, 0xCAB394) || !bytes_are(gs_at(GS_VA_UNLOAD + 11), "56be01000000"))
		return "the rules unload";
	if (!readable(gs_at(GS_VA_LOAD), 12) || !bytes_are(gs_at(GS_VA_LOAD), "558bec64a1000000006aff68"))
		return "the rules load";
	return "";
}

/* Session set-up, both paths: entered by the game's own call, with the game's registers and stack. Everything
 * is kept, the mode is looked at (and switched), then the call goes on to where the game sent it. */
__attribute__((naked)) static void gs_setup_a(void)
{
	__asm__ volatile(
		"pushal\n\t"
		"pushfl\n\t"
		"cld\n\t"
		"call gs_at_setup\n\t"
		"popfl\n\t"
		"popal\n\t"
		"jmp *gs_setup_a_orig\n");
}
__attribute__((naked)) static void gs_setup_b(void)
{
	__asm__ volatile(
		"pushal\n\t"
		"pushfl\n\t"
		"cld\n\t"
		"call gs_at_setup\n\t"
		"popfl\n\t"
		"popal\n\t"
		"jmp *gs_setup_b_orig\n");
}
/* The XML loader's entry. The file name argument is the second of three on the stack ([esp+8] at entry, 0x24
 * bytes further once everything is saved). Then the five bytes the jump replaced (push ebp / mov ebp, esp /
 * push -1) and on into the loader. */
__attribute__((naked)) static void gs_xml_stub(void)
{
	__asm__ volatile(
		"pushal\n\t"
		"pushfl\n\t"
		"cld\n\t"
		"leal 0x2C(%esp), %eax\n\t"
		"pushl %eax\n\t"
		"call gs_xml_name\n\t"
		"addl $4, %esp\n\t"
		"popfl\n\t"
		"popal\n\t"
		"pushl %ebp\n\t"
		"movl %esp, %ebp\n\t"
		"pushl $-1\n\t"
		"jmp *gs_xml_back\n");
}

static int gs_hook_call(unsigned va, void *stub, void **orig)
{
	unsigned char *p = gs_at(va);
	int rel = (int)((unsigned char *)stub - (p + 5));
	*orig = p + 5 + *(int *)(p + 1);
	return write_code(p + 1, (const unsigned char *)&rel, 4);
}

static int gs_hooks_install(void)
{
	unsigned char j[5] = { 0xE9, 0, 0, 0, 0 };
	gs_fn_unload = (gs_reload_fn)(void *)gs_at(GS_VA_UNLOAD);
	gs_fn_load = (gs_reload_fn)(void *)gs_at(GS_VA_LOAD);
	gs_xml_back = gs_at(GS_VA_XML + 5);
	*(int *)(j + 1) = (int)((unsigned char *)(void *)gs_xml_stub - (gs_at(GS_VA_XML) + 5));
	/* the loader first: from the moment a set-up can switch, the redirect must be there */
	return write_code(gs_at(GS_VA_XML), j, 5) && gs_hook_call(GS_VA_SETUP_A, (void *)gs_setup_a, &gs_setup_a_orig) &&
	       gs_hook_call(GS_VA_SETUP_B, (void *)gs_setup_b, &gs_setup_b_orig);
}

/* ---------------------------------------------------------------- what this PC says, and what the others say */
/* our ronfix_ver value: "<release>:<name>" plus ":g<pack>" while this PC takes part */
static void gs_tag(void)
{
	int i = 0, colons = 0;
	if (!lt_val[0])
		return;
	for (; lt_val[i]; i++)
		if (lt_val[i] == ':' && ++colons == 2)
			break;
	lt_val[i] = 0;
	if (!gs_ready)
		return;
	if (!colons)
		lt_val[i++] = ':'; /* no release name: an empty second part keeps the third in its place */
	wsprintfA(lt_val + i, ":g%d", GS_PACK);
}

/* a member's ronfix_ver -> the pack it has ready (1..99), 0 = none */
static int gs_parse(const char *v)
{
	char name[16];
	int i = 0, colons = 0, n = 0, d = 0;
	if (lt_parse(v, name, sizeof name) < 1)
		return 0;
	for (; v[i] && colons < 2; i++)
		colons += v[i] == ':';
	if (colons < 2 || v[i] != 'g')
		return 0;
	for (i++; v[i] >= '0' && v[i] <= '9' && d < 2; i++, d++)
		n = n * 10 + (v[i] - '0');
	return d && n >= 1 && (!v[i] || v[i] == ':') ? n : 0; /* a later release may add a fourth part */
}

static int gs_member(void *lobby, const void *key)
{
	const char *v = 0;
	char b[40];
	if (td_get_member_prop(lobby, key, LT_KEY, &v) < 0 || !v)
		return 0;
	read_cstr(v, b, sizeof b);
	return gs_parse(b);
}

static void gs_fail(const char *why)
{
	gs_ready = 0;
	lf_strcpy(gs_not, why, sizeof gs_not);
	gs_tag(); /* the lobby no longer hears "g1" from this PC */
}

/* the decision every client takes on its own, from data all of them see. why: at least 128 bytes. */
static int gs_decide(void *lobby, char *why, int cap)
{
	unsigned n = 0;
	const unsigned char *members = 0; /* PFEntityKey { const char *id, *type } */
	const char *p = 0;
	char id[40];
	why[0] = 0;
	if (!gs_ready) {
		lf_strcpy(why, gs_not, cap);
		return 0;
	}
	if (!lobby || !td_api()) {
		lf_strcpy(why, "game lobby not known yet", cap);
		return 0;
	}
	if (!slots_valid) {
		lf_strcpy(why, "slot table not read yet", cap);
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
		if (gs_member(lobby, key) != GS_PACK) {
			wsprintfA(why, "%s has no gameplay pack %d ready (or has not said so yet)", id, GS_PACK);
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
		if (!slots[s].a[0] || gs_member(lobby, &k) != GS_PACK) {
			wsprintfA(why, "slot %d (%s) is not a lobby member with gameplay pack %d", s, slots[s].a[0] ? slots[s].a : slots[s].b, GS_PACK);
			return 0;
		}
	}
	return 1;
}

/* ---------------------------------------------------------------- the switch */
static unsigned gs_grid_hash(void)
{
	const unsigned char *g = gs_at(GS_VA_GRID);
	unsigned h = 2166136261u;
	if (!readable(g, GS_GRID_BYTES))
		return 0;
	for (int i = 0; i < GS_GRID_BYTES; i++)
		h = (h ^ g[i]) * 16777619u;
	return h;
}

/* the game's own rules reload, with the pack's files or the game's. 1 = the rules in memory are that mode's. */
static int gs_load(int pack)
{
	static const int off[4] = { 0x2A, 0x34, 0x35, 0x36 };
	static const unsigned char first[4] = { 0, 0, 0, 7 }; /* the record as it is at the game's first load */
	volatile unsigned char *rec = 0;
	unsigned char saved[4];
	if (!safe_read(gs_at(GS_VA_SETTINGS), &rec, 4) || !rec || !readable((const void *)rec, 0x38))
		return 0;
	for (int k = 0; k < 4; k++)
		saved[k] = rec[off[k]], rec[off[k]] = first[k];
	gs_swapped = 0;
	gs_redirect = pack;
	gs_fn_unload(0);
	gs_fn_load(0);
	gs_reloads++;
	/* back into the record the game has now (the pointer is read again, as the prototype did) */
	if (safe_read(gs_at(GS_VA_SETTINGS), &rec, 4) && rec && readable((const void *)rec, 0x38))
		for (int k = 0; k < 4; k++)
			rec[off[k]] = saved[k];
	gs_grid_seen = gs_grid_hash();
	return gs_grid_seen == gs_grid_want[pack] && gs_swapped == (pack ? (1u << GS_NFILES) - 1 : 0);
}

static int gs_bits(unsigned m)
{
	int n = 0;
	for (; m; m &= m - 1)
		n++;
	return n;
}

static void gs_do_switch(int want)
{
	int bad = -1, st = gs_sites_state(&bad);
	DWORD t0 = GetTickCount();
	if (want && st != 0) { /* never write the pack's code over something unknown */
		gs_fail("the game's code at the fix sites was no longer the original");
		logline("GAMEPLAY switch REFUSED: unexpected code at a fix site (%s). Nothing was changed: this session runs the ORIGINAL rules on this PC. "
		        "If the lobby agreed on the pack, expect out of sync.", bad >= 0 ? gs_sites[bad].name : "some hold the pack's code, some the game's");
		return;
	}
	if (!gs_write_sites(want) && want) {
		gs_write_sites(0);
		gs_fail("the code fixes could not be written");
		logline("GAMEPLAY switch FAILED: the code fixes could not be written. This session runs the ORIGINAL rules on this PC.");
		return;
	}
	if (gs_load(want)) {
		gs_loaded = want;
		if (want)
			logline("GAMEPLAY pack %d loaded for this session in %lu ms: %d data files from ronfix\\gameplay%d, %d code fixes, damage grid %08x",
			        GS_PACK, (unsigned long)(GetTickCount() - t0), gs_bits(gs_swapped), GS_PACK, GS_NSITES, gs_grid_seen);
		else
			logline("GAMEPLAY original rules loaded again in %lu ms: code fixes taken out, damage grid %08x", (unsigned long)(GetTickCount() - t0), gs_grid_seen);
		return;
	}
	/* not what that mode must give: never play on in a half-switched state */
	unsigned got = gs_grid_seen, files = gs_swapped;
	gs_fail("a switch went wrong earlier in this game start");
	if (want) {
		gs_write_sites(0);
		int back = gs_load(0);
		gs_loaded = 0;
		logline("GAMEPLAY switch FAILED: pack %d did not load as expected (%d of %d data files read from the pack, mask %04x; damage grid %08x, expected %08x). %s",
		        GS_PACK, gs_bits(files), GS_NFILES, files, got, gs_grid_want[1],
		        back ? "The original rules are loaded again and this session uses them. If the lobby agreed on the pack, expect out of sync."
		             : "The original rules could NOT be verified either: restart the game before playing with others.");
	} else {
		gs_loaded = 0;
		logline("GAMEPLAY switch FAILED: the original rules did not load as expected (damage grid %08x, expected %08x; %d files still read from the pack). "
		        "Restart the game before playing with others.", got, gs_grid_want[0], gs_bits(files));
	}
}

/* someone else is in the network player list: not a single-player session */
static int gs_others(void)
{
	for (int k = 0; k < 8; k++) {
		unsigned f = 0;
		if (seen[k].ptr && (!safe_read(seen[k].ptr + PEER_FLAGS, &f, 4) || !(f & 4)))
			return 1;
	}
	return 0;
}

/* which rules the session being set up needs, and why (static text) */
static int gs_needed(const char **why)
{
	static char buf[160];
	int pending = gs_pending && gs_used_gen != gs_gen && GetTickCount() - gs_pending_at < GS_PENDING_MS;
	gs_pending = 0;
	if (pending) {
		gs_used_gen = gs_gen, gs_await = 0; /* one lobby window starts one match */
		if (gs_decided == 1) {
			*why = "multiplayer: every player in the lobby has it ready";
			return gs_ready;
		}
		wsprintfA(buf, "multiplayer: %s", gs_decided < 0 ? "no decision was taken in the lobby" : gs_why);
		*why = buf;
		return 0;
	}
	if (!gs_ready) {
		*why = gs_not;
		return 0;
	}
	if (gs_others()) {
		*why = "other players are connected, but no start countdown was seen in a lobby";
		return 0;
	}
	*why = gs_solo ? "no other player: [gamefix] Solo=1" : "no other player: [gamefix] Solo=0";
	return gs_solo != 0;
}

/* called by both set-up stubs, on the game's main thread, before the session's map exists */
void gs_at_setup(void)
{
	const char *why = "";
	int want = gs_needed(&why);
	gs_setups++;
	if (want == gs_loaded) {
		logline("GAMEPLAY session set-up: %s, already loaded (%s)", want ? "the gameplay pack" : "the original game", why);
		return;
	}
	logline("GAMEPLAY session set-up: loading %s (%s)", want ? "the gameplay pack" : "the original game", why);
	gs_do_switch(want);
}

/* ---------------------------------------------------------------- start-up and the lobby */
/* at start-up, on the game's main thread: may this PC take part? Only then is anything in the game changed. */
static void gs_init(unsigned char *exe, const char *gamedir)
{
	char path[MAX_PATH];
	unsigned h = 0, two = 0;
	int bad = -1;
	const char *odd;
	gs_exe = exe, gs_ready = 0, gs_loaded = 0, gs_redirect = 0, gs_not[0] = 0;
	if (!gs_switch) {
		lf_strcpy(gs_not, "switched off ([gamefix] Switch=0)", sizeof gs_not);
		return;
	}
	if (gamefix_version != 0) { /* the classic pack: its files are in Data, its code is patched in, its guard is up */
		wsprintfA(gs_not, "the classic gameplay pack is installed ([gamefix] Version=%d) and stays in charge", gamefix_version < 0 ? -gamefix_version : gamefix_version);
		return;
	}
	gs_site_bytes();
	if (gs_sites_state(&bad) != 0) {
		wsprintfA(gs_not, "unexpected code at a fix site (%s)", bad >= 0 ? gs_sites[bad].name : "mixed");
		return;
	}
	if ((odd = gs_code_odd())[0]) {
		wsprintfA(gs_not, "unexpected code in %s", odd);
		return;
	}
	if (lstrlenA(gamedir) + 64 >= MAX_PATH) {
		lf_strcpy(gs_not, "the game folder's path is too long", sizeof gs_not);
		return;
	}
	lstrcpyA(path, gamedir);
	lstrcatA(path, "Data\\");
	if (!gs_fingerprint(path, &h, &two) || h != gs_want_orig) {
		wsprintfA(gs_not, "the game's own data files are not the original ones (fingerprint %08x, expected %08x): another mod, or the classic pack's files", h, gs_want_orig);
		return;
	}
	lstrcpyA(path, gamedir);
	lstrcatA(path, GS_DIR);
	if (!gs_fingerprint(path, &h, &two)) {
		wsprintfA(gs_not, "the pack's files are not (all) in ronfix\\gameplay%d\\Data: run the gameplay pack's INSTALL-SWITCH.bat", GS_PACK);
		return;
	}
	if (h != gs_want_pack) {
		wsprintfA(gs_not, "the files in ronfix\\gameplay%d\\Data are not pack %d (fingerprint %08x, expected %08x): run INSTALL-SWITCH.bat again", GS_PACK, GS_PACK, h, gs_want_pack);
		return;
	}
	gs_build_names();
	if (!gs_hooks_install()) {
		lf_strcpy(gs_not, "the hooks could not be written", sizeof gs_not);
		return;
	}
	gs_ready = 1;
	gs_tag();
}

static void gs_log_start(void)
{
	if (gs_ready)
		logline("GAMEPLAY switch: ready. This PC tells every lobby it has gameplay pack %d (g%d); a match uses the pack when every player says the same, "
		        "otherwise the original game. Alone: %s.%s", GS_PACK, GS_PACK, gs_solo ? "the pack ([gamefix] Solo=1)" : "the original game ([gamefix] Solo=0)",
		        lt_enabled && lt_val[0] ? "" : " BUT nothing is posted to lobbies ([lobby] Tags=0, or no release number): multiplayer stays original.");
	else if (gs_switch)
		logline("GAMEPLAY switch: NOT possible on this PC: %s. Nothing in the game was changed; every match is the original game.", gs_not);
	else
		logline("GAMEPLAY switch: off ([gamefix] Switch=0)");
}

/* every lobby tick (lobby_tick, after lt_tick) */
static void gs_tick(int setup_open, int setup_closed_now, int setup_opened_now, DWORD now)
{
	if (!gs_switch)
		return;
	if (gs_await && now - gs_await_at >= 20000) {
		gs_await = 0;
		logline("GAMEPLAY note: a lobby closed during its start countdown and no session set-up followed. If a match did start, this PC is playing "
		        "the rules that were loaded before (%s): please report this.", gs_loaded ? "the pack" : "the original game");
	}
	if (setup_opened_now) {
		int bad = -1;
		gs_gen++, gs_decided = -1, gs_frozen = 0, gs_pending = 0, gs_await = 0, gs_why[0] = 0;
		gs_last_read = now - 1000;
		if (gs_ready && gs_sites_state(&bad) != gs_loaded) {
			gs_fail("the game's code at the fix sites changed");
			logline("GAMEPLAY switch: off from now on: unexpected code at a fix site (%s)", bad >= 0 ? gs_sites[bad].name : "mixed");
		}
	}
	if (setup_closed_now) {
		gs_frozen = 0;
		if (gs_pending && gs_used_gen != gs_gen) {
			gs_await = 1, gs_await_at = now;
			if (gs_decided == 1)
				logline("GAMEPLAY for this match: gameplay pack %d (every player has it ready)", GS_PACK);
			else
				logline("GAMEPLAY for this match: the original game: %s", gs_decided < 0 ? "no decision was taken" : gs_why);
		}
	}
	if (!setup_open)
		return;
	/* the host's start countdown runs the ready byte 2..12: membership is locked, the decision stays */
	int countdown = 0;
	for (int i = 0; i < 8; i++)
		countdown |= slots[i].v[10] >= 2 && (slots[i].a[0] || slots[i].b[0]);
	if (countdown)
		gs_pending_at = now, gs_pending = 1;
	else
		gs_pending = 0; /* cancelled: nothing was switched, nothing to undo */
	if (countdown != gs_frozen) {
		gs_frozen = countdown;
		if (countdown && gs_decided >= 0)
			logline("GAMEPLAY start countdown: %s for this match (final)", gs_decided ? "the gameplay pack" : "the original game");
	}
	if (gs_frozen && gs_decided >= 0)
		return;
	if (now - gs_last_read < 250)
		return;
	gs_last_read = now;
	char why[128];
	int want = gs_decide(td_lobby ? td_lobby : lf_game_lobby, why, sizeof why);
	if (want != gs_decided || (!want && lstrcmpA(why, gs_why))) {
		if (want)
			logline("GAMEPLAY pack %d ON for this lobby: every player has it ready (loaded when the match starts)", GS_PACK);
		else
			logline("GAMEPLAY pack off for this lobby: %s", why);
		gs_decided = want;
		lf_strcpy(gs_why, why, sizeof gs_why);
	}
	if (gs_frozen && gs_decided >= 0)
		logline("GAMEPLAY start countdown: %s for this match (final)", gs_decided ? "the gameplay pack" : "the original game");
}
