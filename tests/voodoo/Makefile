# The emulated Voodoo 3 as a Linux program (hosttest.c says what it does).
#   make && ./hosttest out.ppm
SRC   := ../../src/voodoo
VSRC  := $(addprefix $(SRC)/,vid_voodoo.c vid_voodoo_banshee_blitter.c vid_voodoo_blitter.c vid_voodoo_display.c \
         vid_voodoo_fb.c vid_voodoo_fifo.c vid_voodoo_reg.c vid_voodoo_render.c vid_voodoo_setup.c \
         vid_voodoo_texture.c v3.c v3sched.c)
# as the kernel's Makefile builds them
VFLAGS := -m32 -march=i686 -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fcf-protection=none \
          -mfpmath=387 -mno-sse -mno-mmx -O2 -g -fno-strict-aliasing -DNO_CODEGEN -DTEX_CACHE_MAX=16 \
          -I$(SRC)/libc -I$(SRC) -w
VOBJS := $(patsubst $(SRC)/%.c,obj/%.o,$(VSRC))

hosttest: hosttest.c $(VOBJS)
	gcc -m32 -O1 -g -no-pie -o $@ hosttest.c $(VOBJS)

obj/%.o: $(SRC)/%.c
	@mkdir -p obj
	gcc $(VFLAGS) -c $< -o $@

clean:
	rm -rf obj hosttest
