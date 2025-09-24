#include "mqtt_outbox.h"
#include "mqtt_config.h"
#include "esp_log.h"
#include "ED_mqtt_qos1_queue.h"
#include "mqtt_msg.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "outbox";

/* Minimal static ring for non-QoS1 messages */
#define OUTBOX_RING_CAP 8

struct outbox_item
{
    outbox_message_t msg;
    pending_state_t state;
    outbox_tick_t tick;
    bool in_use;
};

struct outbox_t
{
    struct outbox_item ring[OUTBOX_RING_CAP];
    size_t size;
    esp_mqtt_client_handle_t client;
};

static struct outbox_t g_outbox;

outbox_handle_t outbox_init(esp_mqtt_client_handle_t client)
{
    memset(&g_outbox, 0, sizeof(g_outbox));
    g_outbox.client = client;
    /* Initialise QoS1 queue with dynamic slots enabled */
    mqtt_qos1q_init();

    ESP_LOGI(TAG, "Outbox initialised (QoS1 queue owned by client, allow_dynamic=1)");
    return &g_outbox;
}

outbox_item_handle_t outbox_enqueue(outbox_handle_t outbox,
                                    outbox_message_handle_t message,
                                    outbox_tick_t tick)
{
    if (message->msg_qos == 1 && message->msg_type == MQTT_MSG_TYPE_PUBLISH) {
        ESP_LOGE(TAG, "QoS1 should not enter outbox; check call path");
        return NULL;
    }

    // QoS0/QoS2/control messages → store in static ring
    for (int i = 0; i < OUTBOX_RING_CAP; ++i) {
        if (!g_outbox.ring[i].in_use) {
            g_outbox.ring[i].msg    = *message;
            g_outbox.ring[i].state  = QUEUED;
            g_outbox.ring[i].tick   = tick;
            g_outbox.ring[i].in_use = true;
            g_outbox.size += message->len + message->remaining_len;
            return &g_outbox.ring[i];
        }
    }

    ESP_LOGW(TAG, "Outbox ring full — dropping oldest control message");
    g_outbox.ring[0].msg    = *message;
    g_outbox.ring[0].state  = QUEUED;
    g_outbox.ring[0].tick   = tick;
    g_outbox.ring[0].in_use = true;
    return &g_outbox.ring[0];
}



outbox_item_handle_t outbox_get(outbox_handle_t outbox, int msg_id)
{
    for (int i = 0; i < OUTBOX_RING_CAP; ++i)
    {
        if (g_outbox.ring[i].in_use && g_outbox.ring[i].msg.msg_id == msg_id)
        {
            return &g_outbox.ring[i];
        }
    }
    return NULL;
}

outbox_item_handle_t outbox_dequeue(outbox_handle_t outbox,
                                    pending_state_t pending,
                                    outbox_tick_t *tick)
{
    for (int i = 0; i < OUTBOX_RING_CAP; ++i)
    {
        if (g_outbox.ring[i].in_use && g_outbox.ring[i].state == pending)
        {
            if (tick)
                *tick = g_outbox.ring[i].tick;
            return &g_outbox.ring[i];
        }
    }
    return NULL;
}

esp_err_t outbox_delete_item(outbox_handle_t outbox,
                             outbox_item_handle_t item_to_delete)
{
    if (!item_to_delete) {
        return ESP_OK;
    }

    struct outbox_item *item = (struct outbox_item *)item_to_delete;

    if (item->in_use) {
        // 🔧 decrement size accounting
        if (outbox) {
            outbox->size -= item->msg.len + item->msg.remaining_len;
            if (outbox->size < 0) {
                ESP_LOGW(TAG, "Outbox size underflow detected, clamping to 0");
                outbox->size = 0;
            }
        }
        item->in_use = false;
    }

    return ESP_OK;
}


uint8_t *outbox_item_get_data(outbox_item_handle_t item,
                              size_t *len, uint16_t *msg_id,
                              int *msg_type, int *qos)
{
    if (!item)
        return NULL;
    struct outbox_item *it = (struct outbox_item *)item;
    if (len)
        *len = it->msg.len;
    if (msg_id)
        *msg_id = it->msg.msg_id;
    if (msg_type)
        *msg_type = it->msg.msg_type;
    if (qos)
        *qos = it->msg.msg_qos;
    return it->msg.data;
}

esp_err_t outbox_delete(outbox_handle_t outbox, int msg_id, int msg_type)
{
    if (msg_type == MQTT_MSG_TYPE_PUBLISH) {
        mqtt_qos1q_on_published(msg_id);
    }

    outbox_item_handle_t it = outbox_get(outbox, msg_id);
    if (it) {
        return outbox_delete_item(outbox, it);  // ✅ reuse accounting logic
    }

    return ESP_OK;
}

esp_err_t outbox_set_pending(outbox_handle_t outbox,
                             int msg_id, pending_state_t pending)
{
    outbox_item_handle_t it = outbox_get(outbox, msg_id);
    if (it)
        ((struct outbox_item *)it)->state = pending;
    return ESP_OK;
}

pending_state_t outbox_item_get_pending(outbox_item_handle_t item)
{
    if (!item)
        return QUEUED;
    return ((struct outbox_item *)item)->state;
}

esp_err_t outbox_set_tick(outbox_handle_t outbox,
                          int msg_id, outbox_tick_t tick)
{
    outbox_item_handle_t it = outbox_get(outbox, msg_id);
    if (it)
        ((struct outbox_item *)it)->tick = tick;
    return ESP_OK;
}

int outbox_delete_single_expired(outbox_handle_t outbox,
                                 outbox_tick_t current_tick,
                                 outbox_tick_t timeout)
{
    mqtt_qos1q_check_timeouts();

    for (int i = 0; i < OUTBOX_RING_CAP; ++i) {
        if (g_outbox.ring[i].in_use &&
            (current_tick - g_outbox.ring[i].tick) > timeout) {

            int id = g_outbox.ring[i].msg.msg_id;
            // ✅ reuse accounting logic
            outbox_delete_item(outbox, &g_outbox.ring[i]);
            return id;
        }
    }
    return -1;
}


int outbox_delete_expired(outbox_handle_t outbox,
                          outbox_tick_t current_tick,
                          outbox_tick_t timeout)
{
    /* Let the QoS1 queue handle its own timeouts */
    mqtt_qos1q_check_timeouts();

    int removed = 0;
    for (int i = 0; i < OUTBOX_RING_CAP; ++i) {
        if (g_outbox.ring[i].in_use &&
            (current_tick - g_outbox.ring[i].tick) > timeout) {

            // ✅ reuse accounting logic
            outbox_delete_item(outbox, &g_outbox.ring[i]);
            ++removed;
        }
    }
    return removed;
}


size_t outbox_get_size(outbox_handle_t outbox)
{
    return outbox->size;
}


void outbox_delete_all_items(outbox_handle_t outbox)
{
    /* Clear both the static ring and the QoS1 queue */
    memset(&g_outbox, 0, sizeof(g_outbox));
    mqtt_qos1q_clear_all();
}

void outbox_destroy(outbox_handle_t outbox)
{
    outbox_delete_all_items(outbox);
}
