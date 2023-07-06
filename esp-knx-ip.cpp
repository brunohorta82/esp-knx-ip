/**
 * esp-knx-ip library for KNX/IP communication on an ESP8266
 * Author: Nico Weichbrodt <envy>
 * License: MIT
 */

#include "esp-knx-ip.h"

char const *string_defaults[] =
    {
        "Do this",
        "True",
        "False",
        ""};

ESPKNXIP::ESPKNXIP() : registered_callback_assignments(0),
                       free_callback_assignment_slots(0),
                       registered_callbacks(0),
                       free_callback_slots(0),
                       registered_configs(0),
                       registered_feedbacks(0)
{
  DEBUG_PRINTLN();
  DEBUG_PRINTLN("ESPKNXIP starting up");
  // Default physical address is 1.1.0
  physaddr.bytes.high = (/*area*/ 1 << 4) | /*line*/ 1;
  physaddr.bytes.low = /*member*/ 0;
  memset(callback_assignments, 0, MAX_CALLBACK_ASSIGNMENTS * sizeof(callback_assignment_t));
  memset(callbacks, 0, MAX_CALLBACKS * sizeof(callback_fptr_t));
  memset(custom_config_data, 0, MAX_CONFIG_SPACE * sizeof(uint8_t));
  memset(custom_config_default_data, 0, MAX_CONFIG_SPACE * sizeof(uint8_t));
  memset(custom_configs, 0, MAX_CONFIGS * sizeof(config_t));
}

void ESPKNXIP::start()
{
  __start();
}
void ESPKNXIP::reload()
{
  udp.stop();
  __start();
}
void ESPKNXIP::__start()
{
#ifdef ESP8266
  udp.beginMulticast(WiFi.localIP(), MULTICAST_IP, MULTICAST_PORT);
#endif
#ifdef ESP32
  udp.beginMulticast(MULTICAST_IP, MULTICAST_PORT);
#endif
}

uint16_t ESPKNXIP::__ntohs(uint16_t n)
{
  return (uint16_t)((((uint8_t *)&n)[0] << 8) | (((uint8_t *)&n)[1]));
}

callback_assignment_id_t ESPKNXIP::__callback_register_assignment(address_t address, callback_id_t id)
{
  if (registered_callback_assignments >= MAX_CALLBACK_ASSIGNMENTS)
    return -1;

  if (free_callback_assignment_slots == 0)
  {
    callback_assignment_id_t aid = registered_callback_assignments;

    callback_assignments[aid].slot_flags |= SLOT_FLAGS_USED;
    callback_assignments[aid].address = address;
    callback_assignments[aid].callback_id = id;
    registered_callback_assignments++;
    return aid;
  }
  else
  {
    // find the free slot
    for (callback_assignment_id_t aid = 0; aid < registered_callback_assignments; ++aid)
    {
      if (callback_assignments[aid].slot_flags & SLOT_FLAGS_USED)
      {
        // found a used slot
        continue;
      }
      // and now an empty one
      callback_assignments[aid].slot_flags |= SLOT_FLAGS_USED;
      callback_assignments[aid].address = address;
      callback_assignments[aid].callback_id = id;

      free_callback_assignment_slots--;
      return id;
    }
  }
  return -1;
}

void ESPKNXIP::__callback_delete_assignment(callback_assignment_id_t id)
{
  // TODO this can be optimized if we are deleting the last element
  //      as then we can decrement registered_callback_assignments

  // clear slot and mark it as empty
  callback_assignments[id].slot_flags = SLOT_FLAGS_EMPTY;
  callback_assignments[id].address.value = 0;
  callback_assignments[id].callback_id = 0;

  if (id == registered_callback_assignments - 1)
  {
    DEBUG_PRINTLN("last cba deleted");
    // If this is the last callback, we can delete it by decrementing registered_callbacks.
    registered_callback_assignments--;

    // However, if the assignment before this slot are also empty, we can decrement even further
    // First check if this was also the first element
    if (id == 0)
    {
      DEBUG_PRINTLN("really last cba");
      // If this was the last, then we are done.
      return;
    }

    id--;
    while (true)
    {
      DEBUG_PRINT("checking ");
      DEBUG_PRINTLN((int32_t)id);
      if ((callback_assignments[id].slot_flags & SLOT_FLAGS_USED) == 0)
      {
        DEBUG_PRINTLN("merged free slot");
        // Slot before is empty
        free_callback_assignment_slots--;
        registered_callback_assignments--;
      }
      else
      {
        DEBUG_PRINTLN("aborted on used slot");
        // Slot is used, abort
        return;
      }
      id--;
      if (id == CALLBACK_ASSIGNMENT_ID_MAX)
      {
        DEBUG_PRINTLN("abort on wrap");
        // Wrap around, abort
        return;
      }
    }
  }
  else
  {
    DEBUG_PRINTLN("free slot created");
    // there is now one more free slot
    free_callback_assignment_slots++;
  }
}

bool ESPKNXIP::__callback_is_id_valid(callback_id_t id)
{
  if (id < registered_callbacks)
    return true;

  if (callbacks[id].slot_flags & SLOT_FLAGS_USED)
    return true;

  return false;
}

callback_id_t ESPKNXIP::callback_register(String name, callback_fptr_t cb, void *arg, enable_condition_t cond)
{
  if (registered_callbacks >= MAX_CALLBACKS)
    return -1;

  if (free_callback_slots == 0)
  {
    callback_id_t id = registered_callbacks;

    callbacks[id].slot_flags |= SLOT_FLAGS_USED;
    callbacks[id].name = name;
    callbacks[id].fkt = cb;
    callbacks[id].cond = cond;
    callbacks[id].arg = arg;
    registered_callbacks++;
    return id;
  }
  else
  {
    // find the free slot
    for (callback_id_t id = 0; id < registered_callbacks; ++id)
    {
      if (callbacks[id].slot_flags & SLOT_FLAGS_USED)
      {
        // found a used slot
        continue;
      }
      // and now an empty one
      callbacks[id].slot_flags |= SLOT_FLAGS_USED;
      callbacks[id].name = name;
      callbacks[id].fkt = cb;
      callbacks[id].cond = cond;
      callbacks[id].arg = arg;

      free_callback_slots--;
      return id;
    }
  }
  return -1;
}

void ESPKNXIP::callback_deregister(callback_id_t id)
{
  if (!__callback_is_id_valid(id))
    return;

  // clear slot and mark it as empty
  callbacks[id].slot_flags = SLOT_FLAGS_EMPTY;
  callbacks[id].fkt = nullptr;
  callbacks[id].cond = nullptr;
  callbacks[id].arg = nullptr;

  if (id == registered_callbacks - 1)
  {
    // If this is the last callback, we can delete it by decrementing registered_callbacks.
    registered_callbacks--;

    // However, if the callbacks before this slot are also empty, we can decrement even further
    // First check if this was also the first element
    if (id == 0)
    {
      // If this was the last, then we are done.
      return;
    }

    id--;
    while (true)
    {
      if ((callbacks[id].slot_flags & SLOT_FLAGS_USED) == 0)
      {
        // Slot is empty
        free_callback_slots--;
        registered_callbacks--;
      }
      else
      {
        // Slot is used, abort
        return;
      }
      id--;
      if (id == CALLBACK_ASSIGNMENT_ID_MAX)
      {
        // Wrap around, abort
        return;
      }
    }
  }
  else
  {
    // there is now one more free slot
    free_callback_slots++;
  }
}

callback_assignment_id_t ESPKNXIP::callback_assign(callback_id_t id, address_t val)
{
  if (!__callback_is_id_valid(id))
    return -1;

  return __callback_register_assignment(val, id);
}

void ESPKNXIP::callback_unassign(callback_assignment_id_t id)
{
  if (!__callback_is_id_valid(id))
    return;

  __callback_delete_assignment(id);
}

/**
 * Feedback functions start here
 */

feedback_id_t ESPKNXIP::feedback_register_int(String name, int32_t *value, enable_condition_t cond)
{
  if (registered_feedbacks >= MAX_FEEDBACKS)
    return -1;

  feedback_id_t id = registered_feedbacks;

  feedbacks[id].type = FEEDBACK_TYPE_INT;
  feedbacks[id].name = name;
  feedbacks[id].cond = cond;
  feedbacks[id].data = (void *)value;

  registered_feedbacks++;

  return id;
}

feedback_id_t ESPKNXIP::feedback_register_float(String name, float *value, uint8_t precision, char const *prefix, char const *suffix, enable_condition_t cond)
{
  if (registered_feedbacks >= MAX_FEEDBACKS)
    return -1;

  feedback_id_t id = registered_feedbacks;

  feedbacks[id].type = FEEDBACK_TYPE_FLOAT;
  feedbacks[id].name = name;
  feedbacks[id].cond = cond;
  feedbacks[id].data = (void *)value;
  feedbacks[id].options.float_options.precision = precision;
  feedbacks[id].options.float_options.prefix = prefix ? strdup(prefix) : STRING_DEFAULT_EMPTY;
  feedbacks[id].options.float_options.suffix = suffix ? strdup(suffix) : STRING_DEFAULT_EMPTY;

  registered_feedbacks++;

  return id;
}

feedback_id_t ESPKNXIP::feedback_register_bool(String name, bool *value, char const *true_text, char const *false_text, enable_condition_t cond)
{
  if (registered_feedbacks >= MAX_FEEDBACKS)
    return -1;

  feedback_id_t id = registered_feedbacks;

  feedbacks[id].type = FEEDBACK_TYPE_BOOL;
  feedbacks[id].name = name;
  feedbacks[id].cond = cond;
  feedbacks[id].data = (void *)value;
  feedbacks[id].options.bool_options.true_text = true_text ? strdup(true_text) : STRING_DEFAULT_TRUE;
  feedbacks[id].options.bool_options.false_text = false_text ? strdup(false_text) : STRING_DEFAULT_FALSE;

  registered_feedbacks++;

  return id;
}

feedback_id_t ESPKNXIP::feedback_register_action(String name, feedback_action_fptr_t value, const char *btn_text, void *arg, enable_condition_t cond)
{
  if (registered_feedbacks >= MAX_FEEDBACKS)
    return -1;

  feedback_id_t id = registered_feedbacks;

  feedbacks[id].type = FEEDBACK_TYPE_ACTION;
  feedbacks[id].name = name;
  feedbacks[id].cond = cond;
  feedbacks[id].data = (void *)value;
  feedbacks[id].options.action_options.arg = arg;
  feedbacks[id].options.action_options.btn_text = btn_text ? strdup(btn_text) : STRING_DEFAULT_DO_THIS;

  registered_feedbacks++;

  return id;
}

void ESPKNXIP::loop()
{
  __loop_knx();
}

void ESPKNXIP::__loop_knx()
{
  int read = udp.parsePacket();

  if (!read)
  {
    return;
  }
  DEBUG_PRINTLN(F(""));
  DEBUG_PRINT(F("LEN: "));
  DEBUG_PRINTLN(read);

  uint8_t buf[read];
  udp.read(buf, read);

  DEBUG_PRINT(F("Got packet:"));

#ifdef ESP_KNX_DEBUG

  for (int i = 0; i < read; ++i)

  {
    DEBUG_PRINT(F(" 0x"));
    DEBUG_PRINT(buf[i], 16);
  }

#endif

  DEBUG_PRINTLN(F(""));

  knx_ip_pkt_t *knx_pkt = (knx_ip_pkt_t *)buf;

  DEBUG_PRINT(F("ST: 0x"));
  DEBUG_PRINTLN(__ntohs(knx_pkt->service_type), 16);

  if (knx_pkt->header_len != 0x06 && knx_pkt->protocol_version != 0x10 && knx_pkt->service_type != KNX_ST_ROUTING_INDICATION)
    return;

  cemi_msg_t *cemi_msg = (cemi_msg_t *)knx_pkt->pkt_data;

  DEBUG_PRINT(F("MT: 0x"));
  DEBUG_PRINTLN(cemi_msg->message_code, 16);

  if (cemi_msg->message_code != KNX_MT_L_DATA_IND)
    return;

  DEBUG_PRINT(F("ADDI: 0x"));
  DEBUG_PRINTLN(cemi_msg->additional_info_len, 16);

  cemi_service_t *cemi_data = &cemi_msg->data.service_information;

  if (cemi_msg->additional_info_len > 0)
    cemi_data = (cemi_service_t *)(((uint8_t *)cemi_data) + cemi_msg->additional_info_len);

  DEBUG_PRINT(F("C1: 0x"));
  DEBUG_PRINTLN(cemi_data->control_1.byte, 16);

  DEBUG_PRINT(F("C2: 0x"));
  DEBUG_PRINTLN(cemi_data->control_2.byte, 16);

  DEBUG_PRINT(F("DT: 0x"));
  DEBUG_PRINTLN(cemi_data->control_2.bits.dest_addr_type, 16);

  if (cemi_data->control_2.bits.dest_addr_type != 0x01)
    return;

  DEBUG_PRINT(F("HC: 0x"));
  DEBUG_PRINTLN(cemi_data->control_2.bits.hop_count, 16);

  DEBUG_PRINT(F("EFF: 0x"));
  DEBUG_PRINTLN(cemi_data->control_2.bits.extended_frame_format, 16);

  DEBUG_PRINT(F("Source: 0x"));
  DEBUG_PRINT(cemi_data->source.bytes.high, 16);
  DEBUG_PRINT(F(" 0x"));
  DEBUG_PRINTLN(cemi_data->source.bytes.low, 16);

  DEBUG_PRINT(F("Dest: 0x"));
  DEBUG_PRINT(cemi_data->destination.bytes.high, 16);
  DEBUG_PRINT(F(" 0x"));
  DEBUG_PRINTLN(cemi_data->destination.bytes.low, 16);

  knx_command_type_t ct = (knx_command_type_t)(((cemi_data->data[0] & 0xC0) >> 6) | ((cemi_data->pci.apci & 0x03) << 2));

  DEBUG_PRINT(F("CT: 0x"));
  DEBUG_PRINTLN(ct, 16);

#ifdef ESP_KNX_DEBUG
  for (int i = 0; i < cemi_data->data_len; ++i)
  {
    DEBUG_PRINT(F(" 0x"));
    DEBUG_PRINT(cemi_data->data[i], 16);
  }
#endif

  DEBUG_PRINTLN(F("=="));

  // Call callbacks
  for (int i = 0; i < registered_callback_assignments; ++i)
  {
    DEBUG_PRINT(F("Testing: 0x"));
    DEBUG_PRINT(callback_assignments[i].address.bytes.high, 16);
    DEBUG_PRINT(F(" 0x"));
    DEBUG_PRINTLN(callback_assignments[i].address.bytes.low, 16);
    if (cemi_data->destination.value == callback_assignments[i].address.value)
    {
      DEBUG_PRINTLN(F("Found match"));
      if (callbacks[callback_assignments[i].callback_id].cond && !callbacks[callback_assignments[i].callback_id].cond())
      {
        DEBUG_PRINTLN(F("But it's disabled"));
#if ALLOW_MULTIPLE_CALLBACKS_PER_ADDRESS
        continue;
#else
        return;
#endif
      }
      uint8_t data[cemi_data->data_len];
      memcpy(data, cemi_data->data, cemi_data->data_len);
      data[0] = data[0] & 0x3F;
      message_t msg = {};
      msg.ct = ct;
      msg.received_on = cemi_data->destination;
      msg.data_len = cemi_data->data_len;
      msg.data = data;
      callbacks[callback_assignments[i].callback_id].fkt(msg, callbacks[callback_assignments[i].callback_id].arg);
#if ALLOW_MULTIPLE_CALLBACKS_PER_ADDRESS
      continue;
#else
      return;
#endif
    }
  }

  return;
}

// Global "singleton" object
ESPKNXIP knx;
