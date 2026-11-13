// Hyper-V synthetic network adapter (Hyper-V Manager: "Network Adapter",
// not "Legacy Network Adapter"). Linux: drivers/net/hyperv (netvsc.c,
// rndis_filter.c).
//
// A VMBus channel (vmbus.cpp) carrying NVSP, Hyper-V's network protocol,
// which carries RNDIS (USB's remote NDIS): NVSP version agreed on, NDIS
// version and configuration told, a receive buffer shared (GPADL); then
// RNDIS's initialize, the MAC (permanent address) and link asked for, the
// packet filter set. Frames out: an RNDIS packet message in one of our
// buffers, its page given to the host (GPA direct). Frames in: the host
// puts RNDIS messages in the receive buffer and sends a transfer-page
// packet saying where; each such packet is acknowledged so the host can
// use that part of the buffer again. Polled from the timer tick.
//
// The MAC can't be changed from here (Hyper-V drops frames from any other
// address unless the adapter's "Enable MAC address spoofing" is on): a
// different one makes the adapter take everything and vmdos filters.
#include "net.hpp"
#include "vmbus.hpp"

using namespace net;

namespace {
namespace nv {

const char* const NET_GUID = "f8615163-df3e-46c5-913f-f2d2f965ed0e";
const uint32_t RING_PAGES = 16;                 // each way (Linux: 128 KiB and more)
const uint32_t RXBUF = 1024 * 1024;             // the receive buffer (Linux: 16 MiB)
const int NTX = 16, TXSZ = 2048;                // send buffers, two to a page
const uint16_t RXBUF_ID = 0xCAFE;               // NETVSC_RECEIVE_BUFFER_ID
const uint64_t TID_INIT = 1, TID_RXBUF = 2, TID_CTL = 3, TID_TX = 0x100;

enum {
    NVSP_INIT = 1, NVSP_INIT_COMPLETE = 2, NVSP1_SEND_NDIS_VER = 100, NVSP1_SEND_RECV_BUF = 101,
    NVSP1_SEND_RECV_BUF_COMPLETE = 102, NVSP1_SEND_RNDIS_PKT = 107, NVSP1_SEND_RNDIS_PKT_COMPLETE = 108,
    NVSP2_SEND_NDIS_CONFIG = 125,
};
const uint32_t NVSP_SUCCESS = 1, NVSP_MSG = 40;  // sizeof(struct nvsp_message)
const uint32_t nvsp_versions[] = { 0x60001, 0x60000, 0x50000, 0x40000, 0x30002, 0x2 };

enum : uint32_t {
    RNDIS_PACKET = 1, RNDIS_INIT = 2, RNDIS_QUERY = 4, RNDIS_SET = 5, RNDIS_INDICATE = 7,
    RNDIS_INIT_C = 0x80000002, RNDIS_QUERY_C = 0x80000004, RNDIS_SET_C = 0x80000005,
    ST_MEDIA_CONNECT = 0x4001000B, ST_MEDIA_DISCONNECT = 0x4001000C,
    OID_PERMANENT_ADDRESS = 0x01010101, OID_PACKET_FILTER = 0x0001010E, OID_MEDIA_CONNECT_STATUS = 0x00010114,
    F_DIRECTED = 1, F_ALL_MULTICAST = 4, F_BROADCAST = 8, F_PROMISCUOUS = 0x20,
};

struct State {
    VmbusChannel* ch;
    uint8_t* rxbuf;
    uint8_t* pkt;                               // a packet read from the ring (4 KiB)
    uint8_t* ctl;                               // an RNDIS request (a page of its own)
    uint8_t* txb;
    uint32_t tx_busy;                           // bit n: the host hasn't finished with send buffer n
    uint32_t nvsp_ver, req_id, want_req, filter;
    bool got_comp, got_resp, link, ready;
    uint64_t want_tid;
    uint8_t comp[NVSP_MSG];                     // the NVSP completion waited for
    uint8_t resp[128];                          // the RNDIS reply waited for
    uint8_t addr[6];                            // the address DOS uses
    int mode;
};
State st_[MAXCARDS];
int n_st;

inline uint32_t pa(const void* p) { return (uint32_t)(uintptr_t)p; }
inline uint32_t get32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
inline State& S(Card& c) { return *(State*)c.h.st; }

bool wanted(State& h, Card& c, const uint8_t* f)
{
    int m = h.mode;
    if (m <= 1) return false;
    if (m >= 6) return true;
    if (!(f[0] & 1)) return !memcmp(f, h.addr, 6);
    bool bcast = (f[0] & f[1] & f[2] & f[3] & f[4] & f[5]) == 0xFF;
    return bcast ? m >= 3 : m >= 4;
}

void rndis_in(State& h, Card& c, const uint8_t* m, uint32_t len)
{
    if (len < 8) return;
    uint32_t type = get32(m), mlen = get32(m + 4);
    if (mlen > len) mlen = len;
    switch (type) {
    case RNDIS_PACKET: {
        if (mlen < 44) { st.rx_err++; return; }
        uint32_t off = 8 + get32(m + 8), n = get32(m + 12);
        if (off > mlen || n > mlen - off) { st.rx_err++; return; }
        if (h.ready && wanted(h, c, m + off)) enqueue(m + off, n);
        return; }
    case RNDIS_INIT_C: case RNDIS_QUERY_C: case RNDIS_SET_C:
        if (mlen >= 16 && get32(m + 8) == h.want_req) {
            memset(h.resp, 0, sizeof h.resp);
            memcpy(h.resp, m, mlen < sizeof h.resp ? mlen : sizeof h.resp);
            h.got_resp = true;
        }
        return;
    case RNDIS_INDICATE:
        if (mlen >= 12) {
            uint32_t s = get32(m + 8);
            if (s == ST_MEDIA_CONNECT) h.link = true;
            else if (s == ST_MEDIA_DISCONNECT) h.link = false;
            if (log_on && (s == ST_MEDIA_CONNECT || s == ST_MEDIA_DISCONNECT))
                kprintf("net: Hyper-V: link %s\n", h.link ? "up" : "down");
        }
        return;
    }
}

// Everything the host sent, at most max packets. While DOS's queue is full
// received frames are left in the ring, unless force: the host's answers
// (send completions, RNDIS replies) are in the same ring, behind them, so
// sending and control messages take everything (frames DOS has no room
// for are dropped).
void process(State& h, Card& c, int max, bool force = false)
{
    for (int i = 0; i < max && vmbus_pending(*h.ch); i++) {
        if (h.ready && !force && !room()) break;   // leave the rest in the ring
        uint32_t n = vmbus_recv(*h.ch, h.pkt, 4096);
        if (!n) break;
        const VmPacketDesc* d = (const VmPacketDesc*)h.pkt;
        uint32_t got = n < 4096 ? n : 4096, off = (uint32_t)d->offset8 << 3;
        const uint8_t* msg = off + 4 <= got ? h.pkt + off : 0;
        if (d->type == VM_PKT_COMP) {
            uint64_t t = d->trans_id;
            if (t >= TID_TX && t < TID_TX + NTX) h.tx_busy &= ~(1u << (uint32_t)(t - TID_TX));
            else if (t == h.want_tid && msg) {
                memset(h.comp, 0, sizeof h.comp);
                memcpy(h.comp, msg, got - off < NVSP_MSG ? got - off : NVSP_MSG);
                h.got_comp = true;
            }
        } else if (d->type == VM_PKT_DATA_USING_XFER_PAGES) {
            uint32_t status = NVSP_SUCCESS;
            uint16_t set = (uint16_t)(h.pkt[16] | h.pkt[17] << 8);
            uint32_t ranges = get32(h.pkt + 20);
            if (msg && get32(msg) == NVSP1_SEND_RNDIS_PKT && set == RXBUF_ID && 24 + 8 * ranges <= off) {
                for (uint32_t r = 0; r < ranges && 24 + 8 * r + 8 <= got; r++) {
                    uint32_t cnt = get32(h.pkt + 24 + 8 * r), at = get32(h.pkt + 28 + 8 * r);
                    if (at > RXBUF || cnt > RXBUF - at) { status = 2; st.rx_err++; continue; }   // NVSP_STAT_FAIL
                    rndis_in(h, c, h.rxbuf + at, cnt);
                }
            }
            // That part of the receive buffer back to the host (it waits for it).
            uint8_t ack[8];
            put32(ack, NVSP1_SEND_RNDIS_PKT_COMPLETE);
            put32(ack + 4, status);
            for (int t = 0; t < 10000 && !vmbus_send(*h.ch, VM_PKT_COMP, 0, d->trans_id, 0, 0, ack, 8); t++) kdelay_us(10);
        }
        // inband NVSP from the host (indirection table, VF association): nothing to do
    }
}

// An NVSP message that the host completes: its completion into h.comp.
bool nvsp_call(State& h, Card& c, const uint8_t* msg, uint64_t tid)
{
    h.got_comp = false;
    h.want_tid = tid;
    if (!vmbus_send_inband(*h.ch, msg, NVSP_MSG, tid, true)) return false;
    for (int t = 0; t < 200000 && !h.got_comp; t++) { process(h, c, 16, true); if (!h.got_comp) kdelay_us(10); }
    h.want_tid = 0;
    return h.got_comp;
}

// An RNDIS control message (h.ctl, len bytes): its reply into h.resp, status 0 wanted.
bool rndis_call(State& h, Card& c, uint32_t len, uint32_t reply)
{
    put32(h.ctl + 8, h.want_req = ++h.req_id);
    h.got_resp = false;
    uint8_t msg[NVSP_MSG] = {};
    put32(msg, NVSP1_SEND_RNDIS_PKT);
    put32(msg + 4, 1);                          // RMC_CONTROL
    put32(msg + 8, 0xFFFFFFFF);                 // not in a send buffer section
    bool sent = false;
    for (int t = 0; t < 1000 && !(sent = vmbus_send_pagebuf(*h.ch, msg, NVSP_MSG, pa(h.ctl), len, TID_CTL)); t++) {
        process(h, c, 16, true);
        kdelay_us(10);
    }
    if (!sent) return false;
    for (int t = 0; t < 200000 && !h.got_resp; t++) { process(h, c, 16, true); if (!h.got_resp) kdelay_us(10); }
    return h.got_resp && get32(h.resp) == reply && get32(h.resp + 12) == 0;
}

// A query: up to room bytes of the answer into out; its length, -1 failed.
int query(State& h, Card& c, uint32_t oid, void* out, uint32_t room)
{
    memset(h.ctl, 0, 64);
    put32(h.ctl, RNDIS_QUERY);
    put32(h.ctl + 4, 28);
    put32(h.ctl + 12, oid);
    put32(h.ctl + 20, 20);                      // info_buf_offset (from req_id)
    if (!rndis_call(h, c, 28, RNDIS_QUERY_C)) return -1;
    uint32_t n = get32(h.resp + 16), off = 8 + get32(h.resp + 20);
    if (off > sizeof h.resp || n > sizeof h.resp - off) return -1;
    memcpy(out, h.resp + off, n < room ? n : room);
    return (int)n;
}

bool set_filter(State& h, Card& c, uint32_t f)
{
    memset(h.ctl, 0, 64);
    put32(h.ctl, RNDIS_SET);
    put32(h.ctl + 4, 32);
    put32(h.ctl + 12, OID_PACKET_FILTER);
    put32(h.ctl + 16, 4);
    put32(h.ctl + 20, 20);
    put32(h.ctl + 28, f);
    if (!rndis_call(h, c, 32, RNDIS_SET_C)) return false;
    h.filter = f;
    return true;
}

uint32_t filter_for(State& h, Card& c, int m)
{
    if (m <= 1) return 0;
    if (m >= 6 || memcmp(h.addr, c.mac, 6)) return F_PROMISCUOUS;   // another address: everything (we filter)
    uint32_t f = F_DIRECTED;
    if (m >= 3) f |= F_BROADCAST;
    if (m >= 4) f |= F_ALL_MULTICAST;            // no multicast list here
    return f;
}

bool init(Card& c)
{
    if (!c.h.st) return false;
    State& h = S(c);
    if (h.ready) return true;                   // started before (VMNIC switching back)
    if (!h.ch->open && !vmbus_open(*h.ch, RING_PAGES, RING_PAGES, 0, 0)) return false;
    if (!h.pkt) {
        h.pkt = (uint8_t*)phys_alloc(4096);
        h.ctl = (uint8_t*)phys_alloc(4096);
        h.txb = (uint8_t*)phys_alloc(NTX * TXSZ);
    }

    // NVSP: the newest version the host takes (netvsc.c negotiate_nvsp_ver)
    uint8_t m[NVSP_MSG];
    h.nvsp_ver = 0;
    for (uint32_t v : nvsp_versions) {
        memset(m, 0, sizeof m);
        put32(m, NVSP_INIT);
        put32(m + 4, v);
        put32(m + 8, v);
        if (!nvsp_call(h, c, m, TID_INIT)) { kprintf("net: Hyper-V: no answer to NVSP init\n"); return false; }
        if (get32(h.comp) == NVSP_INIT_COMPLETE && get32(h.comp + 12) == NVSP_SUCCESS) { h.nvsp_ver = v; break; }
    }
    if (!h.nvsp_ver) { kprintf("net: Hyper-V: no NVSP version in common\n"); return false; }
    if (h.nvsp_ver > 0x2) {                     // NVSP 2 and later: the NDIS configuration
        memset(m, 0, sizeof m);
        put32(m, NVSP2_SEND_NDIS_CONFIG);
        put32(m + 4, 1514);                     // MTU + Ethernet header
        m[12] = 1 << 3;                         // capability: ieee8021q
        vmbus_send_inband(*h.ch, m, NVSP_MSG, 0, false);
    }
    memset(m, 0, sizeof m);
    put32(m, NVSP1_SEND_NDIS_VER);
    put32(m + 4, 6);
    put32(m + 8, h.nvsp_ver <= 0x40000 ? 1 : 30);   // NDIS 6.1, or 6.30 from NVSP 5 on
    vmbus_send_inband(*h.ch, m, NVSP_MSG, 0, false);

    // The receive buffer
    if (!h.rxbuf) {
        h.rxbuf = (uint8_t*)phys_try_alloc(RXBUF);
        if (!h.rxbuf) { kprintf("net: Hyper-V: no memory for the receive buffer\n"); return false; }
        uint32_t gp;
        if (!vmbus_gpadl(*h.ch, h.rxbuf, RXBUF, &gp)) return false;
        memset(m, 0, sizeof m);
        put32(m, NVSP1_SEND_RECV_BUF);
        put32(m + 4, gp);
        m[8] = (uint8_t)RXBUF_ID; m[9] = (uint8_t)(RXBUF_ID >> 8);
        if (!nvsp_call(h, c, m, TID_RXBUF) || get32(h.comp) != NVSP1_SEND_RECV_BUF_COMPLETE || get32(h.comp + 4) != NVSP_SUCCESS) {
            kprintf("net: Hyper-V: receive buffer refused\n");
            return false;
        }
    }

    // RNDIS
    memset(h.ctl, 0, 64);
    put32(h.ctl, RNDIS_INIT);
    put32(h.ctl + 4, 24);
    put32(h.ctl + 12, 1);                       // version 1.0
    put32(h.ctl + 20, 0x4000);                  // max_xfer_size
    if (!rndis_call(h, c, 24, RNDIS_INIT_C)) { kprintf("net: Hyper-V: RNDIS didn't initialize\n"); return false; }
    if (query(h, c, OID_PERMANENT_ADDRESS, c.mac, 6) != 6) { kprintf("net: Hyper-V: no MAC address\n"); return false; }
    memcpy(h.addr, c.mac, 6);
    uint32_t ls = 1;
    if (query(h, c, OID_MEDIA_CONNECT_STATUS, &ls, 4) == 4) h.link = ls == 0;   // 0: connected
    h.mode = 3;
    if (!set_filter(h, c, filter_for(h, c, 3))) { kprintf("net: Hyper-V: packet filter refused\n"); return false; }
    snprintf(c.name, sizeof c.name, "Hyper-V");
    if (log_on) kprintf("net: Hyper-V: NVSP %x, relid %u\n", h.nvsp_ver, h.ch->relid);
    h.ready = true;
    return true;
}

bool link(Card& c) { return c.h.st && S(c).link; }

void set_mode(Card& c, int m)
{
    State& h = S(c);
    h.mode = m;
    uint32_t f = filter_for(h, c, m);
    if (f != h.filter) set_filter(h, c, f);
}

void set_mac(Card& c, const uint8_t* a)
{
    State& h = S(c);
    memcpy(h.addr, a, 6);
    set_mode(c, h.mode);
}

void poll(Card& c) { process(S(c), c, 64); }

bool send(Card& c, const uint8_t* f, unsigned len)
{
    State& h = S(c);
    if (len > TXSZ - 44) return false;
    int slot = -1;
    for (int tries = 0; tries < 2000 && slot < 0; tries++) {
        for (int s = 0; s < NTX; s++) if (!(h.tx_busy & (1u << s))) { slot = s; break; }
        if (slot < 0) { process(h, c, 16, true); udelay(5); }
    }
    if (slot < 0) return false;
    uint8_t* b = h.txb + slot * TXSZ;
    memset(b, 0, 44);                           // struct rndis_message + rndis_packet
    put32(b, RNDIS_PACKET);
    put32(b + 4, 44 + len);
    put32(b + 8, 36);                           // data_offset (from data_offset itself)
    put32(b + 12, len);
    memcpy(b + 44, f, len);
    uint8_t msg[NVSP_MSG] = {};
    put32(msg, NVSP1_SEND_RNDIS_PKT);           // channel_type 0: RMC_DATA
    put32(msg + 8, 0xFFFFFFFF);
    h.tx_busy |= 1u << slot;
    if (!vmbus_send_pagebuf(*h.ch, msg, NVSP_MSG, pa(b), 44 + len, TID_TX + slot)) {
        h.tx_busy &= ~(1u << slot);
        return false;
    }
    return true;
}

} // namespace nv
} // namespace

// net.cpp: Hyper-V's adapters, as cards (no PCI device behind them).
bool netvsc_card(int i, Card& c)
{
    if (strstr(cmdline, "netvsc=off") || !vmbus_start()) return false;
    VmbusChannel* ch = 0;
    for (int k = 0, n = 0; (ch = vmbus_find(nv::NET_GUID, k)); k++)
        if (!ch->sub_index && n++ == i) break;
    if (!ch || nv::n_st == MAXCARDS) return false;
    nv::State& h = nv::st_[nv::n_st++];
    memset(&h, 0, sizeof h);
    h.ch = ch;
    c.h.st = &h;
    return true;
}

const NicDriver net::nic_netvsc = {
    KIND_HYPERV, [](uint32_t, uint32_t) { return false; }, nv::init, nv::link, nv::set_mode, nv::set_mac, nv::poll, nv::send, nullptr,
};
