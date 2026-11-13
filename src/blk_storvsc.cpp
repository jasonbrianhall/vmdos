// Hyper-V synthetic SCSI disks (blk.h): the disks on a Hyper-V virtual
// machine's SCSI controllers (Gen 1: Settings > SCSI Controller; Gen 2: all
// of them). Linux: drivers/scsi/storvsc_drv.c.
//
// Each controller is a VMBus channel (vmbus.cpp). The storage protocol
// (VSTOR): begin initialization, agree on a protocol version, ask for the
// channel's properties, end initialization; then SCSI commands go to the
// host as VSTOR "execute SRB" packets, the data buffer described by its
// page numbers (GPA direct), and come back as completions. One command at
// a time, polled. Disks are found with INQUIRY on target 0, LUN 0-63 (the
// controller's "location"); 512-byte sectors only.
//
// The IDE disks of a Gen 1 machine are vmdos's IDE driver's (blk_ide.cpp,
// Hyper-V emulates the controller); C: is normally one of those, as Gen 1
// boots from IDE. These are the other disks, for VMHD. storvsc=off: don't.
#include "vmbus.hpp"
#include "blk.h"

namespace {

const char* const SCSI_GUID = "ba6163d9-04a1-4d29-b605-72e2ffb1dc7f";
const int MAX_CTRL = 4, MAX_DISKS = 16, MAX_LUN = 64;
const uint32_t RING_PAGES = 8;                  // each way, header page included

enum { OP_COMPLETE_IO = 1, OP_EXECUTE_SRB = 3, OP_BEGIN_INIT = 7, OP_END_INIT = 8, OP_QUERY_VERSION = 9, OP_QUERY_PROPERTIES = 10 };
enum { SRB_SUCCESS = 0x01, SRB_DATA_OVERRUN = 0x12 };
enum { SRB_FLAGS_DISABLE_SYNCH_TRANSFER = 0x08, SRB_FLAGS_DATA_IN = 0x40, SRB_FLAGS_DATA_OUT = 0x80 };
enum { WRITE_TYPE = 0, READ_TYPE = 1, UNKNOWN_TYPE = 2 };
enum { DIR_NONE, DIR_IN, DIR_OUT };

// struct vstor_packet: operation, flags, status, then (at 12) the SRB, the
// version or the properties. 64 bytes; 16 fewer before win8 (no win8_extension).
const uint32_t PKT = 64, SRB = 12;

struct Proto { uint16_t ver; uint8_t sense; uint8_t delta; };
const Proto protos[] = {
    { 0x0602, 0x14, 0 },                        // WIN10
    { 0x0600, 0x14, 0 },                        // WIN8_1
    { 0x0501, 0x14, 0 },                        // WIN8
    { 0x0402, 0x12, 16 },                       // WIN7
    { 0x0200, 0x12, 16 },                       // WIN6 (2008)
};

struct Ctrl {
    VmbusChannel* ch;
    const Proto* proto;
    uint32_t max_secs;                          // per command
    uint8_t* io;                                // a page: [0] the packet out, [256] the reply, [512] the packet read, [2048] small data
    uint64_t tid;
};
struct Disk { int ctrl; uint8_t lun; uint64_t sectors; char name[24]; };

Ctrl ctrls[MAX_CTRL];
int n_ctrls;
Disk disks[MAX_DISKS];
int n_disks;
bool started;

inline uint32_t pa(const void* p) { return (uint32_t)(uintptr_t)p; }
inline uint32_t get32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
inline void put16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

uint32_t pkt_size(Ctrl& k) { return PKT - (k.proto ? k.proto->delta : 16); }   // before the version's known: the short one

// The packet at k.io out (with a buffer if len), the host's reply into
// k.io + 256. False: no reply (or the ring wouldn't take it).
bool exec(Ctrl& k, uint32_t buf, uint32_t len, uint32_t ms)
{
    uint64_t tid = ++k.tid;
    uint8_t* out = k.io;
    put32(out + 4, 1);                          // REQUEST_COMPLETION_FLAG
    bool sent = false;
    for (int t = 0; t < 1000 && !sent; t++) {
        sent = len ? vmbus_send_buffer(*k.ch, out, pkt_size(k), buf, len, tid)
                   : vmbus_send_inband(*k.ch, out, pkt_size(k), tid, true);
        if (!sent) { vmbus_recv(*k.ch, k.io + 512, 1024); kdelay_us(10); }   // full: make way (stale replies)
    }
    if (!sent) return false;
    uint8_t* in = k.io + 512;
    for (uint32_t t = 0; t < ms * 100; t++) {
        uint32_t n = vmbus_recv(*k.ch, in, 1024);
        if (!n) { kdelay_us(10); continue; }
        const VmPacketDesc* d = (const VmPacketDesc*)in;
        if (d->type != VM_PKT_COMP || d->trans_id != tid) continue;   // the host's own (bus rescans) or stale
        uint32_t off = (uint32_t)d->offset8 << 3, got = n < 1024 ? n : 1024;
        memset(k.io + 256, 0, PKT);
        if (off < got) memcpy(k.io + 256, in + off, got - off < PKT ? got - off : PKT);
        return true;
    }
    kprintf("storvsc: relid %u: no reply\n", k.ch->relid);
    return false;
}

// A VSTOR operation with no data: 0 done (completed with status 0).
int vstor_op(Ctrl& k, uint32_t op, bool status_check = true)
{
    put32(k.io, op);
    if (!exec(k, 0, 0, 5000)) return -1;
    const uint8_t* r = k.io + 256;
    if (get32(r) != OP_COMPLETE_IO) return -1;
    return status_check && get32(r + 8) ? -1 : 0;
}

// A SCSI command: 0 done, 2 check condition (the sense key in *key), -1 failed.
int scsi(Ctrl& k, uint8_t lun, const uint8_t* cdb, int cdblen, uint32_t buf, uint32_t len, int dir, uint8_t* key = 0)
{
    uint8_t* p = k.io;
    memset(p, 0, PKT);
    put32(p, OP_EXECUTE_SRB);
    uint8_t* s = p + SRB;
    put16(s, (uint16_t)(52 - k.proto->delta));  // sizeof(vmscsi_request)
    s[4] = 0; s[5] = 0; s[6] = 0; s[7] = lun;   // port, path (bus), target, LUN
    s[8] = (uint8_t)cdblen;
    s[9] = k.proto->sense;
    s[10] = dir == DIR_OUT ? WRITE_TYPE : dir == DIR_IN ? READ_TYPE : UNKNOWN_TYPE;
    put32(s + 12, len);
    memcpy(s + 16, cdb, cdblen);
    if (!k.proto->delta) {                      // win8 extension
        put32(s + 40, SRB_FLAGS_DISABLE_SYNCH_TRANSFER | (dir == DIR_IN ? SRB_FLAGS_DATA_IN : dir == DIR_OUT ? SRB_FLAGS_DATA_OUT : 0));
        put32(s + 44, 60);                      // time-out, seconds
    }
    if (!exec(k, len ? buf : 0, len, 30000)) return -1;
    const uint8_t* r = k.io + 256;
    if (get32(r) != OP_COMPLETE_IO) return -1;
    uint8_t srb = r[SRB + 2] & 0x3F, st = r[SRB + 3];
    if (st == 2) {                              // CHECK CONDITION: the sense data where the CDB was
        if (key) *key = r[SRB + 16 + 2] & 0x0F;
        return 2;
    }
    if (st) return -1;
    return srb == SRB_SUCCESS || srb == SRB_DATA_OVERRUN ? 0 : -1;
}

// Again on a check condition (UNIT ATTENTION after a reset, NOT READY while it spins up).
int scsi_retry(Ctrl& k, uint8_t lun, const uint8_t* cdb, int cdblen, uint32_t buf, uint32_t len, int dir)
{
    int r = -1;
    for (int t = 0; t < 4; t++) {
        uint8_t key = 0;
        r = scsi(k, lun, cdb, cdblen, buf, len, dir, &key);
        if (r != 2) return r;
        if (key == 2) kdelay_us(100000);
    }
    return r;
}

bool init_ctrl(Ctrl& k)
{
    uint8_t props[24] = {};
    if (!vmbus_open(*k.ch, RING_PAGES, RING_PAGES, props, sizeof props)) return false;
    k.io = (uint8_t*)phys_alloc(4096);
    memset(k.io, 0, PKT);
    if (vstor_op(k, OP_BEGIN_INIT)) { kprintf("storvsc: relid %u: initialization refused\n", k.ch->relid); return false; }
    for (const Proto& p : protos) {
        memset(k.io, 0, PKT);
        put16(k.io + SRB, p.ver);               // major_minor; revision 0
        if (vstor_op(k, OP_QUERY_VERSION, false)) return false;
        if (!get32(k.io + 256 + 8)) { k.proto = &p; break; }
    }
    if (!k.proto) { kprintf("storvsc: relid %u: no protocol version in common\n", k.ch->relid); return false; }
    memset(k.io, 0, PKT);
    if (vstor_op(k, OP_QUERY_PROPERTIES)) return false;
    uint32_t max = get32(k.io + 256 + SRB + 12);  // max_transfer_bytes
    k.max_secs = max >= 65536 || !max ? 128 : max >= 512 ? max / 512 : 1;
    memset(k.io, 0, PKT);
    if (vstor_op(k, OP_END_INIT)) return false;
    kprintf("storvsc: SCSI controller %d: protocol %u.%u, %u KiB a command\n", n_ctrls, k.proto->ver >> 8,
            k.proto->ver & 0xFF, k.max_secs / 2);
    return true;
}

void scan(int ci)
{
    Ctrl& k = ctrls[ci];
    uint8_t* d = k.io + 2048;
    for (int lun = 0; lun < MAX_LUN && n_disks < MAX_DISKS; lun++) {
        static const uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
        memset(d, 0, 64);
        if (scsi_retry(k, (uint8_t)lun, inq, 6, pa(d), 36, DIR_IN)) continue;
        if (d[0] & 0xE0) continue;              // nothing at this LUN
        if ((d[0] & 0x1F) != 0) {               // not a disk (a DVD drive, say)
            kprintf("storvsc: %d:%d: device type %u, skipped\n", ci, lun, d[0] & 0x1F);
            continue;
        }
        char prod[17];
        int pl = 0;
        for (int i = 16; i < 32 && d[i] >= ' ' && d[i] < 127; i++) prod[pl++] = (char)d[i];
        while (pl && prod[pl - 1] == ' ') pl--;
        prod[pl] = 0;
        char shortp[15];                        // the name: 14 characters of it
        memcpy(shortp, prod, 15);
        shortp[14] = 0;

        static const uint8_t tur[6] = { 0 };
        scsi_retry(k, (uint8_t)lun, tur, 6, 0, 0, DIR_NONE);
        static const uint8_t rc10[10] = { 0x25 };
        memset(d, 0, 32);
        if (scsi_retry(k, (uint8_t)lun, rc10, 10, pa(d), 8, DIR_IN)) { kprintf("storvsc: %d:%d: no capacity\n", ci, lun); continue; }
        uint64_t last = be32(d);
        uint32_t bs = be32(d + 4);
        if (last == 0xFFFFFFFFu) {              // 2 TiB or more: READ CAPACITY (16)
            uint8_t rc16[16] = { 0x9E, 0x10 };
            rc16[13] = 32;
            memset(d, 0, 32);
            if (scsi_retry(k, (uint8_t)lun, rc16, 16, pa(d), 32, DIR_IN)) continue;
            last = (uint64_t)be32(d) << 32 | be32(d + 4);
            bs = be32(d + 8);
        }
        if (bs != 512) { kprintf("storvsc: %d:%d: %u-byte sectors (only 512 here)\n", ci, lun, bs); continue; }
        Disk& dk = disks[n_disks++];
        dk.ctrl = ci;
        dk.lun = (uint8_t)lun;
        dk.sectors = last + 1;
        snprintf(dk.name, sizeof dk.name, "%s %d:%d", pl ? shortp : "SCSI disk", ci, lun);
        kprintf("storvsc: disk %d:%d: %s, %u MiB\n", ci, lun, prod, (uint32_t)(dk.sectors >> 11));
    }
}

int start(void)
{
    if (started) return n_disks;
    started = true;
    if (strstr(cmdline, "storvsc=off") || !vmbus_start()) return 0;
    for (int i = 0; n_ctrls < MAX_CTRL; i++) {
        VmbusChannel* ch = vmbus_find(SCSI_GUID, i);
        if (!ch) break;
        if (ch->sub_index) continue;            // a sub-channel (multi-queue): the primary does it all
        Ctrl& k = ctrls[n_ctrls];
        memset(&k, 0, sizeof k);
        k.ch = ch;
        if (!init_ctrl(k)) continue;
        scan(n_ctrls++);
    }
    return n_disks;
}

uint64_t sectors(int i) { return i < n_disks ? disks[i].sectors : 0; }
const char* name(int i) { return i < n_disks ? disks[i].name : ""; }

int rw(int i, uint64_t lba, uint32_t n, void* buf, int write)
{
    if (i >= n_disks || !n || n > 128 || lba + n > disks[i].sectors) return -1;
    Disk& d = disks[i];
    Ctrl& k = ctrls[d.ctrl];
    uint8_t* b = (uint8_t*)buf;
    while (n) {
        uint32_t c = n < k.max_secs ? n : k.max_secs;
        uint8_t cdb[16] = {};
        int len;
        if (lba + c <= 0xFFFFFFFFull) {         // READ/WRITE (10)
            cdb[0] = write ? 0x2A : 0x28;
            for (int j = 0; j < 4; j++) cdb[2 + j] = (uint8_t)(lba >> (24 - 8 * j));
            cdb[7] = (uint8_t)(c >> 8); cdb[8] = (uint8_t)c;
            len = 10;
        } else {                                // (16)
            cdb[0] = write ? 0x8A : 0x88;
            for (int j = 0; j < 8; j++) cdb[2 + j] = (uint8_t)(lba >> (56 - 8 * j));
            for (int j = 0; j < 4; j++) cdb[10 + j] = (uint8_t)(c >> (24 - 8 * j));
            len = 16;
        }
        if (scsi_retry(k, d.lun, cdb, len, pa(b), c * 512, write ? DIR_OUT : DIR_IN)) {
            kprintf("storvsc: %s: %s of %u sectors at %u failed\n", d.name, write ? "write" : "read", c, (uint32_t)lba);
            return -1;
        }
        lba += c; n -= c; b += c * 512;
    }
    return 0;
}

} // namespace

extern "C" const struct blk_driver blk_storvsc = {
    "Hyper-V", "storvsc=off", 1, 0, start, nullptr, sectors, name, nullptr, rw, nullptr, nullptr,
};
