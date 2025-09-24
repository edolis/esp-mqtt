#include "ED_mqtt_qos1_queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

// Size of each dynamic block (number of slots per block)
// #define DYN_BLOCK_SLOT_COUNT   3
// Max number of dynamic blocks allowed
#define MAX_DYNAMIC_BLOCKS 8
// Idle timeout in milliseconds before freeing an unused dynamic block
#define DYN_BLOCK_IDLE_TIMEOUT_MS 60000 // adjust as needed
// static esp_mqtt_client_handle_t s_client = NULL;
extern esp_mqtt_client_handle_t ED_MQTT_client_handle; // expose from ED_mqtt.cpp
typedef struct
{
    MqttSlot *slots;         // array of DYNAMIC_SLOT_COUNT slots
    char *topics_block;      // DYNAMIC_SLOT_COUNT * TOPIC_MAX
    char *payloads_block;    // DYNAMIC_SLOT_COUNT * PAYLOAD_MAX
    bool in_use;             // block has at least one active slot
    uint64_t last_active_us; // last time all slots were free
} DynBlock;

static DynBlock *dynamic_blocks[MAX_DYNAMIC_BLOCKS] = {0};
static int dynamic_block_count = 0;

static const char *TAG = "MQTT_QOS1Q";

static TimerHandle_t timeout_timer = NULL;

// Forward decls for helper functions (replaces lambda)
static void sweep_slots(MqttSlot *pool, int count, uint64_t now_us, uint64_t thresh_us);
// static void mqtt_alloc_dynamic_pool(void);
// static void mqtt_free_dynamic_pool_if_empty(void);
static void log_qos1_queue_stats(void);

static MqttSlot static_slots[STATIC_SLOT_COUNT];
static char static_topics[STATIC_SLOT_COUNT][TOPIC_MAX];
static char static_payloads[STATIC_SLOT_COUNT][PAYLOAD_MAX];

// static MqttSlot *dynamic_slots = NULL;
// static char *dynamic_topics_block = NULL;
// static char *dynamic_payloads_block = NULL;

static size_t diag_max_burst = 0;
static size_t diag_max_payload_len = 0;
static size_t diag_timeout_count = 0;

static inline uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }

static DynBlock *alloc_dynamic_block(void)
{
    if (dynamic_block_count >= MAX_DYNAMIC_BLOCKS)
    {
        ESP_LOGW(TAG, "[DYN] max blocks reached (%d)", MAX_DYNAMIC_BLOCKS);
        return NULL;
    }
    DynBlock *blk = (DynBlock *)calloc(1, sizeof(DynBlock));
    if (!blk)
    {
        ESP_LOGE(TAG, "Failed to allocate DynBlock struct");
        return NULL;
    }

    blk->slots = (MqttSlot *)calloc(DYNAMIC_SLOT_COUNT, sizeof(MqttSlot));
    blk->topics_block = (char *)malloc(DYNAMIC_SLOT_COUNT * TOPIC_MAX);
    blk->payloads_block = (char *)malloc(DYNAMIC_SLOT_COUNT * PAYLOAD_MAX);

    if (!blk->slots || !blk->topics_block || !blk->payloads_block)
    {
        ESP_LOGE(TAG, "Failed to allocate dynamic block buffers");
        free(blk->payloads_block);
        free(blk->topics_block);
        free(blk->slots);
        free(blk);
        return NULL;
    }

    // Wire slot pointers into this block’s contiguous buffers
    for (int i = 0; i < DYNAMIC_SLOT_COUNT; ++i)
    {
        blk->slots[i].topic = blk->topics_block + i * TOPIC_MAX;
        blk->slots[i].payload = blk->payloads_block + i * PAYLOAD_MAX;
        blk->slots[i].msg_id = -1;
        blk->slots[i].in_use = false;
    }

    blk->in_use = false;
    blk->last_active_us = 0;

    dynamic_blocks[dynamic_block_count++] = blk;
    ESP_LOGI(TAG, "Allocated dynamic block %d (%d slots)", dynamic_block_count, DYNAMIC_SLOT_COUNT);
    return blk;
}

static void free_dynamic_block_at_index(int idx)
{
    DynBlock *blk = dynamic_blocks[idx];
    if (!blk)
        return;

    free(blk->payloads_block);
    free(blk->topics_block);
    free(blk->slots);
    free(blk);
    // Compact the array
    for (int j = idx; j < dynamic_block_count - 1; ++j)
    {
        dynamic_blocks[j] = dynamic_blocks[j + 1];
    }
    dynamic_blocks[dynamic_block_count - 1] = NULL;
    dynamic_block_count--;
    ESP_LOGI(TAG, "Freed dynamic block at idx=%d, remaining=%d", idx, dynamic_block_count);
}

static bool block_all_slots_free(const DynBlock *blk)
{
    for (int i = 0; i < DYNAMIC_SLOT_COUNT; ++i)
    {
        if (blk->slots[i].in_use)
            return false;
    }
    return true;
}

static void diag_update_burst(void)
{
    size_t in_use_count = 0;

    for (int i = 0; i < STATIC_SLOT_COUNT; ++i)
        if (static_slots[i].in_use)
            ++in_use_count;

    for (int b = 0; b < dynamic_block_count; ++b)
    {
        DynBlock *blk = dynamic_blocks[b];
        for (int s = 0; s < DYNAMIC_SLOT_COUNT; ++s)
            if (blk->slots[s].in_use)
                ++in_use_count;
    }

    if (in_use_count > diag_max_burst)
        diag_max_burst = in_use_count;
}

static void diag_update_payload_len(size_t len)
{
    if (len > diag_max_payload_len)
        diag_max_payload_len = len;
}

static void diag_inc_timeout(void) { ++diag_timeout_count; }

static void timeout_timer_cb(TimerHandle_t xTimer)
{
    static TickType_t last = 0; // persist across calls
    TickType_t now_ticks = xTaskGetTickCount();
    if (now_ticks - last > pdMS_TO_TICKS(20000))
    {
        mqtt_qos1q_log_diagnostics();
        last = now_ticks;
    }
    mqtt_qos1q_check_timeouts();
}


void mqtt_qos1q_init(void)
{

    // clear static slots
    for (int i = 0; i < STATIC_SLOT_COUNT; ++i) {
        static_slots[i].in_use = false;
        static_slots[i].msg_id = -1;
    }

    // free any dynamic blocks
    for (int b = dynamic_block_count - 1; b >= 0; --b) {
        free_dynamic_block_at_index(b);
    }

    diag_max_burst = 0;
    diag_max_payload_len = 0;
    diag_timeout_count = 0;

    ESP_LOGI(TAG, "QoS1 queue initialized (client handle stored)");
}


static void log_qos1_queue_stats(void)
{
    int static_used = 0, static_free = 0;
    int dynamic_used = 0, dynamic_free = 0;

    // Static slots
    for (int i = 0; i < STATIC_SLOT_COUNT; ++i)
    {
        if (static_slots[i].in_use)
        {
            static_used++;
            ESP_LOGI(TAG, "[STAT%d] msg_id=%d", i + 1, static_slots[i].msg_id);
        }
        else
        {
            static_free++;
            ESP_LOGI(TAG, "[STAT%d] msg_id=na", i + 1);
        }
    }

    // Dynamic blocks
    for (int b = 0; b < dynamic_block_count; ++b)
    {
        DynBlock *blk = dynamic_blocks[b];
        for (int s = 0; s < DYNAMIC_SLOT_COUNT; ++s)
        {
            if (blk->slots[s].in_use)
            {
                dynamic_used++;
                ESP_LOGI(TAG, "{DYN%d.%d} msg_id=%d", b, s + 1, blk->slots[s].msg_id);
            }
            else
            {
                dynamic_free++;
                ESP_LOGI(TAG, "{DYN%d.%d} msg_id=na", b, s + 1);
            }
        }
    }

    ESP_LOGI(TAG, "Static slots: %d used / %d free, Dynamic slots: %d used / %d free (blocks=%d)",
             static_used, static_free, dynamic_used, dynamic_free, dynamic_block_count);
}

int mqtt_qos1q_track(const char *topic, size_t topic_len,
                     const char *payload, size_t payload_len,
                     bool retain,
                     int msg_id)
{
    if (!topic || (!payload && payload_len > 0)) {
        ESP_LOGE(TAG, "[QOS1Q] track: invalid args");
        return -1;
    }

    // Clamp lengths to slot buffer sizes
    if (topic_len >= TOPIC_MAX) {
        ESP_LOGW(TAG, "[QOS1Q] topic length %u exceeds max %u, clamping",
                 (unsigned)topic_len, (unsigned)(TOPIC_MAX - 1));
        topic_len = TOPIC_MAX - 1;
    }
    if (payload_len >= PAYLOAD_MAX) {
        ESP_LOGW(TAG, "[QOS1Q] payload length %u exceeds max %u, clamping",
                 (unsigned)payload_len, (unsigned)(PAYLOAD_MAX - 1));
        payload_len = PAYLOAD_MAX - 1;
    }

    // Hygiene sweep before enqueue
    mqtt_qos1q_check_timeouts();

    // Pick a slot
    MqttSlot *slot = find_slot_or_drop_oldest();
    if (!slot) {
        ESP_LOGE(TAG, "[QOS1Q] no slot available");
        return -2;
    }

    // Fill slot
    memcpy(slot->topic,   topic,   topic_len);
    slot->topic[topic_len] = '\0';
    memcpy(slot->payload, payload, payload_len);
    slot->payload[payload_len] = '\0';

    slot->topic_len    = (uint16_t)topic_len;
    slot->payload_len  = (uint16_t)payload_len;
    slot->in_use       = true;
    slot->timestamp_us = now_us();
    slot->msg_id       = msg_id;
    slot->retain       = retain;

    // Diagnostics
    diag_update_burst();
    diag_update_payload_len(payload_len);
    ESP_LOGI(TAG, "[QOS1Q] Tracked QoS1 msg_id=%d topic='%s' payload_len=%u",
             msg_id, slot->topic, (unsigned)payload_len);
    log_qos1_queue_stats();

    return msg_id;
}

void mqtt_qos1q_rebind_msg_id(int provisional_id, int final_id)
{
    if (provisional_id <= 0 || final_id <= 0 || provisional_id == final_id)
        return;

    // Static
    for (int i = 0; i < STATIC_SLOT_COUNT; ++i) {
        if (static_slots[i].in_use && static_slots[i].msg_id == provisional_id) {
            static_slots[i].msg_id = final_id;
            ESP_LOGI(TAG, "Rebound msg_id %d -> %d (static idx=%d)", provisional_id, final_id, i);
            return;
        }
    }

    // Dynamic blocks
    for (int b = 0; b < dynamic_block_count; ++b) {
        DynBlock *blk = dynamic_blocks[b];
        for (int s = 0; s < DYNAMIC_SLOT_COUNT; ++s) {
            if (blk->slots[s].in_use && blk->slots[s].msg_id == provisional_id) {
                blk->slots[s].msg_id = final_id;
                ESP_LOGI(TAG, "Rebound msg_id %d -> %d (dyn block=%d slot=%d)",
                         provisional_id, final_id, b, s);
                return;
            }
        }
    }

    ESP_LOGW(TAG, "Rebind miss: provisional_id=%d not found to rebind to %d", provisional_id, final_id);
}


// static void mqtt_alloc_dynamic_pool(void) {
//   int i;

//   if (dynamic_slots)
//     return;

//   dynamic_slots = (MqttSlot *)calloc(DYNAMIC_SLOT_COUNT, sizeof(MqttSlot));
//   dynamic_topics_block = (char *)malloc(DYNAMIC_SLOT_COUNT * TOPIC_MAX);
//   dynamic_payloads_block = (char *)malloc(DYNAMIC_SLOT_COUNT * PAYLOAD_MAX);

//   if (!dynamic_slots || !dynamic_topics_block || !dynamic_payloads_block) {
//     ESP_LOGE(TAG, "Failed to allocate dynamic pool");
//     free(dynamic_slots);           dynamic_slots = NULL;
//     free(dynamic_topics_block);    dynamic_topics_block = NULL;
//     free(dynamic_payloads_block);  dynamic_payloads_block = NULL;
//     return;
//   }

//   for (i = 0; i < DYNAMIC_SLOT_COUNT; ++i) {
//     dynamic_slots[i].topic   = dynamic_topics_block + i * TOPIC_MAX;
//     dynamic_slots[i].payload = dynamic_payloads_block + i * PAYLOAD_MAX;
//     dynamic_slots[i].msg_id  = -1;
//   }

//   ESP_LOGI(TAG, "Allocated dynamic overflow pool (%d slots)", DYNAMIC_SLOT_COUNT);
// }

// static void mqtt_free_dynamic_pool_if_empty(void) {
//   int i;

//   if (!dynamic_slots)
//     return;

//   for (i = 0; i < DYNAMIC_SLOT_COUNT; ++i)
//     if (dynamic_slots[i].in_use)
//       return;

//   free(dynamic_payloads_block);  dynamic_payloads_block = NULL;
//   free(dynamic_topics_block);    dynamic_topics_block = NULL;
//   free(dynamic_slots);           dynamic_slots = NULL;

//   ESP_LOGI(TAG, "Freed dynamic overflow pool");
// }

void mqtt_qos1q_check_timeouts(void)
{
    uint64_t now = now_us();
    uint64_t thresh_us = (uint64_t)ACK_TIMEOUT_MS * 1000ULL;

    // Sweep static slots
    sweep_slots(static_slots, STATIC_SLOT_COUNT, now, thresh_us);

    // Sweep dynamic slots per block
    for (int b = 0; b < dynamic_block_count; ++b)
    {
        DynBlock *blk = dynamic_blocks[b];
        sweep_slots(blk->slots, DYNAMIC_SLOT_COUNT, now, thresh_us);
        // If the block is now completely free, remember when it became idle
        if (block_all_slots_free(blk))
        {
            if (!blk->last_active_us)
                blk->last_active_us = now;
            blk->in_use = false;
        }
        else
        {
            blk->last_active_us = 0; // active again
            blk->in_use = true;
        }
    }

    // Free blocks that have been idle for the configured timeout
    for (int b = 0; b < dynamic_block_count; /* advance inside */)
    {
        DynBlock *blk = dynamic_blocks[b];
        if (blk && block_all_slots_free(blk) &&
            blk->last_active_us &&
            (now - blk->last_active_us) > ((uint64_t)DYN_BLOCK_IDLE_TIMEOUT_MS * 1000ULL))
        {
            ESP_LOGI(TAG, "Freeing idle dynamic block %d", b);
            free_dynamic_block_at_index(b);
            // do not increment b; array now shifted
        }
        else
        {
            ++b;
        }
    }
}

static void sweep_slots(MqttSlot *pool, int count, uint64_t now, uint64_t thresh_us)
{
    int i;
    for (i = 0; i < count; ++i)
    {
        if (pool[i].in_use && (now - pool[i].timestamp_us) > thresh_us)
        {
            ESP_LOGW(TAG, "Timeout msg_id=%d, freeing slot", pool[i].msg_id);
            pool[i].in_use = false;
            pool[i].msg_id = -1;
            diag_inc_timeout();
        }
    }
}

void mqtt_qos1q_on_published(int msg_id)
{
    int i;

    for (i = 0; i < STATIC_SLOT_COUNT; ++i)
    {
        if (static_slots[i].in_use && static_slots[i].msg_id == msg_id)
        {
            static_slots[i].in_use = false;
            static_slots[i].msg_id = -1;
            ESP_LOGI(TAG, "ACK msg_id=%d (static)", msg_id);
            return;
        }
    }

    if (dynamic_block_count > 0)
    {
        for (int b = 0; b < dynamic_block_count; ++b)
        {
            DynBlock *blk = dynamic_blocks[b];
            for (int s = 0; s < DYNAMIC_SLOT_COUNT; ++s)
            {
                if (blk->slots[s].in_use && blk->slots[s].msg_id == msg_id)
                {
                    blk->slots[s].in_use = false;
                    blk->slots[s].msg_id = -1;
                    ESP_LOGI(TAG, "ACK msg_id=%d (dynamic block=%d slot=%d)", msg_id, b, s);
                    // If all slots free, mark block idle time
                    if (block_all_slots_free(blk))
                    {
                        blk->in_use = false;
                        blk->last_active_us = now_us();
                    }
                    return;
                }
            }
        }
    }

    ESP_LOGW(TAG, "Late ACK msg_id=%d (no matching slot)", msg_id);
}

MqttSlot *find_slot_or_drop_oldest(void)
{
    int i;

    // 1) Static free
    for (i = 0; i < STATIC_SLOT_COUNT; ++i)
        if (!static_slots[i].in_use)
        {

            return &static_slots[i];
        }

    // 2) Existing dynamic blocks: find a free slot
    for (int b = 0; b < dynamic_block_count; ++b)
    {
        DynBlock *blk = dynamic_blocks[b];
        for (int s = 0; s < DYNAMIC_SLOT_COUNT; ++s)
        {
            if (!blk->slots[s].in_use)
            {
                ESP_LOGI(TAG, "[DYN] using block=%d slot=%d (reuse)", b, s);
                blk->in_use = true;
                return &blk->slots[s];
            }
        }
    }

    // 3) No free slot: allocate a new dynamic block (tier growth)
    DynBlock *new_blk = alloc_dynamic_block();
    if (new_blk)
    {
        new_blk->in_use = true;
        ESP_LOGI(TAG, "[DYN] allocated new block=%d; using slot=0", dynamic_block_count - 1);

        return &new_blk->slots[0];
    }

    // 4) No capacity: drop oldest (static or dynamic)
    uint64_t oldest_time = ULLONG_MAX;
    MqttSlot *oldest = NULL;

    for (i = 0; i < STATIC_SLOT_COUNT; ++i)
    {
        if (static_slots[i].in_use && static_slots[i].timestamp_us < oldest_time)
        {
            oldest_time = static_slots[i].timestamp_us;
            oldest = &static_slots[i];
        }
    }
    for (int b = 0; b < dynamic_block_count; ++b)
    {
        DynBlock *blk = dynamic_blocks[b];
        for (int s = 0; s < DYNAMIC_SLOT_COUNT; ++s)
        {
            MqttSlot *ms = &blk->slots[s];
            if (ms->in_use && ms->timestamp_us < oldest_time)
            {
                oldest_time = ms->timestamp_us;
                oldest = ms;
            }
        }
    }

    if (oldest)
    {
        ESP_LOGW(TAG, "Dropping oldest msg_id=%d to enqueue new", oldest->msg_id);
        oldest->in_use = false;
        oldest->msg_id = -1;
        return oldest;
    }

    return NULL;
}


// int mqtt_qos1q_publish(esp_mqtt_client_handle_t client,
//                        const char *topic, size_t topic_len,
//                        const char *payload, size_t payload_len,
//                        int qos, bool retain)
// {
//     if (!client || !topic || (!payload && payload_len > 0)) {
//         ESP_LOGE(TAG, "[QOS1Q] invalid args");
//         return -1;
//     }

//     // Clamp lengths to slot buffer sizes
//     if (topic_len >= TOPIC_MAX) {
//         ESP_LOGW(TAG, "[QOS1Q] topic length %u exceeds max %u, clamping",
//                  (unsigned)topic_len, (unsigned)(TOPIC_MAX - 1));
//         topic_len = TOPIC_MAX - 1;
//     }
//     if (payload_len >= PAYLOAD_MAX) {
//         ESP_LOGW(TAG, "[QOS1Q] payload length %u exceeds max %u, clamping",
//                  (unsigned)payload_len, (unsigned)(PAYLOAD_MAX - 1));
//         payload_len = PAYLOAD_MAX - 1;
//     }

//     // Hygiene sweep
//     mqtt_qos1q_check_timeouts();

//     // Pick a slot
//     MqttSlot *slot = find_slot_or_drop_oldest();
//     if (!slot) {
//         ESP_LOGE(TAG, "[QOS1Q] no slot available");
//         return -2;
//     }

//     // Fill slot
//     memcpy(slot->topic,   topic,   topic_len);
//     slot->topic[topic_len] = '\0';
//     memcpy(slot->payload, payload, payload_len);
//     slot->payload[payload_len] = '\0';
//     slot->topic_len    = (uint16_t)topic_len;
//     slot->payload_len  = (uint16_t)payload_len;
//     slot->in_use       = true;
//     slot->timestamp_us = now_us();

//     // --- Build and send packet (same as original make_publish) ---
//     uint16_t msg_id = 0;
//     struct mqtt_message *msg = NULL;

//     if (client->mqtt_state.connection.information.protocol_ver == MQTT_PROTOCOL_V_5) {
//     #ifdef CONFIG_MQTT_PROTOCOL_5
//         msg = mqtt5_msg_publish(&client->mqtt_state.connection,
//                                 topic, payload, (int)payload_len,
//                                 qos, retain,
//                                 &msg_id,
//                                 client->mqtt5_config->publish_property_info,
//                                 client->mqtt5_config->server_resp_property_info.response_info);

//         if (client->mqtt_state.connection.outbound_message.length) {
//             client->mqtt5_config->publish_property_info = NULL;
//         }

//         ESP_LOGD(TAG, "[QOS1Q] mqtt5_msg_publish done, outbound_len=%d, msg_id=%u",
//                  client->mqtt_state.connection.outbound_message.length, msg_id);
//     #endif
//     } else {
//         msg = mqtt_msg_publish(&client->mqtt_state.connection,
//                                topic, payload, (int)payload_len,
//                                qos, retain, &msg_id);

//         ESP_LOGD(TAG, "[QOS1Q] mqtt_msg_publish done, outbound_len=%d, msg_id=%u",
//                  client->mqtt_state.connection.outbound_message.length, msg_id);
//     }

//     if (!msg || client->mqtt_state.connection.outbound_message.length == 0) {
//         ESP_LOGE(TAG, "[QOS1Q] build failed (topic='%s', len=%u)", topic, (unsigned)payload_len);
//         slot->in_use = false;
//         slot->msg_id = -1;
//         return -1;
//     }

//     if (client->state == MQTT_STATE_CONNECTED) {
//         if (esp_mqtt_write(client) != ESP_OK) {
//             ESP_LOGE(TAG, "[QOS1Q] write failed");
//             // keep slot for timeout retry
//         }
//     } else {
//         ESP_LOGW(TAG, "[QOS1Q] client not connected, send skipped");
//     }

//     // Record the final msg_id in the slot
//     slot->msg_id = msg_id;

//     // Diagnostics
//     diag_update_burst();
//     diag_update_payload_len(payload_len);
//     ESP_LOGI(TAG, "[QOS1Q] Tracked+Published QoS%d msg_id=%d", qos, (int)msg_id);
//     log_qos1_queue_stats();

//     return (int)msg_id;
// // }


void mqtt_qos1q_log_diagnostics(void)
{
    ESP_LOGI(TAG, "Max burst size: %u", (unsigned)diag_max_burst);
    ESP_LOGI(TAG, "Max payload len: %u", (unsigned)diag_max_payload_len);
    ESP_LOGI(TAG, "Timeout count: %u", (unsigned)diag_timeout_count);
    ESP_LOGI(TAG, "Dynamic blocks: %d (slots per block=%d, idle_timeout_ms=%d)",
             dynamic_block_count, DYNAMIC_SLOT_COUNT, DYN_BLOCK_IDLE_TIMEOUT_MS);
}

void mqtt_qos1q_clear_all(void)
{
    // Clear static slots
    for (int i = 0; i < STATIC_SLOT_COUNT; ++i)
    {
        static_slots[i].in_use = false;
        static_slots[i].msg_id = -1;
        static_slots[i].topic_len = 0;
        static_slots[i].payload_len = 0;
    }

    // Free all dynamic blocks
    for (int b = dynamic_block_count - 1; b >= 0; --b)
    {
        free_dynamic_block_at_index(b);
    }

    // Reset diagnostics
    diag_max_burst = 0;
    diag_max_payload_len = 0;
    diag_timeout_count = 0;

    ESP_LOGI(TAG, "QoS1 queue cleared");
}
