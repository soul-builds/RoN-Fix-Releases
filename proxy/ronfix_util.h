/* Helpers shared by loader.c and update.c (no CRT). The including file defines gdir and ulog. */
/* ------------------------------------------------------------------ small helpers (no CRT) */
static __attribute__((unused)) void zero(void *p, DWORD n)
{
	volatile unsigned char *b = p;
	while (n--)
		*b++ = 0;
}

static __attribute__((unused)) void copy(void *d, const void *s, DWORD n)
{
	volatile unsigned char *o = d;
	const unsigned char *i = s;
	while (n--)
		*o++ = *i++;
}

/* dst = a + b (+ c), all wide; 0 if it does not fit */
static __attribute__((unused)) int wjoin(WCHAR *dst, int cap, const WCHAR *a, const WCHAR *b, const WCHAR *c)
{
	int n = 0;
	const WCHAR *parts[3] = { a, b, c };
	for (int k = 0; k < 3; k++)
		for (const WCHAR *s = parts[k]; s && *s; s++) {
			if (n + 1 >= cap)
				return 0;
			dst[n++] = *s;
		}
	dst[n] = 0;
	return 1;
}

static __attribute__((unused)) void widen(const char *s, WCHAR *w, int cap)
{
	int n = 0;
	while (s[n] && n + 1 < cap) {
		w[n] = (unsigned char)s[n];
		n++;
	}
	w[n] = 0;
}

static __attribute__((unused)) const char *narrow(const WCHAR *w, char *buf, int cap)
{
	if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, cap, NULL, NULL))
		buf[0] = 0;
	return buf;
}

static __attribute__((unused)) int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static __attribute__((unused)) int starts(const char *s, const char *p)
{
	while (*p)
		if (*s++ != *p++)
			return 0;
	return 1;
}

static __attribute__((unused)) WCHAR lower_w(WCHAR c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* " -original" as a separate word anywhere on the command line */
static __attribute__((unused)) int has_switch(const WCHAR *cl, const WCHAR *sw)
{
	for (const WCHAR *p = cl; *p; p++) {
		if (p != cl && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '"')
			continue;
		const WCHAR *a = p, *b = sw;
		while (*b && lower_w(*a) == lower_w(*b))
			a++, b++;
		if (!*b && (*a == 0 || *a == ' ' || *a == '\t' || *a == '"'))
			return 1;
	}
	return 0;
}

static __attribute__((unused)) void ulogf(const char *fmt, ...)
{
	char buf[1100];
	SYSTEMTIME t;
	va_list ap;
	DWORD w;
	if (ulog == INVALID_HANDLE_VALUE)
		return;
	GetLocalTime(&t);
	int n = wsprintfA(buf, "%04d-%02d-%02d %02d:%02d:%02d [%lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
	                  t.wSecond, GetCurrentProcessId());
	va_start(ap, fmt);
	n += wvsprintfA(buf + n, fmt, ap);
	va_end(ap);
	buf[n++] = '\r';
	buf[n++] = '\n';
	WriteFile(ulog, buf, (DWORD)n, &w, NULL);
}

static __attribute__((unused)) void ulog_open(void)
{
	WCHAR p[MAX_PATH];
	if (ulog == INVALID_HANDLE_VALUE && wjoin(p, MAX_PATH, gdir, L"ronfix-update.log", 0))
		ulog = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
		                   FILE_ATTRIBUTE_NORMAL, NULL);
}

