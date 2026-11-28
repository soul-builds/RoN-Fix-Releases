/* ------------------------------------------------------------------ lobby tags (core v18): who in the lobby has RoN-Fix
 * Included at the end of netproxy.c, after turndelay.c (it uses turndelay's PlayFab readers and lobby handle).
 * Some of what RoN-Fix does only works when everybody in the lobby has it, and until now nobody could see who does.
 * So every RoN-Fix player says which release it runs, and every RoN-Fix player works out, from data all of them see,
 * who in the lobby has it and who does not:
 *   1. our own member posts in the game lobby get one more key, ronfix_ver = "<release>:<name>" (for example
 *      "16:0.16"), next to ronfix_turn. Member keys without a leading '_' are invisible to the game on every PC
 *      (see turndelay.c), and a PC without RoN-Fix never posts either key;
 *   2. once a second while the lobby window is open, the members' keys are read: ronfix_ver = that release;
 *      only ronfix_turn = a release from 0.11 to 0.15, which did not say more; neither, for LT_GRACE_MS after the
 *      member appeared = no RoN-Fix (a first post can follow the joining by a few seconds);
 *   3. each player is announced once (again if what is known changes), and when the host's start countdown begins
 *      one line sums the lobby up:
 *        LOBBY tag: <name> has Accord 0.16
 *        LOBBY tag: <name> has an older version (RoN-Fix 0.11 to 0.15)
 *        LOBBY tag: <name> plays without Accord
 *        LOBBY tag: Accord: 3 of 4 players have it; without: <name>
 *      (players read "Accord", the name since 0.16; the releases up to 0.15 were called RoN-Fix)
 * The lines go to ronfix-net.log and, when lt_show is set, onto the screen of THIS PC only. Nothing is sent to the
 * other players beyond the member key, no name and no chat text is changed, and the simulation is not touched.
 * [lobby] Tags=1 (default) / 0 = off: no key posted, nothing read, nothing shown. */

#define LT_KEY "ronfix_ver"
#define LT_PRODUCT "Accord" /* the name players read; the one place to change it */
#define LT_GRACE_MS 6000
#define LT_READ_EVERY 1000
#define LT_MAX_GAME_KEYS 26  /* with ronfix_turn already in: never push a member post towards PlayFab's 30 */
#define LT_MEMBERS 16
enum { LT_UNKNOWN = -1, LT_NONE = 0, LT_OLD = 1 }; /* any other state = that release number */

static int lt_enabled = 1;                /* [lobby] Tags */
static void (*lt_show)(const char *line); /* draws one line in the lobby on this PC only; NULL = log only */
static char lt_val[24];                   /* what we post, "" = nothing (release not known) */
static char lt_self[40];                  /* our own PlayFab id, noted from our member posts */
struct lt_member {
	char id[40], vname[16]; /* PlayFab id; the release name that member posted ("0.18") */
	DWORD since;            /* when we first saw that member in this lobby */
	int state, told, here;  /* told: 0 nothing said yet, -1 said "without", else the state we announced */
};
static struct lt_member ltm[LT_MEMBERS];
static int lt_summed; /* the countdown's summary line is out */
static DWORD lt_last_read;
static const char *lt_mkeys[64], *lt_mvals[64];
static unsigned lt_mu[3];

/* "<release>:<name>" from our own version file's two words; called once the release number is known */
static void lt_set_release(unsigned release, const char *name)
{
	int n = 0;
	lt_val[0] = 0;
	if (!release || release > 9999)
		return;
	n = wsprintfA(lt_val, "%u", release);
	if (name && name[0]) {
		lt_val[n++] = ':';
		for (int i = 0; name[i] && i < 12; i++) {
			char c = name[i];
			if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_')
				lt_val[n++] = c;
		}
		lt_val[n] = 0;
	}
}

/* a member's ronfix_ver -> release number (1..9999) and its name; 0 = not a value we understand */
static int lt_parse(const char *v, char *name, int cap)
{
	int rel = 0, i = 0, k = 0;
	name[0] = 0;
	for (; v[i] >= '0' && v[i] <= '9' && i < 4; i++)
		rel = rel * 10 + (v[i] - '0');
	if (!i || rel < 1 || (v[i] && v[i] != ':'))
		return 0;
	if (v[i] == ':')
		for (i++; v[i] && v[i] != ':' && k < cap - 1; i++) {
			char c = v[i];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_'))
				return 0;
			name[k++] = c;
		}
	name[k] = 0;
	return rel; /* anything after a second ':' is for later releases to define */
}

/* PFLobbyPostUpdate's member data once more (w_pf_post, after td_rewrite): our release rides along */
static void lt_rewrite(const void *user, const void **mu)
{
	unsigned n;
	char id[40], ty[40];
	if (!lt_enabled || !lt_val[0] || !*mu || !td_setup_open() || !safe_read(*mu, lt_mu, 12) || (n = lt_mu[0]) == 0 || n > LT_MAX_GAME_KEYS)
		return;
	const char *const *k = (const char *const *)lt_mu[1], *const *v = (const char *const *)lt_mu[2];
	int has = 0, game = 0;
	for (unsigned i = 0; i < n; i++) {
		const char *kp = 0, *vp = 0;
		char kk[24];
		if (!safe_read(k + i, &kp, 4) || !safe_read(v + i, &vp, 4))
			return;
		lt_mkeys[i] = kp, lt_mvals[i] = vp;
		read_cstr(kp, kk, sizeof kk);
		has |= !lstrcmpA(kk, LT_KEY);
		game |= kk[0] == '_'; /* slot keys: the game lobby, not the Game Browser's chat lobby */
	}
	if (!game)
		return;
	key_strings(user, id, ty);
	if (id[0])
		lf_strcpy(lt_self, id, sizeof lt_self);
	if (has)
		return;
	lt_mkeys[n] = LT_KEY, lt_mvals[n] = lt_val;
	lt_mu[0] = n + 1, lt_mu[1] = (unsigned)lt_mkeys, lt_mu[2] = (unsigned)lt_mvals;
	*mu = lt_mu;
}

static void lt_say(const char *line)
{
	logline("LOBBY tag: %s", line);
	if (lt_show)
		lt_show(line);
}

/* the player's name as the game shows it, from the network DLL's player list; 0 = not known (yet) */
static const char *lt_name(const char *id)
{
	for (int k = 0; k < 8; k++)
		if (seen[k].ptr && seen[k].name[0] && !lstrcmpA(seen[k].id, id))
			return seen[k].name;
	return 0;
}

/* what that member's keys say right now */
static int lt_read_state(void *lobby, const void *key, char *vname, int cap)
{
	const char *v = 0;
	char b[40];
	vname[0] = 0;
	if (td_get_member_prop(lobby, key, LT_KEY, &v) >= 0 && v) {
		read_cstr(v, b, sizeof b);
		int rel = lt_parse(b, vname, cap);
		return rel > LT_OLD ? rel : LT_OLD; /* a value we cannot read still means RoN-Fix is there */
	}
	return td_member_value(lobby, key) ? LT_OLD : LT_NONE;
}

static void lt_announce(struct lt_member *m, const char *name)
{
	char line[160];
	if (m->state == LT_NONE)
		wsprintfA(line, "%s plays without " LT_PRODUCT, name);
	else if (m->state == LT_OLD)
		wsprintfA(line, "%s has an older version (RoN-Fix 0.11 to 0.15)", name);
	else if (m->vname[0])
		wsprintfA(line, "%s has " LT_PRODUCT " %s", name, m->vname);
	else
		wsprintfA(line, "%s has " LT_PRODUCT " (release %d)", name, m->state);
	lt_say(line);
	m->told = m->state == LT_NONE ? -1 : m->state;
}

/* when the host's countdown starts: the whole lobby in one line (only when somebody else is in it) */
static void lt_summary(void)
{
	char line[240];
	int all = 0, with = 0, n = 0, listed = 0;
	for (int i = 0; i < LT_MEMBERS; i++) {
		if (!ltm[i].id[0] || !ltm[i].here)
			continue;
		all++;
		with += !lstrcmpA(ltm[i].id, lt_self) || ltm[i].state >= LT_OLD;
	}
	if (all < 2)
		return;
	if (with == all) {
		lt_say(LT_PRODUCT ": every player in this lobby has it");
		return;
	}
	n = wsprintfA(line, LT_PRODUCT ": %d of %d players have it; without:", with, all);
	for (int i = 0; i < LT_MEMBERS && listed < 4; i++) {
		if (!ltm[i].id[0] || !ltm[i].here || !lstrcmpA(ltm[i].id, lt_self) || ltm[i].state >= LT_OLD)
			continue;
		const char *nm = lt_name(ltm[i].id);
		n += wsprintfA(line + n, "%s %s", listed ? "," : "", nm ? nm : "a player still joining");
		listed++;
	}
	if (all - with > listed)
		wsprintfA(line + n, " and %d more", all - with - listed);
	lt_say(line);
}

/* every lobby tick (lobby_tick, after td_tick) */
static void lt_tick(int setup_open, int setup_opened_now, DWORD now)
{
	if (setup_opened_now) {
		volatile unsigned char *z = (volatile unsigned char *)ltm;
		for (unsigned i = 0; i < sizeof ltm; i++)
			z[i] = 0;
		lt_summed = 0;
		lt_last_read = now - LT_READ_EVERY;
	}
	if (!lt_enabled || !setup_open || now - lt_last_read < LT_READ_EVERY)
		return;
	lt_last_read = now;
	void *lobby = td_lobby ? td_lobby : lf_game_lobby;
	unsigned n = 0;
	const unsigned char *members = 0; /* PFEntityKey { const char *id, *type } */
	if (!lobby || !td_api() || td_get_members(lobby, &n, (const void **)&members) < 0 || !members || n == 0 || n > LT_MEMBERS)
		return;
	for (int i = 0; i < LT_MEMBERS; i++)
		ltm[i].here = 0;
	for (unsigned i = 0; i < n; i++) {
		const unsigned char *key = members + 8 * i;
		const char *p = 0;
		char id[40];
		struct lt_member *m = 0, *free_m = 0;
		safe_read(key, &p, 4);
		read_cstr(p, id, sizeof id);
		if (!id[0])
			continue;
		for (int j = 0; j < LT_MEMBERS; j++) {
			if (ltm[j].id[0] && !lstrcmpA(ltm[j].id, id))
				m = &ltm[j];
			else if (!ltm[j].id[0] && !free_m)
				free_m = &ltm[j];
		}
		if (!m) {
			if (!(m = free_m))
				continue;
			lf_strcpy(m->id, id, sizeof m->id);
			m->since = now, m->state = LT_UNKNOWN, m->told = 0, m->vname[0] = 0;
		}
		m->here = 1;
		m->state = lt_read_state(lobby, key, m->vname, sizeof m->vname);
		if (!lstrcmpA(id, lt_self))
			continue; /* this PC: nothing to tell ourselves */
		const char *name = lt_name(id);
		if (!name)
			continue; /* not in the game's player list yet: wait for the name */
		if (m->state >= LT_OLD ? m->told != m->state : (m->told == 0 && now - m->since >= LT_GRACE_MS))
			lt_announce(m, name);
	}
	for (int i = 0; i < LT_MEMBERS; i++) /* gone: forget, so a player who comes back is announced again */
		if (ltm[i].id[0] && !ltm[i].here)
			ltm[i].id[0] = 0;
	int countdown = 0;
	for (int i = 0; i < 8; i++)
		countdown |= slots[i].v[10] >= 2 && (slots[i].a[0] || slots[i].b[0]);
	if (countdown && !lt_summed)
		lt_summary();
	lt_summed = countdown;
}
