/* ------------------------------------------------------------------ ban list (ronfix-bans.txt), included by setup.c
 * The core (proxy/lobbyfix.c) appends "<entity id> <Steam ID>  # <name>, banned YYYY-MM-DD" when the host bans with
 * Shift + Boot Player, and re-reads the file within 5 s of any change, also while the game runs. This lists the
 * bans and removes the ones the player unbans.
 * Ids are what the core reads: 16 hex digits (PlayFab entity id) or 17 digits starting with 76 (Steam ID64), any
 * number per line; '#' to the end of the line is a comment. Unbanning removes EVERY line that holds one of that
 * player's ids (a hand-written second line would keep him banned otherwise); every other byte of the file stays as
 * it was (comments, hand-written lines, line ends). Once no ban is left the file is deleted: the core re-reads (and
 * logs) a file without bans every 5 s while a lobby is open, and comments about bans that are gone help nobody. */

#define BAN_MAX 256
#define BAN_FILE_MAX 65536

struct ban {
	int start, len;     /* the line in bans_buf, its line end included */
	char id[4][24];     /* the ids on the line */
	int nid;
	WCHAR name[64], date[16];
};
static struct ban bans[BAN_MAX];
static int nbans;
static char bans_buf[BAN_FILE_MAX + 1];
static int bans_len;

static int ban_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int ban_is_id(const char *s, int n)
{
	int hex = 1, dec = 1;
	for (int i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
			hex = 0;
		if (c < '0' || c > '9')
			dec = 0;
	}
	return (n == 16 && hex) || (n == 17 && dec && s[0] == '7' && s[1] == '6');
}

/* UTF-8 (what the core writes), the ANSI code page for a hand-edited file that is not UTF-8 */
static void ban_widen(const char *s, int n, WCHAR *out, int cap)
{
	int k = n > 0 ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, n, out, cap - 1) : 0;
	if (k <= 0 && n > 0)
		k = MultiByteToWideChar(CP_ACP, 0, s, n, out, cap - 1);
	out[k > 0 ? k : 0] = 0;
}

static void bans_parse(void)
{
	nbans = 0;
	for (int i = 0; i < bans_len && nbans < BAN_MAX;) {
		int s = i, hash = -1;
		while (i < bans_len && bans_buf[i] != '\n')
			i++;
		int end = i; /* the line's text ends here, its '\n' (if any) follows */
		if (i < bans_len)
			i++;
		struct ban *b = &bans[nbans];
		b->nid = 0;
		for (int p = s; p < end;) {
			while (p < end && ban_ws(bans_buf[p]))
				p++;
			if (p >= end)
				break;
			if (bans_buf[p] == '#') {
				hash = p;
				break;
			}
			int q = p;
			while (q < end && !ban_ws(bans_buf[q]) && bans_buf[q] != '#')
				q++;
			if (ban_is_id(bans_buf + p, q - p) && b->nid < 4) {
				memcpy(b->id[b->nid], bans_buf + p, (size_t)(q - p));
				b->id[b->nid++][q - p] = 0;
			}
			p = q;
		}
		if (!b->nid)
			continue; /* a comment, an empty line or something that is no id: not a ban, kept as it is */
		b->start = s;
		b->len = i - s;
		b->name[0] = b->date[0] = 0;
		if (hash >= 0) {
			/* "# Bob, banned 2026-11-27" -> name Bob, date 2026-11-27; anything else: the whole comment is the name */
			int a = hash + 1, z = end;
			while (a < z && ban_ws(bans_buf[a]))
				a++;
			while (z > a && ban_ws(bans_buf[z - 1]))
				z--;
			int cut = -1;
			for (int k = z - 9; k >= a; k--)
				if (!memcmp(bans_buf + k, ", banned ", 9)) {
					cut = k;
					break;
				}
			if (cut >= 0) {
				ban_widen(bans_buf + a, cut - a, b->name, 64);
				ban_widen(bans_buf + cut + 9, z - cut - 9, b->date, 16);
			} else
				ban_widen(bans_buf + a, z - a, b->name, 64);
		}
		if (!b->name[0])
			ban_widen(b->id[0], lstrlenA(b->id[0]), b->name, 64);
		nbans++;
	}
}

static void bans_path(const WCHAR *dir, WCHAR *p)
{
	_snwprintf(p, MAX_PATH, L"%ls\\ronfix-bans.txt", dir);
	p[MAX_PATH - 1] = 0;
}

/* reads the game folder's ban list: the number of bans (0 = no file), -1 = too big to edit here */
static int bans_read(const WCHAR *dir)
{
	WCHAR p[MAX_PATH];
	nbans = bans_len = 0;
	bans_buf[0] = 0;
	if (!dir || !dir[0])
		return 0;
	bans_path(dir, p);
	HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	DWORD size = GetFileSize(f, NULL), got = 0;
	if (size > BAN_FILE_MAX) {
		CloseHandle(f);
		return -1;
	}
	ReadFile(f, bans_buf, size, &got, NULL);
	CloseHandle(f);
	bans_len = (int)got;
	bans_buf[bans_len] = 0;
	bans_parse();
	return nbans;
}

static int ban_holds(const struct ban *b, char ids[][24], int n)
{
	for (int i = 0; i < b->nid; i++)
		for (int k = 0; k < n; k++)
			if (!lstrcmpiA(b->id[i], ids[k]))
				return 1;
	return 0;
}

/* removes every ban line that holds one of ids. The file is read again first (the game may have added a ban since
 * the list was shown) and replaced in one step (a new file moved over the old one), so the game never reads half a
 * file. 0 done, 1 the file is too big, 2 Windows refused the write. */
static int bans_unban(const WCHAR *dir, char ids[][24], int n)
{
	static char out[BAN_FILE_MAX + 1];
	WCHAR p[MAX_PATH], tmp[MAX_PATH + 8];
	if (bans_read(dir) < 0)
		return 1;
	int len = 0, from = 0, left = 0;
	for (int k = 0; k < nbans; k++) {
		if (!ban_holds(&bans[k], ids, n)) {
			left++;
			continue;
		}
		memcpy(out + len, bans_buf + from, (size_t)(bans[k].start - from));
		len += bans[k].start - from;
		from = bans[k].start + bans[k].len;
	}
	memcpy(out + len, bans_buf + from, (size_t)(bans_len - from));
	len += bans_len - from;
	bans_path(dir, p);
	for (int tries = 0; tries < 20; tries++) { /* the game holds the file open for a moment when it reads it */
		if (!left) {
			if (DeleteFileW(p) || GetLastError() == ERROR_FILE_NOT_FOUND)
				return bans_read(dir), 0;
		} else {
			_snwprintf(tmp, MAX_PATH + 8, L"%ls.new", p);
			tmp[MAX_PATH + 7] = 0;
			HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (f != INVALID_HANDLE_VALUE) {
				DWORD w = 0;
				BOOL ok = WriteFile(f, out, (DWORD)len, &w, NULL) && w == (DWORD)len;
				CloseHandle(f);
				if (ok && MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
					return bans_read(dir), 0;
				DeleteFileW(tmp);
			}
		}
		Sleep(100);
	}
	bans_read(dir);
	return 2;
}
