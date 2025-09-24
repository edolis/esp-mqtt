Here’s a clear, technical description you can drop straight into your project documentation for the current **ED_mqtt_qos1_queue** setup.

---

## **QoS1 Queue Architecture & Behaviour**

### **Purpose**
The QoS1 queue is designed to reliably track and manage MQTT messages with Quality of Service level 1 (at least once delivery) in an embedded environment. It ensures messages are retained until acknowledged (PUBACK) or expired, while minimising heap fragmentation and scaling capacity on demand.

---

### **Core Structure**

#### **Static Slots**
- **Definition:** A fixed-size array of `MqttSlot` structures (`STATIC_SLOT_COUNT`).
- **Lifetime:** Always allocated at init; never freed.
- **Usage:** First tier for storing outgoing QoS1 messages.
  - Each slot contains topic buffer, payload buffer, lengths, message ID, and timestamps.
- **Selection:** Always preferred before using dynamic capacity.

#### **Dynamic Blocks**
- **Definition:** An array of pointers to `DynBlock` structures (`dynamic_blocks[MAX_DYNAMIC_BLOCKS]`).
- **Block Composition:**
  - Each block contains `DYNAMIC_SLOT_COUNT` `MqttSlot` entries.
  - Contiguous topic and payload buffers for all slots in the block.
  - `in_use` flag (true if any slot in the block is active).
  - `last_active_us` timestamp (set when all slots become free).
- **Allocation:**
  - Created only when all static slots and existing dynamic slots are full.
  - Allocated as a single contiguous block for topics, payloads, and slot metadata.
- **Scaling:**
  - Capacity grows in **chunks** (blocks) instead of per-slot allocations.
  - Reduces allocator churn and keeps memory layout predictable.
- **Deallocation:**
  - A block is freed only if all its slots are free **and** it has been idle for longer than `DYN_BLOCK_IDLE_TIMEOUT_MS`.
  - Idle timeout prevents rapid alloc/free cycles during bursts.

---

### **Operational Flow**

#### **Enqueue (find_slot_or_drop_oldest)**
1. **Static Search:** Scan static slots for a free entry.
2. **Dynamic Search:** Scan existing dynamic blocks for a free slot.
3. **Block Allocation:** If no free slot exists and block count < `MAX_DYNAMIC_BLOCKS`, allocate a new block and return its first slot.
4. **Drop Oldest:** If at capacity, find the oldest slot (static or dynamic) by timestamp, clear it, and reuse.

#### **Publish Tracking**
- When a message is enqueued, the slot is marked `in_use` and timestamped.
- Burst diagnostics (`diag_max_burst`) updated to track peak simultaneous usage.

#### **Acknowledgement (mqtt_qos1q_on_published)**
- Locate the slot by `msg_id` in static or dynamic storage.
- Mark slot free; reset lengths and IDs.
- If all slots in a dynamic block are now free, set `last_active_us` and mark block `in_use = false`.

#### **Timeout Sweep (mqtt_qos1q_check_timeouts)**
- Periodically checks all slots for ACK timeout (`ACK_TIMEOUT_MS`).
- Frees timed-out slots.
- For dynamic blocks:
  - If all slots are free, check idle duration.
  - Free block if idle longer than `DYN_BLOCK_IDLE_TIMEOUT_MS`.

#### **Diagnostics**
- Reports:
  - Static slots used/free.
  - Dynamic slots used/free.
  - Number of active dynamic blocks.
  - Max burst size and max payload length observed.
  - Timeout count.

---

### **Advantages of This Design**
- **Tiered Capacity:** Static slots handle steady-state traffic; dynamic blocks absorb bursts.
- **Chunked Allocation:** Allocating slots in blocks reduces heap fragmentation.
- **Idle Timeout:** Prevents premature freeing during short bursts, but eventually returns memory to the system.
- **Predictable Memory Layout:** Contiguous buffers per block improve cache locality and simplify pointer management.
- **Scalable:** Can handle from `STATIC_SLOT_COUNT` up to `STATIC_SLOT_COUNT + MAX_DYNAMIC_BLOCKS * DYNAMIC_SLOT_COUNT` concurrent QoS1 messages.

---
```mermaid
flowchart TD
    A[Publish request QoS1] --> B{Free static slot?}
    B -->|Yes| S[Use static slot]
    B -->|No| C{Free slot in existing dynamic blocks?}
    C -->|Yes| D[Use free dynamic slot]
    C -->|No| E{Can allocate new block?}
    E -->|Yes| F[Allocate new block of N slots\nUse first slot]
    E -->|No| G[Drop oldest slot\n(static or dynamic)]
    S --> H[Mark slot in_use, set timestamp]
    D --> H
    F --> H
    G --> H
    H --> I[Update burst diagnostics]

    ```


```mermaid
stateDiagram-v2
    [*] --> Allocated: Block created on demand
    Allocated --> Active: At least one slot in_use
    Active --> Idle: All slots free, last_active_us set
    Idle --> Active: New message assigned to a slot
    Idle --> Freed: Idle duration > DYN_BLOCK_IDLE_TIMEOUT_MS
    Freed --> [*]

```
