// Hyper-V's VMBus, for the synthetic devices of a Hyper-V virtual machine
// (Gen 1 or Gen 2): the SCSI controller's disks (blk_storvsc.cpp) and the
// network adapter (nic_netvsc.cpp). Linux: drivers/hv (vmbus_drv.c,
// connection.c, channel.c, channel_mgmt.c, ring_buffer.c, hv.c).
//
// Polled, like vmdos's other drivers: the SynIC's message page for the
// control messages (offers, GPADLs, opening channels), and each channel's
// inbound ring for its packets. The host is told about what we write with
// HvSignalEvent; it isn't asked to interrupt us (interrupt_mask in our
// inbound rings).
#pragma once
#include "hw.hpp"

struct VmbusChannel {
    uint8_t type[16], inst[16];                 // interface type and instance GUIDs (as on the wire)
    uint8_t user_def[120];
    uint32_t relid, conn_id;
    uint16_t sub_index;
    bool dedicated;                             // dedicated interrupt: no bit in the shared interrupt page
    bool open;
    uint32_t gpadl;                             // the rings' GPADL
    volatile uint32_t* out_hdr;                 // write_index, read_index, interrupt_mask, pending_send_sz
    volatile uint32_t* in_hdr;
    uint8_t *out_data, *in_data;
    uint32_t out_size, in_size;                 // the data areas' bytes
};

// VM packet types and flags (struct vmpacket_descriptor)
enum { VM_PKT_DATA_INBAND = 6, VM_PKT_DATA_USING_XFER_PAGES = 7, VM_PKT_DATA_USING_GPA_DIRECT = 9, VM_PKT_COMP = 11 };
enum { VMBUS_COMPLETION_REQUESTED = 1 };

struct VmPacketDesc { uint16_t type, offset8, len8, flags; uint64_t trans_id; } __attribute__((packed));

// Hyper-V present and VMBus connected (once; later calls return what the first found).
bool vmbus_start();
// The nth (0-based) offered channel of type guid ("ba6163d9-04a1-4d29-b605-72e2ffb1dc7f"), 0: none.
VmbusChannel* vmbus_find(const char* guid, int nth);
// Rings of out_pages and in_pages pages (each with its header page) shared and the channel opened.
bool vmbus_open(VmbusChannel& c, uint32_t out_pages, uint32_t in_pages, const void* userdata, uint32_t ulen);
// Memory the host may use (a receive buffer): its GPADL handle into *handle.
bool vmbus_gpadl(VmbusChannel& c, const void* buf, uint32_t bytes, uint32_t* handle);
// A packet: the 16-byte descriptor made here (type, flags, trans_id), then extra
// (the rest of the descriptor: page ranges), then data. False: no room in the ring.
bool vmbus_send(VmbusChannel& c, uint16_t type, uint16_t flags, uint64_t tid,
                const void* extra, uint32_t elen, const void* data, uint32_t dlen);
// Inband data, with or without a completion asked for.
inline bool vmbus_send_inband(VmbusChannel& c, const void* data, uint32_t len, uint64_t tid, bool completion)
{
    return vmbus_send(c, VM_PKT_DATA_INBAND, completion ? VMBUS_COMPLETION_REQUESTED : 0, tid, 0, 0, data, len);
}
// GPA direct: inband data plus one buffer (the host reads or writes it) of
// len bytes at physical address pa, any alignment, at most 64 KiB.
bool vmbus_send_buffer(VmbusChannel& c, const void* data, uint32_t dlen, uint32_t pa, uint32_t len, uint64_t tid);
// The same, as page buffers (netvsc): up to 3 pieces, a page each at most.
bool vmbus_send_pagebuf(VmbusChannel& c, const void* data, uint32_t dlen, uint32_t pa, uint32_t len, uint64_t tid);
// The next packet from the host, descriptor first, into buf (room bytes;
// the rest of a longer one is left out): its whole length, 0 if none.
uint32_t vmbus_recv(VmbusChannel& c, void* buf, uint32_t room);
bool vmbus_pending(VmbusChannel& c);            // a packet waits
void vmbus_signal(VmbusChannel& c);
void vmbus_unload();                            // before a reboot: the host lets go of our memory
bool vmbus_up();                                // vmbus_start() said yes
