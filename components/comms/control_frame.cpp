#include "control_frame.h"

namespace comms::frame {
namespace {

void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// ---- Vector file parser: whitespace-separated tokens, '#' starts a comment to end of line.

struct Cursor {
    const char* p;
    const char* end;
};

bool at_line_end(const Cursor& c) { return c.p >= c.end || *c.p == '\n'; }

void skip_blanks(Cursor& c) {
    while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\r')) ++c.p;
    if (c.p < c.end && *c.p == '#') {
        while (c.p < c.end && *c.p != '\n') ++c.p;
    }
}

// Copies the next token on this line into buf. False at the end of the line.
bool token(Cursor& c, char* buf, size_t cap) {
    skip_blanks(c);
    if (at_line_end(c)) return false;
    size_t n = 0;
    while (c.p < c.end && *c.p != ' ' && *c.p != '\t' && *c.p != '\r' && *c.p != '\n') {
        if (n + 1 < cap) buf[n++] = *c.p;
        ++c.p;
    }
    buf[n] = '\0';
    return true;
}

bool streq(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// Hex string to bytes. "-" is an empty buffer. Returns the byte count, or -1 if malformed.
int parse_hex(const char* s, uint8_t* out, size_t cap) {
    if (streq(s, "-")) return 0;
    size_t n = 0;
    while (s[0] && s[1]) {
        const int hi = hex_digit(s[0]);
        const int lo = hex_digit(s[1]);
        if (hi < 0 || lo < 0 || n >= cap) return -1;
        out[n++] = static_cast<uint8_t>((hi << 4) | lo);
        s += 2;
    }
    return s[0] ? -1 : static_cast<int>(n);
}

bool parse_int(const char* s, long* out) {
    bool neg = false;
    if (*s == '-') { neg = true; ++s; }
    if (!*s) return false;
    long v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) return false;
        for (; *s; ++s) {
            const int d = hex_digit(*s);
            if (d < 0) return false;
            v = v * 16 + d;
        }
    } else {
        for (; *s; ++s) {
            if (*s < '0' || *s > '9') return false;
            v = v * 10 + (*s - '0');
        }
    }
    *out = neg ? -v : v;
    return true;
}

Error error_from_name(const char* s, bool* ok) {
    *ok = true;
    if (streq(s, "ok")) return Error::kNone;
    if (streq(s, "length")) return Error::kLength;
    if (streq(s, "magic")) return Error::kMagic;
    if (streq(s, "crc")) return Error::kCrc;
    if (streq(s, "type")) return Error::kType;
    *ok = false;
    return Error::kNone;
}

// One vector line. Returns the number of failed checks and adds to *checked.
//   crc   <hex bytes> <expected crc>
//   frame <hex bytes> <verdict> [type seq roll pitch yaw throttle flags]
//   pong  <hex ping frame> <turnaround us> <hex expected pong frame>
int run_line(Cursor& c, const char* kind, int* checked) {
    char tok[64];
    uint8_t buf[32];
    uint8_t exp[32];
    if (streq(kind, "crc")) {
        long want = 0;
        if (!token(c, tok, sizeof(tok))) return 1;
        const int n = parse_hex(tok, buf, sizeof(buf));
        if (n < 0 || !token(c, tok, sizeof(tok)) || !parse_int(tok, &want)) return 1;
        ++*checked;
        return crc8(buf, static_cast<size_t>(n)) == static_cast<uint8_t>(want) ? 0 : 1;
    }
    if (streq(kind, "frame")) {
        if (!token(c, tok, sizeof(tok))) return 1;
        const int n = parse_hex(tok, buf, sizeof(buf));
        bool ok = false;
        if (n < 0 || !token(c, tok, sizeof(tok))) return 1;
        const Error want = error_from_name(tok, &ok);
        if (!ok) return 1;
        Frame f;
        ++*checked;
        if (decode(buf, static_cast<size_t>(n), &f) != want) return 1;
        if (want != Error::kNone) return 0;
        long v[7];
        for (long& x : v) {
            if (!token(c, tok, sizeof(tok)) || !parse_int(tok, &x)) return 1;
        }
        ++*checked;
        if (static_cast<long>(f.type) != v[0] || f.seq != v[1] || f.roll != v[2] || f.pitch != v[3] ||
            f.yaw != v[4] || f.throttle != v[5] || f.flags != v[6]) {
            return 1;
        }
        uint8_t again[kBytes];  // round trip: encoding the decoded frame gives the same bytes
        encode(f, again);
        ++*checked;
        for (size_t i = 0; i < kBytes; ++i) {
            if (again[i] != buf[i]) return 1;
        }
        return 0;
    }
    if (streq(kind, "pong")) {
        long ta = 0;
        if (!token(c, tok, sizeof(tok))) return 1;
        const int n = parse_hex(tok, buf, sizeof(buf));
        if (n < 0 || !token(c, tok, sizeof(tok)) || !parse_int(tok, &ta) || !token(c, tok, sizeof(tok))) return 1;
        const int m = parse_hex(tok, exp, sizeof(exp));
        Frame ping;
        if (m != static_cast<int>(kBytes) || decode(buf, static_cast<size_t>(n), &ping) != Error::kNone) return 1;
        uint8_t out[kBytes];
        encode(make_pong(ping, static_cast<uint32_t>(ta)), out);
        ++*checked;
        for (size_t i = 0; i < kBytes; ++i) {
            if (out[i] != exp[i]) return 1;
        }
        return 0;
    }
    return 1;  // unknown line kind
}

}  // namespace

const char* error_name(Error e) {
    switch (e) {
        case Error::kNone:   return "ok";
        case Error::kLength: return "length";
        case Error::kMagic:  return "magic";
        case Error::kCrc:    return "crc";
        case Error::kType:   return "type";
    }
    return "?";
}

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

void encode(const Frame& f, uint8_t out[kBytes]) {
    out[0] = kMagic;
    out[1] = static_cast<uint8_t>(f.type);
    put_u16(out + 2, f.seq);
    put_u16(out + 4, static_cast<uint16_t>(f.roll));
    put_u16(out + 6, static_cast<uint16_t>(f.pitch));
    put_u16(out + 8, static_cast<uint16_t>(f.yaw));
    put_u16(out + 10, f.throttle);
    out[12] = f.flags;
    out[13] = crc8(out, kBytes - 1);
}

Error decode(const uint8_t* buf, size_t len, Frame* out) {
    if (len != kBytes) return Error::kLength;
    if (buf[0] != kMagic) return Error::kMagic;
    if (crc8(buf, kBytes - 1) != buf[13]) return Error::kCrc;
    if (buf[1] < static_cast<uint8_t>(Type::kControl) || buf[1] > static_cast<uint8_t>(Type::kPong)) {
        return Error::kType;
    }
    out->type = static_cast<Type>(buf[1]);
    out->seq = get_u16(buf + 2);
    out->roll = static_cast<int16_t>(get_u16(buf + 4));
    out->pitch = static_cast<int16_t>(get_u16(buf + 6));
    out->yaw = static_cast<int16_t>(get_u16(buf + 8));
    out->throttle = get_u16(buf + 10);
    out->flags = buf[12];
    return Error::kNone;
}

Frame make_pong(const Frame& ping, uint32_t turnaround_us) {
    Frame p = ping;
    p.type = Type::kPong;
    p.throttle = turnaround_us > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(turnaround_us);
    return p;
}

int run_vectors(const char* text, size_t len, int* checked, int* first_fail) {
    Cursor c{text, text + len};
    int failed = 0;
    int line = 1;
    *checked = 0;
    *first_fail = 0;
    while (c.p < c.end) {
        char kind[16];
        if (token(c, kind, sizeof(kind))) {
            const int f = run_line(c, kind, checked);
            char extra[8];
            const int bad = f + (token(c, extra, sizeof(extra)) ? 1 : 0);  // trailing tokens
            if (bad && *first_fail == 0) *first_fail = line;
            failed += bad;
        }
        while (c.p < c.end && *c.p != '\n') ++c.p;
        if (c.p < c.end) { ++c.p; ++line; }
    }
    return failed;
}

}  // namespace comms::frame
