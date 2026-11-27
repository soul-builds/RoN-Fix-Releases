/* Unit tests for rfxstream.c (RFX streams, core v17): the wire format, the sender's bookkeeping, the receiver's
 * "each number once, in order", and the mirror's pairing of the two paths. Included by thunk_test.c. The code under
 * test makes no Windows calls and takes the time as an argument, so both ends and the network between them are
 * played here, with a fixed-seed random generator for the long runs. */

static unsigned rt_seed;
static unsigned rt_rand(unsigned below)
{
	rt_seed = rt_seed * 1664525u + 1013904223u;
	return (rt_seed >> 8) % below;
}
static unsigned rt_id_hash(unsigned id) { return id * 2654435761u + 12345u; } /* a different "content" per message */
static struct rfx_item rt_item(unsigned seq, unsigned t)
{
	struct rfx_item it;
	it.seq = seq, it.hash = rt_id_hash(seq), it.t = t, it.len = 20, it.kind = 7, it.used = 0;
	return it;
}

static void test_rfx_frames(void)
{
	static unsigned char buf[RFX_HDR + RFX_MAX_MSG + 8], msg[RFX_MAX_MSG + 8];
	struct rfx_frame f;
	for (unsigned i = 0; i < sizeof msg; i++)
		msg[i] = (unsigned char)(i * 7 + 3);
	unsigned n = rfx_put_data(buf, 0x11111111, 0x22222222, 5, 4, RFX_F_MIRROR | RFX_F_DIRECT, msg, 100);
	check(n == RFX_HDR + 100 && rfx_parse(buf, n, &f) && f.type == RFX_DATA && f.my_epoch == 0x11111111 && f.your_epoch == 0x22222222 &&
	          f.seq == 5 && f.ack == 4 && f.flags == (RFX_F_MIRROR | RFX_F_DIRECT) && f.len == 100 && f.payload == buf + RFX_HDR &&
	          rfx_hash(f.payload, 100) == rfx_hash(msg, 100) && rfx_hash(msg, 100) != rfx_hash(msg, 99),
	      "rfx: a data frame comes back out of the parser with every field and the same content");
	check(rfx_put_data(buf, 1, 2, 1, 0, 0, msg, RFX_MAX_MSG) == RFX_HDR + RFX_MAX_MSG && rfx_parse(buf, RFX_HDR + RFX_MAX_MSG, &f) &&
	          f.len == RFX_MAX_MSG,
	      "rfx: the longest message the game's buffer takes (2056 bytes) travels");
	check(rfx_put_data(buf, 1, 2, 1, 0, 0, msg, RFX_MAX_MSG + 1) == 0 && rfx_put_data(buf, 1, 2, 1, 0, 0, msg, 0) == 0,
	      "rfx: the sender refuses a message longer than the game's buffer, and an empty one");

	n = rfx_put_data(buf, 1, 2, 9, 0, RFX_F_MIRROR, msg, 50);
	int ok = rfx_parse(buf, n, &f), wrong = 0;
	wrong += rfx_parse(buf, n - 1, &f); /* cut short */
	wrong += rfx_parse(buf, n + 1, &f); /* longer than it says */
	buf[3] = 2, wrong += rfx_parse(buf, n, &f), buf[3] = 1; /* another protocol version */
	buf[0] = 'X', wrong += rfx_parse(buf, n, &f), buf[0] = 'R';
	buf[4] = 7, wrong += rfx_parse(buf, n, &f), buf[4] = RFX_DATA; /* a type this release does not know */
	buf[22] = 1, wrong += rfx_parse(buf, n, &f), buf[22] = 0; /* the byte that must be 0 */
	rfx_put32(buf + 13, 0), wrong += rfx_parse(buf, n, &f), rfx_put32(buf + 13, 9); /* number 0 */
	rfx_put32(buf + 5, 0), wrong += rfx_parse(buf, n, &f), rfx_put32(buf + 5, 1);   /* no sender entry */
	rfx_put32(buf + 9, 0), wrong += rfx_parse(buf, n, &f), rfx_put32(buf + 9, 2);   /* no receiver entry */
	buf[23] = 0x09, buf[24] = 0x08, wrong += rfx_parse(buf, RFX_HDR + 0x809, &f), buf[23] = 50, buf[24] = 0; /* says 2057 bytes */
	wrong += rfx_parse(buf, RFX_HDR, &f); /* a header and nothing else */
	check(ok && wrong == 0 && rfx_parse(buf, n, &f), "rfx: the parser refuses a frame that is cut, padded, mislabelled or over the limit");

	n = rfx_put_ack(buf, 3, 4, 77, 80);
	check(n == RFX_ACK_LEN && rfx_parse(buf, n, &f) && f.type == RFX_ACK && f.my_epoch == 3 && f.your_epoch == 4 && f.ack == 77 &&
	          f.seen == 80 && !rfx_parse(buf, n + 1, &f) && !rfx_parse(buf, n - 1, &f),
	      "rfx: an ack frame round trip; any other length is refused");

	/* 30000 random buffers, most with a believable start: whatever the parser accepts must be whole and in limits */
	int accepted = 0, broken = 0;
	rt_seed = 20261127;
	for (int k = 0; k < 30000; k++) {
		unsigned len = rt_rand(90);
		for (unsigned i = 0; i < len; i++)
			buf[i] = (unsigned char)rt_rand(256);
		if (rt_rand(4)) {
			buf[0] = 'R', buf[1] = 'F', buf[2] = 'X', buf[3] = 1, buf[4] = (unsigned char)(5 + rt_rand(2));
			if (rt_rand(2))
				buf[22] = 0, buf[24] = 0, buf[23] = (unsigned char)(len > RFX_HDR ? len - RFX_HDR : 1);
		}
		if (!rfx_parse(buf, len, &f))
			continue;
		accepted++;
		if (f.type == RFX_ACK ? len != RFX_ACK_LEN : (f.type != RFX_DATA || !f.seq || !f.len || f.len > RFX_MAX_MSG || len != RFX_HDR + f.len ||
		                                              f.payload != buf + RFX_HDR))
			broken++;
		if (!f.my_epoch || !f.your_epoch)
			broken++;
	}
	check(accepted > 100 && broken == 0, "rfx: of 30000 random buffers, every one the parser accepts is whole and within the limits");
}

static void test_rfx_tx(void)
{
	static struct rfx_tx t;
	rfx_tx_reset(&t);
	unsigned a = rfx_tx_send(&t, 0, 10), b = rfx_tx_send(&t, 10, 20), c = rfx_tx_send(&t, 20, 30);
	check(a == 1 && b == 2 && c == 3 && t.sent == 3 && t.bytes == 60 && rfx_tx_oldest(&t, 25) == 25, "rfx sender: numbers 1, 2, 3 in the order sent");
	rfx_tx_ack(&t, 2, 50);
	check(t.acked == 2 && t.ack_n == 2 && t.ack_sum == 90 && t.ack_max == 50 && rfx_tx_oldest(&t, 50) == 30,
	      "rfx sender: an ack for 2 confirms 1 and 2 and times them (50 and 40 ms)");
	rfx_tx_ack(&t, 9, 60); /* never sent */
	rfx_tx_ack(&t, 1, 60); /* old news */
	check(t.acked == 2 && t.ack_n == 2, "rfx sender: an ack for a number never sent, or an older one, changes nothing");
	rfx_tx_tick(&t, 1520);
	int before = t.stalls;
	rfx_tx_tick(&t, 1521);
	rfx_tx_tick(&t, 4000);
	check(before == 0 && t.stalls == 1 && t.stalled, "rfx sender: a frame unconfirmed for more than 1.5 s counts as one stall, once");
	rfx_tx_ack(&t, 3, 4100);
	rfx_tx_tick(&t, 9000);
	check(!t.stalled && t.stalls == 1 && rfx_tx_oldest(&t, 9000) == 0, "rfx sender: confirmed -> no stall, nothing out");

	rfx_tx_reset(&t);
	for (unsigned i = 0; i < 300; i++)
		rfx_tx_send(&t, i, 1);
	unsigned oldest = rfx_tx_oldest(&t, 1000); /* number 1's send time is overwritten: number 45's is used (sent at 44) */
	rfx_tx_ack(&t, 300, 1000);
	check(oldest == 1000 - 44 && t.ack_n == RFX_TX_RING && t.acked == 300 && t.ack_max == 1000 - 44,
	      "rfx sender: 300 frames out at once: only the last 256 are timed, nothing reads a send time that is gone");
}

static void test_rfx_rx(void)
{
	static struct rfx_rx r;
	static struct rfx_item due[RFX_HOLD + 1];
	struct rfx_item it;
	rfx_rx_reset(&r);
	it = rt_item(1, 0);
	int n1 = rfx_rx_push(&r, &it, 0, due, RFX_HOLD + 1);
	it = rt_item(2, 0);
	int n2 = rfx_rx_push(&r, &it, 0, due, RFX_HOLD + 1);
	check(n1 == 1 && n2 == 1 && due[0].seq == 2 && r.next == 3, "rfx receiver: frames in order are due at once");
	it = rt_item(4, 10);
	int n4 = rfx_rx_push(&r, &it, 10, due, RFX_HOLD + 1);
	it = rt_item(5, 10);
	int n5 = rfx_rx_push(&r, &it, 10, due, RFX_HOLD + 1);
	it = rt_item(3, 20);
	int n3 = rfx_rx_push(&r, &it, 20, due, RFX_HOLD + 1);
	check(n4 == 0 && n5 == 0 && n3 == 3 && due[0].seq == 3 && due[1].seq == 4 && due[2].seq == 5 && r.early == 2 && r.next == 6 && !r.hold_n,
	      "rfx receiver: 4 and 5 wait for 3, then all three are due in order");
	it = rt_item(4, 30);
	int again = rfx_rx_push(&r, &it, 30, due, RFX_HOLD + 1);
	it = rt_item(8, 30);
	rfx_rx_push(&r, &it, 30, due, RFX_HOLD + 1);
	int held_twice = rfx_rx_push(&r, &it, 31, due, RFX_HOLD + 1);
	check(again == 0 && held_twice == 0 && r.repeated == 2 && r.hold_n == 1, "rfx receiver: a number seen before is dropped, delivered or still waiting");

	int early_tick = rfx_rx_tick(&r, 30 + RFX_GAP_MS, due, RFX_HOLD + 1);
	int late_tick = rfx_rx_tick(&r, 31 + RFX_GAP_MS, due, RFX_HOLD + 1);
	check(early_tick == 0 && late_tick == 1 && due[0].seq == 8 && r.lost == 2 && r.next == 9,
	      "rfx receiver: after 5 s the missing 6 and 7 are given up on and 8 is due");
	it = rt_item(6, 9000);
	check(rfx_rx_push(&r, &it, 9000, due, RFX_HOLD + 1) == 0 && r.next == 9, "rfx receiver: a frame that arrives after it was given up on is not delivered late");

	/* the waiting room is full: stop waiting for the missing one */
	rfx_rx_reset(&r);
	it = rt_item(1, 0);
	rfx_rx_push(&r, &it, 0, due, RFX_HOLD + 1);
	int out = 0;
	for (unsigned s = 3; s < 3 + RFX_HOLD; s++) {
		it = rt_item(s, 1);
		out += rfx_rx_push(&r, &it, 1, due, RFX_HOLD + 1);
	}
	it = rt_item(3 + RFX_HOLD, 2);
	int burst = rfx_rx_push(&r, &it, 2, due, RFX_HOLD + 1), in_order = 1;
	for (int i = 0; i < burst; i++)
		if (due[i].seq != 3u + (unsigned)i)
			in_order = 0;
	check(out == 0 && burst == RFX_HOLD + 1 && in_order && r.lost == 1 && r.next == 4 + RFX_HOLD && !r.hold_n,
	      "rfx receiver: 64 frames waiting and a 65th arrives -> the missing one is given up on, all 65 due in order");

	/* 5000 numbers through a network that reorders (up to 20 places), repeats and loses */
	static unsigned char delivered[5001];
	static unsigned order[5000];
	rfx_rx_reset(&r);
	zero(delivered, sizeof delivered);
	rt_seed = 4242;
	for (unsigned i = 0; i < 5000; i++)
		order[i] = i + 1;
	for (unsigned i = 0; i < 5000; i++) { /* local shuffle */
		unsigned j = i + rt_rand(20);
		if (j < 5000) {
			unsigned x = order[i];
			order[i] = order[j], order[j] = x;
		}
	}
	unsigned last = 0, got = 0, pushed_distinct = 0, now = 0;
	int bad = 0;
	static unsigned char was_pushed[5001];
	zero(was_pushed, sizeof was_pushed);
	for (unsigned i = 0; i < 5000; i++) {
		now += 3;
		int copies = rt_rand(25) == 0 ? 0 : rt_rand(10) == 0 ? 2 : 1; /* 4 % lost, 10 % twice */
		for (int c = 0; c < copies; c++) {
			it = rt_item(order[i], now);
			if (!was_pushed[order[i]])
				was_pushed[order[i]] = 1, pushed_distinct++;
			int n = rfx_rx_push(&r, &it, now, due, RFX_HOLD + 1);
			for (int k = 0; k < n; k++) {
				if (due[k].seq <= last || delivered[due[k].seq] || !was_pushed[due[k].seq] || due[k].hash != rt_id_hash(due[k].seq))
					bad++;
				last = due[k].seq, delivered[last] = 1, got++;
			}
		}
		if (i % 50 == 0) {
			int n = rfx_rx_tick(&r, now, due, RFX_HOLD + 1);
			for (int k = 0; k < n; k++) {
				if (due[k].seq <= last || delivered[due[k].seq])
					bad++;
				last = due[k].seq, delivered[last] = 1, got++;
			}
		}
	}
	for (int round = 0; r.hold_n && round < 100; round++) { /* the end: each gap that is still open times out */
		now += RFX_GAP_MS + 1;
		int n = rfx_rx_tick(&r, now, due, RFX_HOLD + 1);
		for (int k = 0; k < n; k++) {
			if (due[k].seq <= last || delivered[due[k].seq])
				bad++;
			last = due[k].seq, delivered[last] = 1, got++;
		}
	}
	check(bad == 0 && got > 4500 && got <= pushed_distinct && got + r.lost == r.next - 1 && !r.hold_n,
	      "rfx receiver: 5000 numbers reordered, repeated and lost: never one twice, never out of order, the rest counted lost");
}

static void rt_steam(struct rfx_mirror *m, unsigned id, unsigned t)
{
	struct rfx_item it = rt_item(id, t);
	rfx_mirror_steam(m, &it);
}
static void rt_party(struct rfx_mirror *m, unsigned id, unsigned t) { rfx_mirror_party(m, rt_id_hash(id), 20, 7, t); }

static void test_rfx_mirror(void)
{
	static struct rfx_mirror m;
	rfx_mirror_reset(&m);
	/* PlayFab carried messages before the mirror started: they have no twin and are not counted as missing */
	rt_party(&m, 90, 10);
	rt_party(&m, 91, 20);
	rt_steam(&m, 1, 100);
	rt_party(&m, 1, 140);
	check(m.matched == 1 && m.steam_first == 1 && m.sf_max == 40 && m.sf_sum == 40 && m.party_only == 0 && m.aligned && !m.steam.n && !m.party.n,
	      "mirror: Steam had it 40 ms before PlayFab; what PlayFab carried before the mirror started is not called missing");
	rt_party(&m, 2, 200);
	rt_steam(&m, 2, 230);
	rt_steam(&m, 3, 300);
	rt_party(&m, 3, 300);
	check(m.matched == 3 && m.party_first == 1 && m.pf_max == 30 && m.same == 1, "mirror: PlayFab first by 30 ms; both in the same pass");
	rt_party(&m, 4, 400); /* never arrives on Steam */
	rt_party(&m, 5, 410);
	rt_steam(&m, 5, 420);
	check(m.matched == 4 && m.party_only == 1 && m.steam_only == 0, "mirror: a message that came over PlayFab only is counted when the next pair shows it");
	rt_steam(&m, 6, 500); /* never arrives on PlayFab */
	rt_steam(&m, 7, 510);
	rt_party(&m, 7, 515);
	check(m.matched == 5 && m.steam_only == 1, "mirror: a message that came over Steam only");
	rt_steam(&m, 8, 600); /* Steam only */
	rt_party(&m, 9, 600); /* PlayFab only */
	int stuck = m.matched;
	rt_steam(&m, 10, 620);
	rt_party(&m, 10, 650);
	check(stuck == 5 && m.matched == 6 && m.steam_only == 2 && m.party_only == 2 && !m.steam.n && !m.party.n,
	      "mirror: one message missing on each path at the same time does not block the pairs behind them");
	/* the same content twice in a row (seven loading sync packets look alike): paired in order */
	rfx_mirror_report_done(&m);
	rt_steam(&m, 50, 1000);
	rt_steam(&m, 50, 1010);
	rt_party(&m, 50, 1005);
	rt_party(&m, 50, 1020);
	check(m.matched == 2 && m.steam_first == 2 && m.sf_sum == 15 && !m.steam_only && !m.party_only, "mirror: two identical messages pair first with first, second with second");
	struct rfx_item pulse = rt_item(60, 1100);
	pulse.kind = 0x83;
	rfx_mirror_steam(&m, &pulse);
	rfx_mirror_party(&m, 1, 1, 0x83, 1100);
	check(m.dll == 1 && !m.steam.n && !m.party.n, "mirror: the network DLL's own packets (0x80 and up) are counted, never queued for pairing");
	/* during one wait */
	m.w_matched = m.w_party_late = m.w_steam_late = 0;
	rt_steam(&m, 70, 2000);
	rt_party(&m, 70, 2900);
	check(m.w_matched == 1 && m.w_party_late == 900 && m.w_steam_late == 0, "mirror: the per-wait figures say PlayFab was 900 ms behind");
	/* no twin for a minute */
	rfx_mirror_report_done(&m);
	rt_steam(&m, 80, 3000);
	rfx_mirror_tick(&m, 3000 + RFX_GIVEUP_MS);
	int still = m.steam.n;
	rfx_mirror_tick(&m, 3001 + RFX_GIVEUP_MS);
	check(still == 1 && !m.steam.n && m.steam_only == 1, "mirror: a message without a twin is given up on after a minute");
	/* more unpaired messages than the queue holds */
	rfx_mirror_report_done(&m);
	for (unsigned i = 0; i < RFX_MQ + 44; i++)
		rt_steam(&m, 1000 + i, 70000);
	check(m.steam.n == RFX_MQ && m.steam_only == 44, "mirror: a full queue drops its oldest and counts it");

	/* 3000 messages: both paths deliver in order with their own delays, each loses a few */
	enum { N = 3000 };
	static unsigned ts[N], tp[N];
	static unsigned char on_s[N], on_p[N];
	unsigned both = 0, s_only = 0, p_only = 0;
	rfx_mirror_reset(&m);
	rt_seed = 777;
	for (unsigned i = 0; i < N; i++) {
		unsigned sent = i * 20;
		ts[i] = sent + rt_rand(200), tp[i] = sent + rt_rand(200);
		if (i && ts[i] < ts[i - 1])
			ts[i] = ts[i - 1];
		if (i && tp[i] < tp[i - 1])
			tp[i] = tp[i - 1];
		on_s[i] = !i || rt_rand(100) >= 3, on_p[i] = !i || rt_rand(100) >= 2;
		if (on_s[i] && on_p[i])
			both++;
		else if (on_s[i])
			s_only++;
		else if (on_p[i])
			p_only++;
	}
	unsigned a = 0, b = 0, lead_sum = 0, lead_n = 0;
	while (a < N || b < N) {
		if (b >= N || (a < N && ts[a] <= tp[b])) {
			if (on_s[a])
				rt_steam(&m, 5000 + a, ts[a]);
			if (on_s[a] && on_p[a] && tp[a] > ts[a])
				lead_sum += tp[a] - ts[a], lead_n++;
			a++;
		} else {
			if (on_p[b])
				rt_party(&m, 5000 + b, tp[b]);
			b++;
		}
	}
	rfx_mirror_tick(&m, N * 20 + 300 + RFX_GIVEUP_MS);
	check(m.matched == both && m.steam_only == s_only && m.party_only == p_only && m.steam_first == lead_n && m.sf_sum == lead_sum && !m.steam.n &&
	          !m.party.n,
	      "mirror: 3000 messages over two lossy paths: every pair found and timed, every loss counted on the right side");
}

static void test_rfx(void)
{
	test_rfx_frames();
	test_rfx_tx();
	test_rfx_rx();
	test_rfx_mirror();
}
