#include "ED_mqtt_qos1_queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MQTT_QOS1Q";

typedef struct {
  char *topic;
  char *payload;
  uint16_t topic_len;
  uint16_t payload_len;
  bool in_use;
  int msg_id;
  uint64_t timestamp_us;
} MqttSlot;

static TimerHandle_t timeout_timer = NULL;

// Forward decls for helper functions (replaces lambda)
static void sweep_slots(MqttSlot *pool, int count, uint64_t now_us, uint64_t thresh_us);
static void mqtt_alloc_dynamic_pool(void);
static void mqtt_free_dynamic_pool_if_empty(void);
static void log_qos1_queue_stats(void);

static MqttSlot static_slots[STATIC_SLOT_COUNT];
static char static_topics[STATIC_SLOT_COUNT][TOPIC_MAX];
static char static_payloads[STATIC_SLOT_COUNT][PAYLOAD_MAX];

static MqttSlot *dynamic_slots = NULL;
static char *dynamic_topics_block = NULL;
static char *dynamic_payloads_block = NULL;

static size_t diag_max_burst = 0;
static size_t diag_max_payload_len = 0;
static size_t diag_timeout_count = 0;

static inline uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }

static void diag_update_burst(void) {
  size_t in_use_count = 0;
  int i;
  for (i = 0; i < STATIC_SLOT_COUNT; ++i)
    if (static_slots[i].in_use)
      ++in_use_count;
  if (dynamic_slots) {
    for (i = 0; i < DYNAMIC_SLOT_COUNT; ++i)
      if (dynamic_slots[i].in_use)
        ++in_use_count;
  }
  if (in_use_count > diag_max_burst)
    diag_max_burst = in_use_count;
}

static void diag_update_payload_len(size_t len) {
  if (len > diag_max_payload_len)
    diag_max_payload_len = len;
}

static void diag_inc_timeout(void) { ++diag_timeout_count; }

static void timeout_timer_cb(TimerHandle_t xTimer) {
  static TickType_t last = 0; // persist across calls
  TickType_t now_ticks = xTaskGetTickCount();
  if (now_ticks - last > pdMS_TO_TICKS(20000)) {
    mqtt_qos1q_log_diagnostics();
    last = now_ticks;
  }
  mqtt_qos1q_check_timeouts();
}

void mqtt_qos1q_init(void) {
  int i;

  memset(static_slots, 0, sizeof(static_slots));
  for (i = 0; i < STATIC_SLOT_COUNT; ++i) {
    static_slots[i].topic = static_topics[i];
    static_slots[i].payload = static_payloads[i];
    static_slots[i].msg_id = -1;
  }
  diag_max_burst = 0;
  diag_max_payload_len = 0;
  diag_timeout_count = 0;

  if (!timeout_timer) {
    timeout_timer = xTimerCreate("mqtt_qos1q_to",
                                 pdMS_TO_TICKS(1000), // 1s
                                 pdTRUE,              // auto-reload
                                 NULL, timeout_timer_cb);
    if (timeout_timer) {
      xTimerStart(timeout_timer, 0);
    } else {
      ESP_LOGE(TAG, "Failed to create timeout timer");
    }
  }
}

static void log_qos1_queue_stats(void) {
  int static_free = 0, static_used = 0;
  int dynamic_free = 0, dynamic_used = 0;
  int i;

  for (i = 0; i < STATIC_SLOT_COUNT; ++i) {
    if (static_slots[i].in_use) static_used++;
    else static_free++;
  }

  if (dynamic_slots) {
    for (i = 0; i < DYNAMIC_SLOT_COUNT; ++i) {
      if (dynamic_slots[i].in_use) dynamic_used++;
      else dynamic_free++;
    }
  }

  ESP_LOGI(TAG, "Static slots: %d used / %d free, Dynamic slots: %d used / %d free",
           static_used, static_free, dynamic_used, dynamic_free);
}

static void mqtt_alloc_dynamic_pool(void) {
  int i;

  if (dynamic_slots)
    return;

  dynamic_slots = (MqttSlot *)calloc(DYNAMIC_SLOT_COUNT, sizeof(MqttSlot));
  dynamic_topics_block = (char *)malloc(DYNAMIC_SLOT_COUNT * TOPIC_MAX);
  dynamic_payloads_block = (char *)malloc(DYNAMIC_SLOT_COUNT * PAYLOAD_MAX);

  if (!dynamic_slots || !dynamic_topics_block || !dynamic_payloads_block) {
    ESP_LOGE(TAG, "Failed to allocate dynamic pool");
    free(dynamic_slots);           dynamic_slots = NULL;
    free(dynamic_topics_block);    dynamic_topics_block = NULL;
    free(dynamic_payloads_block);  dynamic_payloads_block = NULL;
    return;
  }

  for (i = 0; i < DYNAMIC_SLOT_COUNT; ++i) {
    dynamic_slots[i].topic   = dynamic_topics_block + i * TOPIC_MAX;
    dynamic_slots[i].payload = dynamic_payloads_block + i * PAYLOAD_MAX;
    dynamic_slots[i].msg_id  = -1;
  }

  ESP_LOGI(TAG, "Allocated dynamic overflow pool (%d slots)", DYNAMIC_SLOT_COUNT);
}

static void mqtt_free_dynamic_pool_if_empty(void) {
  int i;

  if (!dynamic_slots)
    return;

  for (i = 0; i < DYNAMIC_SLOT_COUNT; ++i)
    if (dynamic_slots[i].in_use)
      return;

  free(dynamic_payloads_block);  dynamic_payloads_block = NULL;
  free(dynamic_topics_block);    dynamic_topics_block = NULL;
  free(dynamic_slots);           dynamic_slots = NULL;

  ESP_LOGI(TAG, "Freed dynamic overflow pool");
}

void mqtt_qos1q_check_timeouts(void) {
  uint64_t now       = now_us();
  uint64_t thresh_us = (uint64_t)ACK_TIMEOUT_MS * 1000ULL;

  sweep_slots(static_slots, STATIC_SLOT_COUNT, now, thresh_us);
  if (dynamic_slots) {
    sweep_slots(dynamic_slots, DYNAMIC_SLOT_COUNT, now, thresh_us);
    mqtt_free_dynamic_pool_if_empty();
  }
}

static void sweep_slots(MqttSlot *pool, int count, uint64_t now, uint64_t thresh_us) {
  int i;
  for (i = 0; i < count; ++i) {
    if (pool[i].in_use && (now - pool[i].timestamp_us) > thresh_us) {
      ESP_LOGW(TAG, "Timeout msg_id=%d, freeing slot", pool[i].msg_id);
      pool[i].in_use = false;
      pool[i].msg_id = -1;
      diag_inc_timeout();
    }
  }
}

void mqtt_qos1q_on_published(int msg_id) {
  int i;

  for (i = 0; i < STATIC_SLOT_COUNT; ++i) {
    if (static_slots[i].in_use && static_slots[i].msg_id == msg_id) {
      static_slots[i].in_use = false;
      static_slots[i].msg_id = -1;
      ESP_LOGI(TAG, "ACK msg_id=%d (static)", msg_id);
      return;
    }
  }

  if (dynamic_slots) {
    for (i = 0; i < DYNAMIC_SLOT_COUNT; ++i) {
      if (dynamic_slots[i].in_use && dynamic_slots[i].msg_id == msg_id) {
        dynamic_slots[i].in_use = false;
        dynamic_slots[i].msg_id = -1;
        ESP_LOGI(TAG, "ACK msg_id=%d (dynamic)", msg_id);
        mqtt_free_dynamic_pool_if_empty();
        return;
      }
    }
  }

  ESP_LOGW(TAG, "Late ACK msg_id=%d (no matching slot)", msg_id);
}

static MqttSlot *find_slot_or_drop_oldest(void) {
  int i;

  for (i = 0; i < STATIC_SLOT_COUNT; ++i)
    if (!static_slots[i].in_use)
      return &static_slots[i];

  if (!dynamic_slots)
    mqtt_alloc_dynamic_pool();
  if (dynamic_slots) {
    for (i = 0; i < DYNAMIC_SLOT_COUNT; ++i)
      if (!dynamic_slots[i].in_use)
        return &dynamic_slots[i];
  }

  uint64_t oldest_time = ULLONG_MAX;
  MqttSlot *oldest = NULL;

  for (i = 0; i < STATIC_SLOT_COUNT; ++i) {
    if (static_slots[i].in_use && static_slots[i].timestamp_us < oldest_time) {
      oldest_time = static_slots[i].timestamp_us;
      oldest = &static_slots[i];
    }
  }
  if (dynamic_slots) {
    for (i = 0; i < DYNAMIC_SLOT_COUNT; ++i) {
      if (dynamic_slots[i].in_use && dynamic_slots[i].timestamp_us < oldest_time) {
        oldest_time = dynamic_slots[i].timestamp_us;
        oldest = &dynamic_slots[i];
      }
    }
  }

  if (oldest) {
    ESP_LOGW(TAG, "Dropping oldest msg_id=%d to enqueue new", oldest->msg_id);
    oldest->in_use = false;
    oldest->msg_id = -1;
    return oldest;
  }

  return NULL;
}

int mqtt_qos1q_publish(esp_mqtt_client_handle_t client, const char *topic,
                       size_t topic_len, const char *payload,
                       size_t payload_len, bool retain) {
  int msg_id;
  MqttSlot *slot;

  if (!client || !topic || !payload || topic_len == 0 || payload_len == 0)
    return -1;

  if (topic_len >= TOPIC_MAX)
    topic_len = TOPIC_MAX - 1;
  if (payload_len >= PAYLOAD_MAX)
    payload_len = PAYLOAD_MAX - 1;

  mqtt_qos1q_check_timeouts();

  slot = find_slot_or_drop_oldest();
  if (!slot) {
    ESP_LOGE(TAG, "No slot available");
    return -1;
  }

  memcpy(slot->topic,   topic,   topic_len);
  memcpy(slot->payload, payload, payload_len);
  slot->topic[topic_len]   = '\0';
  slot->payload[payload_len] = '\0';
  slot->topic_len   = (uint16_t)topic_len;
  slot->payload_len = (uint16_t)payload_len;

  diag_update_payload_len(payload_len);

  msg_id = esp_mqtt_client_enqueue(client, slot->topic, slot->payload,
                                   (int)slot->payload_len,
                                   1,              // QoS 1
                                   retain ? 1 : 0, // retain
                                   false);         // store=false → zero-copy

  if (msg_id >= 0) {
    slot->in_use = true;
    slot->msg_id = msg_id;
    slot->timestamp_us = now_us();
    diag_update_burst();
    log_qos1_queue_stats();
    return msg_id;
  } else {
    ESP_LOGW(TAG, "Enqueue failed (%d), slot not taken", msg_id);
    slot->in_use = false;
    slot->msg_id = -1;
    log_qos1_queue_stats();
    return msg_id;
  }
}

void mqtt_qos1q_log_diagnostics(void) {
  ESP_LOGI(TAG, "Max burst size: %u", (unsigned)diag_max_burst);
  ESP_LOGI(TAG, "Max payload len: %u", (unsigned)diag_max_payload_len);
  ESP_LOGI(TAG, "Timeout count: %u", (unsigned)diag_timeout_count);
}
