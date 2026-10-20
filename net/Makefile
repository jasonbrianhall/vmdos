# vmdos network tools for DOS: VMTCP, DHCP, PING, NSLOOKUP, VMIPXBOX.
# Built with Open Watcom (16-bit, small model) on top of mTCP
# (third_party/mtcp). The results are committed in net/bin/, so building
# vmdos doesn't need Open Watcom; this rebuilds them:
#
#   make -C net                     WATCOM=/path/to/open-watcom (default: ../.watcom,
#                                   which tools/fetch-watcom.sh fills)
#   make -C net install             also copies them into bin/

WATCOM  ?= $(abspath ../.watcom)
export WATCOM
export INCLUDE := $(WATCOM)/h
WBIN    := $(firstword $(wildcard $(WATCOM)/binl64 $(WATCOM)/binl))
WPP     := $(WBIN)/wpp
WASM    := $(WBIN)/wasm
WLINK   := $(WBIN)/wlink
export PATH := $(WBIN):$(PATH)

MTCP    := ../third_party/mtcp
OUT     := ../build/net
PROGS   := VMTCP DHCP PING NSLOOKUP VMIPXBOX

# mTCP's options (its MAKEFILEs): 8088 code, small model, small and warning-free.
COPTS   := -0 -ms -s -oh -ok -os -oa -ei -zp2 -zpw -we -zq -wcd=7 \
           -i=$(MTCP)/tcpinc -i=lib

MTCP_OBJS := packet arp eth ip utils timer udp trace
LIB_OBJS  := addr net ip6 resolv

all: $(addprefix $(OUT)/,$(addsuffix .EXE,$(PROGS)))

install: all
	cp $(addprefix $(OUT)/,$(addsuffix .EXE,$(PROGS))) bin/

# --- objects with net.cfg (everything but DHCP) ---
NETCFG := '-DCFG_H="net.cfg"'
N := $(OUT)/n
$(N)/%.obj: $(MTCP)/tcplib/%.cpp lib/net.cfg | $(N)
	$(WPP) $(COPTS) $(NETCFG) -fo=$@ $<
$(N)/%.obj: lib/%.cpp lib/vmnet.h lib/net.cfg | $(N)
	$(WPP) $(COPTS) $(NETCFG) -fo=$@ $<
$(N)/ipasm.obj: $(MTCP)/tcplib/ipasm.asm | $(N)
	$(WASM) -0 -ms -zq -fo=$@ $<
NOBJS := $(addprefix $(N)/,$(addsuffix .obj,$(MTCP_OBJS) $(LIB_OBJS) ipasm))

# --- objects with dhcp.cfg ---
DHCPCFG := '-DCFG_H="dhcp.cfg"' -i=dhcp
D := $(OUT)/d
$(D)/%.obj: $(MTCP)/tcplib/%.cpp dhcp/dhcp.cfg | $(D)
	$(WPP) $(COPTS) $(DHCPCFG) -fo=$@ $<
$(D)/%.obj: lib/%.cpp lib/vmnet.h dhcp/dhcp.cfg | $(D)
	$(WPP) $(COPTS) $(DHCPCFG) -fo=$@ $<
$(D)/ipasm.obj: $(MTCP)/tcplib/ipasm.asm | $(D)
	$(WASM) -0 -ms -zq -fo=$@ $<
DOBJS := $(addprefix $(D)/,$(addsuffix .obj,$(MTCP_OBJS) $(LIB_OBJS) ipasm))

$(N) $(D):
	mkdir -p $@

define prog
$(OUT)/$(1).EXE: $(OUT)/$(2).obj $(3)
	$(WLINK) system dos option quiet option eliminate option stack=$(4) name $$@ file { $$^ }
endef

$(N)/vmtcp.obj: vmtcp/vmtcp.cpp lib/vmnet.h | $(N)
	$(WPP) $(COPTS) $(NETCFG) -fo=$@ $<
$(N)/ping.obj: ping/ping.cpp lib/vmnet.h | $(N)
	$(WPP) $(COPTS) $(NETCFG) -fo=$@ $<
$(N)/nslookup.obj: nslookup/nslookup.cpp lib/vmnet.h | $(N)
	$(WPP) $(COPTS) $(NETCFG) -fo=$@ $<
$(N)/vmipxbox.obj: vmipxbox/vmipxbox.cpp lib/vmnet.h | $(N)
	$(WPP) $(COPTS) $(NETCFG) -fo=$@ $<
$(D)/dhcp.obj: dhcp/dhcp.cpp dhcp/dhcp.h lib/vmnet.h dhcp/dhcp.cfg | $(D)
	$(WPP) $(COPTS) $(DHCPCFG) -fo=$@ $<
$(D)/dhcp6.obj: dhcp/dhcp6.cpp lib/vmnet.h dhcp/dhcp.cfg | $(D)
	$(WPP) $(COPTS) $(DHCPCFG) -fo=$@ $<

$(eval $(call prog,VMTCP,n/vmtcp,$(NOBJS),4096))
$(eval $(call prog,PING,n/ping,$(NOBJS),4096))
$(eval $(call prog,NSLOOKUP,n/nslookup,$(NOBJS),4096))
$(eval $(call prog,VMIPXBOX,n/vmipxbox,$(NOBJS),4096))
$(eval $(call prog,DHCP,d/dhcp,$(D)/dhcp6.obj $(DOBJS),4096))

clean:
	rm -rf $(OUT)

.PHONY: all install clean
