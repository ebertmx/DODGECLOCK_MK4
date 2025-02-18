/*Matthew Ebert
 *02-JAN-2024
 *
 */

/** @file DCLK.c
 *  @brief DCLK host Bluetooth Service
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#include <zephyr/sys/byteorder.h>
// #include <zephyr/kernel.h>

#include <zephyr/bluetooth/bluetooth.h>
// #include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/settings/settings.h>

#include "DCLK.h"

#define DLCK_LOG 1

#ifdef DLCK_LOG
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(Controller_app, LOG_LEVEL_INF);
#endif

static bool notify_state_enabled;
static bool notify_clock_enabled;
static uint8_t state;
static uint32_t clock;
static struct dclk_cb dclk_cb;

static dclk_info dclk_status =
	{
		.num_conn = 0,
		.pair_en = false,
};

static unsigned int passkey;

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

//| BT_LE_ADV_OPT_ONE_TIME,
#define BT_LE_ADV_CONN_NO_ACCEPT_LIST          \
	BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONNECTABLE, \
					BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL)

#define BT_LE_ADV_CONN_ACCEPT_LIST                                         \
	BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_FILTER_CONN, \
					BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL)

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),

};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_DCLK_VAL),
};

static void setup_accept_list_cb(const struct bt_bond_info *info, void *user_data)
{
	int *bond_cnt = user_data;

	if ((*bond_cnt) < 0)
	{
		return;
	}

	int err = bt_le_filter_accept_list_add(&info->addr);
	LOG_INF("Added following peer to accept list: %x %x\n", info->addr.a.val[0],
			info->addr.a.val[1]);
	if (err)
	{
		LOG_ERR("Cannot add peer to filter accept list (err: %d)\n", err);
		(*bond_cnt) = -EIO;
	}
	else
	{
		(*bond_cnt)++;
	}
}

// ADVERTISING
static int setup_accept_list(uint8_t local_id)
{
	int err = bt_le_filter_accept_list_clear();

	if (err)
	{
		LOG_INF("Cannot clear BLE accept list (err: %d)\n", err);
		return err;
	}

	int bond_cnt = 0;

	bt_foreach_bond(local_id, setup_accept_list_cb, &bond_cnt);

	return bond_cnt;
}

void pair_DCLK(struct k_work *work)
{
	LOG_INF("Pairing Beginning--");
	int err = 0;

	bt_le_adv_stop();

	err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	if (err)
	{
		LOG_INF("Cannot delete bond (err: %d)\n", err);
	}
	else
	{
		LOG_INF("Bond deleted succesfully \n");
	}

	LOG_INF("Advertising with no Accept list \n");
	// One shot advertizing due to bt_adv_param settings
	// stops upon connection
	err = bt_le_adv_start(BT_LE_ADV_CONN_NO_ACCEPT_LIST, ad, ARRAY_SIZE(ad), sd,
						  ARRAY_SIZE(sd));

	if (err)
	{
		LOG_INF("Advertising failed to start (err %d)\n", err);
		return;
	}
}

void advertise_DCLK(struct k_work *work)
{
	int err = 0;
	bt_le_adv_stop();

	int allowed_cnt = setup_accept_list(BT_ID_DEFAULT);
	LOG_DBG("bond_count = %d", allowed_cnt);
	if (allowed_cnt < 0)
	{
		LOG_INF("Acceptlist setup failed (err:%d)\n", allowed_cnt);
	}
	else if (allowed_cnt > 0)
	{
		LOG_INF("Acceptlist setup number  = %d \n", allowed_cnt);
		// One shot advertizing due to bt_adv_param settings
		// stops upon connection
		err = bt_le_adv_start(BT_LE_ADV_CONN_ACCEPT_LIST, ad, ARRAY_SIZE(ad), sd,
							  ARRAY_SIZE(sd));

		if (err)
		{
			LOG_INF("Advertising failed to start (err %d)\n", err);
			return;
		}
		LOG_INF("Advertising successfully started\n");
		return;
	}

	LOG_INF("No Bonds -- Please start pairing");
	return;
}

K_WORK_DEFINE(advertise_DCLK_work, advertise_DCLK);
K_WORK_DEFINE(pair_DCLK_work, pair_DCLK);

/*CONNECTION*/

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err)
	{
		LOG_INF("Connection failed (err %u)\n", err);
		k_work_submit(&advertise_DCLK_work);
		return;
	}

	LOG_INF("Connected\n");
	dclk_status.num_conn++;
	// bt_conn_set_security(conn, BT_SECURITY_L4);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected (reason %u)\n", reason);
	dclk_status.num_conn--;
	// advertize to try and reconnect
	k_work_submit(&advertise_DCLK_work);
}

static void on_security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err)
	{
		LOG_INF("Security changed: %s level %u\n", addr, level);
	}
	else
	{
		LOG_INF("Security failed: %s level %u err %d\n", addr, level, err);
	}
}

struct bt_conn_cb connection_callbacks = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.security_changed = on_security_changed,
};
/*



*/
/*NOTIFICATION AND STATE*/
/* state configuration change callback function */
static void dclk_ccc_state_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_state_enabled = (value == BT_GATT_CCC_NOTIFY);
}

/* clock configuration change callback function */
static void dclk_ccc_clock_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_clock_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static ssize_t read_state(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
						  uint16_t len, uint16_t offset)
{
	// get a pointer to state which is passed in the BT_GATT_CHARACTERISTIC() and stored in attr->user_data
	const char *value = attr->user_data;

	LOG_DBG("Attribute read, handle: %u, conn: %p", attr->handle, (void *)conn);

	if (dclk_cb.state_cb)
	{
		// Call the application callback function to update the get the current value of the button
		state = dclk_cb.state_cb();
		return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
	}

	return 0;
}

static ssize_t read_clock(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
						  uint16_t len, uint16_t offset)
{
	// get a pointer to state which is passed in the BT_GATT_CHARACTERISTIC() and stored in attr->user_data
	const char *value = attr->user_data;

	LOG_DBG("Attribute read, handle: %u, conn: %p", attr->handle, (void *)conn);

	if (dclk_cb.clock_cb)
	{
		// Call the application callback function to update the get the current value of the button
		clock = dclk_cb.clock_cb();
		return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
	}

	return 0;
}
/*



*/
/*AUTHENTICATION*/

static void auth_pairing(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	int err = bt_conn_auth_pairing_confirm(conn);
	LOG_INF("Pairing Authorized %d: %s\n", err, addr);
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing cancelled: %s\n", addr);
}

static void passkey_entry(struct bt_conn *conn)
{
	LOG_INF("Sending entry passkey = %d", passkey);
	bt_conn_auth_passkey_entry(conn, passkey);
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
static struct bt_conn_auth_cb auth_cb_display = {
	.passkey_display = passkey_display,
	.passkey_confirm = passkey_confirm,
	.passkey_entry = passkey_entry,
	.cancel = auth_cancel,
	.pairing_confirm = auth_pairing,
};

/*


*/
/*PAIRING*/
#ifdef DCLK_INFO
static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_WRN("Pairing failed conn: %s, reason %d", addr, reason);
}

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};
#endif
/*



*/
/* DCLK Service Declaration */
BT_GATT_SERVICE_DEFINE(
	dclk_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DCLK),
	/*Button characteristic declaration */
	BT_GATT_CHARACTERISTIC(BT_UUID_DCLK_STATE, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
						   BT_GATT_PERM_READ_AUTHEN, read_state, NULL, &state),
	/* Client Characteristic Configuration Descriptor */
	BT_GATT_CCC(dclk_ccc_state_cfg_changed, BT_GATT_PERM_READ_AUTHEN | BT_GATT_PERM_WRITE_AUTHEN),

	BT_GATT_CHARACTERISTIC(BT_UUID_DCLK_CLOCK, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
						   BT_GATT_PERM_READ_AUTHEN, read_clock, NULL, &clock),

	BT_GATT_CCC(dclk_ccc_clock_cfg_changed, BT_GATT_PERM_READ_AUTHEN | BT_GATT_PERM_WRITE_AUTHEN),

);

/*


*/

/*API*/
int dclk_init(struct dclk_cb *callbacks)
{
	int err;

	bt_conn_cb_register(&connection_callbacks);
	if (callbacks)
	{
		dclk_cb.clock_cb = callbacks->clock_cb;
		dclk_cb.state_cb = callbacks->state_cb;
	}
	err = bt_enable(NULL);
	if (err)
	{
		LOG_ERR("Bluetooth enable failed (err %d)\n", err);
		return err;
	}

	err = bt_conn_auth_cb_register(&auth_cb_display);
	if (err)
	{
		LOG_ERR("Bluetooth authetication register failed (err %d)\n", err);
		return err;
	}
#ifdef DCLK_INFO
	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err)
	{
		LOG_INF("Bluetooth info register failed (err %d)\n", err);
		return 0;
	}
#endif

	if (IS_ENABLED(CONFIG_SETTINGS))
	{
		err = settings_load();
		if (err)
		{
			LOG_ERR("Bluetooth load settings failed (err %d)\n", err);
			return err;
		}
		LOG_INF("BLE settings loaded \n");
	}

	// err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd,
	// 					  ARRAY_SIZE(sd));

	// if (err)
	// {
	// 	err = -1;
	// }
	k_work_submit(&advertise_DCLK_work);

	return 0;
}

int dclk_pairing(bool enable)
{

	dclk_status.pair_en = enable;
	if (enable)
	{
		k_work_submit(&pair_DCLK_work);
	}
	else
	{
		k_work_submit(&advertise_DCLK_work);
	}

	return 0;
}

int dclk_send_state_notify(uint8_t *state)
{
	if (!notify_state_enabled)
	{
		return -EACCES;
	}
	
	return bt_gatt_notify(NULL, &dclk_svc.attrs[2], state, sizeof(*state));
}

int dclk_send_clock_notify(uint32_t *clock)
{
	if (!notify_clock_enabled)
	{
		return -EACCES;
	}
	//LOG_INF("clock_notify = %d  l=%d", *clock, sizeof(*clock));
	return bt_gatt_notify(NULL, &dclk_svc.attrs[5], clock, sizeof(*clock));
}

int dclk_get_status(struct dclk_info *status)
{
	status->num_conn = dclk_status.num_conn;
	status->pair_en = dclk_status.pair_en;
	return 0;
}
