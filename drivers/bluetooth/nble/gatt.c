/*
 * Copyright (c) 2016 Intel Corporation
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
#include <atomic.h>
#include <misc/byteorder.h>

#include <bluetooth/gatt.h>
#include <bluetooth/log.h>

#include "conn.h"
#include "conn_internal.h"
#include "gatt_internal.h"

#if !defined(CONFIG_BLUETOOTH_DEBUG_GATT)
#undef BT_DBG
#define BT_DBG(fmt, ...)
#endif

#define NBLE_BUF_SIZE	384

/* TODO: Get this value during negotiation */
#define BLE_GATT_MTU_SIZE 23

struct nble_gatt_service {
	const struct bt_gatt_attr *attrs;
	uint16_t attr_count;
};

static struct nble_gatt_service svc_db[BLE_GATTS_MAX_SERVICES];
static uint8_t svc_count;

static struct bt_gatt_subscribe_params *subscriptions;

/**
 * Copy a UUID in a buffer using the smallest memory length
 * @param buf Pointer to the memory where the UUID shall be copied
 * @param uuid Pointer to the UUID to copy
 * @return The length required to store the UUID in the memory
 */
static uint8_t bt_gatt_uuid_memcpy(uint8_t *buf, const struct bt_uuid *uuid)
{
	uint8_t *ptr = buf;

	/* Store the type of the UUID */
	*ptr = uuid->type;
	ptr++;

	/* Store the UUID data */
	if (uuid->type == BT_UUID_TYPE_16) {
		uint16_t le16;

		le16 = sys_cpu_to_le16(BT_UUID_16(uuid)->val);
		memcpy(ptr, &le16, sizeof(le16));
		ptr += sizeof(le16);
	} else {
		memcpy(ptr, BT_UUID_128(uuid)->val, 16);
		ptr += 16;
	}

	return ptr - buf;
}

/* These attributes need the value to be read */
static struct bt_uuid *whitelist[] = {
	BT_UUID_GATT_PRIMARY,
	BT_UUID_GATT_SECONDARY,
	BT_UUID_GATT_INCLUDE,
	BT_UUID_GATT_CHRC,
	BT_UUID_GATT_CEP,
	BT_UUID_GATT_CUD,
	BT_UUID_GATT_CPF,
	BT_UUID_GAP_DEVICE_NAME,
	BT_UUID_GAP_APPEARANCE,
	BT_UUID_GAP_PPCP
};

static int attr_read(struct bt_gatt_attr *attr, uint8_t *data, size_t len)
{
	uint8_t i;
	int data_size;

	if (!data) {
		return -ENOMEM;
	}

	data_size = bt_gatt_uuid_memcpy(data, attr->uuid);

	for (i = 0; i < ARRAY_SIZE(whitelist); i++) {
		if (!bt_uuid_cmp(attr->uuid, whitelist[i])) {
			int read;

			read = attr->read(NULL, attr, data + data_size, len, 0);
			if (read < 0) {
				return read;
			}

			data_size += read;
			break;
		}
	}

	return data_size;
}

int bt_gatt_register(struct bt_gatt_attr *attrs, size_t count)
{
	struct nble_gatt_register_req param;
	size_t i;
	/* TODO: Replace the following with net_buf */
	uint8_t attr_table[NBLE_BUF_SIZE];
	uint8_t attr_table_size;

	if (!attrs || !count) {
		return -EINVAL;
	}
	BT_ASSERT(svc_count < BLE_GATTS_MAX_SERVICES);

	svc_db[svc_count].attrs = attrs;
	svc_db[svc_count].attr_count = count;
	svc_count++;
	param.attr_base = attrs;
	param.attr_count = count;

	attr_table_size = 0;

	for (i = 0; i < count; i++) {
		struct bt_gatt_attr *attr = &attrs[i];
		struct nble_gatt_attr *att;
		int err;

		if (attr_table_size + sizeof(*att) > sizeof(attr_table)) {
			return -ENOMEM;
		}

		att = (void *)&attr_table[attr_table_size];
		att->perm = attr->perm;

		attr_table_size += sizeof(*att);

		/* Read attribute data */
		err = attr_read(attr, att->data,
				sizeof(attr_table) - attr_table_size);
		if (err < 0) {
			BT_ERR("Failed to read attr: %d", err);
			return err;
		}

		att->data_size = err;

		/* Compute the new element size and align it on upper 4 bytes
		 * boundary.
		 */
		attr_table_size += (att->data_size + 3) & ~3;

		BT_DBG("table size = %u attr data_size = %u", attr_table_size,
		       att->data_size);
	}

	nble_gatt_register_req(&param, attr_table, attr_table_size);
	return 0;
}

void on_nble_gatt_register_rsp(const struct nble_gatt_register_rsp *rsp,
			       const struct nble_gatt_attr_handles *handles,
			       uint8_t len)
{
	BT_DBG("status %u", rsp->status);

	if (rsp->status != 0) {
		return;
	}
#if defined(CONFIG_BLUETOOTH_DEBUG_GATT)
	{
		int idx;

		for (idx = 0; idx < rsp->attr_count; idx++) {
			/* The following order of declaration is assumed for
			 * this to work (otherwise idx-2 will fail!):
			 * BT_GATT_CHARACTERISTIC -> ble core returns invalid
			 * handle.
			 * BT_GATT_DESCRIPTOR -> value handle of characteristic
			 * BT_GATT_CCC -> cccd handle is ignored as no storage
			 * but reference value is updated in CCC with value
			 * handle from descriptor.
			 */
			if (handles[idx].handle != 0) {
				char uuid[37];

				bt_uuid_to_str(rsp->attr_base[idx].uuid,
					       uuid, sizeof(uuid));
				BT_DBG("handle 0x%04x uuid %s",
				       handles[idx].handle, uuid);
			}
		}
	}
#endif
}

void bt_gatt_foreach_attr(uint16_t start_handle, uint16_t end_handle,
			  bt_gatt_attr_func_t func, void *user_data)
{
}

struct bt_gatt_attr *bt_gatt_attr_next(const struct bt_gatt_attr *attr)
{
	uint16_t i;

	for (i = 0; i < svc_count; i++) {
		if (attr >= svc_db[i].attrs &&
		    attr < svc_db[i].attrs + svc_db[i].attr_count) {
			uint8_t attr_i;

			attr_i = (attr - svc_db[i].attrs) + 1;

			/* Return next element of current service */
			if (attr_i < svc_db[i].attr_count) {
				return (struct bt_gatt_attr *)&attr[1];
			}

			/* Return next service as next attribute */
			if (i < (svc_count - 1)) {
				return (struct bt_gatt_attr *)svc_db[i+1].attrs;
			}
		}
	}

	return NULL;
}

ssize_t bt_gatt_attr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t buf_len, uint16_t offset,
			  const void *value, uint16_t value_len)
{
	uint16_t len;

	BT_DBG("handle 0x%04x offset %u", attr->handle, offset);

	/* simply return the value length. used as max_value. */
	if (buf == NULL) {
		return value_len;
	}

	if (offset > value_len) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	len = min(buf_len, value_len - offset);

	memcpy(buf, value + offset, len);

	return len;
}

ssize_t bt_gatt_attr_read_service(struct bt_conn *conn,
				  const struct bt_gatt_attr *attr,
				  void *buf, uint16_t len, uint16_t offset)
{
	struct bt_uuid *uuid = attr->user_data;

	if (uuid->type == BT_UUID_TYPE_16) {
		uint16_t uuid16 = sys_cpu_to_le16(BT_UUID_16(uuid)->val);

		return bt_gatt_attr_read(conn, attr, buf, len, offset,
					 &uuid16, 2);
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 BT_UUID_128(uuid)->val, 16);
}

ssize_t bt_gatt_attr_read_included(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
}

struct gatt_chrc {
	uint8_t properties;
	uint16_t value_handle;
	union {
		uint16_t uuid16;
		uint8_t  uuid[16];
	};
} __packed;

ssize_t bt_gatt_attr_read_chrc(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr, void *buf,
			       uint16_t len, uint16_t offset)
{
	struct bt_gatt_chrc *chrc = attr->user_data;
	struct gatt_chrc pdu;
	uint8_t value_len;

	pdu.properties = chrc->properties;

	/* Handle cannot be read at this point */
	pdu.value_handle = 0x0000;

	value_len = sizeof(pdu.properties) + sizeof(pdu.value_handle);

	if (chrc->uuid->type == BT_UUID_TYPE_16) {
		pdu.uuid16 = sys_cpu_to_le16(BT_UUID_16(chrc->uuid)->val);
		value_len += 2;
	} else {
		memcpy(pdu.uuid, BT_UUID_128(chrc->uuid)->val, 16);
		value_len += 16;
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &pdu, value_len);
}

ssize_t bt_gatt_attr_read_ccc(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr, void *buf,
			      uint16_t len, uint16_t offset)
{
	return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
}

ssize_t bt_gatt_attr_write_ccc(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       const void *buf, uint16_t len, uint16_t offset)
{
	struct _bt_gatt_ccc *ccc = attr->user_data;
	const uint16_t *data = buf;

	if (offset > sizeof(*data)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (offset + len > sizeof(*data)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	/* We expect to receive this only when the has really changed */
	ccc->value = sys_le16_to_cpu(*data);

	if (ccc->cfg_changed) {
		ccc->cfg_changed(ccc->value);
	}

	BT_DBG("handle 0x%04x value %u", attr->handle, ccc->value);

	return len;
}

ssize_t bt_gatt_attr_read_cep(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr, void *buf,
			      uint16_t len, uint16_t offset)
{
	struct bt_gatt_cep *value = attr->user_data;
	uint16_t props = sys_cpu_to_le16(value->properties);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &props,
				 sizeof(props));
}

ssize_t bt_gatt_attr_read_cud(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr, void *buf,
			      uint16_t len, uint16_t offset)
{
	char *value = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
				 strlen(value));
}

ssize_t bt_gatt_attr_read_cpf(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr, void *buf,
			      uint16_t len, uint16_t offset)
{
	struct bt_gatt_cpf *value = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
				 sizeof(*value));
}

int bt_gatt_notify(struct bt_conn *conn, const struct bt_gatt_attr *attr,
		   const void *data, uint16_t len)
{
	struct nble_gatt_send_notif_params notif;

	if (conn) {
		notif.conn_handle = conn->handle;
	} else {
		notif.conn_handle = 0xffff;
	}

	notif.params.attr = (struct bt_gatt_attr *)attr;
	notif.params.offset = 0;
	notif.cback = NULL;

	nble_gatt_send_notif_req(&notif, (uint8_t *)data, len);
	return 0;
}

int bt_gatt_indicate(struct bt_conn *conn,
		     struct bt_gatt_indicate_params *params)
{
	struct nble_gatt_send_ind_params ind;

	BT_DBG("conn %p", conn);

	if (!params) {
		return -EINVAL;
	}

	if (conn) {
		ind.conn_handle = conn->handle;
	} else {
		ind.conn_handle = 0xffff;
	}

	ind.params.attr = (void *)params->attr;
	ind.params.offset = 0;
	ind.cback = params->func;

	nble_gatt_send_ind_req(&ind, (void *)params->data, params->len);

	return 0;
}

/* Response to bt_gatt_indicate() */
void on_nble_gatts_send_ind_rsp(const struct nble_gatt_ind_rsp *rsp)
{
	struct bt_conn *conn;

	if (rsp->status) {
		BT_ERR("Send indication failed, status %d", rsp->status);
		return;
	}

	conn = bt_conn_lookup_handle(rsp->conn_handle);
	if (!conn) {
		BT_ERR("Unable to find conn, handle 0x%04x", rsp->conn_handle);
		return;
	}

	if (rsp->cback) {
		rsp->cback(conn, rsp->attr, rsp->status);
	}

	bt_conn_unref(conn);
}

int bt_gatt_exchange_mtu(struct bt_conn *conn, bt_gatt_rsp_func_t func)
{
	return -ENOSYS;
}

int bt_gatt_discover(struct bt_conn *conn,
		     struct bt_gatt_discover_params *params)
{
	struct nble_discover_params discover_params;

	if (!conn || !params || !params->func || !params->start_handle ||
	    !params->end_handle || params->start_handle > params->end_handle) {
		return -EINVAL;
	}

	if (conn->state != BT_CONN_CONNECTED) {
		return -ENOTCONN;
	}

	if (conn->gatt_private) {
		return -EBUSY;
	}

	BT_DBG("conn %p start 0x%04x end 0x%04x", conn, params->start_handle,
	       params->end_handle);

	memset(&discover_params, 0, sizeof(discover_params));

	switch (params->type) {
	case BT_GATT_DISCOVER_PRIMARY:
	case BT_GATT_DISCOVER_CHARACTERISTIC:
		if (params->uuid) {
			/* Always copy a full 128 bit UUID */
			memcpy(&discover_params.uuid, BT_UUID_128(params->uuid),
			       sizeof(discover_params.uuid));
			discover_params.flags = DISCOVER_FLAGS_UUID_PRESENT;
		}

		break;
	case BT_GATT_DISCOVER_INCLUDE:
	case BT_GATT_DISCOVER_DESCRIPTOR:
		break;
	default:
		BT_ERR("Unknown params type %u", params->type);
		return -EINVAL;
	}

	discover_params.conn_handle = conn->handle;
	discover_params.type = params->type;
	discover_params.handle_range.start_handle = params->start_handle;
	discover_params.handle_range.end_handle = params->end_handle;

	conn->gatt_private = params;

	nble_gattc_discover_req(&discover_params);

	return 0;
}

static uint16_t parse_include(struct bt_conn *conn, const uint8_t *data,
			      uint8_t len)
{
	struct bt_gatt_discover_params *params = conn->gatt_private;
	uint16_t end_handle = 0;
	int i;

	for (i = 0; len > 0; i++) {
		const struct nble_gattc_included *att = (void *)data;
		struct bt_gatt_attr *attr = NULL;
		struct bt_gatt_include gatt_include;

		gatt_include.start_handle = att->range.start_handle;
		gatt_include.end_handle = att->range.end_handle;
		end_handle = gatt_include.end_handle;

		BT_DBG("start 0x%04x end 0x%04x", att->range.start_handle,
		       att->range.end_handle);

		/*
		 * 4.5.1 If the service UUID is a 16-bit Bluetooth UUID
		 *  it is also returned in the response.
		 */
		switch (att->uuid.uuid.type) {
		case BT_UUID_TYPE_16:
			gatt_include.uuid = &att->uuid.uuid;
			break;
		case BT_UUID_TYPE_128:
			/* Data is not available at this point */
			break;
		}

		attr = (&(struct bt_gatt_attr)
			BT_GATT_INCLUDE_SERVICE(&gatt_include));
		attr->handle = att->handle;

		data += sizeof(*att);
		len -= sizeof(*att);

		if (params->func(conn, attr, params) == BT_GATT_ITER_STOP) {
			return 0;
		}
	}

	return end_handle;
}

static uint16_t parse_service(struct bt_conn *conn, const uint8_t *data,
			      uint8_t len)
{
	struct bt_gatt_discover_params *params = conn->gatt_private;
	uint16_t end_handle = 0;
	int i;

	for (i = 0; len > 0; i++) {
		const struct nble_gattc_primary *att = (void *)data;
		struct bt_gatt_service gatt_service;
		struct bt_gatt_attr *attr = NULL;

		gatt_service.end_handle = att->range.end_handle;
		gatt_service.uuid = params->uuid;
		end_handle = gatt_service.end_handle;

		attr = (&(struct bt_gatt_attr)
			BT_GATT_PRIMARY_SERVICE(&gatt_service));
		attr->handle = att->handle;

		data += sizeof(*att);
		len -= sizeof(*att);

		if (params->func(conn, attr, params) == BT_GATT_ITER_STOP) {
			return 0;
		}
	}

	return end_handle;
}

static uint16_t parse_characteristic(struct bt_conn *conn, const uint8_t *data,
				     uint8_t len)
{
	struct bt_gatt_discover_params *params = conn->gatt_private;
	uint16_t end_handle = 0;
	int i;

	for (i = 0; len > 0; i++) {
		const struct nble_gattc_characteristic *att = (void *)data;
		struct bt_gatt_attr *attr = NULL;

		attr = (&(struct bt_gatt_attr)
			BT_GATT_CHARACTERISTIC(&att->uuid.uuid, att->prop));
		attr->handle = att->handle;
		end_handle = att->handle;

		data += sizeof(*att);
		len -= sizeof(*att);

		if (params->func(conn, attr, params) == BT_GATT_ITER_STOP) {
			return 0;
		}
	}

	return end_handle;
}

static uint16_t parse_descriptor(struct bt_conn *conn, const uint8_t *data,
				 uint8_t len)
{
	struct bt_gatt_discover_params *params = conn->gatt_private;
	uint16_t end_handle = 0;
	int i;

	for (i = 0; len > 0; i++) {
		const struct nble_gattc_descriptor *att = (void *)data;
		struct bt_gatt_attr *attr = NULL;

		attr = (&(struct bt_gatt_attr)
			BT_GATT_DESCRIPTOR(&att->uuid.uuid, 0, NULL, NULL, NULL));
		attr->handle = att->handle;
		end_handle = att->handle;

		data += sizeof(*att);
		len -= sizeof(*att);

		if (params->func(conn, attr, params) == BT_GATT_ITER_STOP) {
			return 0;
		}
	}

	return end_handle;
}


void on_nble_gattc_discover_rsp(const struct nble_gattc_discover_rsp *rsp,
				const uint8_t *data, uint8_t data_len)
{
	uint16_t end_handle = 0;
	struct bt_gatt_discover_params *params;
	struct bt_conn *conn;
	int status;

	conn = bt_conn_lookup_handle(rsp->conn_handle);
	if (!conn) {
		BT_ERR("Unable to find conn, handle 0x%04x", rsp->conn_handle);
		return;
	}

	params = conn->gatt_private;

	/* Status maybe error or indicate end of discovery */
	if (rsp->status) {
		BT_DBG("status %d", rsp->status);
		goto done;
	}

	BT_DBG("conn %p conn handle 0x%04x status %d len %u", conn,
	       conn->handle, rsp->status, data_len);

	switch (rsp->type) {
	case BT_GATT_DISCOVER_INCLUDE:
		end_handle = parse_include(conn, data, data_len);
		break;
	case BT_GATT_DISCOVER_PRIMARY:
		end_handle = parse_service(conn, data, data_len);
		break;
	case BT_GATT_DISCOVER_CHARACTERISTIC:
		end_handle = parse_characteristic(conn, data, data_len);
		break;
	case BT_GATT_DISCOVER_DESCRIPTOR:
		end_handle = parse_descriptor(conn, data, data_len);
		break;
	default:
		BT_ERR("Wrong discover type %d", rsp->type);
		bt_conn_unref(conn);
		return;
	}

	if (!end_handle) {
		goto stop;
	}

	/* Stop if end_handle is over the range */
	if (end_handle >= params->end_handle) {
		BT_WARN("Handle goes over the range: 0x%04x >= 0x%04x",
			end_handle, params->end_handle);
		goto done;
	}

	/* Continue discovery from last found handle */
	params->start_handle = end_handle;
	if (params->start_handle < UINT16_MAX) {
		params->start_handle++;
	}

	/* This pointer would keep new params set in the function below */
	conn->gatt_private = NULL;

	status = bt_gatt_discover(conn, params);
	if (status) {
		BT_ERR("Unable to continue discovering, status %d", status);
		goto done;
	}

	bt_conn_unref(conn);
	return;

done:
	/* End of discovery */
	params->func(conn, NULL, params);

stop:
	conn->gatt_private = NULL;
	bt_conn_unref(conn);
}


int bt_gatt_read(struct bt_conn *conn, struct bt_gatt_read_params *params)
{
	struct nble_gattc_read_params req;

	if (!conn || !params || !params->handle_count || !params->func) {
		return -EINVAL;
	}

	if (conn->state != BT_CONN_CONNECTED) {
		return -ENOTCONN;
	}

	if (conn->gatt_private) {
		return -EBUSY;
	}

	if (params->handle_count > 1) {
		BT_ERR("Multiple characteristic read is not supported");
		return -ENOSYS;
	}

	BT_DBG("conn %p params %p", conn, params);

	req.conn_handle = conn->handle;
	req.handle = params->single.handle;
	req.offset = params->single.offset;

	/* TODO: Passing parameters with function not working now */
	conn->gatt_private = params;

	nble_gattc_read_req(&req);

	return 0;
}

void on_nble_gattc_read_rsp(const struct nble_gattc_read_rsp *rsp,
			    uint8_t *data, uint8_t len, void *user_data)
{
	struct bt_gatt_read_params *params;
	struct bt_conn *conn;

	if (rsp->status) {
		BT_ERR("GATT read failed, status %d", rsp->status);
		return;
	}

	conn = bt_conn_lookup_handle(rsp->conn_handle);
	if (!conn) {
		BT_ERR("Unable to find conn, handle 0x%04x", rsp->conn_handle);
		return;
	}

	/* TODO: Get params from user_data pointer, not working at the moment */
	params = conn->gatt_private;

	BT_DBG("conn %p params %p", conn, params);

	if (params->func(conn, 0, params, data, len) == BT_GATT_ITER_STOP) {
		goto done;
	}

	/*
	 * Core Spec 4.2, Vol. 3, Part G, 4.8.1
	 * If the Characteristic Value is greater than (ATT_MTU – 1) octets
	 * in length, the Read Long Characteristic Value procedure may be used
	 * if the rest of the Characteristic Value is required.
	 * The data contain only (ATT_MTU – 1) octets.
	 */
	if (len < BLE_GATT_MTU_SIZE) {
		params->func(conn, 0, params, NULL, 0);
		goto done;
	}

	params->single.offset += len;

	/* This pointer would keep new params set in the function below */
	conn->gatt_private = NULL;

	/* Continue reading the attribute */
	if (bt_gatt_read(conn, params)) {
		params->func(conn, BT_ATT_ERR_UNLIKELY, params, NULL, 0);
	}

done:
	conn->gatt_private = NULL;
	bt_conn_unref(conn);
}

int bt_gatt_write(struct bt_conn *conn, uint16_t handle, uint16_t offset,
		  const void *data, uint16_t length, bt_gatt_rsp_func_t func)
{
	struct nble_gattc_write_params req;

	if (!conn || !handle || !func) {
		return -EINVAL;
	}

	if (conn->state != BT_CONN_CONNECTED) {
		return -ENOTCONN;
	}

	if (conn->gatt_private) {
		return -EBUSY;
	}

	BT_DBG("conn %p handle 0x%04x offset 0x%04x len %u data %p",
	       conn, handle, offset, length, data);

	req.conn_handle = conn->handle;
	req.handle = handle;
	req.offset = offset;
	req.with_resp = 1;

	conn->gatt_private = func;

	nble_gattc_write_req(&req, data, length);

	return 0;
}

void on_nble_gattc_write_rsp(const struct nble_gattc_write_rsp *rsp,
			     void *user_data)
{
	struct bt_conn *conn;
	bt_gatt_rsp_func_t func;

	conn = bt_conn_lookup_handle(rsp->conn_handle);
	if (!conn) {
		BT_ERR("Unable to find conn, handle 0x%04x", rsp->conn_handle);
		return;
	}

	BT_DBG("conn %p status %d user_data %p", conn, rsp->status, user_data);

	func = conn->gatt_private;
	if (func) {
		func(conn, rsp->status);
		conn->gatt_private = NULL;
	}

	bt_conn_unref(conn);
}

int bt_gatt_write_without_response(struct bt_conn *conn, uint16_t handle,
				   const void *data, uint16_t length,
				   bool sign)
{
	struct nble_gattc_write_params req;

	if (!conn || !handle) {
		return -EINVAL;
	}

	if (conn->state != BT_CONN_CONNECTED) {
		return -ENOTCONN;
	}

	if (conn->gatt_private) {
		return -EBUSY;
	}

	BT_DBG("conn %p handle 0x%04x len %u data %p sign %d",
	       conn, handle, length, data, sign);

	/* TODO: Handle signing */

	req.conn_handle = conn->handle;
	req.handle = handle;
	req.offset = 0;
	req.with_resp = 0;

	nble_gattc_write_req(&req, data, length);

	return 0;
}

static void gatt_subscription_add(struct bt_conn *conn,
				  struct bt_gatt_subscribe_params *params)
{
	bt_addr_le_copy(&params->_peer, &conn->dst);

	/* Prepend subscription */
	params->_next = subscriptions;
	subscriptions = params;
}

static void gatt_subscription_remove(struct bt_conn *conn,
				     struct bt_gatt_subscribe_params *prev,
				     struct bt_gatt_subscribe_params *params)
{
	/* Remove subscription from the list*/
	if (!prev) {
		subscriptions = params->_next;
	} else {
		prev->_next = params->_next;
	}

	params->notify(conn, params, NULL, 0);
}

static void remove_subscriptions(struct bt_conn *conn)
{
	struct bt_gatt_subscribe_params *params, *prev;

	/* Lookup existing subscriptions */
	for (params = subscriptions, prev = NULL; params;
	     prev = params, params = params->_next) {
		if (bt_addr_le_cmp(&params->_peer, &conn->dst)) {
			continue;
		}

		/* Remove subscription */
		gatt_subscription_remove(conn, prev, params);
	}
}

static void gatt_write_ccc_rsp(struct bt_conn *conn, uint8_t err)
{
	BT_DBG("conn %p err %u", conn, err);

	/* TODO: Remove failed subscription */
}

static int gatt_write_ccc(struct bt_conn *conn,
			  struct bt_gatt_subscribe_params *params)
{
	uint16_t handle = params->ccc_handle;
	uint16_t value = params->value;

	return bt_gatt_write(conn, handle, 0, &value, sizeof(value),
			     gatt_write_ccc_rsp);
}

int bt_gatt_subscribe(struct bt_conn *conn,
		      struct bt_gatt_subscribe_params *params)
{
	struct bt_gatt_subscribe_params *tmp;
	bool has_subscription = false;

	if (!conn || !params || !params->notify ||
	    !params->value || !params->ccc_handle) {
		return -EINVAL;
	}

	if (conn->state != BT_CONN_CONNECTED) {
		return -ENOTCONN;
	}

	BT_DBG("conn %p value_handle 0x%04x ccc_handle 0x%04x value 0x%04x",
	       conn, params->value_handle, params->ccc_handle, params->value);

	/* Lookup existing subscriptions */
	for (tmp = subscriptions; tmp; tmp = tmp->_next) {
		/* Fail if entry already exists */
		if (tmp == params) {
			return -EALREADY;
		}

		/* Check if another subscription exists */
		if (!bt_addr_le_cmp(&tmp->_peer, &conn->dst) &&
		    tmp->value_handle == params->value_handle &&
		    tmp->value >= params->value) {
			has_subscription = true;
		}
	}

	/* Skip write if already subscribed */
	if (!has_subscription) {
		int err;

		err = gatt_write_ccc(conn, params);
		if (err) {
			return err;
		}
	}

	/*
	 * Add subscription before write complete as some implementation were
	 * reported to send notification before reply to CCC write.
	 */
	gatt_subscription_add(conn, params);

	return 0;
}

void on_nble_gattc_value_evt(const struct nble_gattc_value_evt *ev,
			     uint8_t *data, uint8_t length)
{
	struct bt_gatt_subscribe_params *params;
	struct bt_conn *conn;

	conn = bt_conn_lookup_handle(ev->conn_handle);
	if (!conn) {
		BT_ERR("Unable to find conn, handle 0x%04x", ev->conn_handle);
		return;
	}

	BT_DBG("conn %p value handle 0x%04x status %d data len %u",
	       conn, ev->handle, ev->status, length);

	for (params = subscriptions; params; params = params->_next) {
		if (ev->handle != params->value_handle) {
			continue;
		}

		if (params->notify(conn, params, data, length) ==
		    BT_GATT_ITER_STOP) {
			bt_gatt_unsubscribe(conn, params);
		}
	}

	bt_conn_unref(conn);
}

int bt_gatt_unsubscribe(struct bt_conn *conn,
			struct bt_gatt_subscribe_params *params)
{
	struct bt_gatt_subscribe_params *tmp, *found = NULL;
	bool has_subscription = false;

	if (!conn || !params) {
		return -EINVAL;
	}

	if (conn->state != BT_CONN_CONNECTED) {
		return -ENOTCONN;
	}

	BT_DBG("conn %p value_handle 0x%04x ccc_handle 0x%04x value 0x%04x",
	       conn, params->value_handle, params->ccc_handle, params->value);

	/* Check head */
	if (subscriptions == params) {
		subscriptions = params->_next;
		found = params;
	}

	/* Lookup existing subscriptions */
	for (tmp = subscriptions; tmp; tmp = tmp->_next) {
		/* Remove subscription */
		if (tmp->_next == params) {
			tmp->_next = params->_next;
			found = params;
		}

		/* Check if there still remains any other subscription */
		if (!bt_addr_le_cmp(&tmp->_peer, &conn->dst) &&
		    tmp->value_handle == params->value_handle) {
			has_subscription = true;
		}
	}

	if (!found) {
		return -EINVAL;
	}

	if (has_subscription) {
		return 0;
	}

	BT_DBG("Current subscription %p value_handle 0x%04x value 0x%04x",
	       found, found->value_handle, found->value);

	/* Remove subscription bit */
	params->value = found->value & ~params->value;

	return gatt_write_ccc(conn, params);
}

void bt_gatt_cancel(struct bt_conn *conn)
{
	BT_DBG("");
}

void on_nble_gatts_write_evt(const struct nble_gatt_wr_evt *ev,
			     const uint8_t *buf, uint8_t buflen)
{
	const struct bt_gatt_attr *attr = ev->attr;
	struct nble_gatts_wr_reply_params reply_data;

	BT_DBG("handle 0x%04x buf %p len %u", attr->handle, buf, buflen);

	if (attr->write) {
		reply_data.status = attr->write(NULL, attr, buf, buflen,
						ev->offset);
	} else {
		reply_data.status = -EINVAL;
	}

	if (ev->flag & NBLE_GATT_WR_FLAG_REPLY) {
		reply_data.conn_handle = ev->conn_handle;

		nble_gatts_wr_reply_req(&reply_data);
	}
}

void on_nble_gatts_read_evt(const struct nble_gatt_rd_evt *ev)
{
	struct nble_gatts_rd_reply_params reply_data;
	const struct bt_gatt_attr *attr;
	/* TODO: Replace the following with net_buf */
	uint8_t data[NBLE_BUF_SIZE];
	int len = 0;

	reply_data.status = -EACCES;
	memset(data, 0, sizeof(data));

	attr = ev->attr;

	BT_DBG("attr %p", attr);

	if (attr->read) {
		len = attr->read(NULL, attr, data, sizeof(data), ev->offset);
	}

	if (len >= 0) {
		reply_data.status = 0;
		reply_data.offset = ev->offset;
	} else {
		reply_data.status = len;
	}

	reply_data.conn_handle = ev->conn_handle;

	nble_gatts_rd_reply_req(&reply_data, data, len);
}

void bt_gatt_disconnected(struct bt_conn *conn)
{
	BT_DBG("conn %p", conn);

	conn->gatt_private = NULL;

	/* TODO: If bonded don't remove subscriptions */
	remove_subscriptions(conn);
}
