/* RoN-Fix 0.12's "better graphics" (ReShade 6.8.0 as dxgi.dll, SweetFX shaders, two presets): what its setup put
 * into the game folder. 0.13 took the option out, and RoN-Fix removes the files again in two places:
 *   setup/graphics.c   on every install and on "Restore original game"
 *   proxy/update.c     for players who only ever update and so never run the setup again
 * One list for both, so the two cannot drift apart. Every path is relative to the game folder.
 *
 * The rules: only when GFX_MARKER exists (0.12 wrote it when it installed the files). dxgi.dll only when it is byte
 * for byte ReShade 6.8.0's ReShade32.dll: a dxgi.dll that is not ours (the player's own ReShade or another add-on)
 * is never touched. The shader folders only when they are empty: never anything the player added. */
#define GFX_MARKER L"ronfix\\graphics.txt"
#define GFX_DLL L"dxgi.dll"
#define RESHADE32_SHA "538998f66c0197adcdeadfe8e5ea19dd6ca3253e40399476d291f385ce518b88"
/* Where the updater moves that dxgi.dll while the running game has it loaded (Windows lets a loaded DLL be renamed,
 * not deleted). It is deleted at the next start, or by the setup. */
#define GFX_ASIDE_DIR L"ronfix\\previous"
#define GFX_ASIDE L"ronfix\\previous\\dxgi-reshade.dll"

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
/* outermost first: removed from the last to the first, each only if it is empty */
static const WCHAR *const gfx_dirs[] = { L"reshade-shaders", L"reshade-shaders\\Shaders", L"reshade-shaders\\Shaders\\SweetFX" };
#define GFX_NDIRS (int)(sizeof gfx_dirs / sizeof gfx_dirs[0])
