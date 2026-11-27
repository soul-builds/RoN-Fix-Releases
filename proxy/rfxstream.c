/* ------------------------------------------------------------------ RFX streams (core v17): the part without Windows
 * Included by steamlink.c. Plan and reasons: reverse/steam-transport.md, "The port".
 * Everything one RoN-Fix player sends to another is numbered 1, 2, 3 ... per direction, so the receiver can tell
 * exactly what arrived, in which order, and what is missing, whichever path carried it. Stage 1 (mirror) only
 * watches: the frames travel over Steam next to the game's own PlayFab traffic and are compared with it on arrival;
 * nothing here is handed to the game. Later stages deliver from the same numbering.
 *   frame encode / parse      wire format, every field checked before anything is believed
 *   rfx_tx                    sender: gives out numbers, measures how long the other side takes to confirm them
 *   rfx_rx                    receiver: in order, each number once; early frames wait, a lost one is given up on
 *   rfx_mirror                pairs what Steam brought with what PlayFab brought, by content, and times the gap
 * No Windows calls, no globals, no allocation: the caller owns the structs and passes the time in. That is what lets
 * proxy/tests/rfx_tests.c run two ends against each other over a simulated network.
 * No CRT: copies and clears go through volatile pointers so the compiler does not turn them into memcpy/memset. */

#define RFX_MAX_MSG 2056 /* the exe's receive buffer (NetDaemon +8 .. +0x810): nothing longer may ever reach the game */
#define RFX_HDR 25       /* "RFX" 1 type | my_epoch your_epoch seq ack | flags 0 | len16 */
#define RFX_ACK_LEN 21   /* "RFX" 1 type | my_epoch your_epoch ack highest_seen */
enum { RFX_DATA = 5, RFX_ACK = 6 }; /* types 1-4 are the link's hello / ack / ping / pong (steamlink.c) */
enum { RFX_F_MIRROR = 1, RFX_F_DIRECT = 2 }; /* mirror: do not deliver; direct: was sent to one player, not to all */
#define RFX_STALL_MS 1500   /* a frame unconfirmed this long: stage 2 would move the pair to PlayFab */
#define RFX_GAP_MS 5000     /* an early frame has waited this long for the missing one: given up on */
#define RFX_GIVEUP_MS 60000 /* a message seen on one path only for this long has no twin */

struct rfx_frame {
	int type, flags;
	unsigned my_epoch, your_epoch, seq, ack, seen, len;
	const unsigned char *payload;
};

static unsigned rfx_get32(const unsigned char *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}
static void rfx_put32(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)(v >> 16), p[3] = (unsigned char)(v >> 24);
}
static void rfx_clear(void *p, unsigned n)
{
	volatile unsigned char *q = (volatile unsigned char *)p;
	for (unsigned i = 0; i < n; i++)
		q[i] = 0;
}
/* FNV-1a: tells two messages apart, nothing more (the comparison also uses the length) */
static unsigned rfx_hash(const unsigned char *p, unsigned n)
{
	unsigned h = 2166136261u;
	for (unsigned i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

static void rfx_put_head(unsigned char *out, int type, unsigned my_epoch, unsigned your_epoch)
{
	out[0] = 'R', out[1] = 'F', out[2] = 'X', out[3] = 1, out[4] = (unsigned char)type;
	rfx_put32(out + 5, my_epoch);
	rfx_put32(out + 9, your_epoch);
}
/* The header of a data frame whose message already sits at out + RFX_HDR (one copy, a header per receiver).
 * Returns the frame's size, 0 if the message cannot travel (empty or too long). */
static unsigned rfx_put_data_head(unsigned char *out, unsigned my_epoch, unsigned your_epoch, unsigned seq, unsigned ack, int flags,
                                  unsigned len)
{
	if (!len || len > RFX_MAX_MSG)
		return 0;
	rfx_put_head(out, RFX_DATA, my_epoch, your_epoch);
	rfx_put32(out + 13, seq);
	rfx_put32(out + 17, ack);
	out[21] = (unsigned char)flags, out[22] = 0;
	out[23] = (unsigned char)len, out[24] = (unsigned char)(len >> 8);
	return RFX_HDR + len;
}
static unsigned rfx_put_ack(unsigned char *out, unsigned my_epoch, unsigned your_epoch, unsigned ack, unsigned seen)
{
	rfx_put_head(out, RFX_ACK, my_epoch, your_epoch);
	rfx_put32(out + 13, ack);
	rfx_put32(out + 17, seen);
	return RFX_ACK_LEN;
}
/* 1 = d[0..n) is one whole, well-formed frame. Anything else is refused: never a partly believed frame. */
static int rfx_parse(const unsigned char *d, unsigned n, struct rfx_frame *f)
{
	if (n < RFX_ACK_LEN || d[0] != 'R' || d[1] != 'F' || d[2] != 'X' || d[3] != 1)
		return 0;
	f->type = d[4];
	f->my_epoch = rfx_get32(d + 5);
	f->your_epoch = rfx_get32(d + 9);
	f->flags = 0, f->seq = f->ack = f->seen = f->len = 0, f->payload = 0;
	if (!f->my_epoch || !f->your_epoch)
		return 0;
	if (f->type == RFX_ACK) {
		if (n != RFX_ACK_LEN)
			return 0;
		f->ack = rfx_get32(d + 13);
		f->seen = rfx_get32(d + 17);
		return 1;
	}
	if (f->type != RFX_DATA || n <= RFX_HDR)
		return 0;
	f->seq = rfx_get32(d + 13);
	f->ack = rfx_get32(d + 17);
	f->flags = d[21];
	f->len = (unsigned)d[23] | (unsigned)d[24] << 8;
	f->payload = d + RFX_HDR;
	return f->seq && d[22] == 0 && f->len && f->len <= RFX_MAX_MSG && n == RFX_HDR + f->len;
}

/* ---- sender */
#define RFX_TX_RING 256
struct rfx_tx {
	unsigned next_seq, acked;      /* numbers 1 .. next_seq-1 are out, 1 .. acked are confirmed */
	unsigned t_sent[RFX_TX_RING];  /* when number n went out, at [n % RFX_TX_RING] */
	int stalled;
	unsigned sent, bytes, refused, ack_n, ack_sum, ack_max, stalls; /* since the last report */
};
static void rfx_tx_reset(struct rfx_tx *t)
{
	rfx_clear(t, sizeof *t);
	t->next_seq = 1;
}
static void rfx_tx_report_done(struct rfx_tx *t)
{
	t->sent = t->bytes = t->refused = t->ack_n = t->ack_sum = t->ack_max = t->stalls = 0;
}
static unsigned rfx_tx_send(struct rfx_tx *t, unsigned now, unsigned bytes)
{
	unsigned seq = t->next_seq++;
	t->t_sent[seq % RFX_TX_RING] = now;
	t->sent++;
	t->bytes += bytes;
	return seq;
}
static int rfx_tx_timed(const struct rfx_tx *t, unsigned seq) { return t->next_seq - seq <= RFX_TX_RING; }
/* the other side has everything up to `ack` */
static void rfx_tx_ack(struct rfx_tx *t, unsigned ack, unsigned now)
{
	if (ack <= t->acked || ack >= t->next_seq)
		return; /* nothing new, or a number never given out */
	unsigned s = t->acked + 1;
	if (!rfx_tx_timed(t, s))
		s = t->next_seq - RFX_TX_RING; /* send times of older numbers were overwritten long ago */
	for (; s <= ack; s++) {
		unsigned d = now - t->t_sent[s % RFX_TX_RING];
		t->ack_n++;
		t->ack_sum += d;
		if (d > t->ack_max)
			t->ack_max = d;
	}
	t->acked = ack;
	t->stalled = 0;
}
/* age of the oldest unconfirmed frame, 0 = none out */
static unsigned rfx_tx_oldest(const struct rfx_tx *t, unsigned now)
{
	unsigned s = t->acked + 1;
	if (s >= t->next_seq)
		return 0;
	if (!rfx_tx_timed(t, s))
		s = t->next_seq - RFX_TX_RING; /* its own send time is gone: a later frame's age is a lower bound */
	unsigned age = now - t->t_sent[s % RFX_TX_RING];
	return age ? age : 1;
}
static void rfx_tx_tick(struct rfx_tx *t, unsigned now)
{
	if (!t->stalled && rfx_tx_oldest(t, now) > RFX_STALL_MS) {
		t->stalled = 1;
		t->stalls++;
	}
}

/* ---- receiver: each number once, in order */
#define RFX_HOLD 64
struct rfx_item {
	unsigned seq, hash, t; /* t: when it arrived here */
	unsigned short len;
	unsigned char kind, used; /* kind: the message's first byte (its type for the game) */
};
struct rfx_rx {
	unsigned next, highest; /* next number due; highest number seen */
	struct rfx_item hold[RFX_HOLD];
	int hold_n;
	unsigned hold_since;
	unsigned got, early, repeated, lost; /* since the last report */
};
static void rfx_rx_reset(struct rfx_rx *r)
{
	rfx_clear(r, sizeof *r);
	r->next = 1;
}
static void rfx_rx_report_done(struct rfx_rx *r) { r->got = r->early = r->repeated = r->lost = 0; }
/* held frames that have become due -> out[n..] */
static int rfx_rx_drain(struct rfx_rx *r, struct rfx_item *out, int n, int max)
{
	for (int again = 1; again && r->hold_n;) {
		again = 0;
		for (int k = 0; k < RFX_HOLD; k++)
			if (r->hold[k].used && r->hold[k].seq == r->next && n < max) {
				out[n++] = r->hold[k];
				r->hold[k].used = 0;
				r->hold_n--;
				r->next++;
				again = 1;
			}
	}
	return n;
}
/* the numbers before the lowest held frame are not coming any more */
static int rfx_rx_skip(struct rfx_rx *r, unsigned now, struct rfx_item *out, int n, int max)
{
	unsigned low = ~0u;
	for (int k = 0; k < RFX_HOLD; k++)
		if (r->hold[k].used && r->hold[k].seq < low)
			low = r->hold[k].seq;
	if (low == ~0u || low <= r->next)
		return n;
	r->lost += low - r->next;
	r->next = low;
	n = rfx_rx_drain(r, out, n, max);
	r->hold_since = now;
	return n;
}
/* One frame arrived. Writes the frames that are now due, in order, to out[0..max) and returns how many.
 * max should be RFX_HOLD + 1. */
static int rfx_rx_push(struct rfx_rx *r, const struct rfx_item *it, unsigned now, struct rfx_item *out, int max)
{
	int n = 0;
	r->got++;
	if (it->seq > r->highest)
		r->highest = it->seq;
	if (it->seq < r->next) {
		r->repeated++;
		return 0;
	}
	if (it->seq == r->next) {
		if (max < 1)
			return 0;
		out[n++] = *it;
		r->next++;
		n = rfx_rx_drain(r, out, n, max);
		if (r->hold_n)
			r->hold_since = now; /* the next gap starts counting now */
		return n;
	}
	for (int k = 0; k < RFX_HOLD; k++)
		if (r->hold[k].used && r->hold[k].seq == it->seq) {
			r->repeated++;
			return 0;
		}
	if (r->hold_n == RFX_HOLD)
		n = rfx_rx_skip(r, now, out, n, max); /* full: stop waiting for the missing ones */
	if (it->seq < r->next) { /* it was one of those just given up on: too late, order comes first */
		r->repeated++;
		return n;
	}
	if (it->seq == r->next) { /* the skip made this one due */
		if (n < max) {
			out[n++] = *it;
			r->next++;
		}
		return rfx_rx_drain(r, out, n, max);
	}
	for (int k = 0; k < RFX_HOLD; k++)
		if (!r->hold[k].used) {
			r->hold[k] = *it;
			r->hold[k].used = 1;
			if (!r->hold_n++)
				r->hold_since = now;
			r->early++;
			break;
		}
	return n;
}
/* now and then: an early frame that has waited RFX_GAP_MS is released, the missing numbers counted as lost */
static int rfx_rx_tick(struct rfx_rx *r, unsigned now, struct rfx_item *out, int max)
{
	if (!r->hold_n || now - r->hold_since <= RFX_GAP_MS)
		return 0;
	return rfx_rx_skip(r, now, out, 0, max);
}

/* ---- mirror: the same messages on two paths, paired by content
 * Both paths keep the sender's order, so the two queues are the same sequence with pieces missing on one side or
 * the other. The pair to take next is the earliest message present in both; whatever lies before it in either
 * queue has no twin. Until the first pair nothing is counted as missing: PlayFab carried messages before the
 * mirror started. */
#define RFX_MQ 256
#define RFX_MSEARCH 32
struct rfx_mitem {
	unsigned hash, t;
	unsigned short len, kind;
};
struct rfx_mq {
	struct rfx_mitem q[RFX_MQ];
	unsigned head, n;
};
struct rfx_mirror {
	struct rfx_mq steam, party;
	int aligned;
	/* since the last report; "first" = that path had the message at an earlier pass of the game's network pump */
	unsigned matched, same, steam_first, sf_sum, sf_max, party_first, pf_sum, pf_max, steam_only, party_only, dll;
	/* since the caller last cleared them (one wait for this player) */
	unsigned w_matched, w_party_late, w_steam_late;
};
static void rfx_mirror_reset(struct rfx_mirror *m) { rfx_clear(m, sizeof *m); }
static void rfx_mirror_report_done(struct rfx_mirror *m)
{
	m->matched = m->same = m->steam_first = m->sf_sum = m->sf_max = m->party_first = m->pf_sum = m->pf_max = 0;
	m->steam_only = m->party_only = m->dll = 0;
}
static struct rfx_mitem *rfx_mq_at(struct rfx_mq *q, unsigned i) { return &q->q[(q->head + i) % RFX_MQ]; }
static void rfx_mq_drop(struct rfx_mq *q, unsigned k)
{
	q->head = (q->head + k) % RFX_MQ;
	q->n -= k;
}
static void rfx_mirror_run(struct rfx_mirror *m)
{
	while (m->steam.n && m->party.n) {
		unsigned ns = m->steam.n < RFX_MSEARCH ? m->steam.n : RFX_MSEARCH;
		unsigned np = m->party.n < RFX_MSEARCH ? m->party.n : RFX_MSEARCH;
		unsigned bi = 0, bj = 0;
		int found = 0;
		for (unsigned sum = 0; !found && sum < ns + np - 1; sum++)
			for (unsigned j = sum < np ? 0 : sum - np + 1; j <= sum && j < ns; j++) {
				struct rfx_mitem *s = rfx_mq_at(&m->steam, j), *p = rfx_mq_at(&m->party, sum - j);
				if (s->hash == p->hash && s->len == p->len) {
					bj = j, bi = sum - j, found = 1;
					break;
				}
			}
		if (!found)
			return; /* wait for more of either */
		if (m->aligned)
			m->steam_only += bj, m->party_only += bi;
		rfx_mq_drop(&m->steam, bj);
		rfx_mq_drop(&m->party, bi);
		int d = (int)(rfx_mq_at(&m->party, 0)->t - rfx_mq_at(&m->steam, 0)->t); /* > 0: PlayFab was later */
		rfx_mq_drop(&m->steam, 1);
		rfx_mq_drop(&m->party, 1);
		m->aligned = 1;
		m->matched++, m->w_matched++;
		if (d > 0) {
			m->steam_first++, m->sf_sum += (unsigned)d;
			if ((unsigned)d > m->sf_max)
				m->sf_max = (unsigned)d;
			if ((unsigned)d > m->w_party_late)
				m->w_party_late = (unsigned)d;
		} else if (d < 0) {
			m->party_first++, m->pf_sum += (unsigned)-d;
			if ((unsigned)-d > m->pf_max)
				m->pf_max = (unsigned)-d;
			if ((unsigned)-d > m->w_steam_late)
				m->w_steam_late = (unsigned)-d;
		} else
			m->same++;
	}
}
static void rfx_mq_push(struct rfx_mirror *m, struct rfx_mq *q, unsigned *only, unsigned hash, unsigned len, unsigned kind, unsigned t)
{
	if (q->n == RFX_MQ) { /* full: the oldest never found its twin */
		rfx_mq_drop(q, 1);
		if (m->aligned)
			(*only)++;
	}
	struct rfx_mitem *it = rfx_mq_at(q, q->n++);
	it->hash = hash, it->t = t, it->len = (unsigned short)len, it->kind = (unsigned short)kind;
}
/* a frame the receiver released, in order. The network DLL's own packets (0x80 and up) never show up in the
 * game's queue, so they are only counted. */
static void rfx_mirror_steam(struct rfx_mirror *m, const struct rfx_item *it)
{
	if (it->kind >= 0x80) {
		m->dll++;
		return;
	}
	rfx_mq_push(m, &m->steam, &m->steam_only, it->hash, it->len, it->kind, it->t);
	rfx_mirror_run(m);
}
/* a message the game took from PlayFab's queue */
static void rfx_mirror_party(struct rfx_mirror *m, unsigned hash, unsigned len, unsigned kind, unsigned now)
{
	if (kind >= 0x80)
		return;
	rfx_mq_push(m, &m->party, &m->party_only, hash, len, kind, now);
	rfx_mirror_run(m);
}
static void rfx_mirror_tick(struct rfx_mirror *m, unsigned now)
{
	while (m->steam.n && now - rfx_mq_at(&m->steam, 0)->t > RFX_GIVEUP_MS) {
		rfx_mq_drop(&m->steam, 1);
		if (m->aligned)
			m->steam_only++;
	}
	while (m->party.n && now - rfx_mq_at(&m->party, 0)->t > RFX_GIVEUP_MS) {
		rfx_mq_drop(&m->party, 1);
		if (m->aligned)
			m->party_only++;
	}
	rfx_mirror_run(m);
}
