#pragma once
/* kdos: DOSBox-X's LOG(type,sev)(fmt,...) -> the vmdos log at debug level only */
enum LOG_TYPES { LOG_ALL, LOG_VGA, LOG_VGAGFX, LOG_VGAMISC, LOG_INT10, LOG_SB, LOG_DMACONTROL, LOG_FPU, LOG_CPU, LOG_PAGING,
  LOG_FCB, LOG_FILES, LOG_IOCTL, LOG_EXEC, LOG_DOSMISC, LOG_PIT, LOG_KEYBOARD, LOG_PIC, LOG_MOUSE, LOG_BIOS, LOG_GUI, LOG_MISC,
  LOG_IO, LOG_PCI, LOG_MAX };
enum LOG_SEVERITIES { LOG_DEBUG, LOG_NORMAL, LOG_WARN, LOG_ERROR, LOG_FATAL };
struct LOG { LOG(int, int) {} void operator()(const char *, ...) {} };
#define LOG_ERR(...) do{}while(0)
#define LOG_WARN_MSG(...) do{}while(0)
