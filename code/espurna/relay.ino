/*

RELAY MODULE

Copyright (C) 2016-2018 by Xose Pérez <xose dot perez at gmail dot com>

Module key prefix: rly

*/

#include <EEPROM_Rotate.h>
#include <Ticker.h>
#include <ArduinoJson.h>
#include <vector>
#include <functional>

typedef struct {

    // Configuration variables

    unsigned char pin;          // GPIO pin for the relay
    unsigned char type;         // RELAY_TYPE_NORMAL, RELAY_TYPE_INVERSE, RELAY_TYPE_LATCHED or RELAY_TYPE_LATCHED_INVERSE
    unsigned char reset_pin;    // GPIO to reset the relay if RELAY_TYPE_LATCHED
    unsigned long delay_on;     // Delay to turn relay ON
    unsigned long delay_off;    // Delay to turn relay OFF
    unsigned char pulse;        // RELAY_PULSE_NONE, RELAY_PULSE_OFF or RELAY_PULSE_ON
    unsigned long pulse_ms;     // Pulse length in millis

    // Status variables

    bool current_status;        // Holds the current (physical) status of the relay
    bool target_status;         // Holds the target status
    unsigned long fw_start;     // Flood window start time
    unsigned char fw_count;     // Number of changes within the current flood window
    unsigned long change_time;  // Scheduled time to change
    bool report;                // Whether to report to own topic
    bool group_report;          // Whether to report to group topic

    // Helping objects

    Ticker pulseTicker;         // Holds the pulse back timer

} relay_t;
std::vector<relay_t> _relays;
bool _relayRecursive = false;
Ticker _relaySaveTicker;

// -----------------------------------------------------------------------------
// RELAY PROVIDERS
// -----------------------------------------------------------------------------

void _relayProviderStatus(unsigned char id, bool status) {

    // Check relay ID
    if (id >= _relays.size()) return;

    // Store new current status
    _relays[id].current_status = status;

    #if RELAY_PROVIDER == RELAY_PROVIDER_RFBRIDGE
        rfbStatus(id, status);
    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_DUAL

        // Calculate mask
        unsigned char mask=0;
        for (unsigned char i=0; i<_relays.size(); i++) {
            if (_relays[i].current_status) mask = mask + (1 << i);
        }

        // Send it to F330
        Serial.flush();
        Serial.write(0xA0);
        Serial.write(0x04);
        Serial.write(mask);
        Serial.write(0xA1);
        Serial.flush();

    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_STM
        Serial.flush();
        Serial.write(0xA0);
        Serial.write(id + 1);
        Serial.write(status);
        Serial.write(0xA1 + status + id);
        Serial.flush();
    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_LIGHT

        // If the number of relays matches the number of light channels
        // assume each relay controls one channel.
        // If the number of relays is the number of channels plus 1
        // assume the first one controls all the channels and
        // the rest one channel each.
        // Otherwise every relay controls all channels.
        // TODO: this won't work with a mixed of dummy and real relays
        // but this option is not allowed atm (YANGNI)
        if (_relays.size() == lightChannels()) {
            lightState(id, status);
            lightState(true);
        } else if (_relays.size() == (lightChannels() + 1u)) {
            if (id == 0) {
                lightState(status);
            } else {
                lightState(id-1, status);
            }
        } else {
            lightState(status);
        }

        lightUpdate(true, true);

    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_RELAY
        if (_relays[id].type == RELAY_TYPE_NORMAL) {
            digitalWrite(_relays[id].pin, status);
        } else if (_relays[id].type == RELAY_TYPE_INVERSE) {
            digitalWrite(_relays[id].pin, !status);
        } else if (_relays[id].type == RELAY_TYPE_LATCHED || _relays[id].type == RELAY_TYPE_LATCHED_INVERSE) {
            bool pulse = RELAY_TYPE_LATCHED ? HIGH : LOW;
            digitalWrite(_relays[id].pin, !pulse);
            if (GPIO_NONE != _relays[id].reset_pin) digitalWrite(_relays[id].reset_pin, !pulse);
            if (status || (GPIO_NONE == _relays[id].reset_pin)) {
                digitalWrite(_relays[id].pin, pulse);
            } else {
                digitalWrite(_relays[id].reset_pin, pulse);
            }
            nice_delay(RELAY_LATCHING_PULSE);
            digitalWrite(_relays[id].pin, !pulse);
            if (GPIO_NONE != _relays[id].reset_pin) digitalWrite(_relays[id].reset_pin, !pulse);
        }
    #endif

}

/**
 * Walks the relay vector processing only those relays
 * that have to change to the requested mode
 * @bool mode Requested mode
 */
void _relayProcess(bool mode) {

    unsigned long current_time = millis();

    for (unsigned char id = 0; id < _relays.size(); id++) {

        bool target = _relays[id].target_status;

        // Only process the relays we have to change
        if (target == _relays[id].current_status) continue;

        // Only process the relays we have change to the requested mode
        if (target != mode) continue;

        // Only process if the change_time has arrived
        if (current_time < _relays[id].change_time) continue;

        DEBUG_MSG_P(PSTR("[RELAY] #%d set to %s\n"), id, target ? "ON" : "OFF");

        // Call the provider to perform the action
        _relayProviderStatus(id, target);

        // Send to Broker
        #if BROKER_SUPPORT
            brokerPublish(MQTT_TOPIC_RELAY, id, target ? "1" : "0");
        #endif

        // Send MQTT
        #if MQTT_SUPPORT
            relayMQTT(id);
        #endif

        if (!_relayRecursive) {
            relayPulse(id);
            _relaySaveTicker.once_ms(RELAY_SAVE_DELAY, relaySave);
            #if WEB_SUPPORT
                wsSend(_relayWebSocketUpdate);
            #endif
        }

        #if DOMOTICZ_SUPPORT
            domoticzSendRelay(id);
        #endif

        #if INFLUXDB_SUPPORT
            relayInfluxDB(id);
        #endif

        #if THINGSPEAK_SUPPORT
            tspkEnqueueRelay(id, target);
            tspkFlush();
        #endif

        // Flag relay-based LEDs to update status
        #if LED_SUPPORT
            ledUpdate(true);
        #endif

        _relays[id].report = false;
        _relays[id].group_report = false;

    }

}

#if defined(ITEAD_SONOFF_IFAN02)

unsigned char _relay_ifan02_speeds[] = {0, 1, 3, 5};

unsigned char getSpeed() {
    unsigned char speed =
        (_relays[1].target_status ? 1 : 0) +
        (_relays[2].target_status ? 2 : 0) +
        (_relays[3].target_status ? 4 : 0);
    for (unsigned char i=0; i<4; i++) {
        if (_relay_ifan02_speeds[i] == speed) return i;
    }
    return 0;
}

void setSpeed(unsigned char speed) {
    if ((0 <= speed) & (speed <= 3)) {
        if (getSpeed() == speed) return;
        unsigned char states = _relay_ifan02_speeds[speed];
        for (unsigned char i=0; i<3; i++) {
            relayStatus(i+1, states & 1 == 1);
            states >>= 1;
        }
    }
}

#endif

// -----------------------------------------------------------------------------
// RELAY
// -----------------------------------------------------------------------------

void relayPulse(unsigned char id) {

    _relays[id].pulseTicker.detach();

    byte mode = _relays[id].pulse;
    if (mode == RELAY_PULSE_NONE) return;
    unsigned long ms = _relays[id].pulse_ms;
    if (ms == 0) return;

    bool status = relayStatus(id);
    bool pulseStatus = (mode == RELAY_PULSE_ON);

    if (pulseStatus != status) {
        DEBUG_MSG_P(PSTR("[RELAY] Scheduling relay #%d back in %lums (pulse)\n"), id, ms);
        _relays[id].pulseTicker.once_ms(ms, relayToggle, id);
        // Reconfigure after dynamic pulse
        _relays[id].pulse = getSetting("rlyPulse", id, RELAY_PULSE_MODE).toInt();
        _relays[id].pulse_ms = 1000 * getSetting("rlyTime", id, RELAY_PULSE_MODE).toFloat();
    }

}

bool relayStatus(unsigned char id, bool status, bool report, bool group_report) {

    if (id >= _relays.size()) return false;

    bool changed = false;

    if (_relays[id].current_status == status) {

        if (_relays[id].target_status != status) {
            DEBUG_MSG_P(PSTR("[RELAY] #%d scheduled change cancelled\n"), id);
            _relays[id].target_status = status;
            _relays[id].report = false;
            _relays[id].group_report = false;
            changed = true;
        }

        // For RFBridge, keep sending the message even if the status is already the required
        #if RELAY_PROVIDER == RELAY_PROVIDER_RFBRIDGE
            rfbStatus(id, status);
        #endif

        // Update the pulse counter if the relay is already in the non-normal state (#454)
        relayPulse(id);

    } else {

        unsigned long current_time = millis();
        unsigned long fw_end = _relays[id].fw_start + 1000 * RELAY_FLOOD_WINDOW;
        unsigned long delay = status ? _relays[id].delay_on : _relays[id].delay_off;

        _relays[id].fw_count++;
        _relays[id].change_time = current_time + delay;

        // If current_time is off-limits the floodWindow...
        if (current_time < _relays[id].fw_start || fw_end <= current_time) {

            // We reset the floodWindow
            _relays[id].fw_start = current_time;
            _relays[id].fw_count = 1;

        // If current_time is in the floodWindow and there have been too many requests...
        } else if (_relays[id].fw_count >= RELAY_FLOOD_CHANGES) {

            // We schedule the changes to the end of the floodWindow
            // unless it's already delayed beyond that point
            if (fw_end - delay > current_time) {
                _relays[id].change_time = fw_end;
            }

        }

        _relays[id].target_status = status;
        if (report) _relays[id].report = true;
        if (group_report) _relays[id].group_report = true;

        relaySync(id);

        DEBUG_MSG_P(PSTR("[RELAY] #%d scheduled %s in %u ms\n"),
                id, status ? "ON" : "OFF",
                (_relays[id].change_time - current_time));

        changed = true;

    }

    return changed;

}

bool relayStatus(unsigned char id, bool status) {
    return relayStatus(id, status, true, true);
}

bool relayStatus(unsigned char id) {

    // Check relay ID
    if (id >= _relays.size()) return false;

    // Get status from storage
    return _relays[id].current_status;

}

void relaySync(unsigned char id) {

    // No sync if none or only one relay
    if (_relays.size() < 2) return;

    // Do not go on if we are comming from a previous sync
    if (_relayRecursive) return;

    // Flag sync mode
    _relayRecursive = true;

    byte relaySync = getSetting("rlySync", RELAY_SYNC).toInt();
    bool status = _relays[id].target_status;

    // If RELAY_SYNC_SAME all relays should have the same state
    if (relaySync == RELAY_SYNC_SAME) {
        for (unsigned short i=0; i<_relays.size(); i++) {
            if (i != id) relayStatus(i, status);
        }

    // If NONE_OR_ONE or ONE and setting ON we should set OFF all the others
    } else if (status) {
        if (relaySync != RELAY_SYNC_ANY) {
            for (unsigned short i=0; i<_relays.size(); i++) {
                if (i != id) relayStatus(i, false);
            }
        }

    // If ONLY_ONE and setting OFF we should set ON the other one
    } else {
        if (relaySync == RELAY_SYNC_ONE) {
            unsigned char i = (id + 1) % _relays.size();
            relayStatus(i, true);
        }
    }

    // Unflag sync mode
    _relayRecursive = false;

}

void relaySave() {
    unsigned char bit = 1;
    unsigned char mask = 0;
    for (unsigned int i=0; i < _relays.size(); i++) {
        if (relayStatus(i)) mask += bit;
        bit += bit;
    }
    EEPROMr.write(EEPROM_RELAY_STATUS, mask);
    DEBUG_MSG_P(PSTR("[RELAY] Saving mask: %d\n"), mask);
    EEPROMr.commit();
}

void relayToggle(unsigned char id, bool report, bool group_report) {
    if (id >= _relays.size()) return;
    relayStatus(id, !relayStatus(id), report, group_report);
}

void relayToggle(unsigned char id) {
    relayToggle(id, true, true);
}

unsigned char relayCount() {
    return _relays.size();
}

unsigned char relayParsePayload(const char * payload) {

    // Payload could be "OFF", "ON", "TOGGLE"
    // or its number equivalents: 0, 1 or 2

    if (payload[0] == '0') return 0;
    if (payload[0] == '1') return 1;
    if (payload[0] == '2') return 2;

    // trim payload
    char * p = ltrim((char *)payload);

    // to lower
    unsigned int l = strlen(p);
    if (l>6) l=6;
    for (unsigned char i=0; i<l; i++) {
        p[i] = tolower(p[i]);
    }

    unsigned int value = 0xFF;
    if (strcmp(p, "off") == 0) {
        value = 0;
    } else if (strcmp(p, "on") == 0) {
        value = 1;
    } else if (strcmp(p, "toggle") == 0) {
        value = 2;
    } else if (strcmp(p, "query") == 0) {
        value = 3;
    }

    return value;

}

bool _relayKeyCheck(const char * key) {
    return (strncmp(key, "rly", 3) == 0);
}

void _relayBackwards() {

    // 1.11.0 - 2017-12-26
    byte relayMode = getSetting("relayMode", RELAY_BOOT_MODE).toInt();
    byte relayPulseMode = getSetting("relayPulseMode", RELAY_PULSE_MODE).toInt();
    float relayPulseTime = getSetting("relayPulseTime", RELAY_PULSE_TIME).toFloat();
    if (relayPulseMode == RELAY_PULSE_NONE) relayPulseTime = 0;
    for (unsigned int i=0; i<_relays.size(); i++) {
        if (!hasSetting("rlyBoot", i)) setSetting("rlyBoot", i, relayMode);
        if (!hasSetting("rlyPulse", i)) setSetting("rlyPulse", i, relayPulseMode);
        if (!hasSetting("rlyTime", i)) setSetting("rlyTime", i, relayPulseTime);
    }
    delSetting("relayMode");
    delSetting("relayPulseMode");
    delSetting("relayPulseTime");

    // 1.14.0 - 2018-06-26
    moveSettings("relayBoot", "rlyBoot");
    moveSettings("relayPulse", "rlyPulse");
    moveSettings("relayTime", "rlyTime");
    moveSettings("relayOnDisc", "rlyOnDisc");
    moveSetting("relaySync", "rlySync");

}

void _relayBoot() {

    _relayRecursive = true;

    unsigned char bit = 1;
    bool trigger_save = false;

    // Get last statuses from EEPROM
    unsigned char mask = EEPROMr.read(EEPROM_RELAY_STATUS);
    DEBUG_MSG_P(PSTR("[RELAY] Retrieving mask: %d\n"), mask);

    // Walk the relays
    bool status = false;
    for (unsigned int i=0; i<_relays.size(); i++) {
        unsigned char boot_mode = getSetting("rlyBoot", i, RELAY_BOOT_MODE).toInt();
        DEBUG_MSG_P(PSTR("[RELAY] Relay #%d boot mode %d\n"), i, boot_mode);
        switch (boot_mode) {
            case RELAY_BOOT_SAME:
                status = ((mask & bit) == bit);
                break;
            case RELAY_BOOT_TOGGLE:
                status = ((mask & bit) != bit);
                mask ^= bit;
                trigger_save = true;
                break;
            case RELAY_BOOT_ON:
                status = true;
                break;
            case RELAY_BOOT_OFF:
            default:
                status = false;
                break;
        }
        _relays[i].current_status = !status;
        _relays[i].target_status = status;
        #if RELAY_PROVIDER == RELAY_PROVIDER_STM
            _relays[i].change_time = millis() + 3000 + 1000 * i;
        #else
            _relays[i].change_time = millis();
        #endif
        bit <<= 1;
    }

    // Save if there is any relay in the RELAY_BOOT_TOGGLE mode
    if (trigger_save) {
        EEPROMr.write(EEPROM_RELAY_STATUS, mask);
        EEPROMr.commit();
    }

    _relayRecursive = false;

}

void _relayClear() {

    for (unsigned char i = 0; i < _relays.size(); i++) {
        relay_t element = _relays[i];
        element.pulseTicker.detach();
    }
    _relays.clear();

}

void _relayConfigure() {

    _relayClear();

    // Dummy relays for AI Light, Magic Home LED Controller, H801,
    // Sonoff Dual and Sonoff RF Bridge
    unsigned char dummy = getSetting("rlyDummy", 0).toInt();
    if (dummy > 0) {

        for (unsigned char index=0; index < dummy; index++) {
            unsigned long delay_on = getSetting("rlyDelayOn", index, 0).toInt();
            unsigned long delay_off = getSetting("rlyDelayOff", index, 0).toInt();
            _relays.push_back((relay_t) {0, RELAY_TYPE_NORMAL, 0, delay_on, delay_off});
        }

    } else {

        unsigned char index = 0;
        while (index < MAX_COMPONENTS) {

            unsigned char pin = getSetting("rlyGPIO", index, GPIO_NONE).toInt();
            if (GPIO_NONE == pin) break;
            pinMode(pin, OUTPUT);

            unsigned char type = getSetting("rlyType", index, RELAY_TYPE_NORMAL).toInt();
            if (RELAY_TYPE_INVERSE == type) digitalWrite(pin, HIGH);

            unsigned char reset = getSetting("rlyResetGPIO", index, GPIO_NONE).toInt();
            if (GPIO_NONE != reset) pinMode(reset, OUTPUT);

            unsigned long delay_on = getSetting("rlyDelayOn", index, 0).toInt();
            unsigned long delay_off = getSetting("rlyDelayOff", index, 0).toInt();

            unsigned char pulse = getSetting("rlyPulse", index, RELAY_PULSE_MODE).toInt();
            unsigned long pulse_ms = 1000 * getSetting("rlyTime", index, RELAY_PULSE_TIME).toFloat();

            _relays.push_back((relay_t) { pin, type, reset, delay_on, delay_off, pulse, pulse_ms });
            ++index;

        }

    }

    DEBUG_MSG_P(PSTR("[RELAY] Relays: %d\n"), _relays.size());

}

//------------------------------------------------------------------------------
// WEBSOCKETS
//------------------------------------------------------------------------------

#if WEB_SUPPORT

void _relayWebSocketUpdate(JsonObject& root) {
    JsonArray& relay = root.createNestedArray("relayStatus");
    for (unsigned char i=0; i<relayCount(); i++) {
        relay.add(_relays[i].target_status);
    }
}

void _relayWebSocketOnStart(JsonObject& root) {

    if (relayCount() == 0) return;

    // Statuses
    _relayWebSocketUpdate(root);

    // Configuration
    JsonArray& config = root.createNestedArray("relayConfig");
    for (unsigned char i=0; i<relayCount(); i++) {
        JsonObject& line = config.createNestedObject();
        line["gpio"] = _relays[i].pin;
        line["type"] = _relays[i].type;
        line["reset"] = _relays[i].reset_pin;
        line["boot"] = getSetting("rlyBoot", i, RELAY_BOOT_MODE).toInt();
        line["pulse"] = _relays[i].pulse;
        line["pulse_ms"] = _relays[i].pulse_ms / 1000.0;
        #if MQTT_SUPPORT
            line["group"] = getSetting("mqttGroup", i, "");
            line["group_inv"] = getSetting("mqttGroupInv", i, 0).toInt();
            line["on_disc"] = getSetting("rlyOnDisc", i, 0).toInt();
        #endif
    }

    if (relayCount() > 1) {
        root["mrlyVisible"] = 1;
        root["rlySync"] = getSetting("rlySync", RELAY_SYNC);
    }

    root["rlyVisible"] = 1;

}

void _relayWebSocketOnAction(uint32_t client_id, const char * action, JsonObject& data) {

    if (strcmp(action, "relay") != 0) return;

    if (data.containsKey("status")) {

        unsigned char value = relayParsePayload(data["status"]);

        if (value == 3) {

            wsSend(_relayWebSocketUpdate);

        } else if (value < 3) {

            unsigned int relayID = 0;
            if (data.containsKey("id")) {
                String value = data["id"];
                relayID = value.toInt();
            }

            // Action to perform
            if (value == 0) {
                relayStatus(relayID, false);
            } else if (value == 1) {
                relayStatus(relayID, true);
            } else if (value == 2) {
                relayToggle(relayID);
            }

        }

    }

}

void relaySetupWS() {
    wsOnSendRegister(_relayWebSocketOnStart);
    wsOnActionRegister(_relayWebSocketOnAction);
    wsOnAfterParseRegister(_relayConfigure);
}

#endif // WEB_SUPPORT

//------------------------------------------------------------------------------
// REST API
//------------------------------------------------------------------------------

#if WEB_SUPPORT

void relaySetupAPI() {

    char key[20];

    // API entry points (protected with apikey)
    for (unsigned int relayID=0; relayID<relayCount(); relayID++) {

        snprintf_P(key, sizeof(key), PSTR("%s/%d"), MQTT_TOPIC_RELAY, relayID);
        apiRegister(key,
            [relayID](char * buffer, size_t len) {
				snprintf_P(buffer, len, PSTR("%d"), _relays[relayID].target_status ? 1 : 0);
            },
            [relayID](const char * payload) {

                unsigned char value = relayParsePayload(payload);

                if (value == 0xFF) {
                    DEBUG_MSG_P(PSTR("[RELAY] Wrong payload (%s)\n"), payload);
                    return;
                }

                if (value == 0) {
                    relayStatus(relayID, false);
                } else if (value == 1) {
                    relayStatus(relayID, true);
                } else if (value == 2) {
                    relayToggle(relayID);
                }

            }
        );

        snprintf_P(key, sizeof(key), PSTR("%s/%d"), MQTT_TOPIC_PULSE, relayID);
        apiRegister(key,
            [relayID](char * buffer, size_t len) {
                dtostrf((double) _relays[relayID].pulse_ms / 1000, 1-len, 3, buffer);
            },
            [relayID](const char * payload) {

                unsigned long pulse = 1000 * String(payload).toFloat();
                if (0 == pulse) return;

                if (RELAY_PULSE_NONE != _relays[relayID].pulse) {
                    DEBUG_MSG_P(PSTR("[RELAY] Overriding relay #%d pulse settings\n"), relayID);
                }

                _relays[relayID].pulse_ms = pulse;
                _relays[relayID].pulse = relayStatus(relayID) ? RELAY_PULSE_ON : RELAY_PULSE_OFF;
                relayToggle(relayID, true, false);

            }
        );

        #if defined(ITEAD_SONOFF_IFAN02)

            apiRegister(MQTT_TOPIC_SPEED,
                [relayID](char * buffer, size_t len) {
                    snprintf(buffer, len, "%u", getSpeed());
                },
                [relayID](const char * payload) {
                    setSpeed(atoi(payload));
                }
            );

        #endif

    }

}

#endif // WEB_SUPPORT

//------------------------------------------------------------------------------
// MQTT
//------------------------------------------------------------------------------

#if MQTT_SUPPORT

void relayMQTT(unsigned char id) {

    if (id >= _relays.size()) return;

    // Send state topic
    if (_relays[id].report) {
        _relays[id].report = false;
        mqttSend(MQTT_TOPIC_RELAY, id, _relays[id].current_status ? RELAY_MQTT_ON : RELAY_MQTT_OFF);
    }

    // Check group topic
    if (_relays[id].group_report) {
        _relays[id].group_report = false;
        String t = getSetting("mqttGroup", id, "");
        if (t.length() > 0) {
            bool status = relayStatus(id);
            if (getSetting("mqttGroupInv", id, 0).toInt() == 1) status = !status;
            mqttSendRaw(t.c_str(), status ? RELAY_MQTT_ON : RELAY_MQTT_OFF);
        }
    }

    // Send speed for IFAN02
    #if defined (ITEAD_SONOFF_IFAN02)
        char buffer[5];
        snprintf(buffer, sizeof(buffer), "%u", getSpeed());
        mqttSend(MQTT_TOPIC_SPEED, buffer);
    #endif

}

void relayMQTT() {
    for (unsigned int id=0; id < _relays.size(); id++) {
        mqttSend(MQTT_TOPIC_RELAY, id, _relays[id].current_status ? RELAY_MQTT_ON : RELAY_MQTT_OFF);
    }
}

void relayStatusWrap(unsigned char id, unsigned char value, bool is_group_topic) {
    switch (value) {
        case 0:
            relayStatus(id, false, mqttForward(), !is_group_topic);
            break;
        case 1:
            relayStatus(id, true, mqttForward(), !is_group_topic);
            break;
        case 2:
            relayToggle(id, true, true);
            break;
        default:
            _relays[id].report = true;
            relayMQTT(id);
            break;
    }
}

void relayMQTTCallback(unsigned int type, const char * topic, const char * payload) {

    if (type == MQTT_CONNECT_EVENT) {

        // Send status on connect
        #if not HEARTBEAT_REPORT_RELAY
            relayMQTT();
        #endif

        // Subscribe to own /set topic
        char relay_topic[strlen(MQTT_TOPIC_RELAY) + 3];
        snprintf_P(relay_topic, sizeof(relay_topic), PSTR("%s/+"), MQTT_TOPIC_RELAY);
        mqttSubscribe(relay_topic);

        // Subscribe to pulse topic
        char pulse_topic[strlen(MQTT_TOPIC_PULSE) + 3];
        snprintf_P(pulse_topic, sizeof(pulse_topic), PSTR("%s/+"), MQTT_TOPIC_PULSE);
        mqttSubscribe(pulse_topic);

        #if defined(ITEAD_SONOFF_IFAN02)
            mqttSubscribe(MQTT_TOPIC_SPEED);
        #endif

        // Subscribe to group topics
        for (unsigned int i=0; i < _relays.size(); i++) {
            String t = getSetting("mqttGroup", i, "");
            if (t.length() > 0) mqttSubscribeRaw(t.c_str());
        }

    }

    if (type == MQTT_MESSAGE_EVENT) {

        String t = mqttMagnitude((char *) topic);

        // magnitude is relay/#/pulse
        if (t.startsWith(MQTT_TOPIC_PULSE)) {

            unsigned int id = t.substring(strlen(MQTT_TOPIC_PULSE)+1).toInt();

            if (id >= relayCount()) {
                DEBUG_MSG_P(PSTR("[RELAY] Wrong relayID (%d)\n"), id);
                return;
            }

            unsigned long pulse = 1000 * String(payload).toFloat();
            if (0 == pulse) return;

            if (RELAY_PULSE_NONE != _relays[id].pulse) {
                DEBUG_MSG_P(PSTR("[RELAY] Overriding relay #%d pulse settings\n"), id);
            }

            _relays[id].pulse_ms = pulse;
            _relays[id].pulse = relayStatus(id) ? RELAY_PULSE_ON : RELAY_PULSE_OFF;
            relayToggle(id, true, false);

            return;

        }

        // magnitude is relay/#
        if (t.startsWith(MQTT_TOPIC_RELAY)) {

            // Get relay ID
            unsigned int id = t.substring(strlen(MQTT_TOPIC_RELAY)+1).toInt();
            if (id >= relayCount()) {
                DEBUG_MSG_P(PSTR("[RELAY] Wrong relayID (%d)\n"), id);
                return;
            }

            // Get value
            unsigned char value = relayParsePayload(payload);
            if (value == 0xFF) return;

            relayStatusWrap(id, value, false);

            return;
        }


        // Check group topics
        for (unsigned int i=0; i < _relays.size(); i++) {

            String t = getSetting("mqttGroup", i, "");

            if ((t.length() > 0) && t.equals(topic)) {

                unsigned char value = relayParsePayload(payload);
                if (value == 0xFF) return;

                if (value < 2) {
                    if (getSetting("mqttGroupInv", i, 0).toInt() == 1) {
                        value = 1 - value;
                    }
                }

                DEBUG_MSG_P(PSTR("[RELAY] Matched group topic for relayID %d\n"), i);
                relayStatusWrap(i, value, true);

            }
        }

        // Itead Sonoff IFAN02
        #if defined (ITEAD_SONOFF_IFAN02)
            if (t.startsWith(MQTT_TOPIC_SPEED)) {
                setSpeed(atoi(payload));
            }
        #endif

    }

    if (type == MQTT_DISCONNECT_EVENT) {
        for (unsigned int i=0; i < _relays.size(); i++){
            int reaction = getSetting("rlyOnDisc", i, 0).toInt();
            if (1 == reaction) {     // switch relay OFF
                DEBUG_MSG_P(PSTR("[RELAY] Reset relay (%d) due to MQTT disconnection\n"), i);
                relayStatusWrap(i, false, false);
            } else if(2 == reaction) { // switch relay ON
                DEBUG_MSG_P(PSTR("[RELAY] Set relay (%d) due to MQTT disconnection\n"), i);
                relayStatusWrap(i, true, false);
            }
        }

    }

}

void relaySetupMQTT() {
    mqttRegister(relayMQTTCallback);
}

#endif

//------------------------------------------------------------------------------
// InfluxDB
//------------------------------------------------------------------------------

#if INFLUXDB_SUPPORT

void relayInfluxDB(unsigned char id) {
    if (id >= _relays.size()) return;
    idbSend(MQTT_TOPIC_RELAY, id, relayStatus(id) ? "1" : "0");
}

#endif


//------------------------------------------------------------------------------
// Settings
//------------------------------------------------------------------------------

#if TERMINAL_SUPPORT

void _relayInitCommands() {

    settingsRegisterCommand(F("RELAY"), [](Embedis* e) {
        if (e->argc < 2) {
            DEBUG_MSG_P(PSTR("-ERROR: Wrong arguments\n"));
            return;
        }
        int id = String(e->argv[1]).toInt();
        if (id >= relayCount()) {
            DEBUG_MSG_P(PSTR("-ERROR: Wrong relayID (%d)\n"), id);
            return;
        }

        if (e->argc > 2) {
            int value = String(e->argv[2]).toInt();
            if (value == 2) {
                relayToggle(id);
            } else {
                relayStatus(id, value == 1);
            }
        }
        DEBUG_MSG_P(PSTR("Status: %s\n"), _relays[id].target_status ? "true" : "false");
        if (_relays[id].pulse != RELAY_PULSE_NONE) {
            DEBUG_MSG_P(PSTR("Pulse: %s\n"), (_relays[id].pulse == RELAY_PULSE_ON) ? "ON" : "OFF");
            DEBUG_MSG_P(PSTR("Pulse time: %d\n"), _relays[id].pulse_ms);

        }
        DEBUG_MSG_P(PSTR("+OK\n"));
    });

}

#endif // TERMINAL_SUPPORT

//------------------------------------------------------------------------------
// Setup
//------------------------------------------------------------------------------

void _relayLoop() {
    _relayProcess(false);
    _relayProcess(true);
}

void relaySetup() {

    _relayBackwards();
    _relayConfigure();
    _relayBoot();
    _relayLoop();

    settingsRegisterKeyCheck(_relayKeyCheck);
    espurnaRegisterLoop(_relayLoop);

    #if WEB_SUPPORT
        relaySetupAPI();
        relaySetupWS();
    #endif
    #if MQTT_SUPPORT
        relaySetupMQTT();
    #endif
    #if TERMINAL_SUPPORT
        _relayInitCommands();
    #endif

}
