#include <zephyr/types.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/init.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>
#include <zmk/battery.h>

// Custom BLE UUIDs
// Status Service:        5F3A0000-D0E1-4D9A-8E40-12866D57AA42
// Layer Status Chrc:     5F3A0001-D0E1-4D9A-8E40-12866D57AA42
// Split Link Chrc:       5F3A0002-D0E1-4D9A-8E40-12866D57AA42
// Peripheral Battery:    5F3A0003-D0E1-4D9A-8E40-12866D57AA42

#define BT_UUID_ZMK_STATUS_VAL \
    BT_UUID_128_ENCODE(0x5F3A0000, 0xD0E1, 0x4D9A, 0x8E40, 0x12866D57AA42)
#define BT_UUID_ZMK_STATUS_LAYER_VAL \
    BT_UUID_128_ENCODE(0x5F3A0001, 0xD0E1, 0x4D9A, 0x8E40, 0x12866D57AA42)
#define BT_UUID_ZMK_STATUS_SPLIT_VAL \
    BT_UUID_128_ENCODE(0x5F3A0002, 0xD0E1, 0x4D9A, 0x8E40, 0x12866D57AA42)
#define BT_UUID_ZMK_STATUS_PERIPH_BAT_VAL \
    BT_UUID_128_ENCODE(0x5F3A0003, 0xD0E1, 0x4D9A, 0x8E40, 0x12866D57AA42)

static struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(BT_UUID_ZMK_STATUS_VAL);
static struct bt_uuid_128 layer_uuid  = BT_UUID_INIT_128(BT_UUID_ZMK_STATUS_LAYER_VAL);
static struct bt_uuid_128 split_uuid  = BT_UUID_INIT_128(BT_UUID_ZMK_STATUS_SPLIT_VAL);
static struct bt_uuid_128 periph_bat_uuid = BT_UUID_INIT_128(BT_UUID_ZMK_STATUS_PERIPH_BAT_VAL);

static uint8_t active_layer      = 0;
static uint8_t split_connected   = 0;
static uint8_t peripheral_battery = 0;

// --- Read callbacks ---

static ssize_t read_layer(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &active_layer, sizeof(active_layer));
}

static ssize_t read_split(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &split_connected, sizeof(split_connected));
}

static ssize_t read_periph_bat(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &peripheral_battery, sizeof(peripheral_battery));
}

static void status_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    // Invoked when a client subscribes or unsubscribes
}

// GATT Service definition
// Indices (0-based):
//   0: Primary Service
//   1: Layer Characteristic declaration
//   2: Layer Characteristic value       <-- notify target
//   3: Layer CCC descriptor
//   4: Split Characteristic declaration
//   5: Split Characteristic value       <-- notify target
//   6: Split CCC descriptor
//   7: Peripheral Battery decl
//   8: Peripheral Battery value         <-- notify target
//   9: Peripheral Battery CCC
BT_GATT_SERVICE_DEFINE(zmk_status_svc,
    BT_GATT_PRIMARY_SERVICE(&status_uuid),

    // Active Layer (index 2 = value attribute)
    BT_GATT_CHARACTERISTIC(&layer_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           read_layer, NULL, &active_layer),
    BT_GATT_CCC(status_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    // Split Connection Status (index 5 = value attribute)
    BT_GATT_CHARACTERISTIC(&split_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           read_split, NULL, &split_connected),
    BT_GATT_CCC(status_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    // Peripheral Battery Level (index 8 = value attribute)
    BT_GATT_CHARACTERISTIC(&periph_bat_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           read_periph_bat, NULL, &peripheral_battery),
    BT_GATT_CCC(status_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
);

// --- Event Handlers ---

static int status_event_handler(const zmk_event_t *eh) {
    // Layer changed
    const struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);
    if (layer_ev) {
        active_layer = zmk_keymap_highest_active_layer();
        bt_gatt_notify(NULL, &zmk_status_svc.attrs[2], &active_layer, sizeof(active_layer));
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Split peripheral connect / disconnect
    const struct zmk_split_peripheral_status_changed *split_ev =
        as_zmk_split_peripheral_status_changed(eh);
    if (split_ev) {
        split_connected = split_ev->connected ? 1 : 0;
        if (!split_ev->connected) {
            // Reset peripheral battery when right half disconnects
            peripheral_battery = 0;
            bt_gatt_notify(NULL, &zmk_status_svc.attrs[8], &peripheral_battery, sizeof(peripheral_battery));
        }
        bt_gatt_notify(NULL, &zmk_status_svc.attrs[5], &split_connected, sizeof(split_connected));
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Peripheral (right half) battery level update
    const struct zmk_peripheral_battery_state_changed *periph_bat_ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (periph_bat_ev) {
        peripheral_battery = periph_bat_ev->state_of_charge;
        bt_gatt_notify(NULL, &zmk_status_svc.attrs[8], &peripheral_battery, sizeof(peripheral_battery));
        return ZMK_EV_EVENT_BUBBLE;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(status_listener, status_event_handler);
ZMK_SUBSCRIPTION(status_listener, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(status_listener, zmk_split_peripheral_status_changed);
ZMK_SUBSCRIPTION(status_listener, zmk_peripheral_battery_state_changed);
