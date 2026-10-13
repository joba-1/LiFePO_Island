/**
 * @file MqttCmd.h
 * @brief Parse a payload received on <topic>/cmd into a command.
 *
 * Header-only, no Arduino dependency (native unit tests). The payload is not
 * NUL-terminated (PubSubClient passes pointer + length). The whole payload,
 * without surrounding white space, must equal a command name ignoring case:
 * a prefix like "load" or an empty payload is no command.
 */
#pragma once

#include <stddef.h>
#include <ctype.h>

namespace MqttCmd {

enum Cmd { NONE, LOAD_ON, LOAD_OFF };

struct Entry { const char* name; Cmd cmd; };

static const Entry COMMANDS[] = {
    { "load on",  LOAD_ON },
    { "load off", LOAD_OFF },
};

/// Command name for logs ("" for NONE).
inline const char* name(Cmd cmd) {
    for (const Entry& e : COMMANDS) {
        if (e.cmd == cmd) return e.name;
    }
    return "";
}

/// Parse @p len bytes at @p payload; NONE if it is not exactly a command.
inline Cmd parse(const char* payload, size_t len) {
    if (!payload) return NONE;
    while (len && isspace((unsigned char)*payload)) { payload++; len--; }
    while (len && isspace((unsigned char)payload[len - 1])) len--;
    if (!len) return NONE;
    for (const Entry& e : COMMANDS) {
        size_t i = 0;
        while (i < len && e.name[i] &&
               tolower((unsigned char)payload[i]) == e.name[i]) i++;
        if (i == len && e.name[i] == '\0') return e.cmd;
    }
    return NONE;
}

}  // namespace MqttCmd
