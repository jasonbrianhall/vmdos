/* Roland MPU-401 in UART mode (330h data, 331h status/command), feeding the
   General MIDI synth. Intelligent mode isn't emulated: commands are ACKed
   and data bytes are treated as MIDI either way, which is what games use. */
#include "mpu.h"
#include "gmsynth.h"

static uint16_t base;
static uint8_t rxq[16], rx_head, rx_tail;
static uint8_t uart;

static void rx_push(uint8_t v)
{
    uint8_t next = (rx_tail + 1) & 15;
    if (next != rx_head) { rxq[rx_tail] = v; rx_tail = next; }
}

void mpu_init(uint16_t port)
{
    base = port;
    rx_head = rx_tail = 0;
    uart = 0;
    gm_reset();
}

int mpu_owns(uint16_t port)
{
    return base && (port == base || port == base + 1);
}

uint8_t mpu_in(uint16_t port)
{
    if (port == base) {                             /* data: ACKs and replies */
        uint8_t v = 0xFE;
        if (rx_head != rx_tail) { v = rxq[rx_head]; rx_head = (rx_head + 1) & 15; }
        return v;
    }
    /* status: bit 6 clear = ready to accept, bit 7 clear = data to read */
    return (rx_head == rx_tail ? 0x80 : 0x00) | 0x3F;
}

void mpu_out(uint16_t port, uint8_t v)
{
    if (port == base) {
        gm_midi_byte(v);
        return;
    }
    switch (v) {
    case 0xFF:                                      /* reset */
        gm_reset();
        uart = 0;
        rx_head = rx_tail = 0;
        rx_push(0xFE);
        break;
    case 0x3F:                                      /* enter UART mode */
        uart = 1;
        rx_push(0xFE);
        break;
    case 0xAC: rx_push(0xFE); rx_push(0x15); break; /* version 1.5 */
    case 0xAD: rx_push(0xFE); rx_push(0x01); break; /* revision */
    default:
        if (!uart) rx_push(0xFE);                   /* ACK everything else */
        break;
    }
}
