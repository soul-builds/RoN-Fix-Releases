/* Unit tests for soundfix.c (quit-crash fix in memory on the game's own dssl.dll, moving old installs back to it).
 * Included by thunk_test.c after ghostroom_tests.c: reuses zero(), the log capture and check(). The "dssl.dll" is a
 * fake image in memory with the real build's header values, table and channel pools at the real offsets. */

static unsigned char snd_img[SND_IMAGESIZE]; /* the fake dssl.dll image */
static unsigned char snd_slots[3][4 * SND_SLOT_SIZE];
static int snd_fake_calls;
static void *snd_fake_self;
static int THISCALL snd_fake_shutdown(void *self)
{
	snd_fake_calls++;
	snd_fake_self = self;
	return 7;
}

static void snd_make_image(DWORD stamp)
{
	zero(snd_img, sizeof snd_img);
	*(LONG *)(snd_img + 0x3c) = 0x80;                     /* e_lfanew */
	*(DWORD *)(snd_img + 0x80) = 0x4550;                  /* "PE\0\0" */
	*(DWORD *)(snd_img + 0x80 + 8) = stamp;               /* FileHeader.TimeDateStamp */
	*(DWORD *)(snd_img + 0x80 + 24 + 56) = SND_IMAGESIZE; /* OptionalHeader.SizeOfImage */
	*(void **)(snd_img + SND_VT_RVA + 8) = snd_img + SND_SHUTDOWN_RVA;
	for (int p = 0; p < 3; p++) {
		zero(snd_slots[p], sizeof snd_slots[p]);
		*(int *)(snd_img + snd_pool_rva[p] + 4) = 4;
		*(void **)(snd_img + snd_pool_rva[p] + 0x10) = snd_slots[p];
		for (int i = 0; i < 3; i++) /* 3 of 4 slots point at a (freed) sound object */
			*(void **)(snd_slots[p] + i * SND_SLOT_SIZE + SND_SLOT_OWNER_OFF) = (void *)(0x5000 + p * 16 + i);
	}
}

/* a file with just enough PE header for snd_file_build */
static void snd_write_pe(const char *path, DWORD stamp)
{
	static unsigned char h[0x400];
	DWORD w;
	zero(h, sizeof h);
	h[0] = 'M', h[1] = 'Z';
	*(DWORD *)(h + 0x3c) = 0x80;
	h[0x80] = 'P', h[0x81] = 'E';
	*(DWORD *)(h + 0x80 + 8) = stamp;
	*(DWORD *)(h + 0x80 + 0x50) = SND_IMAGESIZE;
	HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	WriteFile(f, h, sizeof h, &w, NULL);
	CloseHandle(f);
}
static int snd_file_is(const char *path, DWORD stamp)
{
	DWORD st = 0, sz = 0;
	return snd_file_build(path, &st, &sz) && st == stamp;
}

static void test_soundfix(void)
{
	char saved_dir[MAX_PATH], base[MAX_PATH], cur[MAX_PATH], orig[MAX_PATH], keep[MAX_PATH];

	/* 1. the game's own dssl.dll: slot 2 of the sound system's table now runs the fix, then the real shutdown */
	snd_make_image(SND_TIMESTAMP);
	cap_clear();
	int r = snd_patch(snd_img);
	void **vt = (void **)(snd_img + SND_VT_RVA);
	check(r == 1 && vt[2] == (void *)snd_w_shutdown && snd_orig_shutdown == (snd_shutdown_fn)(snd_img + SND_SHUTDOWN_RVA) &&
	          count_of("SOUND quit-crash fix active") == 1,
	      "sound: the game's own dssl.dll gets the quit-crash fix in memory (table slot 2)");
	snd_orig_shutdown = snd_fake_shutdown; /* the fake image has no code to run */
	snd_fake_calls = 0;
	int ret = ((snd_shutdown_fn)vt[2])((void *)0x1234);
	int left = 0;
	for (int p = 0; p < 3; p++)
		for (int i = 0; i < 4; i++)
			left += *(void **)(snd_slots[p] + i * SND_SLOT_SIZE + SND_SLOT_OWNER_OFF) != 0;
	check(left == 0 && snd_fake_calls == 1 && snd_fake_self == (void *)0x1234 && ret == 7 &&
	          count_of("cleared 9 stale owner pointers in 12 channel slots") == 1,
	      "sound: at shutdown the stale owner pointers are cleared, then the real shutdown runs");

	/* 2. another build, or an unexpected table: nothing is touched */
	snd_make_image(0x12345678);
	cap_clear();
	r = snd_patch(snd_img);
	int other = r == -1 && vt[2] == (void *)(snd_img + SND_SHUTDOWN_RVA) && count_of("not the build") == 1;
	snd_make_image(SND_TIMESTAMP);
	vt[2] = (void *)0x10009999;
	r = snd_patch(snd_img);
	check(other && r == -1 && vt[2] == (void *)0x10009999 && count_of("not as expected") == 1,
	      "sound: another dssl.dll build or an unexpected table is left alone (logged)");

	/* 3. old installs: stand-in dssl.dll + the game's own dssl_orig.dll -> the original back, the stand-in kept aside */
	lstrcpyA(saved_dir, dir);
	GetTempPathA(MAX_PATH - 40, base);
	lstrcatA(base, "ronfix-snd-test\\");
	CreateDirectoryA(base, NULL);
	lstrcpyA(dir, base);
	lstrcpyA(cur, base), lstrcatA(cur, "dssl.dll");
	lstrcpyA(orig, base), lstrcatA(orig, "dssl_orig.dll");
	lstrcpyA(keep, base), lstrcatA(keep, "ronfix\\previous\\dssl-standin.dll");
	snd_write_pe(cur, 0x6a6f0001); /* the stand-in */
	snd_write_pe(orig, SND_TIMESTAMP);
	cap_clear();
	snd_migrate();
	check(snd_file_is(cur, SND_TIMESTAMP) && GetFileAttributesA(orig) == INVALID_FILE_ATTRIBUTES && snd_file_is(keep, 0x6a6f0001) &&
	          count_of("the game's own dssl.dll is back") == 1,
	      "sound: an old install gets the game's own dssl.dll back, the stand-in goes to ronfix\\previous");
	snd_migrate();
	check(snd_file_is(cur, SND_TIMESTAMP), "sound: nothing more to do on the next start");

	/* both are the original (a player put it back by hand): the spare copy goes */
	snd_write_pe(orig, SND_TIMESTAMP);
	snd_migrate();
	int spare = GetFileAttributesA(orig) == INVALID_FILE_ATTRIBUTES && snd_file_is(cur, SND_TIMESTAMP);
	/* dssl_orig.dll of an unknown build: left alone */
	snd_write_pe(cur, 0x6a6f0001);
	snd_write_pe(orig, 0x11111111);
	cap_clear();
	snd_migrate();
	check(spare && snd_file_is(cur, 0x6a6f0001) && snd_file_is(orig, 0x11111111) && count_of("left as it is") == 1,
	      "sound: a spare original copy is removed; an unknown dssl_orig.dll is left alone");
	DeleteFileA(cur), DeleteFileA(orig), DeleteFileA(keep);
	lstrcpyA(keep, base), lstrcatA(keep, "ronfix\\previous");
	RemoveDirectoryA(keep);
	lstrcpyA(keep, base), lstrcatA(keep, "ronfix");
	RemoveDirectoryA(keep);
	RemoveDirectoryA(base);
	lstrcpyA(dir, saved_dir);

	/* 4. Smart App Control's state is read and logged */
	cap_clear();
	int s = sac_state();
	sac_log();
	check(s >= -1 && s <= 2 && count_of("Windows Smart App Control: ") == 1, "sound: Smart App Control's state is logged");
	snd_state = 0, snd_base = 0, snd_orig_shutdown = 0;
}
