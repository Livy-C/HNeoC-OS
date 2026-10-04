#ifndef USER_H
#define USER_H

#include "types.h"

/* ============================================================
 * 账户与登录
 *
 * 账户表就是磁盘上的 /etc/passwd，一行一个账户：
 *     name:password_hash:uid:home:admin
 *
 * 口令用 djb2 散列后存 8 位十六进制。**这不是安全机制**：没有盐、
 * 算法公开、而且能写这个文件的人可以直接给自己加账户。教学够用。
 * ============================================================ */

#define USER_NAME_MAX   16
#define USER_HOME_MAX   64

/* 登录：提示输入用户名和口令，成功后设置"当前进程"的 uid 和工作目录。
 * 返回 false 表示用户按了 Ctrl+C 放弃。只在 ring 0 用（它自己读写键盘）。 */
bool user_login(void);

/* 纯校验：只回答"这组用户名和口令对不对"，不打印、不读键盘、不改任何状态。
 * SYS_LOGIN 用它 —— Shell 搬到 ring 3 之后登录提示是用户程序画的，
 * 但口令比对必须留在内核，否则任何程序都能自己宣布是 root。 */
bool user_authenticate(const char* name, const char* password,
                       uint32_t* uid_out, bool* admin_out,
                       char* home_out, uint32_t home_max);

/* 让 user_name() 跟上当前登录的人 */
void user_set_current_name(const char* name);

/* 当前登录的用户名；没登录过时返回 "?" */
const char* user_name(void);

/* 按用户名查 uid；找不到返回 0xFFFFFFFF */
uint32_t user_uid_of_name(const char* name);

/* 取某个用户的 home 目录（绝对路径）。找不到返回 false */
bool user_home_of(const char* name, char* out, uint32_t max);

/* 当前用户是不是管理员（uid 0 或 admin=1） */
bool user_is_admin(void);

/* 注册新账户：把一行追加进 /etc/passwd。
 * 返回 0 成功，负数表示：-1 名字不合法、-2 已存在、-3 文件系统出错。 */
int user_add(const char* name, const char* password, bool admin);

/* 交互式注册：提示输入两遍口令，再调 user_add */
bool user_add_interactive(const char* name, bool admin);

/* 口令散列（djb2）。useradd 和登录都用它 */
uint32_t user_hash_password(const char* s);

/* 列出账户表（Shell 的 users 命令用） */
void user_list(void);

#endif /* USER_H */
