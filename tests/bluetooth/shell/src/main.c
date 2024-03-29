/** @file
 *  @brief Interactive Bluetooth LE shell application
 *
 *  Application allows implement Bluetooth LE functional commands performing
 *  simple diagnostic interaction between LE host stack and LE controller
 */

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

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <misc/printk.h>
#include <misc/byteorder.h>

#include <console/uart_console.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/gatt.h>
#include <bluetooth/l2cap.h>

#include <misc/shell.h>

#include <gatt/gap.h>

#define DEVICE_NAME		"test shell"
#define DEVICE_NAME_LEN		(sizeof(DEVICE_NAME) - 1)
#define AD_SHORT_NAME		0x08
#define AD_COMPLETE_NAME	0x09
#define CREDITS			10
#define DATA_MTU		(23 * CREDITS)

static struct bt_conn *default_conn;

/* Connection context for BR/EDR legacy pairing in sec mode 3 */
static struct bt_conn *pairing_conn;

static struct nano_fifo data_fifo;
static NET_BUF_POOL(data_pool, 1, DATA_MTU, &data_fifo, NULL, 0);

static const char *current_prompt(void)
{
	static char str[BT_ADDR_LE_STR_LEN + 2];
	static struct bt_conn_info info;

	if (!default_conn) {
		return NULL;
	}

	if (bt_conn_get_info(default_conn, &info) < 0) {
		return NULL;
	}

	if (info.type != BT_CONN_TYPE_LE) {
		return NULL;
	}

	bt_addr_le_to_str(info.le.dst, str, sizeof(str) - 2);
	strcat(str, "> ");
	return str;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t evtype,
			 const uint8_t *ad, uint8_t len)
{
	char le_addr[BT_ADDR_LE_STR_LEN];
	char name[30];

	memset(name, 0, sizeof(name));

	while (len) {
		if (len < 2) {
			break;
		};

		/* Look for early termination */
		if (!ad[0]) {
			break;
		}

		/* Check if field length is correct */
		if (ad[0] > len - 1) {
			break;
		}

		switch (ad[1]) {
		case AD_SHORT_NAME:
		case AD_COMPLETE_NAME:
			if (ad[0] > sizeof(name) - 1) {
				memcpy(name, &ad[2], sizeof(name) - 1);
			} else {
				memcpy(name, &ad[2], ad[0] - 1);
			}
			break;
		default:
			break;
		}

		/* Parse next AD Structure */
		len -= ad[0] + 1;
		ad += ad[0] + 1;
	}

	bt_addr_le_to_str(addr, le_addr, sizeof(le_addr));
	printk("[DEVICE]: %s, AD evt type %u, RSSI %i %s\n", le_addr, evtype,
	       rssi, name);
}

static void conn_addr_str(struct bt_conn *conn, char *addr, size_t len)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) < 0) {
		addr[0] = '\0';
		return;
	}

	switch (info.type) {
#if defined(CONFIG_BLUETOOTH_BREDR)
	case BT_CONN_TYPE_BR:
		bt_addr_to_str(info.br.dst, addr, len);
		break;
#endif
	case BT_CONN_TYPE_LE:
		bt_addr_le_to_str(info.le.dst, addr, len);
		break;
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	conn_addr_str(conn, addr, sizeof(addr));

	if (err) {
		printk("Failed to connect to %s (%u)\n", addr, err);
		goto done;
	}

	printk("Connected: %s\n", addr);

	if (!default_conn) {
		default_conn = bt_conn_ref(conn);
	}

done:
	/* clear connection reference for sec mode 3 pairing */
	if (pairing_conn) {
		bt_conn_unref(pairing_conn);
		pairing_conn = NULL;
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	conn_addr_str(conn, addr, sizeof(addr));
	printk("Disconnected: %s (reason %u)\n", addr, reason);

	if (default_conn == conn) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
	}
}

static void identity_resolved(struct bt_conn *conn, const bt_addr_le_t *rpa,
			      const bt_addr_le_t *identity)
{
	char addr_identity[BT_ADDR_LE_STR_LEN];
	char addr_rpa[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(identity, addr_identity, sizeof(addr_identity));
	bt_addr_le_to_str(rpa, addr_rpa, sizeof(addr_rpa));

	printk("Identity resolved %s -> %s\n", addr_rpa, addr_identity);
}

static void security_changed(struct bt_conn *conn, bt_security_t level)
{
	char addr[BT_ADDR_LE_STR_LEN];

	conn_addr_str(conn, addr, sizeof(addr));
	printk("Security changed: %s level %u\n", addr, level);
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
	.identity_resolved = identity_resolved,
	.security_changed = security_changed,
};

static uint16_t appearance_value = 0x0001;

static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	gap_init(DEVICE_NAME, appearance_value);
}

static void cmd_init(int argc, char *argv[])
{
	int err;

	err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
	}
}

static int char2hex(const char *c, uint8_t *x)
{
	if (*c >= '0' && *c <= '9') {
		*x = *c - '0';
	} else if (*c >= 'a' && *c <= 'f') {
		*x = *c - 'a' + 10;
	} else if (*c >= 'A' && *c <= 'F') {
		*x = *c - 'A' + 10;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int str2bt_addr_le(const char *str, const char *type, bt_addr_le_t *addr)
{
	int i, j;
	uint8_t tmp;

	if (strlen(str) != 17) {
		return -EINVAL;
	}

	for (i = 5, j = 1; *str != '\0'; str++, j++) {
		if (!(j % 3) && (*str != ':')) {
			return -EINVAL;
		} else if (*str == ':') {
			i--;
			continue;
		}

		addr->a.val[i] = addr->a.val[i] << 4;

		if (char2hex(str, &tmp) < 0) {
			return -EINVAL;
		}

		addr->a.val[i] |= tmp;
	}

	if (!strcmp(type, "public") || !strcmp(type, "(public)")) {
		addr->type = BT_ADDR_LE_PUBLIC;
	} else if (!strcmp(type, "random") || !strcmp(type, "(random)")) {
		addr->type = BT_ADDR_LE_RANDOM;
	} else {
		return -EINVAL;
	}

	return 0;
}

static void cmd_connect_le(int argc, char *argv[])
{
	int err;
	bt_addr_le_t addr;
	struct bt_conn *conn;

	if (argc < 2) {
		printk("Peer address required\n");
		return;
	}

	if (argc < 3) {
		printk("Peer address type required\n");
		return;
	}

	err = str2bt_addr_le(argv[1], argv[2], &addr);
	if (err) {
		printk("Invalid peer address (err %d)\n", err);
		return;
	}

	conn = bt_conn_create_le(&addr, BT_LE_CONN_PARAM_DEFAULT);

	if (!conn) {
		printk("Connection failed\n");
	} else {

		printk("Connection pending\n");

		/* unref connection obj in advance as app user */
		bt_conn_unref(conn);
	}
}

static void cmd_disconnect(int argc, char *argv[])
{
	struct bt_conn *conn;
	int err;

	if (default_conn && argc < 3) {
		conn = bt_conn_ref(default_conn);
	} else {
		bt_addr_le_t addr;

		if (argc < 2) {
			printk("Peer address required\n");
			return;
		}

		if (argc < 3) {
			printk("Peer address type required\n");
			return;
		}

		err = str2bt_addr_le(argv[1], argv[2], &addr);
		if (err) {
			printk("Invalid peer address (err %d)\n", err);
			return;
		}

		conn = bt_conn_lookup_addr_le(&addr);
	}

	if (!conn) {
		printk("Not connected\n");
		return;
	}

	err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	if (err) {
		printk("Disconnection failed (err %d)\n", err);
	}

	bt_conn_unref(conn);
}

static void cmd_auto_conn(int argc, char *argv[])
{
	bt_addr_le_t addr;
	int err;

	if (argc < 2) {
		printk("Peer address required\n");
		return;
	}

	if (argc < 3) {
		printk("Peer address type required\n");
		return;
	}

	err = str2bt_addr_le(argv[1], argv[2], &addr);
	if (err) {
		printk("Invalid peer address (err %d)\n", err);
		return;
	}

	if (argc < 4) {
		bt_le_set_auto_conn(&addr, BT_LE_CONN_PARAM_DEFAULT);
	} else if (!strcmp(argv[3], "on")) {
		bt_le_set_auto_conn(&addr, BT_LE_CONN_PARAM_DEFAULT);
	} else if (!strcmp(argv[3], "off")) {
		bt_le_set_auto_conn(&addr, NULL);
	} else {
		printk("Specify \"on\" or \"off\"\n");
	}
}

static void cmd_select(int argc, char *argv[])
{
	struct bt_conn *conn;
	bt_addr_le_t addr;
	int err;

	if (argc < 2) {
		printk("Peer address required\n");
		return;
	}

	if (argc < 3) {
		printk("Peer address type required\n");
		return;
	}

	err = str2bt_addr_le(argv[1], argv[2], &addr);
	if (err) {
		printk("Invalid peer address (err %d)\n", err);
		return;
	}

	conn = bt_conn_lookup_addr_le(&addr);
	if (!conn) {
		printk("No matching connection found\n");
		return;
	}

	if (default_conn) {
		bt_conn_unref(default_conn);
	}

	default_conn = conn;
}

static void cmd_active_scan_on(void)
{
	int err;

	err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, device_found);
	if (err) {
		printk("Bluetooth set active scan failed (err %d)\n", err);
	} else {
		printk("Bluetooth active scan enabled\n");
	}
}

static void cmd_scan_off(void)
{
	int err;

	err = bt_le_scan_stop();
	if (err) {
		printk("Stopping scanning failed (err %d)\n", err);
	} else {
		printk("Scan successfully stopped\n");
	}
}

static void cmd_scan(int argc, char *argv[])
{
	const char *action;

	if (argc < 2) {
		printk("Scan [on/off] parameter required\n");
		return;
	}

	action = argv[1];
	if (!strcmp(action, "on")) {
		cmd_active_scan_on();
	} else if (!strcmp(action, "off")) {
		cmd_scan_off();
	} else {
		printk("Scan [on/off] parameter required\n");
	}
}

static void cmd_security(int argc, char *argv[])
{
	int err, sec;

	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	if (argc < 2) {
		printk("Security level required\n");
		return;
	}

	sec = *argv[1] - '0';

	err = bt_conn_security(default_conn, sec);
	if (err) {
		printk("Setting security failed (err %d)\n", err);
	}
}

static void exchange_rsp(struct bt_conn *conn, uint8_t err)
{
	printk("Exchange %s\n", err == 0 ? "successful" : "failed");
}

static void cmd_gatt_exchange_mtu(int argc, char *argv[])
{
	int err;

	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	err = bt_gatt_exchange_mtu(default_conn, exchange_rsp);
	if (err) {
		printk("Exchange failed (err %d)\n", err);
	} else {
		printk("Exchange pending\n");
	}
}

static const struct bt_data ad_discov[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static void cmd_advertise(int argc, char *argv[])
{
	struct bt_le_adv_param param;
	const struct bt_data *ad, *scan_rsp;
	size_t ad_len, scan_rsp_len;

	if (argc < 2) {
		goto fail;
	}

	if (!strcmp(argv[1], "off")) {
		if (bt_le_adv_stop() < 0) {
			printk("Failed to stop advertising\n");
		} else {
			printk("Advertising stopped\n");
		}

		return;
	}

	param.interval_min = BT_GAP_ADV_FAST_INT_MIN_2;
	param.interval_max = BT_GAP_ADV_FAST_INT_MAX_2;

	if (!strcmp(argv[1], "on")) {
		param.type = BT_LE_ADV_IND;
		param.addr_type = BT_LE_ADV_ADDR_IDENTITY;
		scan_rsp = sd;
		scan_rsp_len = ARRAY_SIZE(sd);
	} else if (!strcmp(argv[1], "scan")) {
		param.type = BT_LE_ADV_SCAN_IND;
		param.addr_type = BT_LE_ADV_ADDR_IDENTITY;
		scan_rsp = sd;
		scan_rsp_len = ARRAY_SIZE(sd);
	} else if (!strcmp(argv[1], "nconn")) {
		param.type = BT_LE_ADV_NONCONN_IND;
		param.addr_type = BT_LE_ADV_ADDR_NRPA;
		scan_rsp = NULL;
		scan_rsp_len = 0;
	} else {
		goto fail;
	}

	/* Parse advertisement data */
	if (argc >= 3) {
		const char *mode = argv[2];

		if (!strcmp(mode, "discov")) {
			ad = ad_discov;
			ad_len = ARRAY_SIZE(ad_discov);
		} else if (!strcmp(mode, "non_discov")) {
			ad = NULL;
			ad_len = 0;
		} else {
			goto fail;
		}
	} else {
		ad = ad_discov;
		ad_len = ARRAY_SIZE(ad_discov);
	}

	if (bt_le_adv_start(&param, ad, ad_len, scan_rsp, scan_rsp_len) < 0) {
		printk("Failed to start advertising\n");
	} else {
		printk("Advertising started\n");
	}

	return;

fail:
	printk("Usage: advertise <type> <ad mode>\n");
	printk("type: off, on, scan, nconn\n");
	printk("ad mode: discov, non_discov\n");
}

static struct bt_gatt_discover_params discover_params;
static struct bt_uuid_16 uuid = BT_UUID_INIT_16(0);

static void print_chrc_props(uint8_t properties)
{
	printk("Properties: ");

	if (properties & BT_GATT_CHRC_BROADCAST) {
		printk("[bcast]");
	}

	if (properties & BT_GATT_CHRC_READ) {
		printk("[read]");
	}

	if (properties & BT_GATT_CHRC_WRITE) {
		printk("[write]");
	}

	if (properties & BT_GATT_CHRC_WRITE_WITHOUT_RESP) {
		printk("[write w/w rsp]");
	}

	if (properties & BT_GATT_CHRC_NOTIFY) {
		printk("[notify]");
	}

	if (properties & BT_GATT_CHRC_INDICATE) {
		printk("[indicate]");
	}

	if (properties & BT_GATT_CHRC_AUTH) {
		printk("[auth]");
	}

	if (properties & BT_GATT_CHRC_EXT_PROP) {
		printk("[ext prop]");
	}

	printk("\n");
}

static uint8_t discover_func(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	struct bt_gatt_service *gatt_service;
	struct bt_gatt_chrc *gatt_chrc;
	struct bt_gatt_include *gatt_include;
	char uuid[37];

	if (!attr) {
		printk("Discover complete\n");
		memset(params, 0, sizeof(*params));
		return BT_GATT_ITER_STOP;
	}

	switch (params->type) {
	case BT_GATT_DISCOVER_SECONDARY:
	case BT_GATT_DISCOVER_PRIMARY:
		gatt_service = attr->user_data;
		bt_uuid_to_str(gatt_service->uuid, uuid, sizeof(uuid));
		printk("Service %s found: start handle %x, end_handle %x\n",
		       uuid, attr->handle, gatt_service->end_handle);
		break;
	case BT_GATT_DISCOVER_CHARACTERISTIC:
		gatt_chrc = attr->user_data;
		bt_uuid_to_str(gatt_chrc->uuid, uuid, sizeof(uuid));
		printk("Characteristic %s found: handle %x\n", uuid,
		       attr->handle);
		print_chrc_props(gatt_chrc->properties);
		break;
	case BT_GATT_DISCOVER_INCLUDE:
		gatt_include = attr->user_data;
		bt_uuid_to_str(gatt_include->uuid, uuid, sizeof(uuid));
		printk("Include %s found: handle, start %x, end %x\n",
		       uuid, attr->handle, gatt_include->start_handle,
		       gatt_include->end_handle);
		break;
	default:
		bt_uuid_to_str(attr->uuid, uuid, sizeof(uuid));
		printk("Descriptor %s found: handle %x\n", uuid, attr->handle);
		break;
	}

	return BT_GATT_ITER_CONTINUE;
}

static void cmd_gatt_discover(int argc, char *argv[])
{
	int err;

	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	discover_params.func = discover_func;
	discover_params.start_handle = 0x0001;
	discover_params.end_handle = 0xffff;

	if (argc < 2) {
		if (!strcmp(argv[0], "gatt-discover-primary") ||
		    !strcmp(argv[0], "gatt-discover-secondary")) {
			printk("UUID type required\n");
			return;
		}
		goto done;
	}

	/* Only set the UUID if the value is valid (non zero) */
	uuid.val = strtoul(argv[1], NULL, 16);
	if (uuid.val) {
		discover_params.uuid = &uuid.uuid;
	}

	if (argc > 2) {
		discover_params.start_handle = strtoul(argv[2], NULL, 16);
		if (argc > 3) {
			discover_params.end_handle = strtoul(argv[3], NULL, 16);
		}
	}

done:
	if (!strcmp(argv[0], "gatt-discover-secondary")) {
		discover_params.type = BT_GATT_DISCOVER_SECONDARY;
	} else if (!strcmp(argv[0], "gatt-discover-include")) {
		discover_params.type = BT_GATT_DISCOVER_INCLUDE;
	} else if (!strcmp(argv[0], "gatt-discover-characteristic")) {
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
	} else if (!strcmp(argv[0], "gatt-discover-descriptor")) {
		discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
	} else {
		discover_params.type = BT_GATT_DISCOVER_PRIMARY;
	}

	err = bt_gatt_discover(default_conn, &discover_params);
	if (err) {
		printk("Discover failed (err %d)\n", err);
	} else {
		printk("Discover pending\n");
	}
}

static struct bt_gatt_read_params read_params;

static uint8_t read_func(struct bt_conn *conn, int err,
			 struct bt_gatt_read_params *params,
			 const void *data, uint16_t length)
{
	printk("Read complete: err %u length %u\n", err, length);

	if (!data) {
		memset(params, 0, sizeof(*params));
		return BT_GATT_ITER_STOP;
	}

	return BT_GATT_ITER_CONTINUE;
}

static void cmd_gatt_read(int argc, char *argv[])
{
	int err;

	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	read_params.func = read_func;

	if (argc < 2) {
		printk("handle required\n");
		return;
	}

	read_params.handle_count = 1;
	read_params.single.handle = strtoul(argv[1], NULL, 16);

	if (argc > 2) {
		read_params.single.offset = strtoul(argv[2], NULL, 16);
	}

	err = bt_gatt_read(default_conn, &read_params);
	if (err) {
		printk("Read failed (err %d)\n", err);
	} else {
		printk("Read pending\n");
	}
}

static void cmd_gatt_mread(int argc, char *argv[])
{
	uint16_t h[8];
	int i, err;

	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	if (argc < 3) {
		printk("Attribute handles in hex format to read required\n");
		return;
	}

	if (argc - 1 >  ARRAY_SIZE(h)) {
		printk("Enter max %u handle items to read\n", ARRAY_SIZE(h));
		return;
	}

	for (i = 0; i < argc - 1; i++) {
		h[i] = strtoul(argv[i + 1], NULL, 16);
	}

	read_params.func = read_func;
	read_params.handle_count = i;
	read_params.handles = h; /* not used in read func */

	err = bt_gatt_read(default_conn, &read_params);
	if (err) {
		printk("GATT multiple read request failed (err %d)\n", err);
	}
}

static void write_func(struct bt_conn *conn, uint8_t err)
{
	printk("Write complete: err %u\n", err);
}

static void cmd_gatt_write(int argc, char *argv[])
{
	int err;
	uint16_t handle, offset;
	uint8_t data;

	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	if (argc < 2) {
		printk("handle required\n");
		return;
	}

	handle = strtoul(argv[1], NULL, 16);

	if (argc < 3) {
		printk("offset required\n");
		return;
	}

	/* TODO: Add support for longer data */
	offset = strtoul(argv[2], NULL, 16);

	if (argc < 4) {
		printk("data required\n");
		return;
	}

	/* TODO: Add support for longer data */
	data = strtoul(argv[3], NULL, 16);

	err = bt_gatt_write(default_conn, handle, offset, &data, sizeof(data),
			    write_func);
	if (err) {
		printk("Write failed (err %d)\n", err);
	} else {
		printk("Write pending\n");
	}
}

static void cmd_gatt_write_without_rsp(int argc, char *argv[])
{
	int err;
	uint16_t handle;
	uint8_t data;

	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	if (argc < 2) {
		printk("handle required\n");
		return;
	}

	handle = strtoul(argv[1], NULL, 16);

	if (argc < 3) {
		printk("data required\n");
		return;
	}

	data = strtoul(argv[2], NULL, 16);

	err = bt_gatt_write_without_response(default_conn, handle, &data,
					     sizeof(data), false);
	printk("Write Complete (err %d)\n", err);
}

static void cmd_gatt_write_signed(int argc, char *argv[])
{
	int err;
	uint16_t handle;
	uint8_t data;

	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	if (argc < 2) {
		printk("handle required\n");
		return;
	}

	handle = strtoul(argv[1], NULL, 16);

	if (argc < 3) {
		printk("data required\n");
		return;
	}

	data = strtoul(argv[2], NULL, 16);

	err = bt_gatt_write_without_response(default_conn, handle, &data,
					     sizeof(data), true);
	printk("Write Complete (err %d)\n", err);
}

static struct bt_gatt_subscribe_params subscribe_params;

static uint8_t notify_func(struct bt_conn *conn,
			   struct bt_gatt_subscribe_params *params,
			   const void *data, uint16_t length)
{
	if (!data) {
		printk("Ubsubscribed\n");
		params->value_handle = 0;
		return BT_GATT_ITER_STOP;
	}

	printk("Notification: data %p length %u\n", data, length);

	return BT_GATT_ITER_CONTINUE;
}

static void cmd_gatt_subscribe(int argc, char *argv[])
{
	int err;

	if (subscribe_params.value_handle) {
		printk("Cannot subscribe: subscription to %x already exists\n",
		       subscribe_params.value_handle);
		return;
	}

	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	if (argc < 2) {
		printk("CCC handle required\n");
		return;
	}

	if (argc < 3) {
		printk("value handle required\n");
		return;
	}

	subscribe_params.ccc_handle = strtoul(argv[1], NULL, 16);
	subscribe_params.value_handle = strtoul(argv[2], NULL, 16);
	subscribe_params.value = BT_GATT_CCC_NOTIFY;
	subscribe_params.notify = notify_func;

	if (argc > 3) {
		subscribe_params.value = strtoul(argv[3], NULL, 16);
	}

	err = bt_gatt_subscribe(default_conn, &subscribe_params);
	if (err) {
		printk("Subscribe failed (err %d)\n", err);
		return;
	}

	printk("Subscribed\n");
}

static void cmd_gatt_unsubscribe(int argc, char *argv[])
{
	int err;

	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	if (!subscribe_params.value_handle) {
		printk("No subscription found\n");
		return;
	}

	err = bt_gatt_unsubscribe(default_conn, &subscribe_params);
	if (err) {
		printk("Unsubscribe failed (err %d)\n", err);
	} else {
		printk("Unsubscribe success\n");
	}

	/* Clear subscribe_params to reuse it */
	memset(&subscribe_params, 0, sizeof(subscribe_params));
}

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];
	char passkey_str[7];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	snprintf(passkey_str, 7, "%06u", passkey);

	printk("Passkey for %s: %s\n", addr, passkey_str);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];
	char passkey_str[7];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	snprintf(passkey_str, 7, "%06u", passkey);

	printk("Confirm passkey for %s: %s\n", addr, passkey_str);
}

static void auth_passkey_entry(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Enter passkey for %s\n", addr);
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	conn_addr_str(conn, addr, sizeof(addr));

	printk("Pairing cancelled: %s\n", addr);

	/* clear connection reference for sec mode 3 pairing */
	if (pairing_conn) {
		bt_conn_unref(pairing_conn);
		pairing_conn = NULL;
	}
}

#if defined(CONFIG_BLUETOOTH_BREDR)
static void auth_pincode_entry(struct bt_conn *conn, bool highsec)
{
	char addr[BT_ADDR_STR_LEN];
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) < 0) {
		return;
	}

	if (info.type != BT_CONN_TYPE_BR) {
		return;
	}

	bt_addr_to_str(info.br.dst, addr, sizeof(addr));

	if (highsec) {
		printk("Enter 16 digits wide PIN code for %s\n", addr);
	} else {
		printk("Enter PIN code for %s\n", addr);
	}

	/*
	 * Save connection info since in security mode 3 (link level enforced
	 * security) PIN request callback is called before connected callback
	 */
	if (!default_conn && !pairing_conn) {
		pairing_conn = bt_conn_ref(conn);
	}
}
#endif

static struct bt_conn_auth_cb auth_cb_display = {
	.passkey_display = auth_passkey_display,
	.passkey_entry = NULL,
	.passkey_confirm = NULL,
#if defined(CONFIG_BLUETOOTH_BREDR)
	.pincode_entry = auth_pincode_entry,
#endif
	.cancel = auth_cancel,
};

static struct bt_conn_auth_cb auth_cb_display_yes_no = {
	.passkey_display = auth_passkey_display,
	.passkey_entry = NULL,
	.passkey_confirm = auth_passkey_confirm,
#if defined(CONFIG_BLUETOOTH_BREDR)
	.pincode_entry = auth_pincode_entry,
#endif
	.cancel = auth_cancel,
};

static struct bt_conn_auth_cb auth_cb_input = {
	.passkey_display = NULL,
	.passkey_entry = auth_passkey_entry,
	.passkey_confirm = NULL,
#if defined(CONFIG_BLUETOOTH_BREDR)
	.pincode_entry = auth_pincode_entry,
#endif
	.cancel = auth_cancel,
};

static struct bt_conn_auth_cb auth_cb_all = {
	.passkey_display = auth_passkey_display,
	.passkey_entry = auth_passkey_entry,
	.passkey_confirm = auth_passkey_confirm,
#if defined(CONFIG_BLUETOOTH_BREDR)
	.pincode_entry = auth_pincode_entry,
#endif
	.cancel = auth_cancel,
};

static void cmd_auth(int argc, char *argv[])
{
	if (argc < 2) {
		printk("auth [display, yesno, input, all, none] "
		       "parameter required\n");
		return;
	}

	if (!strcmp(argv[1], "all")) {
		bt_conn_auth_cb_register(&auth_cb_all);
	} else if (!strcmp(argv[1], "input")) {
		bt_conn_auth_cb_register(&auth_cb_input);
	} else if (!strcmp(argv[1], "display")) {
		bt_conn_auth_cb_register(&auth_cb_display);
	} else if (!strcmp(argv[1], "yesno")) {
		bt_conn_auth_cb_register(&auth_cb_display_yes_no);
	} else if (!strcmp(argv[1], "none")) {
		bt_conn_auth_cb_register(NULL);
	} else {
		printk("auth [display, yesno, input, all, none] "
		       "parameter required\n");
	}
}

static void cmd_auth_cancel(int argc, char *argv[])
{
	struct bt_conn *conn;

	if (default_conn) {
		conn = default_conn;
	} else if (pairing_conn) {
		conn = pairing_conn;
	} else {
		conn = NULL;
	}

	if (!conn) {
		printk("Not connected\n");
		return;
	}

	bt_conn_auth_cancel(conn);
}

static void cmd_auth_passkey_confirm(int argc, char *argv[])
{
	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	bt_conn_auth_passkey_confirm(default_conn);
}

static void cmd_auth_passkey(int argc, char *argv[])
{
	unsigned int passkey;

	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	if (argc < 2) {
		printk("passkey required\n");
		return;
	}

	passkey = atoi(argv[1]);
	if (passkey > 999999) {
		printk("Passkey should be between 0-999999\n");
		return;
	}

	bt_conn_auth_passkey_entry(default_conn, passkey);
}

#if defined(CONFIG_BLUETOOTH_BREDR)
static void cmd_auth_pincode(int argc, char *argv[])
{
	struct bt_conn *conn;

	uint8_t max = 16;

	if (default_conn) {
		conn = default_conn;
	} else if (pairing_conn) {
		conn = pairing_conn;
	} else {
		conn = NULL;
	}

	if (!conn) {
		printk("Not connected\n");
		return;
	}

	if (argc < 2) {
		printk("PIN code required\n");
		return;
	}

	if (strlen(argv[1]) > max) {
		printk("PIN code value invalid - enter max %u digits\n", max);
		return;
	}

	printk("PIN code \"%s\" applied\n", argv[1]);

	bt_conn_auth_pincode_entry(conn, argv[1]);
}

static int str2bt_addr(const char *str, bt_addr_t *addr)
{
	int i, j;
	uint8_t tmp;

	if (strlen(str) != 17) {
		return -EINVAL;
	}

	for (i = 5, j = 1; *str != '\0'; str++, j++) {
		if (!(j % 3) && (*str != ':')) {
			return -EINVAL;
		} else if (*str == ':') {
			i--;
			continue;
		}

		addr->val[i] = addr->val[i] << 4;

		if (char2hex(str, &tmp) < 0) {
			return -EINVAL;
		}

		addr->val[i] |= tmp;
	}

	return 0;
}

static void cmd_connect_bredr(int argc, char *argv[])
{
	struct bt_conn *conn;
	bt_addr_t addr;
	int err;

	if (argc < 2) {
		printk("Peer address required\n");
		return;
	}

	err = str2bt_addr(argv[1], &addr);
	if (err) {
		printk("Invalid peer address (err %d)\n", err);
		return;
	}

	conn = bt_conn_create_br(&addr, BT_BR_CONN_PARAM_DEFAULT);
	if (!conn) {
		printk("Connection failed\n");
	} else {

		printk("Connection pending\n");

		/* unref connection obj in advance as app user */
		bt_conn_unref(conn);
	}
}

static void br_device_found(const bt_addr_t *addr, int8_t rssi,
				  const uint8_t cod[3], const uint8_t eir[240])
{
	char br_addr[BT_ADDR_STR_LEN];
	char name[239];
	int len = 240;

	memset(name, 0, sizeof(name));

	while (len) {
		if (len < 2) {
			break;
		};

		/* Look for early termination */
		if (!eir[0]) {
			break;
		}

		/* Check if field length is correct */
		if (eir[0] > len - 1) {
			break;
		}

		switch (eir[1]) {
		case AD_SHORT_NAME:
		case AD_COMPLETE_NAME:
			if (eir[0] > sizeof(name) - 1) {
				memcpy(name, &eir[2], sizeof(name) - 1);
			} else {
				memcpy(name, &eir[2], eir[0] - 1);
			}
			break;
		default:
			break;
		}

		/* Parse next AD Structure */
		len -= eir[0] + 1;
		eir += eir[0] + 1;
	}

	bt_addr_to_str(addr, br_addr, sizeof(br_addr));

	printk("[DEVICE]: %s, RSSI %i %s\n", br_addr, rssi, name);
}

static struct bt_br_discovery_result br_discovery_results[5];

static void br_discovery_complete(struct bt_br_discovery_result *results,
				  size_t count)
{
	size_t i;

	printk("BR/EDR discovery complete\n");

	for (i = 0; i < count; i++) {
		br_device_found(&results[i].addr, results[i].rssi,
				results[i].cod, results[i].eir);
	}
}

static void cmd_bredr_discovery(int argc, char *argv[])
{
	const char *action;

	if (argc < 2) {
		printk("Discovery [on/off] parameter required\n");
		return;
	}

	action = argv[1];
	if (!strcmp(action, "on")) {
		struct bt_br_discovery_param param;

		param.limited_discovery = false;

		if (argc > 2 && !strcmp(argv[2], "limited")) {
			param.limited_discovery = true;
		}

		if (bt_br_discovery_start(&param, br_discovery_results,
					  ARRAY_SIZE(br_discovery_results),
					  br_discovery_complete) < 0) {
			printk("Failed to start discovery\n");
			return;
		}

		printk("Discovery started\n");
	} else if (!strcmp(action, "off")) {
		if (bt_br_discovery_stop()) {
			printk("Failed to stop discovery\n");
			return;
		}

		printk("Discovery stopped\n");
	} else {
		printk("Discovery [on/off] parameter required\n");
	}
}

#endif /* CONFIG_BLUETOOTH_BREDR */

#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
static void l2cap_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	printk("Incoming data channel %p len %u\n", chan, buf->len);
}

static void l2cap_connected(struct bt_l2cap_chan *chan)
{
	printk("Channel %p connected\n", chan);
}

static void l2cap_disconnected(struct bt_l2cap_chan *chan)
{
	printk("Channel %p disconnected\n", chan);
}

static struct net_buf *l2cap_alloc_buf(struct bt_l2cap_chan *chan)
{
	printk("Channel %p requires buffer\n", chan);

	return net_buf_get(&data_fifo, 0);
}

static struct bt_l2cap_chan_ops l2cap_ops = {
	.alloc_buf	= l2cap_alloc_buf,
	.recv		= l2cap_recv,
	.connected	= l2cap_connected,
	.disconnected	= l2cap_disconnected,
};

static struct bt_l2cap_chan l2cap_chan = {
	.ops		= &l2cap_ops,
	.rx.mtu		= DATA_MTU,
};

static int l2cap_accept(struct bt_conn *conn, struct bt_l2cap_chan **chan)
{
	printk("Incoming conn %p\n", conn);

	if (l2cap_chan.conn) {
		printk("No channels available");
		return -ENOMEM;
	}

	*chan = &l2cap_chan;

	return 0;
}

static struct bt_l2cap_server server = {
	.accept		= l2cap_accept,
};

static void cmd_l2cap_register(int argc, char *argv[])
{
	if (argc < 2) {
		printk("psm required\n");
		return;
	}

	if (server.psm) {
		printk("Already registered\n");
		return;
	}

	server.psm = strtoul(argv[1], NULL, 16);

	if (bt_l2cap_server_register(&server) < 0) {
		printk("Unable to register psm\n");
		server.psm = 0;
		return;
	}
}

static void cmd_l2cap_connect(int argc, char *argv[])
{
	uint16_t psm;
	int err;

	if (!default_conn) {
		printk("Not connected\n");
		return;
	}

	if (argc < 2) {
		printk("psm required\n");
		return;
	}

	psm = strtoul(argv[1], NULL, 16);

	err = bt_l2cap_chan_connect(default_conn, &l2cap_chan, psm);
	if (err < 0) {
		printk("Unable to connect to psm %u (err %u)\n", psm, err);
		return;
	}
}

static void cmd_l2cap_disconnect(int argc, char *argv[])
{
	int err;

	err = bt_l2cap_chan_disconnect(&l2cap_chan);
	if (err) {
		printk("Unable to disconnect: %u\n", -err);
	}
}

static void cmd_l2cap_send(int argc, char *argv[])
{
	static uint8_t buf_data[DATA_MTU] = { [0 ... (DATA_MTU - 1)] = 0xff };
	int ret, len, count = 1;
	struct net_buf *buf;

	if (argc > 1) {
		count = strtoul(argv[1], NULL, 10);
	}

	len = min(l2cap_chan.tx.mtu, DATA_MTU - BT_L2CAP_CHAN_SEND_RESERVE);

	while (count--) {
		buf = net_buf_get(&data_fifo, BT_L2CAP_CHAN_SEND_RESERVE);
		if (!buf) {
			printk("Unable acquire buffer\n");
			break;
		}

		memcpy(net_buf_add(buf, len), buf_data, len);
		ret = bt_l2cap_chan_send(&l2cap_chan, buf);
		if (ret < 0) {
			printk("Unable to send: %d\n", -ret);
			net_buf_unref(buf);
			break;
		}
	}
}
#endif

#if defined(CONFIG_BLUETOOTH_BREDR)
static void cmd_bredr_discoverable(int argc, char *argv[])
{
	int err;
	const char *action;

	if (argc < 2) {
		printk("[on/off] parameter required\n");
		return;
	}

	action = argv[1];

	if (!strcmp(action, "on")) {
		err = bt_br_set_discoverable(true);
	} else if (!strcmp(action, "off")) {
		err = bt_br_set_discoverable(false);
	} else {
		printk("[on/off] parameter required\n");
		return;
	}

	if (err) {
		printk("BR/EDR set/reset discoverable failed (err %d)\n", err);
		return;
	}

	printk("BR/EDR set/reset discoverable done\n");
}

static void cmd_bredr_connectable(int argc, char *argv[])
{
	int err;
	const char *action;

	if (argc < 2) {
		printk("[on/off] parameter required\n");
		return;
	}

	action = argv[1];

	if (!strcmp(action, "on")) {
		err = bt_br_set_connectable(true);
	} else if (!strcmp(action, "off")) {
		err = bt_br_set_connectable(false);
	} else {
		printk("[on/off] parameter required\n");
		return;
	}

	if (err) {
		printk("BR/EDR set/rest connectable failed (err %d)\n", err);
		return;
	}

	printk("BR/EDR set/reset connectable done\n");
}
#endif

static const struct shell_cmd commands[] = {
	{ "init", cmd_init },
	{ "connect", cmd_connect_le },
	{ "disconnect", cmd_disconnect },
	{ "auto-conn", cmd_auto_conn },
	{ "select", cmd_select },
	{ "scan", cmd_scan },
	{ "advertise", cmd_advertise },
	{ "security", cmd_security },
	{ "auth", cmd_auth },
	{ "auth-cancel", cmd_auth_cancel },
	{ "auth-passkey", cmd_auth_passkey },
	{ "auth-confirm", cmd_auth_passkey_confirm },
#if defined(CONFIG_BLUETOOTH_BREDR)
	{ "auth-pincode", cmd_auth_pincode },
#endif
	{ "gatt-exchange-mtu", cmd_gatt_exchange_mtu },
	{ "gatt-discover-primary", cmd_gatt_discover },
	{ "gatt-discover-secondary", cmd_gatt_discover },
	{ "gatt-discover-include", cmd_gatt_discover },
	{ "gatt-discover-characteristic", cmd_gatt_discover },
	{ "gatt-discover-descriptor", cmd_gatt_discover },
	{ "gatt-read", cmd_gatt_read },
	{ "gatt-read-multiple", cmd_gatt_mread },
	{ "gatt-write", cmd_gatt_write },
	{ "gatt-write-without-response", cmd_gatt_write_without_rsp },
	{ "gatt-write-signed", cmd_gatt_write_signed },
	{ "gatt-subscribe", cmd_gatt_subscribe },
	{ "gatt-unsubscribe", cmd_gatt_unsubscribe },
#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
	{ "l2cap-register", cmd_l2cap_register },
	{ "l2cap-connect", cmd_l2cap_connect },
	{ "l2cap-disconnect", cmd_l2cap_disconnect },
	{ "l2cap-send", cmd_l2cap_send },
#endif
#if defined(CONFIG_BLUETOOTH_BREDR)
	{ "br-iscan", cmd_bredr_discoverable },
	{ "br-pscan", cmd_bredr_connectable },
	{ "br-connect", cmd_connect_bredr },
	{ "br-discovery", cmd_bredr_discovery },
#endif
	{ NULL, NULL }
};

#ifdef CONFIG_MICROKERNEL
void mainloop(void)
#else
void main(void)
#endif
{
	bt_conn_cb_register(&conn_callbacks);

	net_buf_pool_init(data_pool);

	printk("Type \"help\" for supported commands.\n");
	printk("Before any Bluetooth commands you must run \"init\".\n");

	shell_init("btshell> ", commands);

	shell_register_prompt_handler(current_prompt);
}
