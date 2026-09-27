#ifndef ATA_H
#define ATA_H

#include "types.h"

/* ATA PIO 模式驱动
 *
 * 只用主通道（Primary）的 PIO 读写，不做 DMA、不做中断。
 * 对教学用途来说 PIO 足够，而且逻辑简单、容易调试。
 */

#define ATA_SECTOR_SIZE   512
#define ATA_MAX_DRIVES    4

/* 通道与驱动编号 */
#define ATA_PRIMARY       0
#define ATA_SECONDARY     1
#define ATA_MASTER        0
#define ATA_SLAVE         1

/* 一个已识别出来的 ATA 设备 */
typedef struct {
    bool     present;
    bool     lba_supported;
    uint8_t  channel;        /* ATA_PRIMARY / ATA_SECONDARY */
    uint8_t  drive;          /* ATA_MASTER / ATA_SLAVE */
    uint16_t signature;
    uint32_t sectors;        /* 可用扇区总数（LBA28） */
    char     model[41];      /* 型号字符串，已去掉尾部空格 */
    char     serial[21];     /* 序列号 */
} ata_device_t;

/* 探测所有通道上的设备，返回识别到的设备数量 */
uint32_t ata_init(void);

/* 取第 index 个已识别的设备（0 起算），越界返回 NULL */
const ata_device_t* ata_get_device(uint32_t index);

/* 取第一个可用设备（通常是主通道主盘），没有则返回 NULL */
const ata_device_t* ata_primary_device(void);

/* 从 LBA 起读 count 个扇区到 buffer。成功返回 true */
bool ata_read_sectors(uint32_t lba, uint8_t count, void* buffer);

/* 往 LBA 起写 count 个扇区。成功返回 true */
bool ata_write_sectors(uint32_t lba, uint8_t count, const void* buffer);

/* 最近一次操作的错误信息（失败时用来打印原因） */
const char* ata_last_error(void);

/* 统计：读写了多少扇区，供 taskmgr 之类的界面展示 */
uint32_t ata_sectors_read(void);
uint32_t ata_sectors_written(void);

#endif /* ATA_H */
