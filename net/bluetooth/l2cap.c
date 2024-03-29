/* l2cap.c - L2CAP handling */

/*
 * Copyright (c) 2015 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <nanokernel.h>
#include <arch/cpu.h>
#include <toolchain.h>
#include <string.h>
#include <errno.h>
#include <atomic.h>
#include <misc/byteorder.h>
#include <misc/util.h>

#include <bluetooth/log.h>
#include <bluetooth/hci.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/driver.h>

#include "hci_core.h"
#include "conn_internal.h"
#include "l2cap_internal.h"

#if !defined(CONFIG_BLUETOOTH_DEBUG_L2CAP)
#undef BT_DBG
#define BT_DBG(fmt, ...)
#endif

#define L2CAP_LE_MIN_MTU		23
#define L2CAP_LE_MAX_CREDITS		(CONFIG_BLUETOOTH_ACL_IN_COUNT - 1)
#define L2CAP_LE_CREDITS_THRESHOLD	(L2CAP_LE_MAX_CREDITS / 2)

#define L2CAP_BR_MIN_MTU		48

#define L2CAP_LE_DYN_CID_START	0x0040
#define L2CAP_LE_DYN_CID_END	0x007f
#define L2CAP_BR_DYN_CID_START	0x0040
#define L2CAP_BR_DYN_CID_END	0xffff

#define L2CAP_LE_PSM_START	0x0001
#define L2CAP_LE_PSM_END	0x00ff

/* Size of MTU is based on the maximum amount of data the buffer can hold
 * excluding ACL and driver headers.
 */
#define BT_L2CAP_MAX_LE_MPS	CONFIG_BLUETOOTH_L2CAP_IN_MTU
/* For now use MPS - SDU length to disable segmentation */
#define BT_L2CAP_MAX_LE_MTU	(BT_L2CAP_MAX_LE_MPS - 2)

#define l2cap_lookup_ident(conn, ident) __l2cap_lookup_ident(conn, ident, false)
#define l2cap_remove_ident(conn, ident) __l2cap_lookup_ident(conn, ident, true)

static struct bt_l2cap_fixed_chan *le_channels;
#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
static struct bt_l2cap_server *servers;
#endif /* CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL */

/* Pool for outgoing LE signaling packets, MTU is 23 */
static struct nano_fifo le_sig;
static NET_BUF_POOL(le_sig_pool, CONFIG_BLUETOOTH_MAX_CONN,
		    BT_L2CAP_BUF_SIZE(L2CAP_LE_MIN_MTU), &le_sig, NULL,
		    BT_BUF_USER_DATA_MIN);

#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
/* Pool for outgoing LE data packets, MTU is 23 */
static struct nano_fifo le_data;
static NET_BUF_POOL(le_data_pool, CONFIG_BLUETOOTH_MAX_CONN,
		    BT_L2CAP_BUF_SIZE(L2CAP_LE_MIN_MTU), &le_data, NULL,
		    BT_BUF_USER_DATA_MIN);
#endif /* CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL */

#if defined(CONFIG_BLUETOOTH_BREDR)
static struct bt_l2cap_fixed_chan *br_channels;

/* Pool for outgoing BR/EDR signaling packets, min MTU is 48 */
static struct nano_fifo br_sig;
static NET_BUF_POOL(br_sig_pool, CONFIG_BLUETOOTH_MAX_CONN,
		    BT_L2CAP_BUF_SIZE(L2CAP_BR_MIN_MTU), &br_sig, NULL,
		    BT_BUF_USER_DATA_MIN);
#endif /* CONFIG_BLUETOOTH_BREDR */

/* L2CAP signalling channel specific context */
struct bt_l2cap {
	/* The channel this context is associated with */
	struct bt_l2cap_chan	chan;

	uint8_t			ident;
};

static struct bt_l2cap bt_l2cap_pool[CONFIG_BLUETOOTH_MAX_CONN];

static struct bt_l2cap *l2cap_chan_get(struct bt_conn *conn)
{
	struct bt_l2cap_chan *chan;

	chan = bt_l2cap_lookup_rx_cid(conn, BT_L2CAP_CID_LE_SIG);
	if (!chan) {
		BT_ERR("Unable to find L2CAP Signalling channel");
		return 0;
	}

	return CONTAINER_OF(chan, struct bt_l2cap, chan);
}

static uint8_t get_ident(struct bt_conn *conn)
{

	struct bt_l2cap *l2cap;

	l2cap = l2cap_chan_get(conn);
	if (!l2cap) {
		return 0;
	}

	l2cap->ident++;

	/* handle integer overflow (0 is not valid) */
	if (!l2cap->ident) {
		l2cap->ident++;
	}

	return l2cap->ident;
}

void bt_l2cap_le_fixed_chan_register(struct bt_l2cap_fixed_chan *chan)
{
	BT_DBG("CID 0x%04x", chan->cid);

	chan->_next = le_channels;
	le_channels = chan;
}

static void l2cap_chan_alloc_cid(struct bt_conn *conn,
				 struct bt_l2cap_chan *chan)
{
	uint16_t cid, cid_min, cid_max;

	/*
	 * No action needed if there's already a CID allocated, e.g. in
	 * the case of a fixed channel.
	 */
	if (chan->rx.cid > 0) {
		return;
	}

	cid_min = L2CAP_LE_DYN_CID_START;
	cid_max = L2CAP_LE_DYN_CID_END;

#if defined(CONFIG_BLUETOOTH_BREDR)
	if (conn->type == BT_CONN_TYPE_BR) {
		cid_min = L2CAP_BR_DYN_CID_START;
		cid_max = L2CAP_BR_DYN_CID_END;
	}
#endif /* CONFIG_BLUETOOTH_BREDR */

	for (cid = cid_min; cid <= cid_max; cid++) {
		if (!bt_l2cap_lookup_rx_cid(conn, cid)) {
			chan->rx.cid = cid;
			return;
		}
	}
}

static int l2cap_chan_add(struct bt_conn *conn, struct bt_l2cap_chan *chan)
{
	l2cap_chan_alloc_cid(conn, chan);

	if (!chan->rx.cid) {
		BT_ERR("Unable to allocate L2CAP CID");
		return -ENOMEM;
	}

	/* Attach channel to the connection */
	chan->_next = conn->channels;
	conn->channels = chan;
	chan->conn = conn;

	BT_DBG("conn %p chan %p cid 0x%04x", conn, chan, chan->rx.cid);

	return 0;
}

void bt_l2cap_connected(struct bt_conn *conn)
{
	struct bt_l2cap_fixed_chan *fchan;
	struct bt_l2cap_chan *chan;

	fchan = le_channels;

#if defined(CONFIG_BLUETOOTH_BREDR)
	if (conn->type == BT_CONN_TYPE_BR) {
		fchan = br_channels;
	}
#endif /* CONFIG_BLUETOOTH_BREDR */

	for (; fchan; fchan = fchan->_next) {
		if (fchan->accept(conn, &chan) < 0) {
			continue;
		}

		chan->rx.cid = fchan->cid;
		chan->tx.cid = fchan->cid;

		l2cap_chan_add(conn, chan);

		if (chan->ops->connected) {
			chan->ops->connected(chan);
		}
	}
}

void bt_l2cap_disconnected(struct bt_conn *conn)
{
	struct bt_l2cap_chan *chan;

	for (chan = conn->channels; chan;) {
		struct bt_l2cap_chan *next;

		/* prefetch since disconnected callback may cleanup */
		next = chan->_next;

		if (chan->ops->disconnected) {
			chan->ops->disconnected(chan);
		}

		chan->conn = NULL;
		chan = next;
	}

	conn->channels = NULL;
}

void bt_l2cap_encrypt_change(struct bt_conn *conn)
{
	struct bt_l2cap_chan *chan;

	for (chan = conn->channels; chan; chan = chan->_next) {
		if (chan->ops->encrypt_change) {
			chan->ops->encrypt_change(chan);
		}
	}
}

struct net_buf *bt_l2cap_create_pdu(struct nano_fifo *fifo)
{
	return bt_conn_create_pdu(fifo, sizeof(struct bt_l2cap_hdr));
}

void bt_l2cap_send(struct bt_conn *conn, uint16_t cid, struct net_buf *buf)
{
	struct bt_l2cap_hdr *hdr;

	hdr = net_buf_push(buf, sizeof(*hdr));
	hdr->len = sys_cpu_to_le16(buf->len - sizeof(*hdr));
	hdr->cid = sys_cpu_to_le16(cid);

	bt_conn_send(conn, buf);
}

static void l2cap_send_reject(struct bt_conn *conn, uint8_t ident,
			      uint16_t reason)
{
	struct bt_l2cap_cmd_reject *rej;
	struct bt_l2cap_sig_hdr *hdr;
	struct net_buf *buf;

	buf = bt_l2cap_create_pdu(&le_sig);
	if (!buf) {
		return;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_CMD_REJECT;
	hdr->ident = ident;
	hdr->len = sys_cpu_to_le16(sizeof(*rej));

	rej = net_buf_add(buf, sizeof(*rej));
	rej->reason = sys_cpu_to_le16(reason);

	bt_l2cap_send(conn, BT_L2CAP_CID_LE_SIG, buf);
}

static void le_conn_param_rsp(struct bt_l2cap *l2cap, struct net_buf *buf)
{
	struct bt_l2cap_conn_param_rsp *rsp = (void *)buf->data;

	if (buf->len < sizeof(*rsp)) {
		BT_ERR("Too small LE conn param rsp");
		return;
	}

	BT_DBG("LE conn param rsp result %u", sys_le16_to_cpu(rsp->result));
}

#if defined(CONFIG_BLUETOOTH_CENTRAL)
static void le_conn_param_update_req(struct bt_l2cap *l2cap, uint8_t ident,
				     struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.conn;
	const struct bt_le_conn_param *param;
	uint16_t min, max, latency, timeout;
	bool params_valid;
	struct bt_l2cap_sig_hdr *hdr;
	struct bt_l2cap_conn_param_rsp *rsp;
	struct bt_l2cap_conn_param_req *req = (void *)buf->data;

	if (buf->len < sizeof(*req)) {
		BT_ERR("Too small LE conn update param req");
		return;
	}

	if (conn->role != BT_HCI_ROLE_MASTER) {
		l2cap_send_reject(conn, ident, BT_L2CAP_REJ_NOT_UNDERSTOOD);
		return;
	}

	min = sys_le16_to_cpu(req->min_interval);
	max = sys_le16_to_cpu(req->max_interval);
	latency = sys_le16_to_cpu(req->latency);
	timeout = sys_le16_to_cpu(req->timeout);
	param = BT_LE_CONN_PARAM(min, max, latency, timeout);

	BT_DBG("min 0x%4.4x max 0x%4.4x latency: 0x%4.4x timeout: 0x%4.4x",
	       min, max, latency, timeout);

	buf = bt_l2cap_create_pdu(&le_sig);
	if (!buf) {
		return;
	}

	params_valid = bt_le_conn_params_valid(min, max, latency, timeout);

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_CONN_PARAM_RSP;
	hdr->ident = ident;
	hdr->len = sys_cpu_to_le16(sizeof(*rsp));

	rsp = net_buf_add(buf, sizeof(*rsp));
	if (params_valid) {
		rsp->result = sys_cpu_to_le16(BT_L2CAP_CONN_PARAM_ACCEPTED);
	} else {
		rsp->result = sys_cpu_to_le16(BT_L2CAP_CONN_PARAM_REJECTED);
	}

	bt_l2cap_send(l2cap->chan.conn, BT_L2CAP_CID_LE_SIG, buf);

	if (params_valid) {
		bt_conn_le_conn_update(conn, param);
	}
}
#endif /* CONFIG_BLUETOOTH_CENTRAL */

#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
static struct bt_l2cap_server *l2cap_server_lookup_psm(uint16_t psm)
{
	struct bt_l2cap_server *server;

	for (server = servers; server; server = server->_next) {
		if (server->psm == psm) {
			return server;
		}
	}

	return NULL;
}

int bt_l2cap_server_register(struct bt_l2cap_server *server)
{
	if (server->psm < L2CAP_LE_PSM_START ||
	    server->psm > L2CAP_LE_PSM_END || !server->accept) {
		return -EINVAL;
	}

	/* Check if given PSM is already in use */
	if (l2cap_server_lookup_psm(server->psm)) {
		BT_DBG("PSM already registered");
		return -EADDRINUSE;
	}

	BT_DBG("PSM 0x%04x", server->psm);

	server->_next = servers;
	servers = server;

	return 0;
}

static void l2cap_chan_rx_init(struct bt_l2cap_chan *chan)
{
	BT_DBG("chan %p", chan);

	/* Use existing MTU if defined */
	if (!chan->rx.mtu) {
		chan->rx.mtu = BT_L2CAP_MAX_LE_MTU;
	}

	chan->rx.mps = BT_L2CAP_MAX_LE_MPS;
	nano_sem_init(&chan->rx.credits);
}

static void l2cap_chan_tx_init(struct bt_l2cap_chan *chan)
{
	BT_DBG("chan %p", chan);

	memset(&chan->tx, 0, sizeof(chan->tx));
	nano_sem_init(&chan->tx.credits);
}

static void l2cap_chan_tx_give_credits(struct bt_l2cap_chan *chan,
				       uint16_t credits)
{
	BT_DBG("chan %p credits %u", chan, credits);

	while (credits--) {
		nano_sem_give(&chan->tx.credits);
	}
}

static void l2cap_chan_rx_give_credits(struct bt_l2cap_chan *chan,
				       uint16_t credits)
{
	BT_DBG("chan %p credits %u", chan, credits);

	while (credits--) {
		nano_sem_give(&chan->rx.credits);
	}
}

static void le_conn_req(struct bt_l2cap *l2cap, uint8_t ident,
			struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.conn;
	struct bt_l2cap_chan *chan;
	struct bt_l2cap_server *server;
	struct bt_l2cap_le_conn_req *req = (void *)buf->data;
	struct bt_l2cap_le_conn_rsp *rsp;
	struct bt_l2cap_sig_hdr *hdr;
	uint16_t psm, scid, mtu, mps, credits;

	if (buf->len < sizeof(*req)) {
		BT_ERR("Too small LE conn req packet size");
		return;
	}

	psm = sys_le16_to_cpu(req->psm);
	scid = sys_le16_to_cpu(req->scid);
	mtu = sys_le16_to_cpu(req->mtu);
	mps = sys_le16_to_cpu(req->mps);
	credits = sys_le16_to_cpu(req->credits);

	BT_DBG("psm 0x%02x scid 0x%04x mtu %u mps %u credits %u", psm, scid,
	       mtu, mps, credits);

	if (mtu < L2CAP_LE_MIN_MTU || mps < L2CAP_LE_MIN_MTU) {
		BT_ERR("Invalid LE-Conn Req params");
		return;
	}

	buf = bt_l2cap_create_pdu(&le_sig);
	if (!buf) {
		return;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_LE_CONN_RSP;
	hdr->ident = ident;
	hdr->len = sys_cpu_to_le16(sizeof(*rsp));

	rsp = net_buf_add(buf, sizeof(*rsp));
	memset(rsp, 0, sizeof(*rsp));

	/* Check if there is a server registered */
	server = l2cap_server_lookup_psm(psm);
	if (!server) {
		rsp->result = sys_cpu_to_le16(BT_L2CAP_ERR_PSM_NOT_SUPP);
		goto rsp;
	}

	/* TODO: Add security check */

	if (scid < L2CAP_LE_DYN_CID_START || scid > L2CAP_LE_DYN_CID_END) {
		rsp->result = sys_cpu_to_le16(BT_L2CAP_ERR_INVALID_SCID);
		goto rsp;
	}

	chan = bt_l2cap_lookup_tx_cid(conn, scid);
	if (chan) {
		rsp->result = sys_cpu_to_le16(BT_L2CAP_ERR_SCID_IN_USE);
		goto rsp;
	}

	/* Request server to accept the new connection and allocate the
	 * channel.
	 *
	 * TODO: Handle different errors, it may be required to respond async.
	 */
	if (server->accept(conn, &chan) < 0) {
		rsp->result = sys_cpu_to_le16(BT_L2CAP_ERR_NO_RESOURCES);
		goto rsp;
	}

	/* Init TX parameters */
	l2cap_chan_tx_init(chan);
	chan->tx.cid = scid;
	chan->tx.mps = mps;
	chan->tx.mtu = mtu;
	l2cap_chan_tx_give_credits(chan, credits);

	/* Init RX parameters */
	l2cap_chan_rx_init(chan);
	l2cap_chan_rx_give_credits(chan, L2CAP_LE_MAX_CREDITS);

	l2cap_chan_add(conn, chan);

	if (chan->ops->connected) {
		chan->ops->connected(chan);
	}

	rsp->dcid = sys_cpu_to_le16(chan->rx.cid);
	rsp->mps = sys_cpu_to_le16(chan->rx.mps);
	rsp->mtu = sys_cpu_to_le16(chan->rx.mtu);
	rsp->credits = sys_cpu_to_le16(L2CAP_LE_MAX_CREDITS);
	rsp->result = BT_L2CAP_SUCCESS;

rsp:
	bt_l2cap_send(conn, BT_L2CAP_CID_LE_SIG, buf);
}

static struct bt_l2cap_chan *l2cap_remove_tx_cid(struct bt_conn *conn,
						 uint16_t cid)
{
	struct bt_l2cap_chan *chan, *prev;

	for (chan = conn->channels, prev = NULL; chan;
	     prev = chan, chan = chan->_next) {
		if (chan->tx.cid != cid) {
			continue;
		}

		if (!prev) {
			conn->channels = chan->_next;
		} else {
			prev->_next = chan->_next;
		}

		return chan;
	}

	return NULL;
}

static void l2cap_chan_del(struct bt_l2cap_chan *chan)
{
	BT_DBG("conn %p chan %p cid 0x%04x", chan->conn, chan, chan->rx.cid);

	chan->conn = NULL;

	if (chan->ops->disconnected) {
		chan->ops->disconnected(chan);
	}

	/* There could be a writer waiting for credits so return a dummy credit
	 * to wake it up.
	 */
	if (!chan->tx.credits.nsig) {
		l2cap_chan_tx_give_credits(chan, 1);
	}

	/* Destroy segmented SDU if it exists */
	if (chan->_sdu) {
		net_buf_unref(chan->_sdu);
		chan->_sdu = NULL;
		chan->_sdu_len = 0;
	}
}

static void le_disconn_req(struct bt_l2cap *l2cap, uint8_t ident,
			   struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.conn;
	struct bt_l2cap_chan *chan;
	struct bt_l2cap_disconn_req *req = (void *)buf->data;
	struct bt_l2cap_disconn_rsp *rsp;
	struct bt_l2cap_sig_hdr *hdr;
	uint16_t scid, dcid;

	if (buf->len < sizeof(*req)) {
		BT_ERR("Too small LE conn req packet size");
		return;
	}

	dcid = sys_le16_to_cpu(req->dcid);
	scid = sys_le16_to_cpu(req->scid);

	BT_DBG("scid 0x%04x dcid 0x%04x", dcid, scid);

	chan = l2cap_remove_tx_cid(conn, scid);
	if (!chan) {
		l2cap_send_reject(conn, ident, BT_L2CAP_REJ_INVALID_CID);
		return;
	}

	buf = bt_l2cap_create_pdu(&le_sig);
	if (!buf) {
		return;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_DISCONN_RSP;
	hdr->ident = ident;
	hdr->len = sys_cpu_to_le16(sizeof(*rsp));

	rsp = net_buf_add(buf, sizeof(*rsp));
	rsp->dcid = sys_cpu_to_le16(chan->rx.cid);
	rsp->scid = sys_cpu_to_le16(chan->tx.cid);

	l2cap_chan_del(chan);

	bt_l2cap_send(conn, BT_L2CAP_CID_LE_SIG, buf);
}

static struct bt_l2cap_chan *__l2cap_lookup_ident(struct bt_conn *conn,
						  uint16_t ident, bool remove)
{
	struct bt_l2cap_chan *chan, *prev;

	for (chan = conn->channels, prev = NULL; chan;
	     prev = chan, chan = chan->_next) {
		if (chan->_ident != ident) {
			continue;
		}

		if (!remove) {
			return chan;
		}

		if (!prev) {
			conn->channels = chan->_next;
		} else {
			prev->_next = chan->_next;
		}

		return chan;
	}

	return NULL;
}

static void le_conn_rsp(struct bt_l2cap *l2cap, uint8_t ident,
			struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.conn;
	struct bt_l2cap_chan *chan;
	struct bt_l2cap_le_conn_rsp *rsp = (void *)buf->data;
	uint16_t dcid, mtu, mps, credits, result;

	if (buf->len < sizeof(*rsp)) {
		BT_ERR("Too small LE conn rsp packet size");
		return;
	}

	dcid = sys_le16_to_cpu(rsp->dcid);
	mtu = sys_le16_to_cpu(rsp->mtu);
	mps = sys_le16_to_cpu(rsp->mps);
	credits = sys_le16_to_cpu(rsp->credits);
	result = sys_le16_to_cpu(rsp->result);

	BT_DBG("dcid 0x%04x mtu %u mps %u credits %u result 0x%04x", dcid,
	       mtu, mps, credits, result);

	if (result == BT_L2CAP_SUCCESS) {
		chan = l2cap_lookup_ident(conn, ident);
	} else {
		chan = l2cap_remove_ident(conn, ident);
	}

	if (!chan) {
		BT_ERR("Cannot find channel for ident %u", ident);
		return;
	}

	switch (result) {
	case BT_L2CAP_SUCCESS:
		/* Reset _ident since it is no longer pending */
		chan->_ident = 0;
		chan->tx.cid = dcid;
		chan->tx.mtu = mtu;
		chan->tx.mps = mps;

		if (chan->ops->connected) {
			chan->ops->connected(chan);
		}

		/* Give credits */
		l2cap_chan_tx_give_credits(chan, credits);
		l2cap_chan_rx_give_credits(chan, L2CAP_LE_MAX_CREDITS);

		break;
	/* TODO: Retry on Authentication and Encryption errors */
	default:
		l2cap_chan_del(chan);
	}
}

static void le_disconn_rsp(struct bt_l2cap *l2cap, uint8_t ident,
			   struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.conn;
	struct bt_l2cap_chan *chan;
	struct bt_l2cap_disconn_rsp *rsp = (void *)buf->data;
	uint16_t dcid, scid;

	if (buf->len < sizeof(*rsp)) {
		BT_ERR("Too small LE disconn rsp packet size");
		return;
	}

	dcid = sys_le16_to_cpu(rsp->dcid);
	scid = sys_le16_to_cpu(rsp->scid);

	BT_DBG("dcid 0x%04x scid 0x%04x", dcid, scid);

	chan = l2cap_remove_tx_cid(conn, dcid);
	if (!chan) {
		return;
	}

	l2cap_chan_del(chan);
}

static void le_credits(struct bt_l2cap *l2cap, uint8_t ident,
		       struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.conn;
	struct bt_l2cap_chan *chan;
	struct bt_l2cap_le_credits *ev = (void *)buf->data;
	uint16_t credits, cid;

	if (buf->len < sizeof(*ev)) {
		BT_ERR("Too small LE Credits packet size");
		return;
	}

	cid = sys_le16_to_cpu(ev->cid);
	credits = sys_le16_to_cpu(ev->credits);

	BT_DBG("cid 0x%04x credits %u", cid, credits);

	chan = bt_l2cap_lookup_tx_cid(conn, cid);
	if (!chan) {
		BT_ERR("Unable to find channel of LE Credits packet");
		return;
	}

	if (chan->tx.credits.nsig + credits > UINT16_MAX) {
		BT_ERR("Credits overflow");
		bt_l2cap_chan_disconnect(chan);
		return;
	}

	l2cap_chan_tx_give_credits(chan, credits);

	BT_DBG("chan %p total credits %u", chan, chan->tx.credits.nsig);
}

static void reject_cmd(struct bt_l2cap *l2cap, uint8_t ident,
		       struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.conn;
	struct bt_l2cap_chan *chan;

	/* Check if there is a outstanding channel */
	chan = l2cap_remove_ident(conn, ident);
	if (!chan) {
		return;
	}

	l2cap_chan_del(chan);
}
#endif /* CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL */

static void l2cap_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	struct bt_l2cap *l2cap = CONTAINER_OF(chan, struct bt_l2cap, chan);
	struct bt_l2cap_sig_hdr *hdr = (void *)buf->data;
	uint16_t len;

	if (buf->len < sizeof(*hdr)) {
		BT_ERR("Too small L2CAP signaling PDU");
		return;
	}

	len = sys_le16_to_cpu(hdr->len);
	net_buf_pull(buf, sizeof(*hdr));

	BT_DBG("Signaling code 0x%02x ident %u len %u", hdr->code,
	       hdr->ident, len);

	if (buf->len != len) {
		BT_ERR("L2CAP length mismatch (%u != %u)", buf->len, len);
		return;
	}

	if (!hdr->ident) {
		BT_ERR("Invalid ident value in L2CAP PDU");
		return;
	}

	switch (hdr->code) {
	case BT_L2CAP_CONN_PARAM_RSP:
		le_conn_param_rsp(l2cap, buf);
		break;
#if defined(CONFIG_BLUETOOTH_CENTRAL)
	case BT_L2CAP_CONN_PARAM_REQ:
		le_conn_param_update_req(l2cap, hdr->ident, buf);
		break;
#endif /* CONFIG_BLUETOOTH_CENTRAL */
#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
	case BT_L2CAP_LE_CONN_REQ:
		le_conn_req(l2cap, hdr->ident, buf);
		break;
	case BT_L2CAP_LE_CONN_RSP:
		le_conn_rsp(l2cap, hdr->ident, buf);
		break;
	case BT_L2CAP_DISCONN_REQ:
		le_disconn_req(l2cap, hdr->ident, buf);
		break;
	case BT_L2CAP_DISCONN_RSP:
		le_disconn_rsp(l2cap, hdr->ident, buf);
		break;
	case BT_L2CAP_LE_CREDITS:
		le_credits(l2cap, hdr->ident, buf);
		break;
	case BT_L2CAP_CMD_REJECT:
		reject_cmd(l2cap, hdr->ident, buf);
		break;
#else
	case BT_L2CAP_CMD_REJECT:
		/* Ignored */
		break;
#endif /* CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL */
	default:
		BT_WARN("Unknown L2CAP PDU code 0x%02x", hdr->code);
		l2cap_send_reject(chan->conn, hdr->ident,
				  BT_L2CAP_REJ_NOT_UNDERSTOOD);
		break;
	}
}

#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
static void l2cap_chan_update_credits(struct bt_l2cap_chan *chan)
{
	struct net_buf *buf;
	struct bt_l2cap_sig_hdr *hdr;
	struct bt_l2cap_le_credits *ev;
	uint16_t credits;

	/* Only give more credits if it went bellow the defined threshold */
	if (chan->rx.credits.nsig > L2CAP_LE_CREDITS_THRESHOLD) {
		goto done;
	}

	/* Restore credits */
	credits = L2CAP_LE_MAX_CREDITS - chan->rx.credits.nsig;
	l2cap_chan_rx_give_credits(chan, credits);

	buf = bt_l2cap_create_pdu(&le_sig);
	if (!buf) {
		BT_ERR("Unable to send credits");
		return;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_LE_CREDITS;
	hdr->ident = get_ident(chan->conn);
	hdr->len = sys_cpu_to_le16(sizeof(*ev));

	ev = net_buf_add(buf, sizeof(*ev));
	ev->cid = sys_cpu_to_le16(chan->rx.cid);
	ev->credits = sys_cpu_to_le16(credits);

	bt_l2cap_send(chan->conn, BT_L2CAP_CID_LE_SIG, buf);

done:
	BT_DBG("chan %p credits %u", chan, chan->rx.credits.nsig);
}

static void l2cap_chan_le_recv_sdu(struct bt_l2cap_chan *chan,
				   struct net_buf *buf)
{
	BT_DBG("chan %p len %u sdu len %u", chan, buf->len, chan->_sdu->len);

	if (chan->_sdu->len + buf->len > chan->_sdu_len) {
		BT_ERR("SDU length mismatch");
		bt_l2cap_chan_disconnect(chan);
		return;
	}

	memcpy(net_buf_add(chan->_sdu, buf->len), buf->data, buf->len);

	if (chan->_sdu->len == chan->_sdu_len) {
		/* Receiving complete SDU, notify channel and reset SDU buf */
		chan->ops->recv(chan, chan->_sdu);
		net_buf_unref(chan->_sdu);
		chan->_sdu = NULL;
		chan->_sdu_len = 0;
	}

	l2cap_chan_update_credits(chan);
}

static void l2cap_chan_le_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	uint16_t sdu_len;

	if (!nano_fiber_sem_take(&chan->rx.credits, TICKS_NONE)) {
		BT_ERR("No credits to receive packet");
		bt_l2cap_chan_disconnect(chan);
		return;
	}

	/* Check if segments already exist */
	if (chan->_sdu) {
		l2cap_chan_le_recv_sdu(chan, buf);
		return;
	}

	sdu_len = net_buf_pull_le16(buf);

	BT_DBG("chan %p len %u sdu_len %u", chan, buf->len, sdu_len);

	if (sdu_len > chan->rx.mtu) {
		BT_ERR("Invalid SDU length");
		bt_l2cap_chan_disconnect(chan);
		return;
	}

	/* Always allocate buffer from the channel if supported. */
	if (chan->ops->alloc_buf) {
		chan->_sdu = chan->ops->alloc_buf(chan);
		if (!chan->_sdu) {
			BT_ERR("Unable to allocate buffer for SDU");
			bt_l2cap_chan_disconnect(chan);
			return;
		}
		chan->_sdu_len = sdu_len;
		l2cap_chan_le_recv_sdu(chan, buf);
		return;
	}

	chan->ops->recv(chan, buf);

	l2cap_chan_update_credits(chan);
}
#endif /* CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL */

static void l2cap_chan_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
	/* TODO: Check the conn type to differentiate BR/EDR and LE or
	 * introduce a mode.
	 */
	if (chan->rx.cid >= L2CAP_LE_DYN_CID_START &&
	    chan->rx.cid <= L2CAP_LE_DYN_CID_END) {
		l2cap_chan_le_recv(chan, buf);
		return;
	}
#endif /* CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL */

	BT_DBG("chan %p len %u", chan, buf->len);

	chan->ops->recv(chan, buf);
}

void bt_l2cap_recv(struct bt_conn *conn, struct net_buf *buf)
{
	struct bt_l2cap_hdr *hdr = (void *)buf->data;
	struct bt_l2cap_chan *chan;
	uint16_t cid;

	if (buf->len < sizeof(*hdr)) {
		BT_ERR("Too small L2CAP PDU received");
		net_buf_unref(buf);
		return;
	}

	cid = sys_le16_to_cpu(hdr->cid);
	net_buf_pull(buf, sizeof(*hdr));

	BT_DBG("Packet for CID %u len %u", cid, buf->len);

	chan = bt_l2cap_lookup_rx_cid(conn, cid);
	if (!chan) {
		BT_WARN("Ignoring data for unknown CID 0x%04x", cid);
		net_buf_unref(buf);
		return;
	}

	l2cap_chan_recv(chan, buf);

	net_buf_unref(buf);
}

int bt_l2cap_update_conn_param(struct bt_conn *conn,
			       const struct bt_le_conn_param *param)
{
	struct bt_l2cap_sig_hdr *hdr;
	struct bt_l2cap_conn_param_req *req;
	struct net_buf *buf;

	buf = bt_l2cap_create_pdu(&le_sig);
	if (!buf) {
		return -ENOBUFS;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_CONN_PARAM_REQ;
	hdr->ident = get_ident(conn);
	hdr->len = sys_cpu_to_le16(sizeof(*req));

	req = net_buf_add(buf, sizeof(*req));
	req->min_interval = sys_cpu_to_le16(param->interval_min);
	req->max_interval = sys_cpu_to_le16(param->interval_max);
	req->latency = sys_cpu_to_le16(param->latency);
	req->timeout = sys_cpu_to_le16(param->timeout);

	bt_l2cap_send(conn, BT_L2CAP_CID_LE_SIG, buf);

	return 0;
}

static void l2cap_connected(struct bt_l2cap_chan *chan)
{
	BT_DBG("chan %p cid 0x%04x", chan, chan->rx.cid);
}

static void l2cap_disconnected(struct bt_l2cap_chan *chan)
{
	BT_DBG("chan %p cid 0x%04x", chan, chan->rx.cid);
}

static int l2cap_accept(struct bt_conn *conn, struct bt_l2cap_chan **chan)
{
	int i;
	static struct bt_l2cap_chan_ops ops = {
		.connected = l2cap_connected,
		.disconnected = l2cap_disconnected,
		.recv = l2cap_recv,
	};

	BT_DBG("conn %p handle %u", conn, conn->handle);

	for (i = 0; i < ARRAY_SIZE(bt_l2cap_pool); i++) {
		struct bt_l2cap *l2cap = &bt_l2cap_pool[i];

		if (l2cap->chan.conn) {
			continue;
		}

		l2cap->chan.ops = &ops;

		*chan = &l2cap->chan;

		return 0;
	}

	BT_ERR("No available L2CAP context for conn %p", conn);

	return -ENOMEM;
}

void bt_l2cap_init(void)
{
	static struct bt_l2cap_fixed_chan chan = {
		.cid	= BT_L2CAP_CID_LE_SIG,
		.accept	= l2cap_accept,
	};
#if defined(CONFIG_BLUETOOTH_BREDR)
	static struct bt_l2cap_fixed_chan chan_br = {
		.cid	= BT_L2CAP_CID_BR_SIG,
		.mask	= BT_L2CAP_MASK_BR_SIG,
		.accept = l2cap_accept,
	};

	net_buf_pool_init(br_sig_pool);
	bt_l2cap_br_fixed_chan_register(&chan_br);
#endif /* CONFIG_BLUETOOTH_BREDR */

	net_buf_pool_init(le_sig_pool);
#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
	net_buf_pool_init(le_data_pool);
#endif /* CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL */

	bt_l2cap_le_fixed_chan_register(&chan);
}

struct bt_l2cap_chan *bt_l2cap_lookup_tx_cid(struct bt_conn *conn,
					     uint16_t cid)
{
	struct bt_l2cap_chan *chan;

	for (chan = conn->channels; chan; chan = chan->_next) {
		if (chan->tx.cid == cid)
			return chan;
	}

	return NULL;
}

struct bt_l2cap_chan *bt_l2cap_lookup_rx_cid(struct bt_conn *conn,
					     uint16_t cid)
{
	struct bt_l2cap_chan *chan;

	for (chan = conn->channels; chan; chan = chan->_next) {
		if (chan->rx.cid == cid) {
			return chan;
		}
	}

	return NULL;
}

#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
static int l2cap_le_connect(struct bt_conn *conn, struct bt_l2cap_chan *chan,
			    uint16_t psm)
{
	struct net_buf *buf;
	struct bt_l2cap_sig_hdr *hdr;
	struct bt_l2cap_le_conn_req *req;

	if (psm < L2CAP_LE_PSM_START || psm > L2CAP_LE_PSM_END) {
		return -EINVAL;
	}

	l2cap_chan_tx_init(chan);
	l2cap_chan_rx_init(chan);

	if (l2cap_chan_add(conn, chan) < 0) {
		return -ENOMEM;
	}

	buf = bt_l2cap_create_pdu(&le_sig);
	if (!buf) {
		BT_ERR("Unable to send L2CP connection request");
		return -ENOMEM;
	}

	chan->_ident = get_ident(chan->conn);

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_LE_CONN_REQ;
	hdr->ident = chan->_ident;
	hdr->len = sys_cpu_to_le16(sizeof(*req));

	req = net_buf_add(buf, sizeof(*req));
	req->psm = sys_cpu_to_le16(psm);
	req->scid = sys_cpu_to_le16(chan->rx.cid);
	req->mtu = sys_cpu_to_le16(chan->rx.mtu);
	req->mps = sys_cpu_to_le16(chan->rx.mps);
	req->credits = sys_cpu_to_le16(L2CAP_LE_MAX_CREDITS);

	bt_l2cap_send(chan->conn, BT_L2CAP_CID_LE_SIG, buf);

	return 0;
}

int bt_l2cap_chan_connect(struct bt_conn *conn, struct bt_l2cap_chan *chan,
			  uint16_t psm)
{
	BT_DBG("conn %p chan %p psm 0x%04x", conn, chan, psm);

	if (!conn || conn->state != BT_CONN_CONNECTED) {
		return -ENOTCONN;
	}

	if (!chan) {
		return -EINVAL;
	}

	/* TODO: Check conn/address type when BR/EDR is introduced */
	return l2cap_le_connect(conn, chan, psm);
}

int bt_l2cap_chan_disconnect(struct bt_l2cap_chan *chan)
{
	struct bt_conn *conn = chan->conn;
	struct net_buf *buf;
	struct bt_l2cap_disconn_req *req;
	struct bt_l2cap_sig_hdr *hdr;

	BT_DBG("chan %p scid 0x%04x dcid 0x%04x", chan, chan->rx.cid,
	       chan->tx.cid);

	if (!conn) {
		return -ENOTCONN;
	}

	buf = bt_l2cap_create_pdu(&le_sig);
	if (!buf) {
		BT_ERR("Unable to send L2CP disconnect request");
		return -ENOMEM;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_DISCONN_REQ;
	hdr->ident = get_ident(conn);
	hdr->len = sys_cpu_to_le16(sizeof(*req));

	req = net_buf_add(buf, sizeof(*req));
	req->dcid = sys_cpu_to_le16(chan->tx.cid);
	req->scid = sys_cpu_to_le16(chan->rx.cid);

	bt_l2cap_send(conn, BT_L2CAP_CID_LE_SIG, buf);

	return 0;
}

static struct net_buf *l2cap_chan_create_seg(struct bt_l2cap_chan *chan,
					     struct net_buf *buf,
					     size_t sdu_hdr_len)
{
	struct net_buf *seg;
	uint16_t headroom;
	uint16_t len;

	/* Segment if data (+ data headroom) is bigger than MPS */
	if (buf->len + sdu_hdr_len > chan->tx.mps) {
		goto segment;
	}

	headroom = sizeof(struct bt_hci_acl_hdr) +
		   sizeof(struct bt_l2cap_hdr) + sdu_hdr_len;

	/* Check if original buffer has enough headroom */
	if (net_buf_headroom(buf) >= headroom) {
		if (sdu_hdr_len) {
			/* Push SDU length if set */
			net_buf_push_le16(buf, buf->len);
		}
		return net_buf_ref(buf);
	}

segment:
	seg = bt_l2cap_create_pdu(&le_data);
	if (!seg) {
		return NULL;
	}

	if (sdu_hdr_len) {
		net_buf_add_le16(seg, buf->len);
	}

	len = min(min(buf->len, L2CAP_LE_MIN_MTU - sdu_hdr_len), chan->tx.mps);
	memcpy(net_buf_add(seg, len), buf->data, len);
	net_buf_pull(buf, len);

	BT_DBG("chan %p seg %p len %u", chan, seg, seg->len);

	return seg;
}

static int l2cap_chan_le_send(struct bt_l2cap_chan *chan, struct net_buf *buf,
			      uint16_t sdu_hdr_len)
{
	int len;

	/* Wait for credits */
	nano_sem_take(&chan->tx.credits, TICKS_UNLIMITED);

	buf = l2cap_chan_create_seg(chan, buf, sdu_hdr_len);
	if (!buf) {
		return -ENOMEM;
	}

	/* Channel may have been disconnected while waiting for credits */
	if (!chan->conn) {
		net_buf_unref(buf);
		return -ECONNRESET;
	}

	BT_DBG("chan %p cid 0x%04x len %u credits %u", chan, chan->tx.cid,
	       buf->len, chan->tx.credits.nsig);

	len = buf->len;

	bt_l2cap_send(chan->conn, chan->tx.cid, buf);

	return len;
}

static int l2cap_chan_le_send_sdu(struct bt_l2cap_chan *chan,
				  struct net_buf *buf)
{
	int ret, sent, total_len;

	if (buf->len > chan->tx.mtu) {
		return -EMSGSIZE;
	}

	total_len = buf->len;

	/* Add SDU length for the first segment */
	ret = l2cap_chan_le_send(chan, buf, BT_L2CAP_SDU_HDR_LEN);
	if (ret < 0) {
		return ret;
	}

	/* Send remaining segments */
	for (sent = ret; sent < total_len; sent += ret) {
		ret = l2cap_chan_le_send(chan, buf, 0);
		if (ret < 0) {
			return ret;
		}
	}

	BT_DBG("chan %p cid 0x%04x sent %u", chan, chan->tx.cid, sent);

	net_buf_unref(buf);

	return sent;
}

int bt_l2cap_chan_send(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	int err;

	if (!buf) {
		return -EINVAL;
	}

	BT_DBG("chan %p buf %p len %u", chan, buf, buf->len);

	if (!chan->conn || chan->conn->state != BT_CONN_CONNECTED) {
		return -ENOTCONN;
	}

	/* TODO: Check conn/address type when BR/EDR is introduced */
	err = l2cap_chan_le_send_sdu(chan, buf);
	if (err < 0) {
		BT_ERR("failed to send message %d", err);
	}

	return err;
}
#endif /* CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL */

#if defined(CONFIG_BLUETOOTH_BREDR)
void bt_l2cap_br_fixed_chan_register(struct bt_l2cap_fixed_chan *chan)
{
	BT_DBG("CID 0x%04x", chan->cid);

	chan->_next = br_channels;
	br_channels = chan;
}
#endif /* CONFIG_BLUETOOTH_BREDR */
