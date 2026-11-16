#!/usr/bin/env python3
# For tools/build-openssh.sh: strerror() with DJGPP's errno numbers (from its
# <errno.h>, argv[1]) and sys_nerr, so Watt-32's util/errnos.c can be built
# and run on the host to make inc/sys/djgpp.err and syserr.c.
import re, sys
msgs = {'EDOM': 'Domain error', 'ERANGE': 'Result too large', 'E2BIG': 'Argument list too long',
        'EACCES': 'Permission denied', 'EAGAIN': 'Resource temporarily unavailable',
        'EBADF': 'Bad file descriptor', 'EBUSY': 'Resource busy', 'ECHILD': 'No child processes',
        'EDEADLK': 'Resource deadlock avoided', 'EEXIST': 'File exists', 'EFAULT': 'Bad address',
        'EFBIG': 'File too large', 'EINTR': 'Interrupted system call', 'EINVAL': 'Invalid argument',
        'EIO': 'Input or output error', 'EISDIR': 'Is a directory', 'EMFILE': 'Too many open files',
        'EMLINK': 'Too many links', 'ENAMETOOLONG': 'File name too long',
        'ENFILE': 'Too many open files in system', 'ENODEV': 'No such device',
        'ENOENT': 'No such file or directory', 'ENOEXEC': 'Unable to execute file',
        'ENOLCK': 'No locks available', 'ENOMEM': 'Not enough memory',
        'ENOSPC': 'No space left on drive', 'ENOSYS': 'Function not implemented',
        'ENOTDIR': 'Not a directory', 'ENOTEMPTY': 'Directory not empty',
        'ENOTTY': 'Inappropriate I/O control operation', 'ENXIO': 'No such device or address',
        'EPERM': 'Operation not permitted', 'EPIPE': 'Broken pipe', 'EROFS': 'Read-only file system',
        'ESPIPE': 'Invalid seek', 'ESRCH': 'No such process', 'EXDEV': 'Improper link',
        'ENMFILE': 'No more files', 'ELOOP': 'Too many levels of symbolic links',
        'EOVERFLOW': 'Value too large', 'EILSEQ': 'Invalid or incomplete multibyte or wide character'}
nums = {}
for m in re.finditer(r'#define\s+(E\w+)\s+(\d+)', open(sys.argv[1]).read()):
    nums[int(m.group(2))] = msgs.get(m.group(1), m.group(1))
print('#include <string.h>\nstatic const char *dj_strerror(int e)\n{\n  switch (e) {')
for k in sorted(nums):
    print(f'  case {k}: return "{nums[k]}";')
print('  }\n  return "Unknown error";\n}\n#define strerror dj_strerror')
print(f'int sys_nerr = {max(nums) + 2};  /* djgpp: 1 - {max(nums)}; errnos.c takes one off */')
