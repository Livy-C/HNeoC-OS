#ifndef _FCNTL_H
#define _FCNTL_H

/* open() 的标志位，取值必须和内核 include/hneofs.h 里的
 * HNEOFS_O_* 保持一致 */

#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_ACCMODE 0x0003

#define O_CREAT   0x0100
#define O_TRUNC   0x0200
#define O_APPEND  0x0400

#endif /* _FCNTL_H */
