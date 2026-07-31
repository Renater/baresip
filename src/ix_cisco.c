/**
 * @file ix_cisco.c  Cisco IX application channel (UDP/UDT/IX) - FarEndMessage
 *
 * Implements the minimal UDT transport subset needed for the IX message
 * channel and transmits FarEndMessage payloads over the "genericmsg"
 * channel.
 *
 * The reliability/congestion logic is intentionally reduced to the minimum
 * required to avoid endless peer retransmissions.
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


enum {
	IX_MARKER          = 0x20,

	IX_CHANNEL_PING    = 0,
	IX_CHANNEL_XCCP    = 2,
	IX_CHANNEL_GENERICMSG = 4,

	UDT_CTRL_HANDSHAKE = 0,
	UDT_CTRL_KEEPALIVE = 1,
	UDT_CTRL_ACK       = 2,
	UDT_CTRL_NAK       = 3,
	UDT_CTRL_SHUTDOWN  = 5,
	UDT_CTRL_ACK2      = 6,

	UDT_VERSION        = 4,
	UDT_SOCKTYPE_STREAM = 1,

	/** Reserved margin at the head of each outgoing mbuf, so that the
	 * TURN helper can prepend its ChannelData header (4 bytes,
	 * CHAN_HDR_SIZE) in place, or before the channel is bound
	 * (ChannelBind), the full Send STUN indication (STUN_HEADER_SIZE=20 +
	 * 2 attribute headers + IPv4 address = 36 bytes). Without this margin,
	 * the helper cannot rewrite the packet and leaves it in clear text,
	 * outside the relay - same mechanism as RTP (RTP_PRESZ) in video.c. */
	IX_TURN_PRESZ      = 36,

	IX_HS_RETRY_MS     = 1000,   /**< handshake retry interval */
	IX_KEEPALIVE_MS    = 1000,   /**< keep-alive interval */
};


struct ix_cisco {
	struct udp_sock *us;
	struct sdp_media *sdpm;
	const struct mnat *mnat;
	struct mnat_media *mnat_st;
	bool active;              /**< offerer state */
	struct tmr tmr_hs;        /**< handshake timer */
	struct tmr tmr_ka;        /**< keep-alive timer */

	struct sa raddr;          /**< remote peer address */
	bool have_raddr;

	uint32_t local_sock_id;
	uint32_t remote_sock_id;
	uint32_t local_seq;       /**< local sequence counter */
	uint32_t local_msgno;
	uint32_t local_ack_num;
	uint32_t last_seq_recv;
	bool have_last_seq_recv;
	bool established;
	int32_t last_conn_type;   /**< last conn_type seen */
};


static void tmr_ka_handler(void *arg);
static void tmr_hs_handler(void *arg);


static void destructor(void *arg)
{
	struct ix_cisco *ix = arg;

	tmr_cancel(&ix->tmr_hs);
	tmr_cancel(&ix->tmr_ka);

	mem_deref(ix->mnat_st);
	mem_deref(ix->sdpm);
	mem_deref(ix->us);
}


static uint32_t ix_now_us(void)
{
	/* Relative clock in microseconds - the absolute value is irrelevant,
	 * only the peer uses it for its own RTT calculations on the send side;
	 * our ACKs return a default RTT value. */
	return (uint32_t)(tmr_jiffies() * 1000);
}


/* ---------------------------------------------------------------------
 * UDT/IX frame construction.
 * ------------------------------------------------------------------- */

static int write_ctrl_header(struct mbuf *mb, uint16_t ctrl_type,
			      uint32_t add_info, uint32_t dst_sock_id)
{
	int err = 0;

	err |= mbuf_write_u32(mb, htonl(0x80000000u | ((uint32_t)ctrl_type << 16)));
	err |= mbuf_write_u32(mb, htonl(add_info));
	err |= mbuf_write_u32(mb, htonl(ix_now_us()));
	err |= mbuf_write_u32(mb, htonl(dst_sock_id));

	return err;
}


static int ix_build_handshake(struct ix_cisco *ix, struct mbuf *mb,
			       int32_t conn_type)
{
	int err = 0;

	err |= write_ctrl_header(mb, UDT_CTRL_HANDSHAKE, 0, ix->remote_sock_id);

	err |= mbuf_write_u32(mb, htonl(UDT_VERSION));
	err |= mbuf_write_u32(mb, htonl(UDT_SOCKTYPE_STREAM));
	err |= mbuf_write_u32(mb, htonl(ix->local_seq));
	err |= mbuf_write_u32(mb, htonl(1402));
	err |= mbuf_write_u32(mb, htonl(8192));
	err |= mbuf_write_u32(mb, htonl((uint32_t)conn_type));
	err |= mbuf_write_u32(mb, htonl(ix->local_sock_id));
	err |= mbuf_write_u32(mb, 0);
	for (int i = 0; i < 4 && !err; i++)
		err |= mbuf_write_u32(mb, 0);

	return err;
}


/** ACK payload with the standard 4 fields: next expected seq, RTT,
 * RTT variance, and available buffer. */
static int ix_build_ack(struct ix_cisco *ix, struct mbuf *mb,
			 uint32_t ack_seq_num, uint32_t next_expected_seq)
{
	int err = 0;

	err |= write_ctrl_header(mb, UDT_CTRL_ACK, ack_seq_num, ix->remote_sock_id);
	err |= mbuf_write_u32(mb, htonl(next_expected_seq & 0x7FFFFFFFu));
	err |= mbuf_write_u32(mb, htonl(100000));
	err |= mbuf_write_u32(mb, htonl(50000));
	err |= mbuf_write_u32(mb, htonl(65536));

	return err;
}


static int ix_build_ack2(struct ix_cisco *ix, struct mbuf *mb, uint32_t ack_seq)
{
	return write_ctrl_header(mb, UDT_CTRL_ACK2, ack_seq, ix->remote_sock_id);
}


static int ix_build_keepalive(struct ix_cisco *ix, struct mbuf *mb)
{
	int err = write_ctrl_header(mb, UDT_CTRL_KEEPALIVE, 0, ix->remote_sock_id);
	err |= mbuf_write_u32(mb, 0);
	return err;
}


/** UDT data packet + IX sub-header + application payload. */
static int ix_build_data(struct ix_cisco *ix, struct mbuf *mb,
			  uint8_t channel, const uint8_t *payload, size_t len)
{
	uint32_t seq_to_use = ix->local_seq;
	uint32_t msgfield;
	int err = 0;

	ix->local_seq = (ix->local_seq + 1) & 0x7FFFFFFFu;
	ix->local_msgno++;
	msgfield = 0xC0000000u | (ix->local_msgno & 0x1FFFFFFFu);

	err |= mbuf_write_u32(mb, htonl(seq_to_use & 0x7FFFFFFFu));
	err |= mbuf_write_u32(mb, htonl(msgfield));
	err |= mbuf_write_u32(mb, htonl(ix_now_us()));
	err |= mbuf_write_u32(mb, htonl(ix->remote_sock_id));

	err |= mbuf_write_u8(mb, IX_MARKER);
	err |= mbuf_write_u8(mb, 0);
	err |= mbuf_write_u8(mb, 0);
	err |= mbuf_write_u8(mb, channel);
	err |= mbuf_write_u32(mb, htonl((uint32_t)len));
	if (len)
		err |= mbuf_write_mem(mb, payload, len);

	return err;
}


/** Allocate an mbuf with the TURN-safe headroom reserved. */
static struct mbuf *ix_mbuf_alloc(size_t extra)
{
	struct mbuf *mb = mbuf_alloc(IX_TURN_PRESZ + extra);

	if (!mb)
		return NULL;

	mbuf_set_end(mb, IX_TURN_PRESZ);
	mbuf_advance(mb, IX_TURN_PRESZ);

	return mb;
}


static int ix_send(struct ix_cisco *ix, struct mbuf *mb)
{
	if (!ix->have_raddr)
		return ENOTCONN;

	/* Keep the TURN headroom available for a later prefix. */
	mbuf_set_pos(mb, IX_TURN_PRESZ);
	return udp_send(ix->us, &ix->raddr, mb);
}


/* ---------------------------------------------------------------------
 * Application send path
 * ------------------------------------------------------------------- */

/** minimal XML escaping for the FarEndMessage text */
static int xml_escape(struct mbuf *mb, const char *text)
{
	int err = 0;

	for (; *text && !err; text++) {
		switch (*text) {
		case '&':  err = mbuf_write_str(mb, "&amp;");  break;
		case '<':  err = mbuf_write_str(mb, "&lt;");   break;
		case '>':  err = mbuf_write_str(mb, "&gt;");   break;
		case '\'': err = mbuf_write_str(mb, "&#39;");  break;
		case '"':  err = mbuf_write_str(mb, "&quot;"); break;
		default:   err = mbuf_write_u8(mb, (uint8_t)*text); break;
		}
	}

	return err;
}


int ix_cisco_send_message(struct ix_cisco *ix, const char *text,
			   const char *type)
{
	struct mbuf *xml, *mb;
	int err = 0;

	if (!ix || !text)
		return EINVAL;
	if (!ix->established)
		return ENOTCONN;

	xml = mbuf_alloc(256);
	mb  = ix_mbuf_alloc(512);
	if (!xml || !mb) {
		err = ENOMEM;
		goto out;
	}

	err |= mbuf_write_str(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	err |= mbuf_write_str(xml, "<generic-message type=\"");
	err |= mbuf_write_str(xml, type ? type : "Text");
	err |= mbuf_write_str(xml, "\">\n  <payload>");
	err |= xml_escape(xml, text);
	err |= mbuf_write_str(xml, "</payload>\n</generic-message>\n");
	if (err)
		goto out;

	err = ix_build_data(ix, mb, IX_CHANNEL_GENERICMSG,
			     xml->buf, xml->end);
	if (err)
		goto out;

	err = ix_send(ix, mb);
	if (!err)
		info("ix_cisco: FarEndMessage sent: '%s'\n", text);

 out:
	mem_deref(xml);
	mem_deref(mb);
	return err;
}


/* ---------------------------------------------------------------------
 * Reception
 * ------------------------------------------------------------------- */

static void handle_generic_message(const uint8_t *xml, size_t len)
{
	/* minimal extraction - no dependency on a full XML parser */
	struct pl body, type_pl, payload_pl;
	const char *type_start, *type_end;
	const char *pl_start, *pl_end;

	body.p = (const char *)xml;
	body.l = len;

	pl_set_str(&type_pl, "");
	pl_set_str(&payload_pl, "");

	type_start = pl_strstr(&body, "type=\"");
	if (type_start) {
		struct pl rest;

		type_start += str_len("type=\"");
		rest.p = type_start;
		rest.l = (size_t)(body.p + body.l - type_start);
		type_end = pl_strstr(&rest, "\"");
		if (type_end) {
			type_pl.p = type_start;
			type_pl.l = (size_t)(type_end - type_start);
		}
	}

	pl_start = pl_strstr(&body, "<payload>");
	if (pl_start) {
		struct pl rest;

		pl_start += str_len("<payload>");
		rest.p = pl_start;
		rest.l = (size_t)(body.p + body.l - pl_start);
		pl_end = pl_strstr(&rest, "</payload>");
		if (pl_end) {
			payload_pl.p = pl_start;
			payload_pl.l = (size_t)(pl_end - pl_start);
		}
	}

	info("ix_cisco: [RX] FarEndMessage Type='%r' Msg='%r'\n",
	     &type_pl, &payload_pl);

	if (payload_pl.p && payload_pl.l) {
		info("ix_cisco: custom event payload length=%zu\n", payload_pl.l);

		if (type_pl.p && type_pl.l) {
			(void)bevent_ua_emit(UA_EVENT_CUSTOM, NULL,
					"ix_cisco:%r:%r",
					&type_pl,
					&payload_pl);
		}
		else {
			(void)bevent_ua_emit(UA_EVENT_CUSTOM, NULL,
					"ix_cisco:unknown:%r",
					&payload_pl);
		}
	}
}


static void ix_udp_recv(const struct sa *src, struct mbuf *mb, void *arg)
{
	struct ix_cisco *ix = arg;
	uint32_t w0, w1, ts, dst_sock;
	bool is_ctrl;

	if (mbuf_get_left(mb) < 16)
		return;

	w0 = ntohl(mbuf_read_u32(mb));
	w1 = ntohl(mbuf_read_u32(mb));
	ts = ntohl(mbuf_read_u32(mb));
	dst_sock = ntohl(mbuf_read_u32(mb));
	(void)ts;
	(void)dst_sock;

	is_ctrl = (w0 & 0x80000000u) != 0;

	if (!ix->have_raddr || !sa_cmp(&ix->raddr, src, SA_ALL)) {
		sa_cpy(&ix->raddr, src);
		ix->have_raddr = true;
		info("ix_cisco: peer detected on %J\n", src);
	}

	if (is_ctrl) {
		uint16_t ctrl_type = (uint16_t)((w0 >> 16) & 0x7FFF);

		switch (ctrl_type) {

		case UDT_CTRL_HANDSHAKE: {
			int32_t conn_type;
			uint32_t remote_sockid;
			struct mbuf *rmb;

			if (mbuf_get_left(mb) < 32)
				break;

			(void)mbuf_read_u32(mb);
			(void)mbuf_read_u32(mb);
			(void)mbuf_read_u32(mb);
			(void)mbuf_read_u32(mb);
			(void)mbuf_read_u32(mb);
			conn_type = (int32_t)ntohl(mbuf_read_u32(mb));
			remote_sockid = ntohl(mbuf_read_u32(mb));

			ix->remote_sock_id = remote_sockid;
			ix->last_conn_type = conn_type;

			info("ix_cisco: [HS] received from %J conn_type=%d remote_sock_id=%08x\n",
			     src, conn_type, remote_sockid);

			rmb = ix_mbuf_alloc(64);
			if (rmb) {
				if (!ix_build_handshake(ix, rmb, conn_type))
					(void)ix_send(ix, rmb);
				mem_deref(rmb);
			}
			ix->established = true;
			tmr_cancel(&ix->tmr_hs);
			tmr_start(&ix->tmr_ka, IX_KEEPALIVE_MS, tmr_ka_handler, ix);
			break;
		}

		case UDT_CTRL_ACK: {
			struct mbuf *rmb = ix_mbuf_alloc(32);
			if (rmb) {
				if (!ix_build_ack2(ix, rmb, w1))
					(void)ix_send(ix, rmb);
				mem_deref(rmb);
			}
			break;
		}

		case UDT_CTRL_ACK2:
		case UDT_CTRL_KEEPALIVE:
			break;

		case UDT_CTRL_SHUTDOWN:
			info("ix_cisco: Shutdown received from %J\n", src);
			ix->established = false;
			ix->have_raddr = false;
			ix->remote_sock_id = 0;
			break;

		default:
			break;
		}

		return;
	}

	/* data packet */
	{
		uint32_t seq = w0 & 0x7FFFFFFFu;
		struct mbuf *rmb;
		uint8_t marker, channel;
		uint32_t ix_len;

		if (ix->remote_sock_id == 0) {
			rmb = ix_mbuf_alloc(64);
			if (rmb) {
				if (!ix_build_handshake(ix, rmb, 0))
					(void)ix_send(ix, rmb);
				mem_deref(rmb);
			}
		}

		if (!ix->have_last_seq_recv ||
		    seq > ix->last_seq_recv ||
		    (ix->last_seq_recv - seq) > 0x40000000u) {
			ix->last_seq_recv = seq;
			ix->have_last_seq_recv = true;
		}
		rmb = ix_mbuf_alloc(32);
		if (rmb) {
			if (!ix_build_ack(ix, rmb, ix->local_ack_num,
					  ix->last_seq_recv + 1))
				(void)ix_send(ix, rmb);
			mem_deref(rmb);
		}
		ix->local_ack_num++;

		if (mbuf_get_left(mb) < 8)
			return;

		marker = mbuf_read_u8(mb);
		(void)mbuf_read_u8(mb);
		(void)mbuf_read_u8(mb);
		channel = mbuf_read_u8(mb);
		ix_len  = ntohl(mbuf_read_u32(mb));

		if (marker != IX_MARKER)
			return;
		if (ix_len > mbuf_get_left(mb))
			ix_len = (uint32_t)mbuf_get_left(mb);

		switch (channel) {

		case IX_CHANNEL_GENERICMSG:
			handle_generic_message(mbuf_buf(mb), ix_len);
			break;

		case IX_CHANNEL_XCCP:
			info("ix_cisco: [RX] xccp (%u bytes, unhandled)\n", ix_len);
			break;

		case IX_CHANNEL_PING:
			info("ix_cisco: [RX] ping (%u bytes)\n", ix_len);
			break;

		default:
			info("ix_cisco: [RX] unknown channel %u (%u bytes)\n",
			     channel, ix_len);
			break;
		}
	}
}


/* ---------------------------------------------------------------------
 * Timers
 * ------------------------------------------------------------------- */

static void tmr_hs_handler(void *arg)
{
	struct ix_cisco *ix = arg;
	struct mbuf *mb;

	if (ix->established || !ix->have_raddr)
		goto reschedule;

	mb = ix_mbuf_alloc(64);
	if (mb) {
		if (!ix_build_handshake(ix, mb, 0))
			(void)ix_send(ix, mb);
		mem_deref(mb);
	}

 reschedule:
	tmr_start(&ix->tmr_hs, IX_HS_RETRY_MS, tmr_hs_handler, ix);
}


static void tmr_ka_handler(void *arg)
{
	struct ix_cisco *ix = arg;
	struct mbuf *mb;

	if (ix->established && ix->have_raddr) {
		mb = ix_mbuf_alloc(32);
		if (mb) {
			if (!ix_build_keepalive(ix, mb))
				(void)ix_send(ix, mb);
			mem_deref(mb);
		}
	}

	tmr_start(&ix->tmr_ka, IX_KEEPALIVE_MS, tmr_ka_handler, ix);
}


/* ---------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */

static void mnat_connected_handler(const struct sa *raddr1,
				    const struct sa *raddr2, void *arg)
{
	struct ix_cisco *ix = arg;
	(void)raddr2;

	info("ix_cisco: mnat '%s' connected: raddr %J\n", ix->mnat->id, raddr1);
	sa_cpy(&ix->raddr, raddr1);
	ix->have_raddr = true;
}


int ix_cisco_alloc(struct ix_cisco **ixp, struct sdp_session *sdp_sess,
		    bool offerer, const struct mnat *mnat,
		    struct mnat_sess *mnat_sess)
{
	struct ix_cisco *ix;
	struct sa laddr;
	int err;

	if (!ixp || !sdp_sess)
		return EINVAL;

	ix = mem_zalloc(sizeof(*ix), destructor);
	if (!ix)
		return ENOMEM;

	ix->active = offerer;
	ix->local_sock_id = rand_u32();
	ix->local_seq = rand_u32() & 0x7FFFFFFFu;
	ix->last_conn_type = 0;
	tmr_init(&ix->tmr_hs);
	tmr_init(&ix->tmr_ka);

	sa_init(&laddr, AF_INET);

	err = udp_listen(&ix->us, &laddr, ix_udp_recv, ix);
	if (err)
		goto out;

	err = udp_local_get(ix->us, &laddr);
	if (err)
		goto out;

	err = sdp_media_add(&ix->sdpm, sdp_sess, "application",
			     sa_port(&laddr), "UDP/UDT/IX");
	if (err)
		goto out;

	err = sdp_format_add(NULL, ix->sdpm, false, "*", NULL,
			      0, 0, NULL, NULL, NULL, false, NULL);
	if (err)
		goto out;

	err |= sdp_media_set_lattr(ix->sdpm, false, "ixmap", "0 ping");
	err |= sdp_media_set_lattr(ix->sdpm, false, "ixmap", "2 xccp");
	err |= sdp_media_set_lattr(ix->sdpm, false, "ixmap", "4 genericmsg");
	if (err)
		goto out;

	if (mnat) {
		ix->mnat = mnat;
		info("ix_cisco: medianat '%s' enabled on the UDP socket\n", mnat->id);
		err = mnat->mediah(&ix->mnat_st, mnat_sess, ix->us, NULL,
			   ix->sdpm, mnat_connected_handler, ix);
		if (err)
			goto out;
	}

	info("ix_cisco: agent %s on port %d\n",
	     ix->active ? "active" : "passive", sa_port(&laddr));

 out:
	if (err)
		mem_deref(ix);
	else
		*ixp = ix;

	return err;
}


int ix_cisco_start(struct ix_cisco *ix)
{
	const struct sa *raddr;
	struct mbuf *mb;
	int err;

	if (!ix)
		return EINVAL;

	if (!sdp_media_rport(ix->sdpm)) {
		info("ix_cisco: channel disabled (no remote port in SDP)\n");
		return 0;
	}

	raddr = sdp_media_raddr(ix->sdpm);
	if (raddr && sa_isset(raddr, SA_ALL)) {
		sa_cpy(&ix->raddr, raddr);
		ix->have_raddr = true;
	}

	mb = ix_mbuf_alloc(64);
	if (!mb)
		return ENOMEM;

	err = ix_build_handshake(ix, mb, 0);
	if (!err)
		err = ix_send(ix, mb);
	mem_deref(mb);

	tmr_start(&ix->tmr_hs, IX_HS_RETRY_MS, tmr_hs_handler, ix);

	return err;
}