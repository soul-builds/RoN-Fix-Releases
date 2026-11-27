/* ------------------------------------------------------------------ ghost room clean-up (core v15)
 * Included by netproxy.c after lobbyfix.c (reuses its PlayFab wrappers and the lobby tick).
 * reverse/lobby-start-network.md, "Ghost room after Create": sometimes the game's Create does not finish.
 *   - PlayFab creates the lobby and the game posts its data once.
 *   - Then the game gives up: no network session, no room window. It goes back to the Game Browser and joins
 *     the browser's chat lobby again.
 *   - It never leaves the lobby it created. That lobby stays listed ("Host: <you>, 1/8"), and hosting again does
 *     not work until the player has joined and left their own ghost room (user report, 2026-11-26 21:49).
 *   - The game's own connection log is overwritten at every start, so the failing step is not known yet.
 *
 * The clean-up does what the game's own Leave would have done, from the same main thread.
 * CrossplayProxy's CrossPlayService::LeaveLobby (CP 0x10017fe0) is two steps:
 *   - QlocPFMultiplayer::LeaveLobby, i.e. PFLobbyLeave on the game lobby handle;
 *   - P2PCloseAll (service vtable +0xCC, CP 0x1001e250): leave the Party network if one exists.
 * Here: PFLobbyLeave(ghost lobby, our entity key, ctx NULL), then P2PCloseAll on the service object
 * (CrossplayNetLib netsys +0xCC, set by C_initialize_crossplay_lib).
 *
 * Why ctx NULL is safe: CrossplayProxy's state-change dispatch finds per-request callbacks by asyncContext
 * (case 6 -> N_remove_list_node_locked(list, ctx)). It finds nothing for NULL and skips, like the kicks in
 * lobbyfix.c (case 5). Its internal callbacks then reset its game-lobby state on their own (+0x46c = 0,
 * N_reset_lobby_member_state CP 0x100372f0):
 *   - "member removed: us" (SetInternalCallbacks lambda_3);
 *   - "disconnected" (lambda_4).
 *
 * When it acts, all of these must hold:
 *   - we created a lobby (CreateAndJoinLobbyCompleted OK), and the room window has not been open at any time
 *     since then;
 *   - the game then JOINED another lobby (the browser's chat): it has given up on the room. A normal Create
 *     opens the room window about 1.7 s after the create, with no join in between;
 *   - 3 s after that join the room window is still closed, the game is not running a match, and the created
 *     lobby has not been left;
 *   - the service object is CrossplayProxy's (vtable and slot checked against this build's addresses).
 *     Otherwise nothing is done and the reason is logged.
 * At most once per created lobby. [lobby] GhostRoom=1 (0 = off). */

#define GR_CONFIRM_MS 3000
#define CP_SERVICE_VT_RVA 0x9adc4   /* CrossplayProxy::CrossPlayService::vftable (CP 0x1009adc4) */
#define CP_P2PCLOSEALL_RVA 0x1e250  /* its slot 51 (+0xCC): P2PCloseAll (CP 0x1001e250) */
#define NETSYS_SERVICE 0xCC         /* CrossplayNetLibSys: the ICrossplayService object */

static int gr_enabled = 1, gr_checked;
static void *gr_lobby;               /* the lobby we created and the game has not left yet */
static int gr_room_seen, gr_done;
static DWORD gr_created_at, gr_join_at;
static void *gr_join_lobby;
static void *gr_vt_expect, *gr_close_expect; /* set from CrossplayProxy.dll's base (tests set them) */

static void gr_on_created(void *lobby, DWORD now)
{
	gr_lobby = lobby;
	gr_created_at = now;
	gr_room_seen = gr_done = 0;
	gr_join_at = 0;
	gr_join_lobby = 0;
}

static void gr_on_joined(void *lobby, DWORD now)
{
	if (gr_lobby && lobby != gr_lobby && !gr_room_seen && !gr_done && !gr_join_at) {
		gr_join_at = now ? now : 1;
		gr_join_lobby = lobby;
	}
}

static void gr_on_gone(void *lobby)
{
	if (lobby && lobby == gr_lobby)
		gr_lobby = 0;
}

static int gr_service(void *self, void **svc, void **close)
{
	void **vt = 0;
	if (!gr_vt_expect) {
		unsigned char *cp = (unsigned char *)GetModuleHandleA("CrossplayProxy.dll");
		if (cp) {
			gr_vt_expect = cp + CP_SERVICE_VT_RVA;
			gr_close_expect = cp + CP_P2PCLOSEALL_RVA;
		}
	}
	*svc = *close = 0;
	if (!gr_vt_expect || !safe_read((char *)self + NETSYS_SERVICE, svc, 4) || !*svc || !safe_read(*svc, &vt, 4) ||
	    (void *)vt != gr_vt_expect || !safe_read(vt + 51, close, 4) || *close != gr_close_expect)
		return 0;
	return 1;
}

typedef void(THISCALL *gr_close_fn)(void *);

/* every lobby tick (100 ms, main thread) */
static void gr_tick(void *self, DWORD now, void *setup, unsigned flags)
{
	void *svc = 0, *close;
	if (gr_enabled && !gr_checked && safe_read((char *)self + NETSYS_SERVICE, &svc, 4) && svc) {
		gr_checked = 1; /* once per start, so every log shows whether the clean-up could act on this build */
		logline("ghost room clean-up ready: CrossplayProxy's service object %s", gr_service(self, &svc, &close)
		        ? "recognised" : "NOT recognised (unexpected build): the clean-up will not act");
	}
	if (!gr_enabled || !gr_lobby || gr_done)
		return;
	if (setup) {
		gr_room_seen = 1; /* the room window opened: a normal create */
		return;
	}
	if (gr_room_seen || !gr_join_at || now - gr_join_at < GR_CONFIRM_MS)
		return;
	gr_done = 1;
	int no = pf_lobby_no(gr_lobby, "ghost room"), back = pf_lobby_no(gr_join_lobby, "ghost room");
	if (flags & 0x40) {
		logline("LOBBY ghost room? lobby #%d: the room window never opened, but a match is running: left alone", no);
		return;
	}
	if (!pf_leave_orig || !pf_user[0] || !gr_service(self, &svc, &close)) {
		logline("LOBBY ghost room: lobby #%d was created %lu ms ago and its room window never opened (the game went back to the "
		        "browser, lobby #%d), but it is NOT cleaned up: %s", no, (unsigned long)(now - gr_created_at), back,
		        !pf_leave_orig ? "no PlayFab leave hook" : !pf_user[0] ? "our PlayFab id is not known yet"
		                                                             : "CrossplayProxy's service object is not the expected build");
		return;
	}
	static struct {
		const char *id, *type;
	} me;
	me.id = pf_user;
	me.type = pf_user_type[0] ? pf_user_type : "title_player_account";
	logline("LOBBY ghost room: lobby #%d was created %lu ms ago, its room window never opened and the game went back to the "
	        "browser (joined lobby #%d %lu ms ago): leaving it, as the game's own Leave would",
	        no, (unsigned long)(now - gr_created_at), back, (unsigned long)(now - gr_join_at));
	long hr = pf_leave_orig(gr_lobby, &me, NULL); /* w_pf_leave logs nothing here: this is the original */
	logline("LOBBY ghost room: PlayFab leave of lobby #%d -> %08lx; closing its network (P2PCloseAll)", no, hr);
	((gr_close_fn)close)(svc);
}
