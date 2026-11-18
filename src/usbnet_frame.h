// USB Ethernet framing for RNDIS and CDC-NCM: wrapping one frame to send,
// and taking frames out of a received transfer. Plain functions on byte
// buffers, nothing of the kernel's, so tests/usbnet can run them on Linux.
// usb.cpp's USB Ethernet driver (un_*) uses them; CDC-ECM has no framing.
#pragma once
#include <stdint.h>
#if __STDC_HOSTED__
#include <string.h>                                   // in the kernel: kernel.h's memcpy, memset
#endif

namespace usbnet {

inline uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
inline uint32_t rd32(const uint8_t* p) { return rd16(p) | rd16(p + 2) << 16; }
inline void wr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void wr32(uint8_t* p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

// ---------------------------------------------------------------- RNDIS
// Every frame on the bulk endpoints is a REMOTE_NDIS_PACKET_MSG: a 44-byte
// header (type 1, message length, data offset and length counted from byte
// 8, then out-of-band and per-packet info we don't use), then the frame.
// A device may put several in one transfer.
enum : uint32_t {
    RNDIS_PACKET_MSG = 1, RNDIS_INITIALIZE_MSG = 2, RNDIS_QUERY_MSG = 4, RNDIS_SET_MSG = 5,
    RNDIS_INDICATE_STATUS_MSG = 7, RNDIS_KEEPALIVE_MSG = 8, RNDIS_CMPLT = 0x80000000u,
    RNDIS_HDR = 44,
    OID_GEN_CURRENT_PACKET_FILTER = 0x0001010E, OID_GEN_MEDIA_CONNECT_STATUS = 0x00010114,
    OID_802_3_PERMANENT_ADDRESS = 0x01010101,
    NDIS_DIRECTED = 0x01, NDIS_MULTICAST = 0x02, NDIS_ALL_MULTICAST = 0x04,
    NDIS_BROADCAST = 0x08, NDIS_PROMISCUOUS = 0x20,
};

// The frame f (len bytes) as a packet message in out; its length.
inline unsigned rndis_wrap(uint8_t* out, const uint8_t* f, unsigned len)
{
    memset(out, 0, RNDIS_HDR);
    wr32(out, RNDIS_PACKET_MSG);
    wr32(out + 4, RNDIS_HDR + len);
    wr32(out + 8, RNDIS_HDR - 8);
    wr32(out + 12, len);
    memcpy(out + RNDIS_HDR, f, len);
    return RNDIS_HDR + len;
}

// Each frame in a received transfer to rx(frame, len); how many there were.
template <class F> int rndis_unwrap(const uint8_t* b, unsigned len, F rx)
{
    int n = 0;
    for (unsigned off = 0; off + RNDIS_HDR <= len; ) {
        const uint8_t* m = b + off;
        uint32_t type = rd32(m), mlen = rd32(m + 4);
        if (mlen < 8 || mlen > len - off) break;        // garbage: drop the rest
        if (type == RNDIS_PACKET_MSG && mlen >= RNDIS_HDR) {
            uint32_t doff = rd32(m + 8), dlen = rd32(m + 12);
            if (doff <= mlen - 8 && dlen <= mlen - 8 - doff && dlen >= 14) { rx(m + 8 + doff, dlen); n++; }
        }
        off += mlen;
    }
    return n;
}

// Whether a transfer being put together packet by packet (UHCI) is whole:
// its messages end exactly where it does. Some devices (QEMU's) send no
// zero-length packet after a transfer that fills its last packet.
inline bool rndis_complete(const uint8_t* b, unsigned len)
{
    unsigned off = 0;
    while (off + 8 <= len) {
        uint32_t mlen = rd32(b + off + 4);
        if (mlen < 8) return false;
        if (mlen > len - off) return false;
        off += mlen;
    }
    return off == len && len > 0;
}

// The filter bits of CDC's SET_ETHERNET_PACKET_FILTER as NDIS's.
inline uint32_t rndis_filter(int cdc)
{
    uint32_t f = 0;
    if (cdc & 0x01) f |= NDIS_PROMISCUOUS;
    if (cdc & 0x02) f |= NDIS_ALL_MULTICAST;
    if (cdc & 0x04) f |= NDIS_DIRECTED;
    if (cdc & 0x08) f |= NDIS_BROADCAST;
    if (cdc & 0x10) f |= NDIS_MULTICAST;
    return f;
}

// ---------------------------------------------------------------- CDC-NCM
// Frames travel in NTBs (here the 16-bit kind, the default): a 12-byte
// header (NTH16: "NCMH", its length, a sequence number, the block's length,
// where the first datagram pointer table is), then NDP16 tables ("NCM0":
// pairs of datagram offset and length, ending with a 0, 0 pair, and where
// the next table is) and the datagrams themselves. We send one frame per
// NTB; a device may put many in each.
enum : uint32_t { NTH16_SIG = 0x484D434E, NDP16_SIG = 0x304D434E, NDP16_SIG_CRC = 0x314D434E };

struct NcmOut {                                       // GET_NTB_PARAMETERS' OUT side
    unsigned divisor = 4, remainder = 0, align = 4, max = 2048;
    uint16_t seq = 0;
};

// Make the device's numbers safe to use (as Linux's cdc_ncm does).
inline void ncm_out_fix(NcmOut& o)
{
    if (o.divisor < 4 || o.divisor > 512 || (o.divisor & (o.divisor - 1))) o.divisor = 4;
    if (o.remainder >= o.divisor) o.remainder = 0;
    if (o.align < 4 || o.align > 64 || (o.align & (o.align - 1))) o.align = 4;
    if (o.max < 2048 || o.max > 65535) o.max = 2048;
}

// The frame as an NTB in out (room for cap bytes); its length, 0 if it
// doesn't fit. pad_to: the bulk endpoint's packet size, so that a block
// that would end exactly on a packet boundary gets one more byte (the
// device then sees a short packet rather than waiting for a zero-length one).
inline unsigned ncm_wrap(uint8_t* out, unsigned cap, NcmOut& o, const uint8_t* f, unsigned len, unsigned pad_to)
{
    unsigned ndp = (12 + o.align - 1) & ~(o.align - 1);
    unsigned dg = ndp + 16;
    while (dg % o.divisor != o.remainder) dg++;
    unsigned block = dg + len;
    if (pad_to && block % pad_to == 0 && block < o.max) block++;
    if (block > cap || block > o.max) return 0;
    memset(out, 0, dg);
    wr32(out, NTH16_SIG);
    wr16(out + 4, 12);
    wr16(out + 6, o.seq++);
    wr16(out + 8, block);
    wr16(out + 10, ndp);
    wr32(out + ndp, NDP16_SIG);
    wr16(out + ndp + 4, 16);
    wr16(out + ndp + 6, 0);
    wr16(out + ndp + 8, dg);
    wr16(out + ndp + 10, len);                        // then the 0, 0 pair (memset)
    memcpy(out + dg, f, len);
    if (block > dg + len) out[dg + len] = 0;
    return block;
}

// As rndis_complete: the NTB's block length says where it ends.
inline bool ncm_complete(const uint8_t* b, unsigned len)
{
    return len >= 12 && rd32(b) == NTH16_SIG && rd16(b + 8) == len;
}

// Each datagram in a received NTB to rx(frame, len); how many there were.
template <class F> int ncm_unwrap(const uint8_t* b, unsigned len, F rx)
{
    if (len < 12 || rd32(b) != NTH16_SIG || rd16(b + 4) < 12) return 0;
    unsigned block = rd16(b + 8);
    if (block && block < len) len = block;            // what's past the block isn't the device's
    int n = 0;
    unsigned ndp = rd16(b + 10);
    for (int tables = 0; ndp && tables < 32; tables++) {
        if ((ndp & 3) || ndp < 12 || ndp + 16 > len) break;
        uint32_t sig = rd32(b + ndp);
        unsigned nlen = rd16(b + ndp + 4);
        if ((sig != NDP16_SIG && sig != NDP16_SIG_CRC) || nlen < 16 || ndp + nlen > len) break;
        for (unsigned e = ndp + 8; e + 4 <= ndp + nlen; e += 4) {
            unsigned at = rd16(b + e), dl = rd16(b + e + 2);
            if (!at || !dl) break;
            if (sig == NDP16_SIG_CRC) { if (dl < 4) continue; dl -= 4; }   // the CRC isn't part of the frame
            if (at < 12 || at > len || dl > len - at || dl < 14) continue;
            rx(b + at, dl);
            n++;
        }
        ndp = rd16(b + ndp + 6);
    }
    return n;
}

} // namespace usbnet
