# USB Ethernet tests

`make -C tests/usbnet` runs the tests of `src/usbnet_frame.h` (RNDIS and
CDC-NCM framing) on Linux, with AddressSanitizer and UBSan.

## In QEMU

QEMU's `usb-net` is a CDC-ECM device with an RNDIS configuration too:

    make run NIC=usb                          # ECM
    make run NIC=usb KARGS=usbnet=rndis       # RNDIS

QEMU has no NCM device. `qemu-usb-net-ncm.patch` (against QEMU 8.2.2)
turns usb-net's ECM configuration into an NCM one, for testing only: it
reports GET_NTB_PARAMETERS with an awkward OUT layout (divisor 32,
remainder 2), takes SET_NTB_INPUT_SIZE, puts each received frame in an NTB
with its table after the datagram, and complains on stderr about any NTB
from vmdos it doesn't like. Build QEMU with it
(`--target-list=i386-softmmu` is enough), then:

    PATH=/path/to/patched/build:$PATH make run NIC=usb KARGS=usbnet=ncm

(`make run` calls `qemu-system-i386` from PATH.) `NCMDBG=1` in the
environment logs every transfer.

For each protocol, ping sizes 22, 42 and 52 make frames that end exactly on
a 64-byte packet (ECM, RNDIS and NCM in that order), and 1024 a large one:

    FOR %S IN (22 42 52 1024) DO PING -count 3 -size %S 10.0.2.2
