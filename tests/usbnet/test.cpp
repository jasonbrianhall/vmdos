// Tests for src/usbnet_frame.h (RNDIS and CDC-NCM framing), on Linux:
//   make -C tests/usbnet
#include "../../src/usbnet_frame.h"
#include <stdio.h>
#include <vector>
using namespace usbnet;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static std::vector<uint8_t> frame(unsigned len, uint8_t seed)
{
    std::vector<uint8_t> f(len);
    for (unsigned i = 0; i < len; i++) f[i] = (uint8_t)(seed + i * 7);
    return f;
}

struct Got { std::vector<std::vector<uint8_t>> f; void operator()(const uint8_t* p, unsigned n) { f.emplace_back(p, p + n); } };

static void rndis_tests()
{
    uint8_t buf[4096];
    for (unsigned len : {14u, 60u, 1514u}) {            // round trip
        auto f = frame(len, (uint8_t)len);
        unsigned n = rndis_wrap(buf, f.data(), len);
        CHECK(n == 44 + len && rd32(buf) == 1 && rd32(buf + 4) == n && rd32(buf + 8) == 36 && rd32(buf + 12) == len);
        Got g; CHECK(rndis_unwrap(buf, n, std::ref(g)) == 1);
        CHECK(g.f.size() == 1 && g.f[0] == f);
    }
    {                                                   // two in one transfer, and a padding byte after
        auto a = frame(100, 1), b = frame(200, 2);
        unsigned n = rndis_wrap(buf, a.data(), 100);
        n += rndis_wrap(buf + n, b.data(), 200);
        buf[n++] = 0;
        Got g; CHECK(rndis_unwrap(buf, n, std::ref(g)) == 2);
        CHECK(g.f.size() == 2 && g.f[0] == a && g.f[1] == b);
    }
    {                                                   // a status message in between is skipped
        auto a = frame(64, 3);
        uint8_t* p = buf;
        memset(p, 0, 20); wr32(p, RNDIS_INDICATE_STATUS_MSG); wr32(p + 4, 20);
        unsigned n = 20 + rndis_wrap(buf + 20, a.data(), 64);
        Got g; CHECK(rndis_unwrap(buf, n, std::ref(g)) == 1 && g.f[0] == a);
    }
    {                                                   // lengths that point outside are dropped, not followed
        auto a = frame(64, 4);
        unsigned n = rndis_wrap(buf, a.data(), 64);
        wr32(buf + 12, 5000);
        Got g; CHECK(rndis_unwrap(buf, n, std::ref(g)) == 0);
        wr32(buf + 12, 64); wr32(buf + 4, 9999);
        CHECK(rndis_unwrap(buf, n, std::ref(g)) == 0);
        wr32(buf + 4, 0);
        CHECK(rndis_unwrap(buf, n, std::ref(g)) == 0);
    }
    {                                                   // whole or not, packet by packet
        auto a = frame(84, 6);
        unsigned n = rndis_wrap(buf, a.data(), 84);    // 128: two 64-byte packets
        CHECK(!rndis_complete(buf, 64) && rndis_complete(buf, 128));
        unsigned m = n + rndis_wrap(buf + n, a.data(), 84);
        CHECK(!rndis_complete(buf, 192) && rndis_complete(buf, m));
        CHECK(!rndis_complete(buf, 0));
    }
    CHECK(rndis_filter(0x0C) == (NDIS_DIRECTED | NDIS_BROADCAST));
    CHECK(rndis_filter(0x0F) == (NDIS_DIRECTED | NDIS_BROADCAST | NDIS_ALL_MULTICAST | NDIS_PROMISCUOUS));
}

static void ncm_tests()
{
    uint8_t buf[8192];
    for (unsigned div : {4u, 8u, 32u, 512u}) for (unsigned rem : {0u, 2u}) for (unsigned len : {14u, 60u, 1514u}) {
        NcmOut o; o.divisor = div; o.remainder = rem; o.align = 4; o.max = 4096; ncm_out_fix(o);
        auto f = frame(len, (uint8_t)(len + div));
        unsigned n = ncm_wrap(buf, sizeof buf, o, f.data(), len, 512);
        CHECK(n > 0 && rd32(buf) == NTH16_SIG && rd16(buf + 4) == 12 && rd16(buf + 8) == n);
        unsigned ndp = rd16(buf + 10);
        CHECK(ndp % 4 == 0 && rd32(buf + ndp) == NDP16_SIG && rd16(buf + ndp + 4) == 16);
        unsigned at = rd16(buf + ndp + 8);
        CHECK(at % o.divisor == o.remainder && rd16(buf + ndp + 10) == len);
        CHECK(rd16(buf + ndp + 12) == 0 && rd16(buf + ndp + 14) == 0);
        CHECK(n % 512 != 0);                            // never ends on a packet boundary
        Got g; CHECK(ncm_unwrap(buf, n, std::ref(g)) == 1 && g.f.size() == 1 && g.f[0] == f);
    }
    {                                                   // sequence numbers count up
        NcmOut o; auto f = frame(60, 9);
        ncm_wrap(buf, sizeof buf, o, f.data(), 60, 64); CHECK(rd16(buf + 6) == 0);
        ncm_wrap(buf, sizeof buf, o, f.data(), 60, 64); CHECK(rd16(buf + 6) == 1);
    }
    {                                                   // too big for the device's NTB: refused
        NcmOut o; o.max = 2048; auto f = frame(1514, 1);
        o.divisor = 512; o.remainder = 0;               // datagram at 512: 2026, fits
        CHECK(ncm_wrap(buf, sizeof buf, o, f.data(), 1514, 512) == 2026);
        CHECK(ncm_wrap(buf, 1000, o, f.data(), 1514, 512) == 0);
    }
    {                                                   // the device's way: one NTB, two NDPs, three datagrams, CRC kind too
        auto a = frame(60, 1), b = frame(1000, 2), c = frame(300, 3);
        memset(buf, 0, sizeof buf);
        wr32(buf, NTH16_SIG); wr16(buf + 4, 12); wr16(buf + 6, 7);
        memcpy(buf + 16, a.data(), 60);
        memcpy(buf + 80, b.data(), 1000);
        memcpy(buf + 1084, c.data(), 300); memset(buf + 1384, 0xEE, 4);   // with a CRC after it
        unsigned n1 = 1392, n2 = n1 + 24;
        wr16(buf + 10, n1);
        wr32(buf + n1, NDP16_SIG); wr16(buf + n1 + 4, 20); wr16(buf + n1 + 6, n2);
        wr16(buf + n1 + 8, 16); wr16(buf + n1 + 10, 60); wr16(buf + n1 + 12, 80); wr16(buf + n1 + 14, 1000);
        wr32(buf + n2, NDP16_SIG_CRC); wr16(buf + n2 + 4, 16); wr16(buf + n2 + 6, 0);
        wr16(buf + n2 + 8, 1084); wr16(buf + n2 + 10, 304);
        unsigned block = n2 + 16;
        wr16(buf + 8, block);
        Got g; CHECK(ncm_unwrap(buf, block + 50, std::ref(g)) == 3);   // bytes past the block ignored
        CHECK(g.f.size() == 3 && g.f[0] == a && g.f[1] == b && g.f[2] == c);
        // An NDP that points back at itself stops, a bad signature yields nothing.
        wr16(buf + n2 + 6, n2);
        Got g2; CHECK(ncm_unwrap(buf, block, std::ref(g2)) <= 3 + 32);
        wr32(buf, 0x12345678);
        Got g3; CHECK(ncm_unwrap(buf, block, std::ref(g3)) == 0);
    }
    {                                                   // datagram entries pointing outside the block are skipped
        NcmOut o; auto f = frame(100, 5);
        unsigned n = ncm_wrap(buf, sizeof buf, o, f.data(), 100, 512);
        unsigned ndp = rd16(buf + 10);
        wr16(buf + ndp + 10, 60000);
        Got g; CHECK(ncm_unwrap(buf, n, std::ref(g)) == 0);
    }
    {                                                   // whole NTB or not
        NcmOut o; auto f = frame(60, 8);
        unsigned n = ncm_wrap(buf, sizeof buf, o, f.data(), 60, 0);
        CHECK(ncm_complete(buf, n) && !ncm_complete(buf, n - 1) && !ncm_complete(buf, 8));
    }
    {                                                   // the device's numbers made safe
        NcmOut o; o.divisor = 3; o.remainder = 7; o.align = 0; o.max = 100; ncm_out_fix(o);
        CHECK(o.divisor == 4 && o.remainder == 0 && o.align == 4 && o.max == 2048);
    }
}

int main()
{
    rndis_tests();
    ncm_tests();
    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
