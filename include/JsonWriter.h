/**
 * @file JsonWriter.h
 * @brief Small bounded JSON writer: never overflows, ok() false once truncated.
 *
 * Header-only, no Arduino dependency (native unit tests). Used for
 * /api/live (LiveJson.h) and the Home Assistant discovery messages
 * (HaDiscovery.h).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "TextEscape.h"

/// Bounded JSON writer: never overflows, ok() false once truncated.
class JsonWriter {
public:
    JsonWriter(char* out, size_t cap) : _out(out), _cap(cap) {
        if (_cap) _out[0] = '\0';
        _first[0] = true;
    }
    bool ok() const { return _ok; }
    size_t len() const { return _len; }

    void open(char c)  { sep(); raw(c); push(); }
    void close(char c) { pop(); raw(c); }
    void key(const char* k) { sep(); raw('"'); raw(k); raw("\":"); _afterKey = true; }

    void null() { sep(); raw("null"); }
    void boolean(bool v) { sep(); raw(v ? "true" : "false"); }
    void integer(long v) { sep(); char b[24]; snprintf(b, sizeof(b), "%ld", v); raw(b); }
    /// @p v / 10^@p decimals (0..6), printed with that many decimals.
    void fixed(long long v, int decimals) {
        sep();
        if (decimals < 0) decimals = 0;
        if (decimals > 6) decimals = 6;
        unsigned long long div = 1;
        for (int i = 0; i < decimals; i++) div *= 10;
        char b[48];
        const char* sign = v < 0 ? "-" : "";
        unsigned long long a = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
        if (decimals > 0) {
            snprintf(b, sizeof(b), "%s%llu.%0*u", sign, a / div, decimals, (unsigned)(a % div));
        } else {
            snprintf(b, sizeof(b), "%s%llu", sign, a);
        }
        raw(b);
    }
    void string(const char* s) {
        sep();
        char esc[2 * 256 + 1];
        TextEscape::json(esc, sizeof(esc), s);
        raw('"'); raw(esc); raw('"');
    }

private:
    void sep() {
        if (_afterKey) { _afterKey = false; return; }
        if (!_first[_depth]) raw(',');
        _first[_depth] = false;
    }
    void push() { if (_depth + 1 < MAX_DEPTH) _first[++_depth] = true; else _ok = false; }
    void pop()  { if (_depth) _depth--; }
    void raw(char c) { char s[2] = { c, '\0' }; raw(s); }
    void raw(const char* s) {
        size_t n = strlen(s);
        if (!_ok || _len + n >= _cap) { _ok = false; return; }
        memcpy(_out + _len, s, n);
        _len += n;
        _out[_len] = '\0';
    }

    static constexpr int MAX_DEPTH = 8;
    char* _out;
    size_t _cap;
    size_t _len = 0;
    bool _ok = true;
    bool _afterKey = false;
    int _depth = 0;
    bool _first[MAX_DEPTH];
};
