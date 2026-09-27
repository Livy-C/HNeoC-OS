#ifndef _ERRNO_H
#define _ERRNO_H

/* 目前错误码直接由系统调用返回负值，没有单独的 errno 变量。
 * 这里先把常量定下来，等以后需要 errno 语义时再补。
 */

#define EPERM    1
#define ENOENT   2
#define EIO      5
#define EBADF    9
#define ENOMEM  12
#define EACCES  13
#define EEXIST  17
#define ENOSPC  28
#define EINVAL  22
#define ENOSYS  38

#endif /* _ERRNO_H */
