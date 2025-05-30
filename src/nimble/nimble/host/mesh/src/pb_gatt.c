/*  Bluetooth Mesh */

/*
 * Copyright (c) 2017 Intel Corporation
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define MESH_LOG_MODULE BLE_MESH_PROV_LOG

#include "nimble/nimble/host/mesh/include/mesh/mesh.h"
#include "prov.h"
#include "net.h"
#include "proxy.h"
#include "adv.h"
#include "nimble/porting/nimble/include/syscfg/syscfg.h"
#include "pb_gatt_srv.h"

#if MYNEWT_VAL(BLE_MESH_PB_GATT)
struct prov_bearer_send_cb {
	prov_bearer_send_complete_t cb;
	void *cb_data;
};

struct prov_link {
	uint16_t conn_handle;
	const struct prov_bearer_cb *cb;
	void *cb_data;
	struct prov_bearer_send_cb comp;
	struct {
		uint8_t  id;        /* Transaction ID */
		uint8_t  prev_id;   /* Previous Transaction ID */
		uint8_t  seg;       /* Bit-field of unreceived segments */
		uint8_t  last_seg;  /* Last segment (to check length) */
		uint8_t  fcs;       /* Expected FCS value */
	} rx;
	struct k_work_delayable prot_timer;
};

static struct prov_link link;

static void reset_state(void)
{
	link.conn_handle = BLE_HS_CONN_HANDLE_NONE;

	/* If this fails, the protocol timeout handler will exit early. */
	(void)k_work_cancel_delayable(&link.prot_timer);
}

static void link_closed(enum prov_bearer_link_status status)
{
	const struct prov_bearer_cb *cb = link.cb;

	void *cb_data = link.cb_data;

	reset_state();

	cb->link_closed(&pb_gatt, cb_data, status);
}

static void protocol_timeout(struct ble_npl_event *work)
{
	if (!link.conn_handle) {
		/* Already disconnected */
		return;
	}

	BT_DBG("Protocol timeout");

	link_closed(PROV_BEARER_LINK_STATUS_TIMEOUT);
}

int bt_mesh_pb_gatt_recv(uint16_t conn_handle, struct os_mbuf *buf)
{
	BT_DBG("%u bytes: %s", buf->om_len, bt_hex(buf->om_data, buf->om_len));

	if (link.conn_handle != conn_handle || !link.cb) {
		BT_WARN("Data for unexpected connection");
		return -ENOTCONN;
	}

	if (buf->om_len < 1) {
		BT_WARN("Too short provisioning packet (len %u)", buf->om_len);
		return -EINVAL;
	}

	k_work_reschedule(&link.prot_timer, PROTOCOL_TIMEOUT);

	link.cb->recv(&pb_gatt, link.cb_data, buf);

	return 0;
}

int bt_mesh_pb_gatt_open(uint16_t conn_handle)
{
	BT_DBG("conn %p", conn_handle);

	if (link.conn_handle) {
		return -EBUSY;
	}

	link.conn_handle = conn_handle;
	k_work_reschedule(&link.prot_timer, PROTOCOL_TIMEOUT);

	link.cb->link_opened(&pb_gatt, link.cb_data);

	return 0;
}

int bt_mesh_pb_gatt_close(uint16_t conn_handle)
{
	BT_DBG("conn %p", conn_handle);

	if (link.conn_handle != conn_handle) {
		BT_DBG("Not connected");
		return -ENOTCONN;
	}

	link.cb->link_closed(&pb_gatt, link.cb_data,
			     PROV_BEARER_LINK_STATUS_SUCCESS);

	reset_state();

	return 0;
}

static int link_accept(const struct prov_bearer_cb *cb, void *cb_data)
{
	(void)bt_mesh_pb_gatt_enable();
	bt_mesh_adv_update();

	link.cb = cb;
	link.cb_data = cb_data;

	return 0;
}

static int buf_send(struct os_mbuf *buf, prov_bearer_send_complete_t cb,
		    void *cb_data)
{
	if (!link.conn_handle) {
		return -ENOTCONN;
	}

	link.comp.cb = cb;
	link.comp.cb_data = cb_data;

	k_work_reschedule(&link.prot_timer, PROTOCOL_TIMEOUT);

	return bt_mesh_pb_gatt_send(link.conn_handle, buf);
}

static void clear_tx(void)
{
	/* No action */
}

void pb_gatt_init(void)
{
	k_work_init_delayable(&link.prot_timer, protocol_timeout);
}

void pb_gatt_reset(void)
{
	reset_state();
}
const struct prov_bearer pb_gatt = {
	.type = BT_MESH_PROV_GATT,
	.link_accept = link_accept,
	.send = buf_send,
	.clear_tx = clear_tx,
};
#endif
