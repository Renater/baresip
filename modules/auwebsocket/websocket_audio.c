/**
 * @file websocket_audio.c WebSocket Audio Source with Embedded Server
 *
 * Copyright (C) 2025 Your Name
 *
 * Audio module for receiving audio data via WebSocket with an embedded server.
 *
 * Sample config:
 *
 \verbatim
  audio_source            websocket_audio,0.0.0.0:9000
 \endverbatim
 */

#define _DEFAULT_SOURCE 1
#define _BSD_SOURCE 1
#include <re.h>
#include <rem.h>
#include <baresip.h>

#define WEBSOCK_OPCODE_BIN 0x2

/* Audio source instance structure */
struct ausrc_st {
    struct ausrc_prm prm;            /**< Audio source parameters        */
    enum aufmt fmt;                  /**< Audio format                   */
    uint32_t ptime;                  /**< Frame time in ms               */
    size_t sampc;                    /**< Samples per frame              */
    ausrc_read_h *rh;                /**< Audio read callback            */
    ausrc_error_h *errh;             /**< Error callback                 */
    void *arg;                       /**< Callback argument              */
    struct http_sock *http_sock;     /**< HTTP server socket             */
    struct websock *ws;              /**< WebSocket                      */
    struct websock_conn *ws_conn;    /**< WebSocket connection           */
};

static struct ausrc *ausrc;

/* Destructor to release resources */
static void destructor(void *arg) {
    struct ausrc_st *st = arg;

    if (st->ws_conn) {
        /* Notify WebSocket client before closing the connection */
        websock_close(st->ws_conn, 1000, "SIP session ended");  // Code 1000: Normal Closure
    }

    mem_deref(st->ws_conn);
    mem_deref(st->http_sock);
    mem_deref(st->ws);
}

/* WebSocket message receive handler */
static void websocket_recv_handler(const struct websock_hdr *hdr, struct mbuf *mb, void *arg) {
    struct ausrc_st *st = arg;
    struct auframe af;

    if (!mb || mbuf_get_left(mb) == 0) {
        warning("websocket_audio: received an empty message\n");
        return;
    }

    if (hdr->opcode != WEBSOCK_OPCODE_BIN) {
        warning("websocket_audio: unsupported opcode (%d)\n", hdr->opcode);
        return;
    }

    auframe_init(&af, st->prm.fmt, mbuf_buf(mb), st->sampc, st->prm.srate, st->prm.ch);
    st->rh(&af, st->arg);

    if (mbuf_get_left(mb) != 2 * st->sampc) {
        warning("websocket_audio: received audio packet size mismatch with baresip config\n");
    }
}

/* WebSocket connection close handler */
static void websocket_close_handler(int err, void *arg) {
    struct ausrc_st *st = arg;
    warning("websocket_audio: WebSocket client disconnected (err=%d)\n", err);
    mem_deref(st->ws_conn);
}

/* HTTP request handler for incoming connections */
static void http_req_handler(struct http_conn *conn, const struct http_msg *msg, void *arg) {
    struct ausrc_st *st = arg;
    int err;

    err = websock_accept(&st->ws_conn, st->ws, conn, msg, 0,
                         websocket_recv_handler,
                         websocket_close_handler, st);
    if (err) {
        warning("Error accepting WebSocket connection: %m\n", err);
        mem_deref(st->ws_conn);
        return;
    }

    info("websocket_audio: WebSocket client connected\n");
}

/* Allocate WebSocket audio source */
int websocket_audio_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
                              struct ausrc_prm *prm, const char *device,
                              ausrc_read_h *rh, ausrc_error_h *errh, void *arg) {
    struct websock *ws = NULL;
    struct ausrc_st *st;
    struct sa laddr;
    int err;
    struct pl ws_url = {device, str_len(device)};

    if (!stp || !prm || !rh)
        return EINVAL;

    if (ws_url.l) {
        sa_decode(&laddr, ws_url.p, ws_url.l);
    } else {
        sa_set_str(&laddr, "0.0.0.0", 9000);
    }

    /* Create WebSocket server */
    err = websock_alloc(&ws, NULL, NULL);
    if (err) {
        warning("Error allocating WebSocket: %m\n", err);
        goto out;
    }

    st = mem_zalloc(sizeof(*st), destructor);
    if (!st)
        return ENOMEM;

    st->rh = rh;
    st->errh = errh;
    st->arg = arg;
    st->ptime = prm->ptime ? prm->ptime : 20;
    st->prm = *prm;
    st->sampc = prm->srate * prm->ch * st->ptime / 1000;
    st->ws = ws;

    /* Start HTTP server */
    err = http_listen(&st->http_sock, &laddr, http_req_handler, st);
    if (err) {
        warning("websocket_audio: error starting HTTP server (%m)\n", err);
        goto out;
    }

out:
    if (err)
        mem_deref(st);
    else
        *stp = st;

    return err;
}

/* Module initialization */
static int module_init(void) {
    return ausrc_register(&ausrc, baresip_ausrcl(), "websocket_audio", websocket_audio_src_alloc);
}

/* Module close */
static int module_close(void) {
    ausrc = mem_deref(ausrc);
    return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(websocket_audio) = {
    "websocket_audio",
    "ausrc",
    module_init,
    module_close
};
