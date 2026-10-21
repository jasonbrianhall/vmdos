// TCP for VMDOSNET's 9P over the network (tcp9p.cpp), used by fs9p.cpp;
// net.cpp hands it the frames.
#pragma once
#include "hw.hpp"

struct __attribute__((packed)) Tcp9pCfg {     // what VMDOSNET.EXE found (the DOS TCP/IP stack's settings)
    u8  family;                               // 4 or 6
    u16 port;                                 // the server's
    u8  server[16];
    u8  hop_mac[6];                           // the next hop to it (router or itself)
    u8  my4[4];                               // our IPv4 address
    u8  my6[16], my6ll[16];                   // our global and link-local IPv6 addresses
    u16 local_port;
    u8  hop4[4];                              // IPv4: the next hop (to ARP for it again)
    u8  hop6[16];                             // IPv6: likewise
};

int tcp9p_open(const Tcp9pCfg& cfg);          // a connection: its number; -1 no memory, -2 no answer, -3 refused
void tcp9p_close(int c);
bool tcp9p_send(int c, const u8* p, u32 n);   // queue bytes (false: no room, or it's gone)
bool tcp9p_pump(int c);                       // poll, take, send; false: the connection is gone
u8* tcp9p_data(int c, u32* have);             // what has come in, in order
void tcp9p_consume(int c, u32 n);

// net.cpp
bool tcp9p_rx(const u8* f, unsigned len);     // a frame from the wire: true if it was ours (not for DOS)
bool tcp9p_active();
void tcp9p_tick();
void net_poll();                              // the card polled now (frames to enqueue / tcp9p_rx)
void net_keep_rx(bool on);                    // the card keeps receiving (and the queue doesn't hold it up)
