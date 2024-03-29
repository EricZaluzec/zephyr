/**
 * @file smp.h
 * Security Manager Protocol implementation header
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

struct bt_smp_hdr {
	uint8_t  code;
} __packed;

#define BT_SMP_ERR_PASSKEY_ENTRY_FAILED		0x01
#define BT_SMP_ERR_OOB_NOT_AVAIL		0x02
#define BT_SMP_ERR_AUTH_REQUIREMENTS		0x03
#define BT_SMP_ERR_CONFIRM_FAILED		0x04
#define BT_SMP_ERR_PAIRING_NOTSUPP		0x05
#define BT_SMP_ERR_ENC_KEY_SIZE			0x06
#define BT_SMP_ERR_CMD_NOTSUPP			0x07
#define BT_SMP_ERR_UNSPECIFIED			0x08
#define BT_SMP_ERR_REPEATED_ATTEMPTS		0x09
#define BT_SMP_ERR_INVALID_PARAMS		0x0a
#define BT_SMP_ERR_DHKEY_CHECK_FAILED		0x0b
#define BT_SMP_ERR_NUMERIC_COMP_FAILED		0x0c
#define BT_SMP_ERR_BREDR_PAIRING_IN_PROGRESS	0x0d
#define BT_SMP_ERR_CROSS_TRANSP_NOT_ALLOWED	0x0e

#define BT_SMP_IO_DISPLAY_ONLY			0x00
#define BT_SMP_IO_DISPLAY_YESNO			0x01
#define BT_SMP_IO_KEYBOARD_ONLY			0x02
#define BT_SMP_IO_NO_INPUT_OUTPUT		0x03
#define BT_SMP_IO_KEYBOARD_DISPLAY		0x04

#define BT_SMP_OOB_NOT_PRESENT			0x00
#define BT_SMP_OOB_PRESENT			0x01

#define BT_SMP_MIN_ENC_KEY_SIZE			7
#define BT_SMP_MAX_ENC_KEY_SIZE			16

#define BT_SMP_DIST_ENC_KEY			0x01
#define BT_SMP_DIST_ID_KEY			0x02
#define BT_SMP_DIST_SIGN			0x04
#define BT_SMP_DIST_LINK_KEY			0x08

#define BT_SMP_DIST_MASK			0x0f

#define BT_SMP_AUTH_NONE			0x00
#define BT_SMP_AUTH_BONDING			0x01
#define BT_SMP_AUTH_MITM			0x04
#define BT_SMP_AUTH_SC				0x08

#define BT_SMP_CMD_PAIRING_REQ			0x01
#define BT_SMP_CMD_PAIRING_RSP			0x02
struct bt_smp_pairing {
	uint8_t  io_capability;
	uint8_t  oob_flag;
	uint8_t  auth_req;
	uint8_t  max_key_size;
	uint8_t  init_key_dist;
	uint8_t  resp_key_dist;
} __packed;

#define BT_SMP_CMD_PAIRING_CONFIRM		0x03
struct bt_smp_pairing_confirm {
	uint8_t  val[16];
} __packed;

#define BT_SMP_CMD_PAIRING_RANDOM		0x04
struct bt_smp_pairing_random {
	uint8_t  val[16];
} __packed;

#define BT_SMP_CMD_PAIRING_FAIL			0x05
struct bt_smp_pairing_fail {
	uint8_t  reason;
} __packed;

#define BT_SMP_CMD_ENCRYPT_INFO			0x06
struct bt_smp_encrypt_info {
	uint8_t  ltk[16];
} __packed;

#define BT_SMP_CMD_MASTER_IDENT			0x07
struct bt_smp_master_ident {
	uint16_t ediv;
	uint64_t rand;
} __packed;

#define BT_SMP_CMD_IDENT_INFO			0x08
struct bt_smp_ident_info {
	uint8_t  irk[16];
} __packed;

#define BT_SMP_CMD_IDENT_ADDR_INFO		0x09
struct bt_smp_ident_addr_info {
	bt_addr_le_t addr;
} __packed;

#define BT_SMP_CMD_SIGNING_INFO			0x0a
struct bt_smp_signing_info {
	uint8_t csrk[16];
} __packed;

#define BT_SMP_CMD_SECURITY_REQUEST		0x0b
struct bt_smp_security_request {
	uint8_t  auth_req;
} __packed;

#define BT_SMP_CMD_PUBLIC_KEY			0x0c
struct bt_smp_public_key {
	uint8_t x[32];
	uint8_t y[32];
} __packed;

#define BT_SMP_DHKEY_CHECK			0x0d
struct bt_smp_dhkey_check {
	uint8_t e[16];
} __packed;

bool bt_smp_irk_matches(const uint8_t irk[16], const bt_addr_t *addr);
int bt_smp_create_rpa(const uint8_t irk[16], bt_addr_t *rpa);
int bt_smp_send_pairing_req(struct bt_conn *conn);
int bt_smp_send_security_req(struct bt_conn *conn);
void bt_smp_update_keys(struct bt_conn *conn);
bool bt_smp_get_tk(struct bt_conn *conn, uint8_t *tk);

void bt_smp_dhkey_ready(const uint8_t *dhkey);
void bt_smp_pkey_ready(const uint8_t *pkey);

int bt_smp_init(void);

int bt_smp_auth_passkey_entry(struct bt_conn *conn, unsigned int passkey);
int bt_smp_auth_passkey_confirm(struct bt_conn *conn);
int bt_smp_auth_cancel(struct bt_conn *conn);

/** brief Verify signed message
 *
 *  @param conn Bluetooth connection
 *  @param buf received packet buffer with message and signature
 *
 *  @return 0 in success, error code otherwise
 */
int bt_smp_sign_verify(struct bt_conn *conn, struct net_buf *buf);

/** brief Sign message
 *
 *  @param conn Bluetooth connection
 *  @param buf message buffer
 *
 *  @return 0 in success, error code otherwise
 */
int bt_smp_sign(struct bt_conn *conn, struct net_buf *buf);
