/* ------------------------------------------------------------------ better graphics (ReShade): REMOVED in 0.13
 * Included by setup.c. RoN-Fix 0.12's setup could install ReShade 6.8.0 (as dxgi.dll) with SweetFX shaders. 0.13
 * takes the option out: with the Steam overlay on (how most players play) ReShade can kill the game at start-up.
 * reverse/reshade-steam-overlay.md has the cause (a data race in ReShade's own hook manager) and what a future
 * version would need. The installer code is in git history (setup/graphics.c before 2026-11-27).
 *
 * What is left: removing what 0.12 installed, on every install and on "Restore original game". Only files 0.12
 * listed itself as owning (ronfix\graphics.txt exists): dxgi.dll only when it is byte for byte ReShade 6.8.0's
 * ReShade32.dll, the 8 shader files, the 2 presets, ReShade.ini / ReShade.log, and the shader folders when empty.
 * A dxgi.dll that is not ours (the player's own ReShade or another add-on) is never touched. */

#define RESHADE32_SHA "538998f66c0197adcdeadfe8e5ea19dd6ca3253e40399476d291f385ce518b88"

static const WCHAR *const gfx_files[] = {
	L"reshade-shaders\\Shaders\\SweetFX\\SMAA.fx",     L"reshade-shaders\\Shaders\\SweetFX\\SMAA.fxh",
	L"reshade-shaders\\Shaders\\SweetFX\\CAS.fx",      L"reshade-shaders\\Shaders\\SweetFX\\Vibrance.fx",
	L"reshade-shaders\\Shaders\\SweetFX\\Curves.fx",   L"reshade-shaders\\Shaders\\SweetFX\\Levels.fx",
	L"reshade-shaders\\Shaders\\ReShade.fxh",          L"reshade-shaders\\Shaders\\ReShadeUI.fxh",
	L"ronfix-graphics-subtle.ini",                     L"ronfix-graphics-strong.ini",
	L"ReShade.ini",                                    L"ReShade.log",
	L"ReShade.log1",
};
#define GFX_N (int)(sizeof gfx_files / sizeof gfx_files[0])
static const WCHAR *const gfx_dirs[] = { L"reshade-shaders", L"reshade-shaders\\Shaders", L"reshade-shaders\\Shaders\\SweetFX" };

static int sha256_hex(const WCHAR *p, char out[65])
{
	static const char hx[] = "0123456789abcdef";
	BCRYPT_ALG_HANDLE alg = 0;
	BCRYPT_HASH_HANDLE h = 0;
	unsigned char d[32], buf[65536];
	DWORD got;
	int ok = 0;
	out[0] = 0;
	HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
		while (ReadFile(f, buf, sizeof buf, &got, NULL) && got)
			BCryptHashData(h, buf, got, 0);
		ok = BCryptFinishHash(h, d, 32, 0) == 0;
	}
	if (h)
		BCryptDestroyHash(h);
	if (alg)
		BCryptCloseAlgorithmProvider(alg, 0);
	CloseHandle(f);
	for (int k = 0; ok && k < 32; k++)
		out[2 * k] = hx[d[k] >> 4], out[2 * k + 1] = hx[d[k] & 15];
	out[ok ? 64 : 0] = 0;
	return ok;
}

static int file_is_sha(const WCHAR *p, const char *sha)
{
	char h[65];
	return sha256_hex(p, h) && !strcmp(h, sha);
}

/* 1 = 0.12's better graphics was there and is now removed, 0 = it was not installed */
static int graphics_remove(const WCHAR *dir)
{
	WCHAR p[MAX_PATH], list[MAX_PATH];
	if (!path(list, dir, L"ronfix\\graphics.txt") || !exists(list))
		return 0;
	path(p, dir, L"dxgi.dll"); /* first: it is the file that switches ReShade on */
	if (file_is_sha(p, RESHADE32_SHA))
		DeleteFileW(p);
	for (int k = 0; k < GFX_N; k++)
		if (path(p, dir, gfx_files[k]))
			DeleteFileW(p);
	for (int k = 2; k >= 0; k--) /* only if empty: never anything the player added */
		if (path(p, dir, gfx_dirs[k]))
			RemoveDirectoryW(p);
	DeleteFileW(list);
	return 1;
}
