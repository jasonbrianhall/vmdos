# The emulated Voodoo 3 as a Linux program (hosttest.c says what it does).
#   make && ./hosttest out.ppm
SRC   := ../../src/voodoo
VSRC  := $(addprefix $(SRC)/,vid_voodoo.c vid_voodoo_banshee_blitter.c vid_voodoo_blitter.c vid_voodoo_display.c \
         vid_voodoo_fb.c vid_voodoo_fifo.c vid_voodoo_reg.c vid_voodoo_render.c vid_voodoo_setup.c \
         vid_voodoo_texture.c vid_svga.c vid_svga_render.c timer.c v3card.c v3.c v3sched.c)
# as the kernel's Makefile builds them
VFLAGS := -m32 -march=i686 -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fcf-protection=none \
          -mfpmath=387 -mno-sse -mno-mmx -O2 -g -fno-strict-aliasing -DNO_CODEGEN -DTEX_CACHE_MAX=16 \
          -I$(SRC)/libc -I$(SRC) -w
VOBJS := $(patsubst $(SRC)/%.c,obj/%.o,$(VSRC))

ROM   := $(firstword $(wildcard ../../roms/voodoo3/3k12sd.rom))

hosttest: hosttest.c $(VOBJS) obj/v3rom.o
	gcc -m32 -O1 -g -no-pie -o $@ hosttest.c $(VOBJS) obj/v3rom.o -lm

obj/v3rom.o: $(SRC)/v3rom.S $(ROM)
	@mkdir -p obj
	gcc -m32 $(if $(ROM),-DV3ROM='"$(ROM)"') -c $< -o $@

obj/%.o: $(SRC)/%.c
	@mkdir -p obj
	gcc $(VFLAGS) -c $< -o $@

clean:
	rm -rf obj hosttest
