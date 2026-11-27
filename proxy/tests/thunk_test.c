/* Unit tests for the gameplay thunks in netproxy.c (G3 Persian capitals vs World Government, H1 ally returns
 * a moved capital). The thunks are entered exactly as the game enters them (same registers, same stack
 * layout), with the game functions they call replaced by fakes, and their exits caught by labels here.
 * Build (32-bit DLL; McAfee blocks freshly built test EXEs, DLLs are fine):
 *   i686-w64-mingw32-clang -O2 -Wall -shared -nostdlib -Wl,--entry,_DllMain@12 -o thunk_test.dll \
 *       thunk_test.c -lkernel32 -luser32 -lbcrypt
 * Run: tools/run-thunk-tests.ps1 (32-bit PowerShell loads the DLL and calls run_thunk_tests).
 * Also runs the v9 lobby-logging tests (lobby_tests.c). */
#define RONFIX_TEST /* log lines go to test_capture (lobby_tests.c) */
#include "../netproxy.c"

static char *out; static int out_left, fails, passes;
static void say(const char *s) { while (*s && out_left > 1) { *out++ = *s++; out_left--; } *out = 0; }
static void check(int ok, const char *what)
{
	char b[200];
	wsprintfA(b, "%s %s\r\n", ok ? "pass" : "FAIL", what);
	say(b);
	if (ok) passes++; else fails++;
}

/* ---------------------------------------------------------------- H1 */
static unsigned char players[8 * 0x6EEC];
static int team_of[8];
static int xfer_new, xfer_idx, xfer_old, xfer_calls;
static __attribute__((thiscall)) int fake_team(unsigned char *rec, int q)
{
	int p = (int)(rec - players) / 0x6EEC;
	return p == q || team_of[p] == team_of[q];
}
static int __stdcall fake_xfer(int new_owner, int idx, int old_owner)
{
	xfer_new = new_owner; xfer_idx = idx; xfer_old = old_owner; xfer_calls++;
	return 0x77;
}
void *t_h1, *t_g3;
int h1_in_ebx, h1_in_edi, h1_in_idx, h1_out_ebx, h1_out_esi, h1_out_edi, h1_esp_before, h1_esp_after;
extern char h1_back_label[] __asm__("h1_back_label");
/* run_h1(best, city): cdecl, so [ebp+8] = best and [ebp+0xC] = city as at 0x627DBF */
__attribute__((naked, used)) static void run_h1(int best, unsigned char *city)
{
	__asm__ volatile(
		"pushl %ebp\n\t"
		"movl %esp, %ebp\n\t"
		"subl $0x80, %esp\n\t"
		"pushl %ebx\n\t"
		"pushl %esi\n\t"
		"pushl %edi\n\t"
		"movl _h1_in_idx, %eax\n\t"
		"movl %eax, -0x54(%ebp)\n\t"
		"movl _h1_in_ebx, %ebx\n\t"
		"movl _h1_in_edi, %edi\n\t"
		"movl $0x5E5E5E5E, %esi\n\t"
		"movl $0x11111111, %ecx\n\t"
		"movl $0x22222222, %edx\n\t"
		"movl %esp, _h1_esp_before\n\t"
		"jmp *_t_h1\n"
		".globl h1_back_label\n"
		"h1_back_label:\n\t"
		"movl %esp, _h1_esp_after\n\t"
		"movl %ebx, _h1_out_ebx\n\t"
		"movl %esi, _h1_out_esi\n\t"
		"movl %edi, _h1_out_edi\n\t"
		"popl %edi\n\t"
		"popl %esi\n\t"
		"popl %ebx\n\t"
		"movl %ebp, %esp\n\t"
		"popl %ebp\n\t"
		"ret\n");
}
/* owner A, capturer's best player, the new owner the game chose, mask, who is in the game */
static void h1_case(const char *name, int owner, int best, int chosen, int mask, int alive_mask, int want_new, int want_60)
{
	static unsigned char city[0x100];
	for (int i = 0; i < 0x100; i++) city[i] = 0;
	city[0x60] = (unsigned char)owner; city[0x5F] = (unsigned char)owner; city[0x68] = (unsigned char)mask;
	for (int p = 0; p < 8; p++) players[p * 0x6EEC] = (alive_mask >> p & 1) ? 3 : 0;
	h1_in_ebx = chosen; h1_in_edi = owner; h1_in_idx = 5; xfer_calls = 0;
	run_h1(best, city);
	char b[200];
	int ok = xfer_calls == 1 && xfer_new == want_new && xfer_idx == 5 && xfer_old == owner && h1_out_ebx == want_new
	         && city[0x60] == want_60 && h1_out_esi == 0x5E5E5E5E && h1_out_edi == owner && h1_esp_after == h1_esp_before;
	wsprintfA(b, "H1 %s (new owner %d, +0x60 %d, xfer calls %d, stack %s)", name, xfer_new, city[0x60], xfer_calls,
	          h1_esp_after == h1_esp_before ? "balanced" : "UNBALANCED");
	check(ok, b);
}
static void test_h1(void)
{
	t_h1 = (void *)h1_thunk; rf_h1_players = players; rf_h1_team = (void *)fake_team; rf_h1_xfer = (void *)fake_xfer; rf_h1_back = h1_back_label;
	/* teams: A=0 and C=2 vs B=1 and D=3 */
	team_of[0] = 0; team_of[2] = 0; team_of[1] = 1; team_of[3] = 1; team_of[4] = 2; team_of[5] = 2; team_of[6] = 3; team_of[7] = 3;
	/* the reported bug: A (0) holds B's (1) capital with his Senate moved in; D (3) takes it */
	h1_case("A's moved capital, B's bit set, D captures -> back to B", 0, 3, 3, 1 << 1, 0xFF, 1, 1);
	/* the game's own rule already returned it (EBX != best): untouched */
	h1_case("game already returned it to B: unchanged", 0, 3, 1, 1 << 1, 0xFF, 1, 0);
	h1_case("never a capital (mask 0): unchanged", 0, 3, 3, 0, 0xFF, 3, 0);
	h1_case("mask bit of the capturer himself: unchanged", 0, 3, 3, 1 << 3, 0xFF, 3, 0);
	h1_case("mask bit of the current owner: unchanged", 0, 3, 3, 1 << 0, 0xFF, 3, 0);
	h1_case("mask bit of an enemy of the capturer (C): unchanged", 0, 3, 3, 1 << 2, 0xFF, 3, 0);
	h1_case("mask bit of an ally who is out of the game: unchanged", 0, 3, 3, 1 << 1, 0xFF & ~(1 << 1), 3, 0);
	h1_case("bits of an enemy and an ally: goes to the ally", 0, 3, 3, (1 << 2) | (1 << 1), 0xFF, 1, 1);
	h1_case("player 7 (last bit)", 4, 6, 6, 1 << 7, 0xFF, 7, 7);
}

/* ---------------------------------------------------------------- G3 */
static int g3_owns_fc, g3_persian, g3_find_idx, g3_find_owner, g3_owns_arg, g3_find_calls;
static __attribute__((thiscall)) int fake_owns(void *self, int id) { (void)self; g3_owns_arg = id; return g3_owns_fc; }
static __attribute__((thiscall)) int fake_power(void *self, int id) { (void)self; return id == 0x17 && g3_persian; }
static __attribute__((thiscall)) void fake_find(void *self, int *idx, int *owner, int a, int b)
{
	(void)self; (void)a; (void)b; g3_find_calls++;
	*idx = g3_find_idx; *owner = g3_find_owner;
}
int g3_exit, g3_esp_body, g3_esp_exit, g3_out_esi, g3_out_edi, g3_out_ebx;
extern char g3_keep_label[] __asm__("g3_keep_label");
extern char g3_loop_label[] __asm__("g3_loop_label");
int g3_player[4];   /* [esi+8] = player index */
/* Builds the frame of 0x6DC6D0 at 0x6DC743: 3 args, return address, ebx/ebp/esi/edi pushed, then the game's
 * push 0x213; ESI = player, EDI = capturer, EBX = 0. */
__attribute__((naked)) static void run_g3(void)
{
	__asm__ volatile(
		"pushl %ebx\n\t"
		"pushl %esi\n\t"
		"pushl %edi\n\t"
		"pushl %ebp\n\t"
		"pushl $0xA3A3A3A3\n\t"           /* arg3 */
		"pushl $0xA2A2A2A2\n\t"           /* arg2 */
		"pushl $0xA1A1A1A1\n\t"           /* arg1 */
		"pushl $0xBADC0DE\n\t"            /* return address of 0x6DC6D0 */
		"pushl $0\n\t"                    /* ebx */
		"pushl %ebp\n\t"
		"pushl $0\n\t"                    /* esi */
		"pushl $0\n\t"                    /* edi */
		"movl %esp, _g3_esp_body\n\t"
		"leal _g3_player, %esi\n\t"
		"movl $3, %edi\n\t"
		"xorl %ebx, %ebx\n\t"
		"pushl $0x213\n\t"
		"jmp *_t_g3\n"
		".globl g3_keep_label\n"
		"g3_keep_label:\n\t"
		"movl $1, _g3_exit\n\t"
		"jmp 1f\n"
		".globl g3_loop_label\n"
		"g3_loop_label:\n\t"
		"movl $2, _g3_exit\n"
		"1:\n\t"
		"movl %esp, _g3_esp_exit\n\t"
		"movl %esi, _g3_out_esi\n\t"
		"movl %edi, _g3_out_edi\n\t"
		"movl %ebx, _g3_out_ebx\n\t"
		"addl $0x20, %esp\n\t"            /* drop the fake frame */
		"popl %ebp\n\t"
		"popl %edi\n\t"
		"popl %esi\n\t"
		"popl %ebx\n\t"
		"ret\n");
}
static void g3_case(const char *name, int fc, int persian, int idx, int owner, int want_exit, int want_find)
{
	g3_owns_fc = fc; g3_persian = persian; g3_find_idx = idx; g3_find_owner = owner; g3_find_calls = 0; g3_exit = 0;
	g3_player[2] = 4;   /* this player = 4 */
	run_g3();
	char b[200];
	int ok = g3_exit == want_exit && g3_owns_arg == 0x213 && g3_esp_exit == g3_esp_body && g3_find_calls == want_find
	         && g3_out_esi == (int)(void *)g3_player && g3_out_edi == 3 && g3_out_ebx == 0;
	wsprintfA(b, "G3 %s (%s, stack %s)", name, g3_exit == 1 ? "no instant elimination" : g3_exit == 2 ? "World Government check" : "NO EXIT",
	          g3_esp_exit == g3_esp_body ? "balanced" : "UNBALANCED");
	check(ok, b);
}
static void test_g3(void)
{
	t_g3 = (void *)g3_thunk; rf_g3_owns = (void *)fake_owns; rf_g3_power = (void *)fake_power; rf_g3_find = (void *)fake_find;
	rf_g3_keep = g3_keep_label; rf_g3_wgloop = g3_loop_label;
	g3_case("Forbidden City owner", 1, 0, -1, 4, 1, 0);
	g3_case("not Persian", 0, 0, -1, 4, 2, 0);
	g3_case("Persian with a second capital", 0, 1, 2, 4, 1, 1);
	g3_case("Persian, no second capital", 0, 1, -1, 4, 2, 1);
	g3_case("Persian, only a captured capital held by player 0", 0, 1, 2, 0, 2, 1);
}

#include "lobby_tests.c"
#include "steam_tests.c"
#include "lobbyfix_tests.c"
#include "ghostroom_tests.c"
#include "turndelay_tests.c"

__declspec(dllexport) int __stdcall run_thunk_tests(char *buf, int len)
{
	out = buf; out_left = len; fails = passes = 0; buf[0] = 0;
	test_h1();
	test_g3();
	test_lobby();
	test_steam();
	test_lobbyfix();
	test_ghostroom();
	test_turndelay();
	char b[80]; wsprintfA(b, "%d passed, %d failed\r\n", passes, fails); say(b);
	return fails;
}
