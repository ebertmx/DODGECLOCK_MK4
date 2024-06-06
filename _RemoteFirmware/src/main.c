/* main.c - Application main entry point */

/*
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/printk.h>
#include <sys/byteorder.h>
#include <zephyr.h>
#include <settings/settings.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>
#include <bluetooth/services/bas.h>
#include <bluetooth/conn.h>

#include "dclk_service.h"

#include <logging/log.h>

LOG_MODULE_REGISTER(main_app, LOG_LEVEL_INF);

static void bt_pairing_stop();
static void bt_pairing_start();
void bt_stop_advertise();
void bt_start_advertise();

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
				  BT_UUID_16_ENCODE(BT_UUID_HRS_VAL),
				  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL),
				  BT_UUID_16_ENCODE(BT_UUID_DIS_VAL))};

typedef struct bt_info
{
	/** number of active BLE connection */
	uint8_t num_paired;
	uint8_t num_conn;

	struct bt_conn *conn_list[CONFIG_BT_MAX_CONN];
	/** pairing status */
	bool pair_en;

} bt_info;

struct bt_info bt_dclk_info =
	{
		.num_conn = 0,
		.num_paired = 0,
		.pair_en = false};

static void bt_count_bonds(const struct bt_bond_info *info, void *user_data)
{
	int *bond_cnt = user_data;
	if ((*bond_cnt) < 0)
	{
		return;
	}
	(*bond_cnt)++;
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	LOG_INF("Pairing Complete: %d", bonded);

	bt_dclk_info.conn_list[bt_dclk_info.num_paired] = conn;
	bt_dclk_info.num_paired++;

	// if (bt_dclk_info.num_paired > CONFIG_BT_MAX_CONN)
	{
		bt_pairing_stop();
	}
}

bool check_paired_list(struct bt_conn *conn)
{

	for (int i = bt_dclk_info.num_paired; i > 0; i--)
	{

		if (conn == bt_dclk_info.conn_list[i])
		{
			LOG_INF("Connection verified");
			return true;
		}
	}
	LOG_INF("Connection not in paired list");

	return false;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err)
	{
		LOG_INF("Connection failed (err 0x%02x)\n", err);
	}
	else
	{
		bt_dclk_info.num_conn++;
		LOG_INF("Connected\n : %d", bt_dclk_info.num_conn);

		if (!bt_dclk_info.pair_en)
		{
			LOG_INF("Validating Connection");

			if (!check_paired_list(conn))
			{
				LOG_INF("Disconnecting, not valid connection");
				bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
			}
			else
			{
				bt_stop_advertise();
			}
		}
		else if (bt_dclk_info.num_paired >= CONFIG_BT_MAX_CONN)
		{
			LOG_INF("Disconnecting, max paired devices");
			bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		}
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected (reason 0x%02x)\n", reason);
	bt_dclk_info.num_conn--;

	if (bt_dclk_info.num_conn < bt_dclk_info.num_paired)
	{
		LOG_INF("Attempting to reestablish connection");
		bt_start_advertise();
	}
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
};

void bt_stop_advertise(void)
{

	int err = bt_le_adv_stop();
	if (err)
	{
		LOG_INF("Falied to terminate advertising");
	}
	LOG_INF("Advertising terminated");
}

void bt_start_advertise(void)
{
	int err;

	// LOG_INF("Bluetooth initialized\n");

	err = bt_le_adv_start(BT_LE_ADV_CONN_NAME, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err)
	{
		LOG_INF("Advertising failed to start (err %d)\n", err);
		return;
	}

	LOG_INF("Advertising successfully started\n");
}

static void auth_pairing(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	int err = bt_conn_auth_pairing_confirm(conn);
	LOG_INF("Pairing Authorized %d: %s\n", err, addr);
}

static void passkey_entry(struct bt_conn *conn)
{
	LOG_INF("Sending entry passkey = %d", 123456);
	bt_conn_auth_passkey_entry(conn, 123456);
}

void passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	LOG_INF("Controller passkey = %d", passkey);
}

void passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	LOG_INF("Confirm Passkey = %d", passkey);
	bt_conn_auth_passkey_confirm(conn);
}
static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing cancelled: %s\n", addr);
}

static struct bt_conn_auth_cb auth_cb_display = {
	.passkey_display = passkey_display,
	.passkey_confirm = passkey_confirm,
	.passkey_entry = passkey_entry,
	.cancel = auth_cancel,
	.pairing_confirm = auth_pairing,
	.pairing_complete = pairing_complete,
};

static void bt_pairing_start()
{
	LOG_INF("Pairing enabled");
	bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	bt_dclk_info.num_paired = 0;
	bt_dclk_info.pair_en = true;
	bt_start_advertise();
}

static void bt_pairing_stop()
{
	LOG_INF("Pairing disabled");
	bt_stop_advertise();
	bt_dclk_info.pair_en = false;
}

static void bas_notify(void)
{
	uint8_t battery_level = bt_bas_get_battery_level();

	battery_level--;

	if (!battery_level)
	{
		battery_level = 100U;
	}

	bt_bas_set_battery_level(bt_dclk_info.num_paired);
}

/*SHOT CLOCK*/

const uint32_t CLOCK_MAX_VALUE = 10000; // msec

static void dclk_expire_cb(struct k_timer *timer_id);
static void dclk_stop_cb(struct k_timer *timer_id);
K_TIMER_DEFINE(dclk_timer, &dclk_expire_cb, &dclk_stop_cb);

enum dclk_state_type
{
	RUN,
	PAUSE,
	STOP,
	EXPRIRE
};
// struct k_timer dclk_timer;
static enum dclk_state_type dclk_state = 0; // shot clock state
static uint32_t dclk_value = 0;

void dclk_start()
{
	k_timer_start(&dclk_timer, K_MSEC(CLOCK_MAX_VALUE), K_NO_WAIT);
	dclk_state = RUN;
}

static void dclk_pause()
{
	dclk_value = k_timer_remaining_get(&dclk_timer);
	k_timer_stop(&dclk_timer);
	dclk_state = PAUSE;
}

static void dclk_resume()
{
	if (dclk_state == PAUSE)
	{
		k_timer_start(&dclk_timer, K_MSEC(dclk_value), K_NO_WAIT);
	}
	else
	{
		// LOG_INF("Clock is not is pause state");
		k_timer_start(&dclk_timer, K_MSEC(CLOCK_MAX_VALUE), K_NO_WAIT);
	}
	dclk_state = RUN;
}

static void dclk_stop()
{
	k_timer_stop(&dclk_timer);
	dclk_state = STOP;
}

void dclk_notify()
{
	if (dclk_state != PAUSE)
	{
		dclk_value = k_timer_remaining_get(&dclk_timer);
	}

	bt_dclk_notify(dclk_value, dclk_state);
}

static void dclk_expire_cb(struct k_timer *timer_id)
{
	// dclk_start();
	dclk_state = EXPRIRE;
	dclk_value = 0;
	bt_dclk_notify(dclk_value, dclk_state);
}

static void dclk_stop_cb(struct k_timer *timer_id)
{
}

const uint16_t UPDATE_PERIOD = 1000;

void main(void)
{
	int err;
	LOG_INF("hello");
	err = bt_enable(NULL);
	if (err)
	{
		LOG_INF("Bluetooth init failed (err %d)\n", err);
		return;
	}

	if (IS_ENABLED(CONFIG_SETTINGS))
	{
		err = settings_load();
		if (err)
		{
			return err;
		}

		int bond_cnt = 0;

		bt_foreach_bond(BT_ID_DEFAULT, &bt_count_bonds, &bond_cnt);

		bt_dclk_info.num_paired = bond_cnt;
	}

	bt_conn_cb_register(&conn_callbacks);
	bt_conn_auth_cb_register(&auth_cb_display);

	// bt_start_advertise();

	// bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);

	bt_pairing_start();

	dclk_start();
	while (1)
	{
		k_sleep(K_MSEC(UPDATE_PERIOD));
		dclk_notify();
		bas_notify();
	}
}
