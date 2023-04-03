/**
 * @file bfcp.c  BFCP client
 *
 * Copyright (C) 2011 Creytiv.com
 */
#include <stdlib.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


struct bfcp {
	struct ua *ua;
	struct bfcp_conn *conn;
	struct sdp_media *sdpm;
	const struct mnat *mnat;
	struct mnat_media *mnat_st;
	bool active;

	/* server */
	uint32_t lconfid;
	uint16_t luserid;

	uint16_t lfloorid;
	uint16_t lmstrm;
};


static void destructor(void *arg)
{
	struct bfcp *bfcp = arg;

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
	//bfcp_msg_print(&pf, msg);
	struct bfcp_supprim  supprim;
	struct bfcp_supattr  supattr;
	enum bfcp_prim  prim[] = { BFCP_FLOOR_REQUEST,
				    BFCP_FLOOR_RELEASE,
				    BFCP_FLOOR_REQUEST_QUERY,
				    BFCP_FLOOR_REQUEST_STATUS,
				    BFCP_USER_QUERY,
				    BFCP_USER_STATUS,
				    BFCP_FLOOR_QUERY,
				    BFCP_FLOOR_STATUS,
				    BFCP_CHAIR_ACTION,
				    BFCP_CHAIR_ACTION_ACK,
				    BFCP_HELLO,
				    BFCP_HELLO_ACK,
				    BFCP_ERROR,
				    BFCP_FLOOR_REQ_STATUS_ACK,
				    BFCP_FLOOR_STATUS_ACK,
				    BFCP_GOODBYE,
				    BFCP_GOODBYE_ACK };

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
		ua_event(bfcp->ua, UA_EVENT_CALL_BFCP, NULL, "BFCP_HELLO");
		(void)bfcp_reply(bfcp->conn, msg,
				 BFCP_HELLO_ACK, 2,
				 BFCP_SUPPORTED_ATTRS, 0, &supattr,
				 BFCP_SUPPORTED_PRIMS, 0, &supprim);
		break;

	case BFCP_FLOOR_REQUEST:
		ua_event(bfcp->ua, UA_EVENT_CALL_BFCP, NULL, "BFCP_FLOOR_REQUEST");
		attr = bfcp_msg_attr(msg, BFCP_FLOOR_ID);
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
		break;

	case BFCP_FLOOR_RELEASE:
		ua_event(bfcp->ua, UA_EVENT_CALL_BFCP, NULL, "BFCP_FLOOR_RELEASE");
		break;

	default:
		(void)bfcp_ereply(bfcp->conn, msg, BFCP_UNKNOWN_PRIM);
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
	       struct ua *ua,
	       const struct config_bfcp *bfcp_cfg, bool offerer,
	       const struct mnat *mnat, struct mnat_sess *mnat_sess)
{
	struct bfcp *bfcp;
	struct sa laddr;
	enum bfcp_transp transp;
	int err;

	if (!bfcpp || !sdp_sess || !ua)
		return EINVAL;

	transp = str2tp(bfcp_cfg->proto);

	bfcp = mem_zalloc(sizeof(*bfcp), destructor);
	if (!bfcp)
		return ENOMEM;

	bfcp->active = offerer;
	bfcp->ua = ua;

	sa_init(&laddr, AF_INET);
	//sa_set_port(&laddr, 5050);

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

#if 0
	if (bfcp->active) {
		err |= sdp_media_set_lattr(bfcp->sdpm, true,
					   "connection", "new");
	}
#endif

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


int bfcp_start(struct bfcp *bfcp)
{
	const struct sa *paddr;
	uint32_t confid = 0;
	uint16_t userid = 0;
	int err = 0;
	char *floorctrl;

	if (!bfcp)
		return EINVAL;

	if (!sdp_media_rport(bfcp->sdpm)) {
		info("bfcp channel is disabled\n");
		return 0;
	}

	floorctrl = sdp_media_rattr(bfcp->sdpm, "floorctrl");

	if (floorctrl )
		if (str_str(floorctrl, "s")) {

			paddr  = sdp_media_raddr(bfcp->sdpm);
			confid = atoi(sdp_media_rattr(bfcp->sdpm, "confid"));
			if(sdp_media_rattr(bfcp->sdpm, "confid"))
				confid = atoi(sdp_media_rattr(bfcp->sdpm, "confid"));

			userid = atoi(sdp_media_rattr(bfcp->sdpm, "userid"));
			if(sdp_media_rattr(bfcp->sdpm, "userid"))
				userid = atoi(sdp_media_rattr(bfcp->sdpm, "userid"));

			uint16_t floor_id = 1;
			err = bfcp_request(bfcp->conn, paddr, BFCP_VER1, BFCP_HELLO,
					confid, userid, bfcp_resp_handler, bfcp, 1,
					BFCP_FLOOR_ID, 0, &floor_id);
		}

	return err;
}
