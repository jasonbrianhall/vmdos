// A simulated Hyper-V host for vmdos's VMBus drivers (src/vmbus.cpp,
// blk_storvsc.cpp, nic_netvsc.cpp), built and run on Linux as a 32-bit
// program: "make -C tests/hvsim". Guest "physical" memory is the program's
// own (phys_alloc is aligned_alloc), so page numbers are pointers >> 12.
//
// Everything the guest sends is parsed with Linux's own structures
// (linux_hv.h, copied from the kernel), not vmdos's offsets, and checked:
// the hypercall/SynIC MSRs, the post-message input, the channel messages
// (version, offers, GPADL header + bodies, open), the ring buffers (indices,
// 8-byte trailers, wrap-around), VSTOR packets and SRBs, NVSP and RNDIS.
// The host answers like Hyper-V: a SCSI controller with two disks (one over
// 2 TiB) and a DVD drive, a network adapter that echoes every frame back.
//
// "old" on the command line: a host that predates VMBus 5.0 (connection
// ID 4 refused), VSTOR win7 (the shorter SRB) and NVSP 2.
#include "linux_hv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <vector>
#include <deque>
#include <map>

#define CHECK(c, ...) do { if (!(c)) { printf("HOST CHECK FAILED %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); exit(1); } } while (0)

// ---------------- the kernel functions the drivers use ----------------
extern "C" {
char cmdline[512] = "vmbuslog netlog";
int debug_level;
void kprintf(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("  | ");
    vprintf(fmt, ap);
    va_end(ap);
}
void* phys_try_alloc(uint32_t bytes)
{
    bytes = (bytes + 4095) & ~4095u;
    void* p = aligned_alloc(4096, bytes);
    if (p) memset(p, 0, bytes);
    CHECK((uintptr_t)p < 0xFFFFF000u, "32-bit");
    return p;
}
void* phys_alloc(uint32_t bytes) { void* p = phys_try_alloc(bytes); CHECK(p, "out of memory"); return p; }
void host_step();
static uint64_t vtime_us;
void kdelay_us(uint32_t us) { vtime_us += us; host_step(); }
}

// ---------------- the host ----------------
static bool old_host;
static uint32_t max_version;
static inline uint8_t* P(uint64_t pfn) { return (uint8_t*)(uintptr_t)(pfn << 12); }
static inline uint8_t* A(uint32_t pa) { return (uint8_t*)(uintptr_t)pa; }

static uint64_t msr_guest_id, msr_hypercall, msr_simp, msr_siefp, msr_scontrol, msr_sint[16];
static std::deque<std::vector<uint8_t>> msgq;   // host -> guest channel messages
static uint32_t conn_msg = 1;                    // the connection ID messages must come on
static uint32_t version;
static bool connected, unloaded;
static uint8_t* int_page;
static int n_signals, n_posts;

struct Gpadl { uint32_t relid, pages; std::vector<uint64_t> pfn; bool done; };
static std::map<uint32_t, Gpadl> gpadls;

enum { DEV_SCSI, DEV_NET, DEV_OTHER };
struct Chan {
    uint32_t relid, conn_id;
    int dev;
    bool dedicated, open;
    hv_ring_buffer *g2h, *h2g;                    // guest's outbound (we read), guest's inbound (we write)
    uint8_t *g2h_data, *h2g_data;
    uint32_t g2h_size, h2g_size;
    std::deque<std::vector<uint8_t>> pending;     // host -> guest packets waiting for room
};
static Chan chans[3] = {
    { 1, 0x10001, DEV_SCSI, true, false, 0, 0, 0, 0, 0, 0, {} },
    { 2, 0x10002, DEV_NET, true, false, 0, 0, 0, 0, 0, 0, {} },
    { 7, 0x10007, DEV_OTHER, false, false, 0, 0, 0, 0, 0, 0, {} },  // a heartbeat, say: not ours
};
static const char* guids[3] = { "ba6163d9-04a1-4d29-b605-72e2ffb1dc7f", "f8615163-df3e-46c5-913f-f2d2f965ed0e",
                                "57164f39-9115-4e78-ab55-382f3bd5422d" };

static void guid_bytes(const char* s, uint8_t* g)
{
    unsigned a, b, c, d[8];
    sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
    memcpy(g, &a, 4); memcpy(g + 4, &b, 2); memcpy(g + 6, &c, 2);   // guid_t: little-endian fields
    for (int i = 0; i < 8; i++) g[8 + i] = (uint8_t)d[i];
}

static void send_msg(const void* m, uint32_t len)
{
    CHECK(len <= 240, "message too long");
    msgq.emplace_back((const uint8_t*)m, (const uint8_t*)m + len);
}

static void deliver()
{
    if (!(msr_simp & 1) || msgq.empty()) return;
    uint8_t* slot = P(msr_simp >> 12) + 2 * 256;
    uint32_t t; memcpy(&t, slot, 4);
    if (t) return;                               // still full
    std::vector<uint8_t> m = msgq.front();
    msgq.pop_front();
    memset(slot, 0, 256);
    slot[4] = (uint8_t)m.size();
    slot[5] = msgq.empty() ? 0 : 1;              // msg_pending
    memcpy(slot + 16, m.data(), m.size());
    t = 1;
    memcpy(slot, &t, 4);
}

// ---- ring buffers (as Linux's hv_ringbuffer_write / hv_pkt_iter do) ----
static bool ring_put(Chan& c, const std::vector<uint8_t>& pkt)
{
    hv_ring_buffer* r = c.h2g;
    uint32_t size = c.h2g_size, w = r->write_index, rd = r->read_index;
    CHECK(w < size && rd < size, "guest wrecked our indices");
    uint32_t avail = w >= rd ? size - (w - rd) : rd - w;
    uint32_t total = pkt.size() + 8;
    if (avail <= total) {                        // full: the guest tells us when there's room
        r->pending_send_sz = total + 1;
        return false;
    }
    uint32_t p = w;
    for (uint8_t b : pkt) { c.h2g_data[p] = b; p = (p + 1) % size; }
    uint64_t prev = (uint64_t)w << 32;
    for (int i = 0; i < 8; i++) { c.h2g_data[p] = ((uint8_t*)&prev)[i]; p = (p + 1) % size; }
    __sync_synchronize();
    r->write_index = p;
    r->pending_send_sz = 0;
    return true;
}

static void host_send(Chan& c, uint16_t type, uint16_t flags, uint64_t tid, const void* extra, uint32_t elen,
                      const void* data, uint32_t dlen)
{
    uint32_t len = sizeof(vmpacket_descriptor) + elen + dlen, al = (len + 7) & ~7u;
    std::vector<uint8_t> pkt(al, 0);
    vmpacket_descriptor d = { type, (u16)((sizeof d + elen) >> 3), (u16)(al >> 3), flags, tid };
    memcpy(pkt.data(), &d, sizeof d);
    if (elen) memcpy(pkt.data() + sizeof d, extra, elen);
    if (dlen) memcpy(pkt.data() + sizeof d + elen, data, dlen);
    c.pending.push_back(pkt);
    while (!c.pending.empty() && ring_put(c, c.pending.front())) c.pending.pop_front();
}

static bool ring_get(Chan& c, std::vector<uint8_t>& pkt)
{
    hv_ring_buffer* r = c.g2h;
    uint32_t size = c.g2h_size, w = r->write_index, rd = r->read_index;
    CHECK(w < size && rd < size, "bad indices w %u r %u", w, rd);
    if (w == rd) return false;
    __sync_synchronize();
    uint32_t avail = w > rd ? w - rd : size - rd + w;
    vmpacket_descriptor d;
    for (uint32_t i = 0; i < sizeof d; i++) ((uint8_t*)&d)[i] = c.g2h_data[(rd + i) % size];
    uint32_t len = d.len8 << 3;
    CHECK(len >= sizeof d && len + 8 <= avail, "packet length %u, %u in the ring", len, avail);
    CHECK((uint32_t)(d.offset8 << 3) <= len, "data offset %u past the packet (%u)", d.offset8 << 3, len);
    pkt.resize(len);
    for (uint32_t i = 0; i < len; i++) pkt[i] = c.g2h_data[(rd + i) % size];
    uint64_t prev = 0;
    for (int i = 0; i < 8; i++) ((uint8_t*)&prev)[i] = c.g2h_data[(rd + len + i) % size];
    CHECK(prev == (uint64_t)rd << 32, "trailer %llx, wanted %llx", (unsigned long long)prev, (unsigned long long)rd << 32);
    __sync_synchronize();
    r->read_index = (rd + len + 8) % size;
    return true;
}

// The guest's buffer behind a GPA-direct packet (one range, Linux's vmbus_packet_mpb_array)
static std::vector<uint8_t*> mpb_bytes(const std::vector<uint8_t>& pkt, uint32_t* len)
{
    const vmbus_packet_mpb_array* m = (const vmbus_packet_mpb_array*)pkt.data();
    CHECK(m->rangecount == 1, "rangecount %u", m->rangecount);
    uint32_t n = (m->range.offset + m->range.len + 4095) >> 12;
    CHECK(sizeof(vmbus_packet_mpb_array) + 8 * n == (uint32_t)(m->dataoffset8 << 3), "mpb descriptor size: %u PFNs, offset %u",
          n, m->dataoffset8 << 3);
    std::vector<uint8_t*> out;
    for (uint32_t i = 0; i < m->range.len; i++) {
        uint32_t o = m->range.offset + i;
        out.push_back(P(m->range.pfn_array[o >> 12]) + (o & 4095));
    }
    *len = m->range.len;
    return out;
}

// ---- the SCSI controller ----
static uint16_t vstor_ver;
static uint32_t vstor_size = 48;                 // until the version is agreed
static bool scsi_ua[64];                          // a unit attention due
static std::vector<uint8_t> disk0(16 << 20);      // LUN 0: 16 MiB
static std::map<uint64_t, std::vector<uint8_t>> disk5;   // LUN 5: 3 TiB, sparse
static const uint64_t DISK5_SECTORS = 3ull << 31;
static int scsi_cmds, scsi_splits_seen;

static uint8_t* disk_sector(int lun, uint64_t lba)
{
    if (lun == 0) return &disk0[lba * 512];
    auto& v = disk5[lba];
    if (v.empty()) v.resize(512);
    return v.data();
}

static void scsi_packet(Chan& c, const std::vector<uint8_t>& pkt)
{
    const vmpacket_descriptor* d = (const vmpacket_descriptor*)pkt.data();
    uint32_t off = d->offset8 << 3, plen = pkt.size() - off;
    CHECK(d->flags == 1, "completion not requested");
    vstor_packet v;
    memset(&v, 0, sizeof v);
    CHECK(plen >= vstor_size && plen < vstor_size + 8, "VSTOR packet %u bytes, wanted %u", plen, vstor_size);
    memcpy(&v, pkt.data() + off, vstor_size);
    CHECK(v.flags == 1, "REQUEST_COMPLETION_FLAG");
    vstor_packet r = v;
    r.operation = VSTOR_OPERATION_COMPLETE_IO;
    r.status = 0;
    switch (v.operation) {
    case VSTOR_OPERATION_BEGIN_INITIALIZATION: case VSTOR_OPERATION_END_INITIALIZATION: break;
    case VSTOR_OPERATION_QUERY_PROTOCOL_VERSION: {
        uint16_t want = old_host ? 0x0402 : 0x0600;   // this host: WIN8_1 (or WIN7)
        if (v.version.major_minor == want) {
            vstor_ver = want;
            vstor_size = want >= 0x0501 ? 64 : 48;
        } else r.status = 0xC0000001;
        break; }
    case VSTOR_OPERATION_QUERY_PROPERTIES:
        CHECK(vstor_ver, "properties before the version");
        memset(&r.storage_channel_properties, 0, sizeof r.storage_channel_properties);
        r.storage_channel_properties.max_transfer_bytes = 0x8000;   // 32 KiB: commands get split
        r.storage_channel_properties.max_channel_cnt = 1;
        break;
    case VSTOR_OPERATION_EXECUTE_SRB: {
        vmscsi_request& s = v.vm_srb;
        vmscsi_request& o = r.vm_srb;
        scsi_cmds++;
        CHECK(s.length == (vstor_size == 64 ? 52 : 36), "SRB length %u", s.length);
        CHECK(s.target_id == 0 && s.path_id == 0, "target %u path %u", s.target_id, s.path_id);
        CHECK(s.sense_info_length == (vstor_size == 64 ? 0x14 : 0x12), "sense length %u", s.sense_info_length);
        uint32_t blen = 0;
        std::vector<uint8_t*> buf;
        if (d->type == 9) buf = mpb_bytes(pkt, &blen);
        else CHECK(d->type == 6, "packet type %u", d->type);
        CHECK(blen == s.data_transfer_length, "buffer %u, transfer %u", blen, s.data_transfer_length);
        if (vstor_size == 64) {
            uint32_t want = s.data_in == 1 ? 0x40 : s.data_in == 0 ? 0x80 : 0;
            CHECK((s.srb_flags & 0xC0) == want, "srb_flags %x for data_in %u", s.srb_flags, s.data_in);
        }
        const uint8_t* cdb = s.cdb;
        int lun = s.lun;
        o.srb_status = 1; o.scsi_status = 0; o.data_transfer_length = blen;
        bool exists = lun == 0 || lun == 5 || lun == 2;
        if (!exists) { o.srb_status = 0x20; o.data_transfer_length = 0; break; }   // SRB_STATUS_INVALID_LUN
        if (scsi_ua[lun] && cdb[0] != 0x12) {    // UNIT ATTENTION (INQUIRY isn't held up by it)
            scsi_ua[lun] = false;
            o.srb_status = 0x84; o.scsi_status = 2;
            memset(o.sense_data, 0, sizeof o.sense_data);
            o.sense_data[0] = 0x70; o.sense_data[2] = 6; o.sense_data[12] = 0x29;
            break;
        }
        auto put = [&](const uint8_t* src, uint32_t n) { CHECK(n <= blen, "%u > %u", n, blen); for (uint32_t i = 0; i < n; i++) *buf[i] = src[i]; };
        auto be = [&](int i, int n) { uint64_t v = 0; for (int k = 0; k < n; k++) v = v << 8 | cdb[i + k]; return v; };
        switch (cdb[0]) {
        case 0x12: {                             // INQUIRY
            uint8_t q[36] = {};
            q[0] = lun == 2 ? 5 : 0;
            memcpy(q + 8, "Msft    ", 8);
            memcpy(q + 16, lun == 2 ? "Virtual DVD-ROM " : "Virtual Disk    ", 16);
            memcpy(q + 32, "1.0 ", 4);
            put(q, 36);
            break; }
        case 0x00: break;                        // TEST UNIT READY
        case 0x25: {                             // READ CAPACITY (10)
            uint64_t last = lun == 0 ? disk0.size() / 512 - 1 : 0xFFFFFFFF;
            uint8_t q[8] = { (uint8_t)(last >> 24), (uint8_t)(last >> 16), (uint8_t)(last >> 8), (uint8_t)last, 0, 0, 2, 0 };
            put(q, 8);
            break; }
        case 0x9E: {                             // READ CAPACITY (16)
            CHECK(cdb[1] == 0x10, "service action");
            uint64_t last = lun == 0 ? disk0.size() / 512 - 1 : DISK5_SECTORS - 1;
            uint8_t q[32] = {};
            for (int k = 0; k < 8; k++) q[k] = (uint8_t)(last >> (56 - 8 * k));
            q[10] = 2;
            put(q, 32);
            break; }
        case 0x28: case 0x2A: case 0x88: case 0x8A: {
            bool w = cdb[0] == 0x2A || cdb[0] == 0x8A, ten = cdb[0] < 0x80;
            uint64_t lba = ten ? be(2, 4) : be(2, 8);
            uint32_t n = ten ? (uint32_t)be(7, 2) : (uint32_t)be(10, 4);
            uint64_t cap = lun == 0 ? disk0.size() / 512 : DISK5_SECTORS;
            CHECK(n * 512 == blen, "%u sectors, %u bytes", n, blen);
            CHECK(blen <= 0x8000, "%u bytes: more than max_transfer_bytes", blen);
            CHECK(s.data_in == (w ? 0 : 1), "direction");
            CHECK(lba + n <= cap, "past the end");
            if (!ten) CHECK(lba + n > 0xFFFFFFFFull, "16-byte CDB where 10 would do");
            if (blen == 0x8000) scsi_splits_seen++;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t* sec = disk_sector(lun, lba + i);
                for (int b = 0; b < 512; b++) { if (w) sec[b] = *buf[i * 512 + b]; else *buf[i * 512 + b] = sec[b]; }
            }
            break; }
        default: o.srb_status = 0x06; break;     // INVALID_REQUEST
        }
        break; }
    default: CHECK(0, "VSTOR operation %u", v.operation);
    }
    host_send(c, 11, 0, d->trans_id, 0, 0, &r, vstor_size);   // VM_PKT_COMP
}

// ---- the network adapter ----
static uint32_t nvsp_ver, ndis_major, ndis_minor, mtu, filter;
static uint8_t* rxbuf;
static uint32_t rxbuf_size;
static const uint32_t SEC = 2048;                 // our sub-allocations of the receive buffer
static std::vector<bool> sec_used;
static std::map<uint64_t, std::vector<uint32_t>> rx_out;   // transfer-page packets the guest hasn't acked
static uint64_t rx_tid = 1000;
static std::deque<std::vector<uint8_t>> rx_wait;  // RNDIS messages for the guest, waiting for buffer space
static const uint8_t MAC[6] = { 0x00, 0x15, 0x5D, 0x01, 0x02, 0x03 };
static bool link_up = true;
int net_frames_sent, net_acks, net_ctl;
std::vector<uint8_t> last_frame;

static void rx_flush(Chan& c)
{
    while (!rx_wait.empty()) {                   // up to 3 messages a packet (several ranges)
        std::vector<uint32_t> used;
        std::vector<vmtransfer_page_range> ranges;
        while (!rx_wait.empty() && used.size() < 3) {
            uint32_t s = 0;
            while (s < sec_used.size() && sec_used[s]) s++;
            if (s == sec_used.size()) break;     // the guest hasn't given any back yet
            std::vector<uint8_t>& m = rx_wait.front();
            CHECK(m.size() <= SEC, "too long");
            sec_used[s] = true;
            memcpy(rxbuf + s * SEC, m.data(), m.size());
            ranges.push_back({ (u32)m.size(), s * SEC });
            used.push_back(s);
            rx_wait.pop_front();
        }
        if (used.empty()) return;
        uint64_t tid = ++rx_tid;
        rx_out[tid] = used;
        std::vector<uint8_t> extra(8 + 8 * ranges.size());   // vmtransfer_page_packet_header less the descriptor
        uint16_t id = 0xCAFE; u32 cnt = ranges.size();
        memcpy(extra.data(), &id, 2); extra[2] = 0; extra[3] = 0;
        memcpy(extra.data() + 4, &cnt, 4);
        memcpy(extra.data() + 8, ranges.data(), 8 * ranges.size());
        nvsp_message n; memset(&n, 0, sizeof n);
        n.hdr.msg_type = NVSP_MSG1_TYPE_SEND_RNDIS_PKT;
        n.msg.v1_msg.send_rndis_pkt.channel_type = 0;
        host_send(c, 7, 0, tid, extra.data(), extra.size(), &n, sizeof n);
    }
}

static void rndis_to_guest(Chan& c, const void* m, uint32_t len)
{
    rx_wait.emplace_back((const uint8_t*)m, (const uint8_t*)m + len);
    rx_flush(c);
}

static void rndis_ctl(Chan& c, const std::vector<uint8_t>& b)
{
    net_ctl++;
    uint32_t type, len;
    memcpy(&type, b.data(), 4); memcpy(&len, b.data() + 4, 4);
    CHECK(len == b.size(), "RNDIS length %u, %zu given", len, b.size());
    uint8_t out[256] = {};
    uint32_t olen = 0;
    const uint8_t* body = b.data() + 8;
    switch (type) {
    case 2: {                                    // INITIALIZE
        const rndis_initialize_request* q = (const rndis_initialize_request*)body;
        CHECK(len == 8 + sizeof *q && q->major_ver == 1 && q->minor_ver == 0, "init");
        rndis_initialize_complete* r = (rndis_initialize_complete*)(out + 8);
        r->req_id = q->req_id; r->status = 0; r->major_ver = 1; r->max_pkt_per_msg = 8; r->max_xfer_size = 0x4000;
        type = 0x80000002; olen = 8 + sizeof *r;
        break; }
    case 4: {                                    // QUERY
        const rndis_query_request* q = (const rndis_query_request*)body;
        CHECK(len == 8 + sizeof *q && q->info_buf_offset == sizeof *q, "query layout");
        rndis_query_complete* r = (rndis_query_complete*)(out + 8);
        r->req_id = q->req_id; r->info_buf_offset = sizeof *r;
        uint8_t* info = out + 8 + sizeof *r;
        if (q->oid == 0x01010101) { memcpy(info, MAC, 6); r->info_buflen = 6; }
        else if (q->oid == 0x00010114) { uint32_t v = link_up ? 0 : 1; memcpy(info, &v, 4); r->info_buflen = 4; }
        else r->status = 0xC00000BB;             // not supported
        type = 0x80000004; olen = 8 + sizeof *r + r->info_buflen;
        break; }
    case 5: {                                    // SET
        const rndis_set_request* q = (const rndis_set_request*)body;
        CHECK(q->info_buf_offset == sizeof *q && len == 8 + sizeof *q + q->info_buflen, "set layout");
        rndis_set_complete* r = (rndis_set_complete*)(out + 8);
        r->req_id = q->req_id;
        if (q->oid == 0x0001010E) { CHECK(q->info_buflen == 4, "filter size"); memcpy(&filter, q->info_buf, 4); }
        else r->status = 0xC00000BB;
        type = 0x80000005; olen = 8 + sizeof *r;
        break; }
    default: CHECK(0, "RNDIS control message %x", type);
    }
    memcpy(out, &type, 4); memcpy(out + 4, &olen, 4);
    rndis_to_guest(c, out, olen);
}

void sim_link(bool up)
{
    link_up = up;
    uint8_t m[20] = {};
    uint32_t t = 7, l = 20, s = up ? 0x4001000B : 0x4001000C;
    memcpy(m, &t, 4); memcpy(m + 4, &l, 4); memcpy(m + 8, &s, 4);
    rndis_to_guest(chans[1], m, 20);
}

static void net_packet(Chan& c, const std::vector<uint8_t>& pkt)
{
    const vmpacket_descriptor* d = (const vmpacket_descriptor*)pkt.data();
    uint32_t off = d->offset8 << 3;
    nvsp_message n; memset(&n, 0, sizeof n);
    memcpy(&n, pkt.data() + off, pkt.size() - off < sizeof n ? pkt.size() - off : sizeof n);
    if (d->type == 11) {                         // the guest's ack of a transfer-page packet
        CHECK(n.hdr.msg_type == NVSP_MSG1_TYPE_SEND_RNDIS_PKT_COMPLETE && n.msg.v1_msg.send_rndis_pkt_complete.status == 1,
              "receive completion %u status %u", n.hdr.msg_type, n.msg.v1_msg.send_rndis_pkt_complete.status);
        auto it = rx_out.find(d->trans_id);
        CHECK(it != rx_out.end(), "ack for transaction %llu we didn't send", (unsigned long long)d->trans_id);
        for (uint32_t s : it->second) sec_used[s] = false;
        rx_out.erase(it);
        net_acks++;
        rx_flush(c);
        return;
    }
    CHECK(pkt.size() - off >= sizeof n, "NVSP message %zu bytes", pkt.size() - off);
    nvsp_message r; memset(&r, 0, sizeof r);
    switch (n.hdr.msg_type) {
    case NVSP_MSG_TYPE_INIT: {
        CHECK(d->type == 6 && d->flags == 1, "init packet");
        uint32_t v = n.msg.init_msg.init.max_protocol_ver;
        CHECK(v == n.msg.init_msg.init.min_protocol_ver, "min/max");
        r.hdr.msg_type = NVSP_MSG_TYPE_INIT_COMPLETE;
        uint32_t want = old_host ? 0x30002 : 0x50000;
        r.msg.init_msg.init_complete.status = v == want ? NVSP_STAT_SUCCESS : NVSP_STAT_PROTOCOL_TOO_NEW;
        if (v == want) nvsp_ver = v;
        r.msg.init_msg.init_complete.negotiated_protocol_ver = v;
        host_send(c, 11, 0, d->trans_id, 0, 0, &r, sizeof r);
        return; }
    case NVSP_MSG2_TYPE_SEND_NDIS_CONFIG:
        CHECK(nvsp_ver >= 0x30002, "NDIS config on NVSP 1");
        mtu = n.msg.v2_msg.send_ndis_config.mtu;
        CHECK(mtu == 1514 && n.msg.v2_msg.send_ndis_config.capability.ieee8021q, "config");
        return;
    case NVSP_MSG1_TYPE_SEND_NDIS_VER:
        ndis_major = n.msg.v1_msg.send_ndis_ver.ndis_major_ver;
        ndis_minor = n.msg.v1_msg.send_ndis_ver.ndis_minor_ver;
        CHECK(ndis_major == 6 && ndis_minor == (nvsp_ver <= 0x40000 ? 1 : 30), "NDIS %u.%u", ndis_major, ndis_minor);
        return;
    case NVSP_MSG1_TYPE_SEND_RECV_BUF: {
        auto it = gpadls.find(n.msg.v1_msg.send_recv_buf.gpadl_handle);
        CHECK(it != gpadls.end() && it->second.done, "receive buffer GPADL");
        CHECK(n.msg.v1_msg.send_recv_buf.id == 0xCAFE, "id");
        Gpadl& g = it->second;
        for (uint32_t i = 1; i < g.pages; i++) CHECK(g.pfn[i] == g.pfn[0] + i, "not contiguous");   // (vmdos's is)
        rxbuf = P(g.pfn[0]);
        rxbuf_size = g.pages * 4096;
        sec_used.assign(rxbuf_size / SEC, false);
        uint8_t rc[sizeof(nvsp_message_header) + sizeof(nvsp_1_message_send_receive_buffer_complete)
                   + sizeof(nvsp_1_receive_buffer_section)] = {};
        nvsp_message_header* rh = (nvsp_message_header*)rc;
        nvsp_1_message_send_receive_buffer_complete* cc = (nvsp_1_message_send_receive_buffer_complete*)(rh + 1);
        rh->msg_type = NVSP_MSG1_TYPE_SEND_RECV_BUF_COMPLETE;
        cc->status = NVSP_STAT_SUCCESS; cc->num_sections = 1;
        cc->sections[0].sub_alloc_size = SEC; cc->sections[0].num_sub_allocs = rxbuf_size / SEC; cc->sections[0].end_offset = rxbuf_size;
        memcpy(&r, rc, sizeof r < sizeof rc ? sizeof r : sizeof rc);
        host_send(c, 11, 0, d->trans_id, 0, 0, &r, sizeof r);
        return; }
    case NVSP_MSG1_TYPE_SEND_RNDIS_PKT: {
        CHECK(d->type == 9 && d->flags == 1, "RNDIS packet type %u flags %u", d->type, d->flags);
        CHECK(n.msg.v1_msg.send_rndis_pkt.send_buf_section_index == 0xFFFFFFFF, "section index");
        const vmbus_channel_packet_page_buffer* pb = (const vmbus_channel_packet_page_buffer*)pkt.data();
        CHECK(pb->rangecount >= 1 && pb->rangecount <= 3, "rangecount %u", pb->rangecount);
        CHECK((uint32_t)(pb->dataoffset8 << 3) == 24 + 16 * pb->rangecount, "page buffer descriptor");
        std::vector<uint8_t> b;
        for (uint32_t i = 0; i < pb->rangecount; i++) {
            CHECK(pb->range[i].offset + pb->range[i].len <= 4096, "range crosses a page");
            uint8_t* p = P(pb->range[i].pfn) + pb->range[i].offset;
            b.insert(b.end(), p, p + pb->range[i].len);
        }
        r.hdr.msg_type = NVSP_MSG1_TYPE_SEND_RNDIS_PKT_COMPLETE;   // done with the guest's buffer
        r.msg.v1_msg.send_rndis_pkt_complete.status = NVSP_STAT_SUCCESS;
        host_send(c, 11, 0, d->trans_id, 0, 0, &r, sizeof r);
        if (n.msg.v1_msg.send_rndis_pkt.channel_type == 1) { rndis_ctl(c, b); return; }
        // data: an RNDIS packet message with one frame; echoed back to the guest
        uint32_t type, len;
        memcpy(&type, b.data(), 4); memcpy(&len, b.data() + 4, 4);
        CHECK(type == 1 && len == b.size(), "RNDIS packet %x len %u/%zu", type, len, b.size());
        const rndis_packet* rp = (const rndis_packet*)(b.data() + 8);
        CHECK(rp->data_offset == sizeof(rndis_packet) && 8 + rp->data_offset + rp->data_len == len, "packet layout");
        last_frame.assign(b.begin() + 8 + rp->data_offset, b.end());
        net_frames_sent++;
        if (filter & 1) {                        // the echo, to our address
            std::vector<uint8_t> e(b);
            memcpy(e.data() + 8 + rp->data_offset, MAC, 6);
            rndis_to_guest(c, e.data(), e.size());
        }
        return; }
    default:
        CHECK(0, "NVSP message %u", n.hdr.msg_type);
    }
}

// ---- channel messages (posted by the guest) ----
static void channel_msg(const uint8_t* m, uint32_t len)
{
    const vmbus_channel_message_header* h = (const vmbus_channel_message_header*)m;
    switch (h->msgtype) {
    case CHANNELMSG_INITIATE_CONTACT: {
        const vmbus_channel_initiate_contact* c = (const vmbus_channel_initiate_contact*)m;
        CHECK(len == sizeof *c, "initiate contact %u bytes", len);
        CHECK(c->monitor_page1 && c->monitor_page2 && !(c->monitor_page1 & 4095), "monitor pages");
        CHECK(c->target_vcpu == 0, "target VP %u", c->target_vcpu);
        if (c->vmbus_version_requested >= (5 << 16)) CHECK(c->msg_sint == 2, "msg_sint %u", c->msg_sint);
        else { CHECK(c->interrupt_page && !(c->interrupt_page & 4095), "interrupt page"); int_page = P(c->interrupt_page >> 12); }
        vmbus_channel_version_response r;
        memset(&r, 0, sizeof r);
        r.header.msgtype = CHANNELMSG_VERSION_RESPONSE;
        r.version_supported = c->vmbus_version_requested <= max_version;
        if (r.version_supported) {
            version = c->vmbus_version_requested;
            connected = true;
            if (version >= (5 << 16)) { r.msg_conn_id = 0x55; conn_msg = 0x55; }   // the host picks it
        }
        send_msg(&r, sizeof r);
        break; }
    case CHANNELMSG_REQUESTOFFERS:
        CHECK(connected, "offers before contact");
        for (int i = 0; i < 3; i++) {
            vmbus_channel_offer_channel o;
            memset(&o, 0, sizeof o);
            o.header.msgtype = CHANNELMSG_OFFERCHANNEL;
            guid_bytes(guids[i], o.offer.if_type.b);
            o.offer.if_instance.b[0] = (u8)i;
            o.child_relid = chans[i].relid;
            o.monitorid = (u8)i; o.monitor_allocated = 1;
            o.is_dedicated_interrupt = chans[i].dedicated;
            o.connection_id = chans[i].conn_id;
            send_msg(&o, sizeof o);
        }
        { vmbus_channel_message_header a = { CHANNELMSG_ALLOFFERS_DELIVERED, 0 }; send_msg(&a, sizeof a); }
        break;
    case CHANNELMSG_GPADL_HEADER: {
        const vmbus_channel_gpadl_header* g = (const vmbus_channel_gpadl_header*)m;
        CHECK(g->rangecount == 1 && g->range[0].byte_offset == 0, "ranges");
        uint32_t pages = (g->range_buflen - sizeof(gpa_range)) / 8;
        CHECK(pages * 4096 == g->range[0].byte_count, "range_buflen %u vs byte_count %u", g->range_buflen, g->range[0].byte_count);
        uint32_t here = (len - sizeof *g - sizeof(gpa_range)) / 8;
        CHECK(len == sizeof *g + sizeof(gpa_range) + 8 * here && here <= pages && here <= 26, "header PFNs");
        CHECK(!gpadls.count(g->gpadl), "GPADL handle %x used twice", g->gpadl);
        Gpadl& n = gpadls[g->gpadl];
        n.relid = g->child_relid; n.pages = pages;
        n.pfn.resize(here); memcpy(n.pfn.data(), (const uint8_t*)g + sizeof *g + sizeof(gpa_range), 8 * here);
        if (n.pfn.size() == pages) goto created;
        break;
    created: {
            n.done = true;
            vmbus_channel_gpadl_created r = { { CHANNELMSG_GPADL_CREATED, 0 }, n.relid, g->gpadl, 0 };
            send_msg(&r, sizeof r);
        }
        break; }
    case CHANNELMSG_GPADL_BODY: {
        const vmbus_channel_gpadl_body* b = (const vmbus_channel_gpadl_body*)m;
        auto it = gpadls.find(b->gpadl);
        CHECK(it != gpadls.end() && !it->second.done, "body for GPADL %x", b->gpadl);
        Gpadl& n = it->second;
        uint32_t k = (len - sizeof *b) / 8;
        CHECK(len == sizeof *b + 8 * k && k && k <= 28 && n.pfn.size() + k <= n.pages, "body PFNs");
        { size_t at = n.pfn.size(); n.pfn.resize(at + k); memcpy(n.pfn.data() + at, (const uint8_t*)b + sizeof *b, 8 * k); }
        if (n.pfn.size() == n.pages) {
            n.done = true;
            vmbus_channel_gpadl_created r = { { CHANNELMSG_GPADL_CREATED, 0 }, n.relid, b->gpadl, 0 };
            send_msg(&r, sizeof r);
        }
        break; }
    case CHANNELMSG_OPENCHANNEL: {
        const vmbus_channel_open_channel* o = (const vmbus_channel_open_channel*)m;
        CHECK(len == sizeof *o, "open %u bytes", len);
        Chan* c = 0;
        for (Chan& x : chans) if (x.relid == o->child_relid) c = &x;
        CHECK(c && !c->open, "open relid %u", o->child_relid);
        auto it = gpadls.find(o->ringbuffer_gpadlhandle);
        CHECK(it != gpadls.end() && it->second.done && it->second.relid == c->relid, "ring GPADL");
        Gpadl& g = it->second;
        uint32_t split = o->downstream_ringbuffer_pageoffset;
        CHECK(split >= 2 && split + 2 <= g.pages, "downstream offset %u of %u pages", split, g.pages);
        for (uint32_t i = 1; i < g.pages; i++) CHECK(g.pfn[i] == g.pfn[0] + i, "ring not contiguous");
        c->g2h = (hv_ring_buffer*)P(g.pfn[0]);
        c->g2h_data = P(g.pfn[0]) + 4096;
        c->g2h_size = (split - 1) * 4096;
        c->h2g = (hv_ring_buffer*)P(g.pfn[split]);
        c->h2g_data = P(g.pfn[split]) + 4096;
        c->h2g_size = (g.pages - split - 1) * 4096;
        CHECK(c->h2g->interrupt_mask == 1, "polling guest masks its inbound ring");
        c->open = true;
        vmbus_channel_open_result r;
        memset(&r, 0, sizeof r);
        r.header.msgtype = CHANNELMSG_OPENCHANNEL_RESULT;
        r.child_relid = o->child_relid; r.openid = o->openid; r.status = 0;
        send_msg(&r, sizeof r);
        break; }
    case CHANNELMSG_UNLOAD: {
        unloaded = true;
        vmbus_channel_message_header a = { CHANNELMSG_UNLOAD_RESPONSE, 0 };
        send_msg(&a, sizeof a);
        break; }
    default: CHECK(0, "channel message %u", h->msgtype);
    }
    deliver();
}

void host_step()
{
    for (Chan& c : chans) {
        if (!c.open) continue;
        std::vector<uint8_t> pkt;
        int n = 0;
        while (n++ < 64 && ring_get(c, pkt)) {
            if (c.dev == DEV_SCSI) scsi_packet(c, pkt);
            else if (c.dev == DEV_NET) net_packet(c, pkt);
        }
        while (!c.pending.empty() && ring_put(c, c.pending.front())) c.pending.pop_front();
        if (c.dev == DEV_NET) rx_flush(c);
    }
    deliver();
}

// ---------------- the hypervisor ----------------
uint64_t sim_rdmsr(uint32_t m)
{
    switch (m) {
    case 0x40000000: return msr_guest_id;
    case 0x40000001: return msr_hypercall;
    case 0x40000002: return 0;                   // VP index
    case 0x40000080: return msr_scontrol;
    case 0x40000082: return msr_siefp;
    case 0x40000083: return msr_simp;
    }
    if (m >= 0x40000090 && m < 0x400000A0) return msr_sint[m - 0x40000090];
    CHECK(0, "rdmsr %x", m);
    return 0;
}

void sim_wrmsr(uint32_t m, uint64_t v)
{
    switch (m) {
    case 0x40000000: msr_guest_id = v; return;
    case 0x40000001: CHECK(msr_guest_id, "hypercall page before the guest OS ID"); msr_hypercall = v; return;
    case 0x40000080: msr_scontrol = v; return;
    case 0x40000082: msr_siefp = v; return;
    case 0x40000083: msr_simp = v; return;
    case 0x40000084: deliver(); return;          // EOM
    }
    if (m >= 0x40000090 && m < 0x400000A0) {
        CHECK((v & 0xFF) >= 16 || (v & 0x10000), "SINT vector %llu", (unsigned long long)(v & 0xFF));
        msr_sint[m - 0x40000090] = v;
        return;
    }
    CHECK(0, "wrmsr %x", m);
}

void sim_cpuid(uint32_t leaf, uint32_t* r)
{
    memset(r, 0, 16);
    switch (leaf) {
    case 1: r[2] = 1u << 31; break;
    case 0x40000000: r[0] = 0x4000000A; memcpy(&r[1], "Micr", 4); memcpy(&r[2], "osof", 4); memcpy(&r[3], "t Hv", 4); break;
    case 0x40000001: memcpy(&r[0], "Hv#1", 4); break;
    case 0x40000002: r[0] = 20348; r[1] = 10 << 16; break;
    case 0x40000003: r[0] = 1 << 1 | 1 << 2 | 1 << 5 | 1 << 6; r[1] = 1 << 4 | 1 << 5; break;
    }
}

uint64_t sim_hypercall(uint64_t control, uint32_t in, uint32_t out)
{
    CHECK(msr_hypercall & 1, "hypercall without the page");
    uint32_t code = control & 0xFFFF;
    bool fast = control & (1u << 16);
    if (code == 0x5C) {                          // HvPostMessage
        CHECK(!fast && in && !(in & 7) && (in & 4095) + 256 <= 4096, "post input at %x", in);
        CHECK(msr_simp & 1 && msr_scontrol & 1, "SynIC off");
        uint64_t s2 = msr_sint[2];
        CHECK(!(s2 & 0x10000) && (s2 & 0xFF) == 0x3F, "SINT2 %llx", (unsigned long long)s2);
        const uint8_t* p = A(in);
        uint32_t conn, type, size;
        memcpy(&conn, p, 4); memcpy(&type, p + 8, 4); memcpy(&size, p + 12, 4);
        CHECK(type == 1 && size <= 240, "type %u size %u", type, size);
        uint32_t ver; memcpy(&ver, p + 16 + 8, 4);
        uint32_t mt; memcpy(&mt, p + 16, 4);
        if (mt == CHANNELMSG_INITIATE_CONTACT) {
            uint32_t want = ver >= (5 << 16) ? 4 : 1;
            CHECK(conn == want, "initiate contact on connection %u", conn);
            if (want == 4 && old_host) return 0x12;  // HV_STATUS_INVALID_CONNECTION_ID
        } else CHECK(conn == conn_msg, "message on connection %u, wanted %u", conn, conn_msg);
        n_posts++;
        channel_msg(p + 16, size);
        return 0;
    }
    if (code == 0x5D) {                          // HvSignalEvent
        CHECK(fast, "slow signal");
        Chan* c = 0;
        for (Chan& x : chans) if (x.conn_id == in) c = &x;
        CHECK(c && c->open, "signal on connection %x", in);
        if (!c->dedicated) CHECK(int_page && (int_page[2048 + c->relid / 8] >> (c->relid % 8) & 1), "interrupt page bit");
        n_signals++;
        host_step();
        return 0;
    }
    CHECK(0, "hypercall %x", code);
    return 0;
}

void sim_config(bool old)
{
    old_host = old;
    max_version = old ? (4 << 16) : ((5 << 16) | 2);
    memset(scsi_ua, 1, sizeof scsi_ua);
}
bool sim_unloaded() { return unloaded; }
uint32_t sim_version() { return version; }
int sim_scsi_splits() { return scsi_splits_seen; }
const uint8_t* sim_disk(int lun, uint64_t lba) { return disk_sector(lun, lba); }
uint32_t sim_filter() { return filter; }
int sim_rx_outstanding() { return (int)rx_out.size(); }
