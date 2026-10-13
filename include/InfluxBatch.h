/**
 * @file InfluxBatch.h
 * @brief Collect InfluxDB line-protocol lines and decide when to POST them.
 *
 * Header-only, no Arduino dependency (native unit tests); the caller does the
 * HTTP POST and reports the result with sent(). See docs/spec-telemetry.md.
 *
 * - Lines go into a caller-supplied fixed buffer, each terminated by '\n',
 *   optionally followed by a millisecond timestamp.
 * - due() is true when the oldest buffered line is older than the flush
 *   interval, or the buffer is more than 3/4 full - but never during the
 *   backoff after a failed POST.
 * - A failed POST keeps the data; the backoff doubles from RETRY_MIN_MS up to
 *   RETRY_MAX_MS. Timestamped points are idempotent, so a retry cannot
 *   duplicate them.
 * - If a new line does not fit, the oldest lines are dropped (and counted).
 *
 * All times are millis() values; comparisons are wrap-around safe.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

class InfluxBatch {
public:
    static constexpr uint32_t FLUSH_MS = 5000;
    static constexpr uint32_t RETRY_MIN_MS = 10000;
    static constexpr uint32_t RETRY_MAX_MS = 60000;

    InfluxBatch(char* buf, size_t cap) : _buf(buf), _cap(cap) { clear(); }

    /**
     * @brief Append one line (without '\n'); @p tsMs 0 = no timestamp.
     * @return false if the line (with timestamp) can never fit, or contains
     *         a newline. Older lines may have been dropped to make room.
     */
    bool add(const char* line, uint64_t tsMs, uint32_t nowMs) {
        if (!line || !*line || strchr(line, '\n')) return false;
        char ts[24] = "";
        if (tsMs) {
            char rev[21];
            size_t n = 0;
            do { rev[n++] = (char)('0' + tsMs % 10); tsMs /= 10; } while (tsMs);
            ts[0] = ' ';
            for (size_t i = 0; i < n; i++) ts[1 + i] = rev[n - 1 - i];
            ts[1 + n] = '\0';
        }
        size_t need = strlen(line) + strlen(ts) + 1;    // + '\n'
        if (need + 1 > _cap) return false;              // + NUL
        while (_len + need + 1 > _cap) dropOldest();
        if (_lines == 0) _firstMs = nowMs;
        memcpy(_buf + _len, line, strlen(line));
        _len += strlen(line);
        memcpy(_buf + _len, ts, strlen(ts));
        _len += strlen(ts);
        _buf[_len++] = '\n';
        _buf[_len] = '\0';
        _lines++;
        return true;
    }

    /// True if a POST should be made now.
    bool due(uint32_t nowMs) const {
        if (_lines == 0) return false;
        if ((int32_t)(nowMs - _nextTryMs) < 0) return false;
        return nowMs - _firstMs >= FLUSH_MS || _len * 4 >= _cap * 3;
    }

    /// Report the result of the POST of data()/size().
    void sent(bool ok, uint32_t nowMs) {
        if (ok) {
            clear();
            _nextTryMs = nowMs;
            return;
        }
        _failures++;
        uint32_t backoff = RETRY_MIN_MS;
        for (uint32_t i = 1; i < _failures && backoff < RETRY_MAX_MS; i++) backoff *= 2;
        if (backoff > RETRY_MAX_MS) backoff = RETRY_MAX_MS;
        _nextTryMs = nowMs + backoff;
    }

    const char* data() const { return _buf; }
    size_t size() const { return _len; }
    size_t lines() const { return _lines; }
    uint32_t dropped() const { return _dropped; }
    uint32_t failures() const { return _failures; }   ///< consecutive failed POSTs

private:
    void clear() {
        _len = 0;
        _lines = 0;
        _failures = 0;
        if (_cap) _buf[0] = '\0';
    }

    void dropOldest() {
        char* nl = (char*)memchr(_buf, '\n', _len);
        size_t cut = nl ? (size_t)(nl - _buf) + 1 : _len;
        memmove(_buf, _buf + cut, _len - cut + 1);     // + NUL
        _len -= cut;
        _lines--;
        _dropped++;
    }

    char* _buf;
    size_t _cap;
    size_t _len = 0;
    size_t _lines = 0;
    uint32_t _firstMs = 0;
    uint32_t _nextTryMs = 0;
    uint32_t _failures = 0;
    uint32_t _dropped = 0;
};
