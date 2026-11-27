/* ------------------------------------------------------------------ Steam link, phase A (core v11): shadow mode
 * Included by netproxy.c. RoN-Fix players find each other over Steam's own networking
 * (ISteamNetworkingMessages002, through Steam Datagram Relay: Valve's relay network, NAT handled by the Steam
 * client) and measure ping and loss on it, next to the game's PlayFab Party traffic. NOTHING of the game's
 * traffic changes. This only says hello, pings and logs, so real games show whether Steam's path would be better
 * than PlayFab's before phase B moves game data onto it (reverse/steam-transport.md):
 *   STEAM link to <name> ready (RoN-Fix release N)  /  STEAM link to <name>: no answer
 *   STEAM <name>: rtt avg A min B max C ms over N pings, L lost; Steam: ping P ms, quality local/remote Q%/R%
 * Peers come from the network DLL's player list (peer+0x5c = Steam ID64 as text). Messages from Steam IDs that are
 * not in that list are thrown away unread. Wire format on channel SL_CHANNEL: "RFX", version 1, type, payload.
 * Runs on the main thread from check_pulse, only while the list has another player (no Steam networking in single
 * player). check_pulse also runs during a freeze: the game's turn-wait loop pumps the network on every pass
 * (wait_for_turn_commands 0x93EF10 -> C_network_message_pump 0x951300 -> 0x950F30, which calls netsys slot 15 first).
 * Never logs Steam's ping location or relay names: they reveal where a PC is. [steam] Shadow=1 (default).
 *
 * Freeze watch (core v14): the question phase B depends on is whether Steam's path stays up while the game's
 * PlayFab traffic stalls. So while the game waits for a player's turn data (netsys slot 62, w_waiting), every
 * WAIT line says what the Steam link to that player does: when it was last heard, how many of the pings sent during
 * this wait were answered (one every 0.5 s to that player instead of every 2 s), and whether that player is itself
 * waiting, and for whom. Our PONG carries that last part (4 + 4 bytes after seq and time; older RoN-Fix reads only
 * the first 8 and ignores the rest): a lockstep freeze chains (A waits for B because B waits for C), and the root
 * is the last one in the chain. The game's player table entry (+0xC0) is the same CrossplayNetLibPlayer object as in
 * the network DLL's list (process_desync_msg 0x10019F70 writes +0xA8 of it, the wait loop reads it via +0xC0). */

#ifdef RONFIX_TEST
static DWORD sl_test_now;
#define SL_NOW() sl_test_now
#else
#define SL_NOW() GetTickCount()
#endif
#define SL_CHANNEL 7
#define SL_PEERS 8
#define SL_HELLO_EVERY 3000
#define SL_HELLO_TRIES 8
#define SL_PING_EVERY 2000
#define SL_LOG_EVERY 30000
#define SL_WAIT_PING_EVERY 500 /* to the player the game waits for */
enum { SL_HELLO = 1, SL_ACK = 2, SL_PING = 3, SL_PONG = 4 };
enum { SL_SEND_UNRELIABLE_NONAGLE = 1, SL_SEND_RELIABLE = 8, SL_SEND_AUTORESTART = 32 };

/* steamnetworkingtypes.h, Windows = VALVE_CALLBACK_PACK_LARGE (pack 8) */
struct sl_ident {
	int type; /* k_ESteamNetworkingIdentityType_SteamID = 16 */
	int size; /* 8 */
	union {
		unsigned long long sid;
		char raw[128];
	} u;
};
_Static_assert(sizeof(struct sl_ident) == 136, "SteamNetworkingIdentity is 136 bytes");
/* SteamNetworkingMessage_t, MEASURED in the game against the real Steam client 2026-11-27
 * (tools/frida/steammsg_check.py, a message sent to our own Steam ID and received back): m_pData +0, m_cbSize +4,
 * m_conn +8, m_identityPeer +12 (SteamNetworkingIdentity is declared pack 1, so not 8-aligned: steam id at +20),
 * m_nConnUserData +152, m_usecTimeReceived +160, m_nMessageNumber +168, m_pfnFreeData +176, m_pfnRelease +180,
 * m_nChannel +184. Slots 0, 1 and 5 below were called there too. */
#define SLM_DATA 0
#define SLM_SIZE 4
#define SLM_IDENT 12
#define SLM_RELEASE 180
#define SLM_CHANNEL 184

/* ISteamNetworkingMessages002 vtable: 0 SendMessageToUser(identity&, data, len, flags, channel) -> EResult,
 * 1 ReceiveMessagesOnChannel(channel, msg**, max) -> int, 2 AcceptSessionWithUser, 3 CloseSessionWithUser,
 * 4 CloseChannelWithUser, 5 GetSessionConnectionInfo(identity&, info*, status*) -> state */
typedef int(THISCALL *sl_send_fn)(void *, const struct sl_ident *, const void *, unsigned, int, int);
typedef int(THISCALL *sl_recv_fn)(void *, int, void **, int);
typedef int(THISCALL *sl_close_fn)(void *, const struct sl_ident *);
typedef int(THISCALL *sl_info_fn)(void *, const struct sl_ident *, void *, void *);
typedef void(__cdecl *sl_release_fn)(void *);

struct sl_peer {
	unsigned char *ptr; /* CrossplayNetLibPlayer */
	unsigned long long sid;
	char name[48];
	int ready, hellos, gave_up, release;
	DWORD hello_at, ping_at, log_at;
	unsigned seq, sent, got, rtt_sum, rtt_min, rtt_max;
	DWORD heard_at;                 /* any message from this player, 0 = never */
	unsigned w_sent, w_got, w_rtt_max; /* pings during the current wait for this player */
	int their_wait;                 /* from their last pong: the player THEY wait for, -1 none */
	unsigned their_wait_ms;
	DWORD their_wait_at;            /* when that pong came, 0 = never */
};
static struct sl_peer slp[SL_PEERS];
static void *sl_msgs, *sl_utils;
static int sl_enabled = 1, sl_started, sl_broken;
static unsigned sl_release_no; /* our RoN-Fix release, sent in HELLO */
static DWORD sl_next;
static struct sl_peer *sl_wpeer;    /* the player the game waits for right now, if we have a link entry */
static int sl_my_wait = -1;         /* the player WE wait for (game player number), sent in our pongs */
static unsigned sl_my_wait_ms;
static DWORD sl_my_wait_at;

static int sl_parse_sid(const char *s, unsigned long long *out)
{
	unsigned long long v = 0;
	int n = 0;
	for (; s[n]; n++) {
		if (s[n] < '0' || s[n] > '9' || n >= 20)
			return 0;
		v = v * 10 + (unsigned)(s[n] - '0');
	}
	*out = v;
	return n == 17 && (v >> 52) == 0x011; /* individual account, public universe: 7656119xxxxxxxxxx */
}

static void sl_ident_of(struct sl_ident *id, unsigned long long sid)
{
	volatile unsigned char *b = (volatile unsigned char *)id;
	for (unsigned i = 0; i < sizeof *id; i++)
		b[i] = 0;
	id->type = 16;
	id->size = 8;
	id->u.sid = sid;
}

static int sl_send(unsigned long long sid, int type, const void *payload, int plen, int flags)
{
	unsigned char m[32];
	struct sl_ident id;
	m[0] = 'R', m[1] = 'F', m[2] = 'X', m[3] = 1, m[4] = (unsigned char)type;
	for (int i = 0; i < plen && i < 24; i++)
		m[5 + i] = ((const unsigned char *)payload)[i];
	sl_ident_of(&id, sid);
	return ((sl_send_fn)(*(void ***)sl_msgs)[0])(sl_msgs, &id, m, (unsigned)(5 + plen), flags, SL_CHANNEL);
}

static int sl_start(void)
{
#ifndef RONFIX_TEST
	HMODULE sa = GetModuleHandleA("steam_api.dll");
	typedef void *(__cdecl *get_client_fn)(void);
	typedef int(__cdecl *get_handle_fn)(void);
	typedef void *(__cdecl *generic_fn)(void *, int, int, const char *);
	get_client_fn steam_client = sa ? (get_client_fn)GetProcAddress(sa, "SteamClient") : 0;
	get_handle_fn get_user = sa ? (get_handle_fn)GetProcAddress(sa, "SteamAPI_GetHSteamUser") : 0;
	get_handle_fn get_pipe = sa ? (get_handle_fn)GetProcAddress(sa, "SteamAPI_GetHSteamPipe") : 0;
	generic_fn generic = sa ? (generic_fn)GetProcAddress(sa, "SteamAPI_ISteamClient_GetISteamGenericInterface") : 0;
	void *client = steam_client ? steam_client() : 0;
	int user = get_user ? get_user() : 0, pipe = get_pipe ? get_pipe() : 0;
	if (!client || !user || !pipe || !generic) {
		logline("STEAM link off: Steam's interfaces are not reachable (client %p user %d pipe %d)", client, user, pipe);
		return 0;
	}
	sl_msgs = generic(client, user, pipe, "SteamNetworkingMessages002");
	sl_utils = generic(client, user, pipe, "SteamNetworkingUtils004");
	if (!sl_msgs || !sl_utils) {
		logline("STEAM link off: this Steam client has no SteamNetworkingMessages002/Utils004");
		return 0;
	}
	/* InitRelayNetworkAccess() is an inline for CheckPingDataUpToDate(1e10f), Utils004 slot 7 */
	typedef int(THISCALL *check_fn)(void *, float);
	((check_fn)(*(void ***)sl_utils)[7])(sl_utils, 1e10f);
	logline("STEAM link on (shadow mode: measures only, the game's traffic stays on PlayFab)");
#endif
	return sl_msgs != 0;
}

static struct sl_peer *sl_find(unsigned long long sid)
{
	for (int k = 0; k < SL_PEERS; k++)
		if (slp[k].ptr && slp[k].sid == sid)
			return &slp[k];
	return 0;
}

static void sl_log_stats(struct sl_peer *p, const char *why)
{
	char steam[120];
	unsigned char st[256];
	struct sl_ident id;
	steam[0] = 0;
	sl_ident_of(&id, p->sid);
	for (int i = 0; i < 256; i++)
		st[i] = 0;
	/* SteamNetConnectionRealTimeStatus_t: m_eState +0, m_nPing +4, m_flConnectionQualityLocal +8, Remote +12 */
	int state = ((sl_info_fn)(*(void ***)sl_msgs)[5])(sl_msgs, &id, NULL, st);
	if (state == 3) {
		float ql = *(float *)(st + 8), qr = *(float *)(st + 12);
		wsprintfA(steam, "; Steam: ping %d ms, quality %d%%/%d%%", *(int *)(st + 4), (int)(ql * 100 + 0.5f), (int)(qr * 100 + 0.5f));
	} else
		wsprintfA(steam, "; Steam session state %d", state);
	if (p->got)
		logline("STEAM %s: rtt avg %u min %u max %u ms over %u pings, %u lost%s (%s)", p->name, p->rtt_sum / p->got, p->rtt_min,
		        p->rtt_max, p->got, p->sent > p->got ? p->sent - p->got : 0, steam, why);
	else
		logline("STEAM %s: %u pings sent, none answered%s (%s)", p->name, p->sent, steam, why);
	p->sent = p->got = p->rtt_sum = p->rtt_max = 0;
	p->rtt_min = ~0u;
}

/* the network DLL's player list -> slp[] (new players get a hello, gone ones a last stats line) */
static void sl_sync_peers(void *self, DWORD now)
{
	unsigned char *cur[8];
	int count = 0;
	for (int i = 0; i < 8; i++)
		cur[i] = 0;
	if (!safe_read((char *)self + 4, &count, 4) || count < 0 || count > 8)
		count = 0;
	if (count && !safe_read((char *)self + 8, cur, 4 * count))
		count = 0;
	for (int k = 0; k < SL_PEERS; k++) {
		int i;
		if (!slp[k].ptr)
			continue;
		for (i = 0; i < count && cur[i] != slp[k].ptr; i++)
			;
		if (i < count)
			continue;
		if (slp[k].ready)
			sl_log_stats(&slp[k], "left");
		struct sl_ident id;
		sl_ident_of(&id, slp[k].sid);
		((sl_close_fn)(*(void ***)sl_msgs)[3])(sl_msgs, &id);
		slp[k].ptr = 0;
	}
	for (int i = 0; i < count; i++) {
		unsigned flags = 0;
		char sid[40];
		unsigned long long v;
		int k;
		if (!cur[i] || !safe_read(cur[i] + PEER_FLAGS, &flags, 4) || (flags & 4))
			continue;
		for (k = 0; k < SL_PEERS && slp[k].ptr != cur[i]; k++)
			;
		if (k < SL_PEERS)
			continue;
		read_wstring(cur[i] + 0x5c, sid, sizeof sid);
		if (!sl_parse_sid(sid, &v) || sl_find(v))
			continue;
		for (k = 0; k < SL_PEERS && slp[k].ptr; k++)
			;
		if (k == SL_PEERS)
			break;
		struct sl_peer *p = &slp[k];
		volatile unsigned char *z = (volatile unsigned char *)p;
		for (unsigned b = 0; b < sizeof *p; b++)
			z[b] = 0;
		p->ptr = cur[i];
		p->sid = v;
		p->rtt_min = ~0u;
		read_refstr(cur[i] + PEER_NAME, p->name, sizeof p->name);
		if (!p->name[0])
			lstrcpyA(p->name, "?");
		p->hello_at = now - SL_HELLO_EVERY; /* say hello now */
		p->log_at = now;
	}
}

static void sl_receive(DWORD now)
{
	void *msgs[16];
	int n = ((sl_recv_fn)(*(void ***)sl_msgs)[1])(sl_msgs, SL_CHANNEL, msgs, 16);
	for (int i = 0; i < n && i < 16; i++) {
		unsigned char *m = msgs[i], hdr[SLM_CHANNEL + 4];
		if (!m || !safe_read(m, hdr, sizeof hdr))
			continue;
		sl_release_fn release = *(sl_release_fn *)(hdr + SLM_RELEASE);
		const struct sl_ident *id = (const struct sl_ident *)(hdr + SLM_IDENT);
		/* layout check before trusting anything (a wrong guess must never become a crash): leak instead */
		if (id->type != 16 || id->size != 8 || *(int *)(hdr + SLM_CHANNEL) != SL_CHANNEL || !release) {
			if (!sl_broken)
				logline("STEAM link off: unexpected message layout (identity type %d size %d channel %d)", id->type, id->size,
				        *(int *)(hdr + SLM_CHANNEL));
			sl_broken = 1;
			return;
		}
		unsigned char d[32];
		int len = *(int *)(hdr + SLM_SIZE);
		const unsigned char *data = *(const unsigned char **)(hdr + SLM_DATA);
		struct sl_peer *p = sl_find(id->u.sid);
		int ok = p && len >= 5 && len <= 29 && data && safe_read(data, d, len) && d[0] == 'R' && d[1] == 'F' && d[2] == 'X' && d[3] == 1;
		release(m);
		if (!ok)
			continue; /* not one of our players, or not ours: ignored */
		unsigned a = len >= 9 ? *(unsigned *)(d + 5) : 0, b = len >= 13 ? *(unsigned *)(d + 9) : 0;
		p->heard_at = now ? now : 1;
		switch (d[4]) {
		case SL_HELLO:
			sl_send(p->sid, SL_ACK, &sl_release_no, 4, SL_SEND_RELIABLE | SL_SEND_AUTORESTART);
			/* fall through: a hello is as good as an answer */
		case SL_ACK:
			if (!p->ready) {
				p->ready = 1;
				p->release = (int)a;
				p->ping_at = now;
				logline("STEAM link to %s ready (RoN-Fix release %u)%s", p->name, a, p->gave_up ? ", after giving up on it" : "");
			}
			break;
		case SL_PING: {
			/* seq, time, then whether we are waiting and for whom (older RoN-Fix reads only the first 8 bytes) */
			int waiting = sl_my_wait >= 0 && now - sl_my_wait_at < 2000;
			unsigned pl[4] = { a, b, waiting ? (unsigned)sl_my_wait : ~0u, waiting ? sl_my_wait_ms : 0 };
			sl_send(p->sid, SL_PONG, pl, 16, SL_SEND_UNRELIABLE_NONAGLE);
			break;
		}
		case SL_PONG:
			if (len >= 13 && a <= p->seq && now - b < 10000) {
				unsigned rtt = now - b;
				p->got++;
				p->rtt_sum += rtt;
				if (rtt < p->rtt_min)
					p->rtt_min = rtt;
				if (rtt > p->rtt_max)
					p->rtt_max = rtt;
				if (p == sl_wpeer) {
					p->w_got++;
					if (rtt > p->w_rtt_max)
						p->w_rtt_max = rtt;
				}
			}
			if (len >= 21) {
				int w = *(int *)(d + 13);
				p->their_wait = w >= 0 && w < 8 ? w : -1;
				p->their_wait_ms = *(unsigned *)(d + 17);
				p->their_wait_at = now ? now : 1;
			}
			break;
		}
	}
}

static void steam_tick(void *self)
{
	DWORD now = SL_NOW();
	int count = 0;
	if (!sl_enabled || sl_broken || (int)(now - sl_next) < 0)
		return;
	sl_next = now + 100;
	if (!sl_started) {
		/* only once there is someone else in the list: no Steam networking in single player */
		if (!safe_read((char *)self + 4, &count, 4) || count < 2)
			return;
		sl_started = 1;
		if (!sl_start()) {
			sl_enabled = 0;
			return;
		}
	}
	if (!sl_msgs)
		return;
	sl_sync_peers(self, now);
	sl_receive(now);
	if (sl_broken)
		return;
	for (int k = 0; k < SL_PEERS; k++) {
		struct sl_peer *p = &slp[k];
		if (!p->ptr)
			continue;
		if (!p->ready) {
			if (p->hellos < SL_HELLO_TRIES && now - p->hello_at >= SL_HELLO_EVERY) {
				sl_send(p->sid, SL_HELLO, &sl_release_no, 4, SL_SEND_RELIABLE | SL_SEND_AUTORESTART);
				p->hellos++;
				p->hello_at = now;
			} else if (p->hellos >= SL_HELLO_TRIES && !p->gave_up && now - p->hello_at >= SL_HELLO_EVERY) {
				p->gave_up = 1;
				logline("STEAM link to %s: no answer (no RoN-Fix with the Steam link there, or Steam networking blocked)", p->name);
			}
			continue;
		}
		if (now - p->ping_at >= (p == sl_wpeer ? SL_WAIT_PING_EVERY : SL_PING_EVERY)) {
			unsigned pl[2] = { ++p->seq, now };
			sl_send(p->sid, SL_PING, pl, 8, SL_SEND_UNRELIABLE_NONAGLE);
			p->sent++;
			if (p == sl_wpeer)
				p->w_sent++;
			p->ping_at = now;
		}
		if (now - p->log_at >= SL_LOG_EVERY) {
			sl_log_stats(p, "every 30 s");
			p->log_at = now;
		}
	}
}

/* ------------------------------------------------------------------ freeze watch (core v14) */
/* the game's player number -> its CrossplayNetLibPlayer (player table +0xC0), and a label for log lines */
static unsigned char *sl_peer_of_player(int player)
{
	unsigned char *players = 0, *peer = 0;
	if (!exe_base || player < 0 || player > 7 || !safe_read(exe_base + RVA_PLAYERS_PTR, &players, 4) || !players ||
	    !safe_read(players + player * PLAYER_STRIDE + PLAYER_PEER_OFF, &peer, 4))
		return 0;
	return peer;
}
static void sl_player_label(int player, char *out)
{
	char name[48];
	unsigned char *peer = sl_peer_of_player(player);
	name[0] = 0;
	if (peer)
		read_refstr(peer + PEER_NAME, name, sizeof name);
	if (name[0])
		wsprintfA(out, "player %d (%s)", player, name);
	else
		wsprintfA(out, "player %d", player);
}

/* w_waiting, every pass of the game's wait loop (after 1 s): the game waits for `player` */
static void sl_wait_pass(int player, unsigned ms)
{
	DWORD now = SL_NOW();
	if (player == sl_my_wait && now - sl_my_wait_at < 50) {
		sl_my_wait_ms = ms; /* the wait loop spins: look the player up at most every 50 ms */
		return;
	}
	unsigned char *peer = sl_peer_of_player(player);
	struct sl_peer *p = 0;
	sl_my_wait = player, sl_my_wait_ms = ms, sl_my_wait_at = now;
	for (int k = 0; peer && k < SL_PEERS; k++)
		if (slp[k].ptr == peer)
			p = &slp[k];
	if (p == sl_wpeer)
		return;
	sl_wpeer = p;
	if (p) {
		p->w_sent = p->w_got = p->w_rtt_max = 0;
		p->ping_at = now - SL_WAIT_PING_EVERY; /* ping at once */
	}
}

/* "; Steam: ..." for a WAIT line about the player the game waits for. out: 300 bytes. */
static void sl_wait_text(char *out, int final)
{
	DWORD now = SL_NOW();
	struct sl_peer *p = sl_wpeer;
	out[0] = 0;
	if (!sl_enabled || sl_broken || !sl_msgs) {
		lstrcpyA(out, !sl_enabled || sl_broken ? "; Steam link off" : "; Steam link not running yet");
		return;
	}
	if (!p || !p->ptr) {
		lstrcpyA(out, "; Steam: no link to this player (no RoN-Fix 0.8+ there, or no Steam ID)");
		return;
	}
	if (!p->ready) {
		lstrcpyA(out, "; Steam: no link to this player (never answered)");
		return;
	}
	int n = wsprintfA(out, "; Steam: last heard %u ms ago, %u of %u pings answered during this wait", p->heard_at ? now - p->heard_at : 0,
	                  p->w_got, p->w_sent);
	if (p->w_got)
		n += wsprintfA(out + n, " (worst %u ms)", p->w_rtt_max);
	if (p->their_wait_at && now - p->their_wait_at < 3000) {
		char who[80];
		if (p->their_wait >= 0) {
			sl_player_label(p->their_wait, who);
			n += wsprintfA(out + n, "; that player is waiting for %s too (%u ms)", who, p->their_wait_ms);
		} else
			n += wsprintfA(out + n, "; that player is not waiting");
	}
	if (final)
		wsprintfA(out + n, " -> %s", p->w_got ? "Steam link UP during the freeze" : p->w_sent ? "Steam link SILENT too" : "too short to tell");
}

/* the wait is over (w_check_pulse, no wait call for 1.5 s) */
static void sl_wait_over(void)
{
	sl_wpeer = 0;
	sl_my_wait = -1;
}
