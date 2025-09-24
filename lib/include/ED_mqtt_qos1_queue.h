#pragma once

#include "mqtt_client.h"
#include "esp_timer.h"
#include <stddef.h>   // size_t
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===== Configuration =====
#ifndef STATIC_SLOT_COUNT
#define STATIC_SLOT_COUNT   3
#endif

#ifndef DYNAMIC_SLOT_COUNT
#define DYNAMIC_SLOT_COUNT  3
#endif

#ifndef PAYLOAD_MAX
#define PAYLOAD_MAX         512
#endif

#ifndef TOPIC_MAX
#define TOPIC_MAX           128
#endif

#ifndef ACK_TIMEOUT_MS
#define ACK_TIMEOUT_MS      5000
#endif




// ===== API =====

typedef struct
{
    char *topic;
    char *payload;
    uint16_t topic_len;
    uint16_t payload_len;
    bool in_use;
    int msg_id;
    uint64_t timestamp_us;
    bool retain;
} MqttSlot;
MqttSlot *find_slot_or_drop_oldest(void);
/**
 * Initialise the QoS1 publish queue (idempotent).
 * Safe to call multiple times (on reconnects).
 * Pass allow_dynamic=true to enable elastic behavior within the queue.
 */
void mqtt_qos1q_init();

/**
 * Periodic timeout sweep (safe to call frequently).
 * Drops expired static slots and frees expired dynamic slots.
 */
void mqtt_qos1q_check_timeouts(void);

/**
 * Notify the queue that a PUBACK was received (idempotent).
 */
void mqtt_qos1q_on_published(int msg_id);

/**
 * Publish a QoS1 message, copying into queue slot and calling client publish.
 * Returns msg_id >=0 on success, -1 on failure, -2 if no slot available.
 */
// int mqtt_qos1q_publish(esp_mqtt_client_handle_t client,
//                        const char *topic, size_t topic_len,
//                        const char *payload, size_t payload_len,
//                        bool retain);

int mqtt_qos1q_publish(esp_mqtt_client_handle_t client,
                       const char *topic, size_t topic_len,
                       const char *payload, size_t payload_len,
                       int qos, bool retain);

/**
 * Track an already enqueued QoS1 message (no sending).
 * Returns msg_id >=0 on success, -1 on failure, -2 if no slot available.
 */
int mqtt_qos1q_track(const char *topic, size_t topic_len,
                     const char *payload, size_t payload_len,
                     bool retain,
                     int msg_id);


void mqtt_qos1q_rebind_msg_id(int provisional_id, int final_id);

/**
 * Clear all slots and free dynamic slots.
 */
void mqtt_qos1q_clear_all(void);

/**
 * Log diagnostics for current state.
 */
void mqtt_qos1q_log_diagnostics(void);

// uint64_t now_us(void);
// void diag_update_burst(void);
// void log_qos1_queue_stats(void);



#ifdef __cplusplus
} // extern "C"
#endif
