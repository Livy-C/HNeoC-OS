#ifndef SHELL_H
#define SHELL_H

#include "types.h"

#define SHELL_MAX_LINE    128
#define SHELL_HISTORY_LEN 8

/* 启动 Shell 主循环（正常情况下永不返回） */
void shell_run(void);

/* 打印启动横幅 */
void shell_print_banner(void);

#endif /* SHELL_H */
