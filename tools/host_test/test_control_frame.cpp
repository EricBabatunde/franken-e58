// Host unit test for the control frame codec (components/comms/control_frame.cpp), M6 D-M6-05.
// Build and run on the laptop (g++, C++17):
//   g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/comms/include components/comms/control_frame.cpp
//       tools/host_test/test_control_frame.cpp -o .pio/host/test_control_frame   (one line)
//   .pio/host/test_control_frame components/comms/test_vectors/control_frame_vectors.txt
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "control_frame.h"

namespace fr = comms::frame;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}

uint32_t g_rng = 0x00E58E58;
uint32_t next_rand() {  // xorshift32, fixed seed: the same frames on every run
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: %s <control_frame_vectors.txt>\n", argv[0]);
        return 2;
    }

    // 1. The catalogue check value, independent of the vectors file.
    const char* check_str = "123456789";
    check(fr::crc8(reinterpret_cast<const uint8_t*>(check_str), 9) == 0xF4, "crc8(\"123456789\") == 0xF4");

    // 2. The shared vectors.
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::printf("cannot open %s\n", argv[1]);
        return 2;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int vchecked = 0;
    int first_fail = 0;
    const int vfailed = fr::run_vectors(text.data(), text.size(), &vchecked, &first_fail);
    std::printf("vectors: %d checks, %d failed%s\n", vchecked, vfailed, vfailed ? " (see first_fail)" : "");
    if (vfailed) {
        std::printf("FAIL: first failing vector on line %d\n", first_fail);
    }
    g_checks += vchecked;
    g_failed += vfailed;
    check(vchecked >= 30, "vectors file has at least 30 checks");

    // 3. Round trip and single-bit error detection on pseudo-random frames.
    for (int i = 0; i < 2000; ++i) {
        fr::Frame f;
        f.type = static_cast<fr::Type>(1 + next_rand() % 3);
        f.seq = static_cast<uint16_t>(next_rand());
        f.roll = static_cast<int16_t>(next_rand());
        f.pitch = static_cast<int16_t>(next_rand());
        f.yaw = static_cast<int16_t>(next_rand());
        f.throttle = static_cast<uint16_t>(next_rand());
        f.flags = static_cast<uint8_t>(next_rand());
        uint8_t buf[fr::kBytes];
        fr::encode(f, buf);
        fr::Frame g;
        const bool rt = fr::decode(buf, sizeof(buf), &g) == fr::Error::kNone && g.type == f.type && g.seq == f.seq &&
                        g.roll == f.roll && g.pitch == f.pitch && g.yaw == f.yaw && g.throttle == f.throttle &&
                        g.flags == f.flags;
        check(rt, "random frame round trip");
        // Every single-bit error in bytes 1 to 13 is rejected (a CRC detects all single-bit
        // errors). Byte 0 errors are caught as magic.
        bool all_caught = true;
        for (size_t byte = 0; byte < fr::kBytes; ++byte) {
            for (int bit = 0; bit < 8; ++bit) {
                uint8_t bad[fr::kBytes];
                std::memcpy(bad, buf, sizeof(bad));
                bad[byte] ^= static_cast<uint8_t>(1u << bit);
                fr::Frame h;
                if (fr::decode(bad, sizeof(bad), &h) == fr::Error::kNone) all_caught = false;
            }
        }
        check(all_caught, "every single-bit error rejected");
    }

    // 4. Pong saturation and echo.
    fr::Frame ping;
    ping.type = fr::Type::kPing;
    ping.seq = 0xBEEF;
    ping.roll = -5;
    ping.flags = 0x80;
    const fr::Frame p1 = fr::make_pong(ping, 100000);
    check(p1.type == fr::Type::kPong && p1.throttle == 0xFFFF && p1.seq == 0xBEEF && p1.roll == -5 && p1.flags == 0x80,
          "pong echoes and saturates");

    std::printf("host test: %d checks, %d failed -> %s\n", g_checks, g_failed, g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
