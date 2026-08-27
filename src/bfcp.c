/**
 * @file bfcp.c  BFCP client
 *
 * Copyright (C) 2011 Creytiv.com
 */
#include <stdlib.h>
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


/**
 * Floor participant (client) state for the slides floor.
 *
 * baresip acts as a BFCP floor participant (a=floorctrl:c-only): the
 * remote endpoint is the floor control server. To send a secondary
 * (slides) video stream, the participant must first be granted the
 * floor: FloorRequest -> FloorRequestStatus(Granted) ... FloorRelease.
 */
enum floor_state {
	FLOOR_IDLE = 0,    /**< No floor request outstanding             */
	FLOOR_REQUESTING,  /**< FloorRequest sent, waiting for a decision */
	FLOOR_GRANTED,     /**< Floor is held, slides may be sent         */
	FLOOR_RELEASING,   /**< FloorRelease sent, waiting for response   */
};


struct bfcp {
	struct bfcp_conn *conn;
	struct sdp_media *sdpm;
	const struct mnat *mnat;
	struct mnat_media *mnat_st;
	bool active;
	struct tmr tmr_hello;

	/* server */
	uint32_t lconfid;
	uint16_t luserid;

	uint16_t lfloorid;
	uint16_t lmstrm;

	/* client (floor participant) */
	enum floor_state fstate;
	uint16_t floorreqid;   /**< Floor Request ID assigned by the server */
	bool floorreqid_set;   /**< True once the server told us the ID     */
	bfcp_floor_h *floorh;
	void *arg;
};


static void destructor(void *arg)
{
	struct bfcp *bfcp = arg;

	tmr_cancel(&bfcp->tmr_hello);

	mem_deref(bfcp->mnat_st);
	mem_deref(bfcp->sdpm);
	mem_deref(bfcp->conn);
}

static int stdout_handler(const char *p, size_t size, void *arg)
{
	(void)arg;

	if (1 != fwrite(p, size, 1, stdout))
		return ENOMEM;

	return 0;
}


static const char *bfcp_sdp_transp(enum bfcp_transp tp)
{
	switch (tp) {

	case BFCP_UDP:  return "UDP/BFCP";
	case BFCP_TCP:  return "TCP/BFCP";
	case BFCP_DTLS: return "UDP/TLS/BFCP";
	default:        return NULL;
	}
}


static enum bfcp_transp str2tp(const char *proto)
{
	if (0 == str_casecmp(proto, "udp"))
		return BFCP_UDP;
	if (0 == str_casecmp(proto, "tcp"))
		return BFCP_TCP;
	else if (0 == str_casecmp(proto, "dtls"))
		return BFCP_DTLS;
	else {
		warning("unsupported BFCP protocol: %s\n", proto);
		return -1;
	}
}


static const char *floor_state_name(enum floor_state st)
{
	switch (st) {

	case FLOOR_IDLE:       return "idle";
	case FLOOR_REQUESTING: return "requesting";
	case FLOOR_GRANTED:    return "granted";
	case FLOOR_RELEASING:  return "releasing";
	default:               return "???";
	}
}


/*
 * Conference ID and User ID of the remote floor control server, as
 * advertised in its SDP (RFC 4583). Both are mandatory on the server
 * side; the RFC-agnostic defaults below are kept for compatibility
 * with the previous Hello behaviour.
 */
static void remote_ids(const struct bfcp *bfcp,
		       uint32_t *confid, uint16_t *userid)
{
	const char *v;

	*confid = 1;
	*userid = 1;

	v = sdp_media_rattr(bfcp->sdpm, "confid");
	if (v)
		*confid = (uint32_t)strtoul(v, NULL, 10);

	v = sdp_media_rattr(bfcp->sdpm, "userid");
	if (v)
		*userid = (uint16_t)strtoul(v, NULL, 10);
}


/*
 * Floor ID of the remote server, as advertised by its "a=floorid"
 * SDP attribute. Observed formats:
 *
 *   Cisco    a=floorid:2 mstrm:12
 *   Poly     a=floorid:1 m-stream:3
 *   baresip  a=floorid:1 mstrm 3
 *
 * Only the first floor is considered. The media stream label is
 * returned for information; it is not used to select the local
 * slides stream (which is identified by a=content:slides).
 */
static int remote_floorid(const struct bfcp *bfcp,
			  uint16_t *floorid, uint16_t *label)
{
	const char *v, *p;
	char *end = NULL;
	unsigned long n;

	v = sdp_media_rattr(bfcp->sdpm, "floorid");
	if (!v)
		return ENOENT;

	n = strtoul(v, &end, 10);
	if (end == v || n == 0 || n > 0xffff)
		return EBADMSG;

	*floorid = (uint16_t)n;

	if (label) {
		*label = 0;

		p = strstr(v, "mstrm");
		if (!p)
			p = strstr(v, "m-stream");

		if (p) {
			p += strcspn(p, ": ");
			p += strspn(p, ": ");
			n = strtoul(p, &end, 10);
			if (end != p && n <= 0xffff)
				*label = (uint16_t)n;
		}
	}

	return 0;
}


static void floor_set_state(struct bfcp *bfcp, enum floor_state st)
{
	if (bfcp->fstate == st)
		return;

	info("bfcp: floor state %s -> %s\n",
	     floor_state_name(bfcp->fstate), floor_state_name(st));

	bfcp->fstate = st;
}


static void floor_notify(struct bfcp *bfcp, bool granted)
{
	if (bfcp->floorh)
		bfcp->floorh(granted, bfcp->arg);
}


/*
 * Extract the Floor Request ID and the overall request status from a
 * FloorRequestStatus or FloorStatus message (RFC 4582 section 5.3):
 *
 *   FLOOR-REQUEST-INFORMATION (frid)
 *     OVERALL-REQUEST-STATUS (frid)
 *       REQUEST-STATUS
 *     FLOOR-REQUEST-STATUS (floorid)
 *       [REQUEST-STATUS]
 *
 * Both Cisco and Poly put REQUEST-STATUS under OVERALL-REQUEST-STATUS.
 * A REQUEST-STATUS directly under FLOOR-REQUEST-STATUS is accepted as
 * a fallback. Only the first FLOOR-REQUEST-INFORMATION is looked at.
 */
static bool parse_reqstatus(const struct bfcp_msg *msg,
			    uint16_t *frid, enum bfcp_reqstat *status)
{
	const struct bfcp_attr *fri, *ors, *frs, *rs = NULL;

	fri = bfcp_msg_attr(msg, BFCP_FLOOR_REQ_INFO);
	if (!fri)
		return false;

	ors = bfcp_attr_subattr(fri, BFCP_OVERALL_REQ_STATUS);
	if (ors)
		rs = bfcp_attr_subattr(ors, BFCP_REQUEST_STATUS);

	if (!rs) {
		frs = bfcp_attr_subattr(fri, BFCP_FLOOR_REQ_STATUS);
		if (frs)
			rs = bfcp_attr_subattr(frs, BFCP_REQUEST_STATUS);
	}

	if (!rs)
		return false;

	*frid   = fri->v.floorreqid;
	*status = rs->v.reqstatus.status;

	return true;
}


/*
 * Apply a request status to the local floor state machine.
 */
static void floor_apply_status(struct bfcp *bfcp, enum bfcp_reqstat status)
{
	info("bfcp: floor request %u status: %s\n",
	     bfcp->floorreqid, bfcp_reqstatus_name(status));

	switch (status) {

	case BFCP_GRANTED:
		if (bfcp->fstate == FLOOR_RELEASING)
			break;
		floor_set_state(bfcp, FLOOR_GRANTED);
		floor_notify(bfcp, true);
		break;

	case BFCP_PENDING:
	case BFCP_ACCEPTED:
		/* Not a decision yet. The server will send a
		 * FloorRequestStatus later (handled in bfcp_msg_handler). */
		break;

	case BFCP_DENIED:
	case BFCP_CANCELLED:
	case BFCP_RELEASED:
	case BFCP_REVOKED:
	default:
		floor_set_state(bfcp, FLOOR_IDLE);
		floor_notify(bfcp, false);
		break;
	}
}


static void floor_request_resp_handler(int err, const struct bfcp_msg *msg,
				       void *arg)
{
	struct bfcp *bfcp = arg;
	const struct bfcp_attr *attr;
	enum bfcp_reqstat status;
	uint16_t frid;

	if (bfcp->fstate != FLOOR_REQUESTING)
		return;

	if (err) {
		warning("bfcp: floor request failed: %m\n", err);
		goto fail;
	}

	if (msg->prim == BFCP_ERROR) {
		attr = bfcp_msg_attr(msg, BFCP_ERROR_CODE);
		warning("bfcp: floor request rejected: %s\n",
			attr ? bfcp_errcode_name(attr->v.errcode.code)
			     : "unknown error");
		goto fail;
	}

	if (msg->prim != BFCP_FLOOR_REQUEST_STATUS) {
		warning("bfcp: unexpected response to FloorRequest: %s\n",
			bfcp_prim_name(msg->prim));
		goto fail;
	}

	if (!parse_reqstatus(msg, &frid, &status)) {
		warning("bfcp: FloorRequestStatus without request status\n");
		goto fail;
	}

	bfcp->floorreqid = frid;
	bfcp->floorreqid_set = true;
	floor_apply_status(bfcp, status);
	return;

 fail:
	floor_set_state(bfcp, FLOOR_IDLE);
	floor_notify(bfcp, false);
}


static void floor_release_resp_handler(int err, const struct bfcp_msg *msg,
				       void *arg)
{
	struct bfcp *bfcp = arg;

	if (err)
		warning("bfcp: floor release failed: %m\n", err);
	else if (msg->prim == BFCP_ERROR)
		warning("bfcp: floor release rejected\n");
	else
		info("bfcp: floor release confirmed: %s\n",
		     bfcp_prim_name(msg->prim));

	/* Whatever the outcome, the local state is idle: we already
	 * stopped sending when the release was issued. */
	if (bfcp->fstate == FLOOR_RELEASING)
		floor_set_state(bfcp, FLOOR_IDLE);
}


/*
 * Server-initiated FloorRequestStatus / FloorStatus: apply it only if
 * it concerns our own floor request. The server may also report other
 * participants' requests (e.g. the remote endpoint sharing its own
 * content), which must not disturb our state.
 */
static void floor_apply_server_msg(struct bfcp *bfcp,
				   const struct bfcp_msg *msg)
{
	enum bfcp_reqstat status;
	uint16_t frid;

	if (bfcp->fstate == FLOOR_IDLE)
		return;

	if (!parse_reqstatus(msg, &frid, &status))
		return;

	if (!bfcp->floorreqid_set || frid != bfcp->floorreqid) {

		/* A FloorRequestStatus is addressed to the participant
		 * that made the request (RFC 4582 section 5.3.4). While
		 * our request is pending and the server has not told us
		 * its ID yet, adopt it. This also covers a transaction
		 * response that the transport layer failed to match
		 * (e.g. a server sending responses with the R flag
		 * clear): the pending transaction will time out, but
		 * that is ignored once we left the requesting state. */
		if (msg->prim == BFCP_FLOOR_REQUEST_STATUS &&
		    bfcp->fstate == FLOOR_REQUESTING &&
		    !bfcp->floorreqid_set) {
			info("bfcp: adopting floor request %u from"
			     " server-initiated FloorRequestStatus\n", frid);
			bfcp->floorreqid = frid;
			bfcp->floorreqid_set = true;
		}
		else {
			debug("bfcp: %s for foreign request %u ignored\n",
			      bfcp_prim_name(msg->prim), frid);
			return;
		}
	}

	floor_apply_status(bfcp, status);
}


static void bfcp_resp_handler(int err, const struct bfcp_msg *msg, void *arg)
{
	struct bfcp *bfcp = arg;
	(void)bfcp;

	if (err) {
		warning("bfcp: error response: %m\n", err);
		return;
	}

	info("bfcp: received BFCP response: '%s'\n",
	     bfcp_prim_name(msg->prim));

	struct re_printf pf;
	pf.vph = stdout_handler;
	pf.arg = NULL;
	bfcp_msg_print(&pf, msg);
}


void print_attr(const struct  bfcp_attr *attr,void *dummy)
{
	struct re_printf pf;
	pf.vph = stdout_handler;
	pf.arg = NULL;
	bfcp_attr_print(&pf, attr);
	info("\n");
}


static void bfcp_msg_handler(const struct bfcp_msg *msg, void *arg)
{
	struct bfcp *bfcp = arg;

	info("bfcp: received BFCP message '%s'\n", bfcp_prim_name(msg->prim));

	struct re_printf pf;
	struct bfcp_attr *attr;
	pf.vph = stdout_handler;
	pf.arg = NULL;
	bfcp_msg_print(&pf, msg);
	struct bfcp_supprim  supprim;
	struct bfcp_supattr  supattr;
	enum bfcp_prim  prim[] = { BFCP_FLOOR_REQUEST,
				    BFCP_FLOOR_RELEASE,
				    BFCP_FLOOR_REQUEST_QUERY,
				    BFCP_FLOOR_REQUEST_STATUS,
				    BFCP_HELLO,
				    BFCP_HELLO_ACK,
				    BFCP_GOODBYE,
				    BFCP_GOODBYE_ACK,
				    BFCP_ERROR };

	enum bfcp_attrib  attrib[] = {  BFCP_BENEFICIARY_ID,
					BFCP_FLOOR_ID,
					BFCP_FLOOR_REQUEST_ID,
					BFCP_PRIORITY,
					BFCP_REQUEST_STATUS,
					BFCP_ERROR_CODE,
					BFCP_ERROR_INFO,
					BFCP_PART_PROV_INFO,
					BFCP_STATUS_INFO,
					BFCP_SUPPORTED_ATTRS ,
					BFCP_SUPPORTED_PRIMS,
					BFCP_USER_DISP_NAME,
					BFCP_USER_URI ,
					BFCP_BENEFICIARY_INFO,
					BFCP_FLOOR_REQ_INFO,
					BFCP_REQUESTED_BY_INFO,
					BFCP_FLOOR_REQ_STATUS,
					BFCP_OVERALL_REQ_STATUS };

	switch (msg->prim) {

	case BFCP_HELLO:
		supprim.primv = prim;
		supprim.primc = sizeof(prim)/sizeof(prim[0]);
		supattr.attrv = attrib;
		supattr.attrc = sizeof(attrib)/sizeof(attrib[0]);

		(void)bfcp_reply(bfcp->conn, msg,
				BFCP_HELLO_ACK, 2,
				BFCP_SUPPORTED_ATTRS, 0, &supattr,
				BFCP_SUPPORTED_PRIMS, 0, &supprim);
		break;

	case BFCP_FLOOR_REQUEST:
		attr = bfcp_msg_attr(msg, BFCP_FLOOR_ID);

		if (!attr) {
			warning("bfcp: FLOOR_REQUEST without FLOOR_ID\n");
			break;
		}

		{
			uint16_t attr_val = attr->v.u16;
			uint16_t floor_request_id = 1;
			struct bfcp_reqstatus reqstatus;

			reqstatus.status = BFCP_GRANTED;
			reqstatus.qpos = 0;

			(void)bfcp_reply(bfcp->conn, msg,
					BFCP_FLOOR_REQUEST_STATUS, 1,
					BFCP_FLOOR_REQ_INFO, 2, &floor_request_id,
					BFCP_OVERALL_REQ_STATUS, 1, &floor_request_id,
					BFCP_REQUEST_STATUS, 0, &reqstatus,
					BFCP_FLOOR_REQ_STATUS, 0, &attr_val);
		}
		break;

	case BFCP_FLOOR_STATUS:

		info("bfcp: received FloorStatus\n");

		(void)bfcp_reply(bfcp->conn,
				msg,
				BFCP_FLOOR_STATUS_ACK,
				0);

		floor_apply_server_msg(bfcp, msg);
		break;

	case BFCP_FLOOR_REQUEST_STATUS:

		info("bfcp: received FloorRequestStatus\n");

		(void)bfcp_reply(bfcp->conn,
				msg,
				BFCP_FLOOR_REQ_STATUS_ACK,
				0);

		floor_apply_server_msg(bfcp, msg);
		break;

	case BFCP_FLOOR_RELEASE:
	case BFCP_HELLO_ACK:
	case BFCP_GOODBYE:
	case BFCP_GOODBYE_ACK:
		break;

	default:
		info("bfcp: ignoring unsupported primitive '%s'\n",
		bfcp_prim_name(msg->prim));
		break;
	}
}

static void mnat_connected_handler(const struct sa *raddr1,
				   const struct sa *raddr2, void *arg)
{
	struct bfcp *bfcp = arg;

	info("BFCP mnat '%s' connected: raddr %J %J\n",
	     bfcp->mnat->id, raddr1, raddr2);
}

int bfcp_alloc(struct bfcp **bfcpp, struct sdp_session *sdp_sess,
	       const struct config_bfcp *bfcp_cfg, bool offerer,
	       const struct mnat *mnat, struct mnat_sess *mnat_sess,
	       bfcp_floor_h *floorh, void *arg)
{
	struct bfcp *bfcp;
	struct sa laddr;
	enum bfcp_transp transp;
	int err;

	if (!bfcpp || !sdp_sess)
		return EINVAL;

	transp = str2tp(bfcp_cfg->proto);

	bfcp = mem_zalloc(sizeof(*bfcp), destructor);
	if (!bfcp)
		return ENOMEM;

	bfcp->active = offerer;
	bfcp->fstate = FLOOR_IDLE;
	bfcp->floorh = floorh;
	bfcp->arg    = arg;

	sa_init(&laddr, AF_INET);
	tmr_init(&bfcp->tmr_hello);

	err = bfcp_listen(&bfcp->conn, transp, &laddr, uag_tls(),
			  NULL, NULL, bfcp_msg_handler, NULL, bfcp);
	if (err)
		goto out;

	err = sdp_media_add(&bfcp->sdpm, sdp_sess, "application",
			    sa_port(&laddr), bfcp_sdp_transp(transp));
	if (err)
		goto out;

	err = sdp_format_add(NULL, bfcp->sdpm, false, "*", NULL,
			     0, 0, NULL, NULL, NULL, false, NULL);
	if (err)
		goto out;

	err |= sdp_media_set_lattr(bfcp->sdpm, true, "floorctrl",
				   str_isset(bfcp_cfg->floorctrl)?
				   bfcp_cfg->floorctrl:"c-s");
	err |= sdp_media_set_lattr(bfcp->sdpm, true, "setup",
				   bfcp->active ? "active" : "actpass");

	if (bfcp->active) {
		err |= sdp_media_set_lattr(bfcp->sdpm, true,
					   "connection", "new");
	}
	else {
		bfcp->lconfid = 1000 + (rand_u16() & 0xf);
		bfcp->luserid = 1    + (rand_u16() & 0x7);

		err |= sdp_media_set_lattr(bfcp->sdpm, true, "confid",
					   "%u", bfcp->lconfid);
		err |= sdp_media_set_lattr(bfcp->sdpm, true, "userid",
					   "%u", bfcp->luserid);

	bfcp->lfloorid = 1;
	bfcp->lmstrm = 3;
		err |= sdp_media_set_lattr(bfcp->sdpm, true, "floorid",
					   "%u mstrm %u", bfcp->lfloorid,
					   bfcp->lmstrm);

		err |= sdp_media_set_lattr(bfcp->sdpm, true,
					   "connection", "new");
	}

	if (err)
		goto out;

	if (mnat) {
		info("bfcp: enabled medianat '%s' on UDP socket\n", mnat->id);
		err = mnat->mediah(&bfcp->mnat_st, mnat_sess,
				   bfcp_sock(bfcp->conn), NULL, bfcp->sdpm,
				   mnat_connected_handler, bfcp);
		if (err)
			goto out;
	}


	info("bfcp: %s BFCP agent protocol '%s' on port %d\n",
	     bfcp->active ? "Active" : "Passive",
	     bfcp_cfg->proto, sa_port(&laddr));

 out:
	if (err)
		mem_deref(bfcp);
	else
		*bfcpp = bfcp;

	return err;
}


static void hello_tmr_handler(void *arg)
{
	(void)bfcp_send_hello(arg);
}


int bfcp_send_hello(struct bfcp *bfcp)
{
	const struct sa *paddr;
	uint32_t confid = 0;
	uint16_t userid = 0;
	int err = 0;

	tmr_start(&bfcp->tmr_hello, 10000, hello_tmr_handler, bfcp);

	paddr  = sdp_media_raddr(bfcp->sdpm);
	remote_ids(bfcp, &confid, &userid);

	uint16_t floor_id = 1;
	err = bfcp_request(bfcp->conn, paddr, BFCP_VER1, BFCP_HELLO,
			confid, userid, bfcp_resp_handler, bfcp, 1,
			BFCP_FLOOR_ID, 0, &floor_id);

	return err;

}

int bfcp_start(struct bfcp *bfcp)
{
	int err = 0;

	if (!bfcp)
		return EINVAL;

	if (!sdp_media_rport(bfcp->sdpm)) {
		info("bfcp channel is disabled\n");
		return 0;
	}

	err = bfcp_send_hello(bfcp);

	return err;
}


/**
 * Request the slides floor from the remote floor control server
 * (RFC 4582 FloorRequest). The floor handler is called with
 * granted=true once the server grants the floor.
 *
 * @param bfcp  BFCP object
 *
 * @return 0 if success, EALREADY if a request is outstanding or the
 *         floor is already held, EBUSY while a release is in progress,
 *         ENOENT if the remote SDP does not advertise a floor id,
 *         otherwise errorcode
 */
int bfcp_floor_request(struct bfcp *bfcp)
{
	uint32_t confid;
	uint16_t userid, floorid, label;
	int err;

	if (!bfcp)
		return EINVAL;

	if (!bfcp->conn || !sdp_media_rport(bfcp->sdpm))
		return ENOTCONN;

	switch (bfcp->fstate) {

	case FLOOR_REQUESTING:
	case FLOOR_GRANTED:
		return EALREADY;

	case FLOOR_RELEASING:
		return EBUSY;

	default:
		break;
	}

	err = remote_floorid(bfcp, &floorid, &label);
	if (err) {
		warning("bfcp: remote SDP has no usable a=floorid,"
			" cannot request floor (%m)\n", err);
		return err;
	}

	remote_ids(bfcp, &confid, &userid);

	info("bfcp: requesting floor %u (m-stream %u, confid=%u, userid=%u)\n",
	     floorid, label, confid, userid);

	err = bfcp_request(bfcp->conn, sdp_media_raddr(bfcp->sdpm),
			   BFCP_VER1, BFCP_FLOOR_REQUEST, confid, userid,
			   floor_request_resp_handler, bfcp, 1,
			   BFCP_FLOOR_ID, 0, &floorid);
	if (err) {
		warning("bfcp: could not send FloorRequest: %m\n", err);
		return err;
	}

	bfcp->floorreqid = 0;
	bfcp->floorreqid_set = false;
	floor_set_state(bfcp, FLOOR_REQUESTING);

	return 0;
}


/**
 * Release the slides floor (RFC 4582 FloorRelease). The floor handler
 * is called with granted=false immediately, without waiting for the
 * server's response.
 *
 * @param bfcp  BFCP object
 *
 * @return 0 if success or nothing to release, EBUSY while the request
 *         decision is still pending, otherwise errorcode
 */
int bfcp_floor_release(struct bfcp *bfcp)
{
	uint32_t confid;
	uint16_t userid, floorid = 0;
	int err;

	if (!bfcp)
		return EINVAL;

	switch (bfcp->fstate) {

	case FLOOR_IDLE:
	case FLOOR_RELEASING:
		return 0;

	case FLOOR_REQUESTING:
		/* The Floor Request ID is only known once the server
		 * has answered; a release cannot be built before that. */
		return EBUSY;

	default:
		break;
	}

	remote_ids(bfcp, &confid, &userid);
	(void)remote_floorid(bfcp, &floorid, NULL);

	info("bfcp: releasing floor %u (request %u)\n",
	     floorid, bfcp->floorreqid);

	/* FLOOR-ID is not required by RFC 4582 for FloorRelease, but
	 * Cisco endpoints include it; do the same. */
	err = bfcp_request(bfcp->conn, sdp_media_raddr(bfcp->sdpm),
			   BFCP_VER1, BFCP_FLOOR_RELEASE, confid, userid,
			   floor_release_resp_handler, bfcp, 2,
			   BFCP_FLOOR_REQUEST_ID, 0, &bfcp->floorreqid,
			   BFCP_FLOOR_ID, 0, &floorid);
	if (err) {
		warning("bfcp: could not send FloorRelease: %m\n", err);
		return err;
	}

	floor_set_state(bfcp, FLOOR_RELEASING);
	floor_notify(bfcp, false);

	return 0;
}


/**
 * Check if the slides floor is currently held
 *
 * @param bfcp  BFCP object
 *
 * @return true if the floor is granted, otherwise false
 */
bool bfcp_floor_granted(const struct bfcp *bfcp)
{
	return bfcp && bfcp->fstate == FLOOR_GRANTED;
}
