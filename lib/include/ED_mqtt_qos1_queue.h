// #region StdManifest
/**
 * @file ED_mqtt_qos1_queue.h
 * @brief implements a MQTT queue for messages with QOS1
 * main issue is that QoS0 messages cannot be managed through enqueue and require dynamic allocation.
 * In case of frequent messages (like logs) that brings to heap fragmentation reducing the uptime of the application
 * QoS1 on MQTT3.1 is enabled to use enqueue avoiding the copy oft the message by MQTT, which implies dynamic allocation
 * and heap fragmentation.
 * QoS1 implies that the items need to wait for a return ack message to be taken off the queue.
 * Here we are implementing a hybrid queue system with a statically allocated
 * part, a timeout setting to remome forcedly from the waiting queue
 * and a dynamic slotted queue to manage inrush message peak.
 *
 *
 * @author Emanuele Dolis (edoliscom@gmail.com)
 * @version 0.1
 * @date 2025-09-21
 */
// #endregion

#pragma once

#include "mqtt_client.h"
#include "esp_timer.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <limits.h>
#include "esp_log.h"

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

/**
 * @brief Initialise the QoS1 publish queue.
 * Call once at startup before using any other API.
 */
void mqtt_qos1q_init(void);

/**
 * @brief Periodic timeout sweep.
 * Frees any poisoned/unacked slots and may free dynamic pool if idle.
 * Call periodically (e.g., every 1s) from a timer or main loop.
 */
void mqtt_qos1q_check_timeouts(void);

/**
 * @brief Notify the queue that a PUBACK was received.
 * Call from your MQTT_EVENT_PUBLISHED handler.
 *
 * @param msg_id The msg_id from the event.
 */
void mqtt_qos1q_on_published(int msg_id);

/**
 * @brief Publish a QoS1 message using zero-copy store=false.
 *
 * Copies topic and payload into an internal slot buffer and enqueues.
 * If all slots are busy, drops the oldest in-flight slot to make room.
 *
 * @param client   MQTT client handle.
 * @param topic    Topic string.
 * @param topic_len Length of topic (without null terminator).
 * @param payload  Payload buffer.
 * @param payload_len Length of payload.
 * @param retain   Retain flag.
 * @return msg_id >=0 on success, -1 on failure, -2 if outbox full.
 */
int mqtt_qos1q_publish(esp_mqtt_client_handle_t client,
                       const char *topic, size_t topic_len,
                       const char *payload, size_t payload_len,
                       bool retain);

/**
 * @brief Log current diagnostics: max burst, max payload length, timeout count.
 */
void mqtt_qos1q_log_diagnostics(void);

#ifdef __cplusplus
} // extern "C"
#endif
