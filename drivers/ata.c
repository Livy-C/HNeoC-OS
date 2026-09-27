#include "../include/ata.h"
#include "../include/ports.h"
#include "../include/string.h"

/* ------------------------------------------------------------
 * 端口与寄存器
 * ------------------------------------------------------------ */

#define ATA_PRIMARY_IO     0x1F0
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_IO   0x170
#define ATA_SECONDARY_CTRL 0x376

/* 相对通道基址的寄存器偏移 */
#define REG_DATA        0   /* 数据寄存器（16 位） */
#define REG_ERROR       1   /* 读：错误信息 */
#define REG_FEATURES    1   /* 写：特性 */
#define REG_SECCOUNT    2   /* 要传输的扇区数 */
#define REG_LBA0        3   /* LBA 位 0-7 */
#define REG_LBA1        4   /* LBA 位 8-15 */
#define REG_LBA2        5   /* LBA 位 16-23 */
#define REG_DRIVESEL    6   /* 驱动器选择 + LBA 位 24-27 */
#define REG_COMMAND     7   /* 写：命令 */
#define REG_STATUS      7   /* 读：状态 */

/* 状态寄存器位 */
#define SR_BSY  0x80   /* 忙，其他位都不可信 */
#define SR_DRDY 0x40   /* 设备就绪 */
#define SR_DF   0x20   /* 设备错误 */
#define SR_DSC  0x10   /* 寻道完成 */
#define SR_DRQ  0x08   /* 数据已就绪，可以读写 */
#define SR_ERR  0x01   /* 上一个命令出错 */

/* 命令 */
#define CMD_READ_PIO    0x20
#define CMD_WRITE_PIO   0x30
#define CMD_CACHE_FLUSH 0xE7
#define CMD_IDENTIFY    0xEC

/* 轮询超时。ATA 操作通常是微秒级，这个数字只是防止设备彻底没响应时死等 */
#define ATA_TIMEOUT 4000000u

/* ------------------------------------------------------------
 * 内部状态
 * ------------------------------------------------------------ */

static ata_device_t devices[ATA_MAX_DRIVES];
static uint32_t     device_count = 0;
static const char*  last_error   = "no error";

static uint32_t stat_reads   = 0;
static uint32_t stat_writes  = 0;

/* 当前操作针对哪个设备（读写接口默认用第一个可用设备） */
static const ata_device_t* active = NULL;

static uint16_t channel_io(uint8_t channel) {
    return (channel == ATA_PRIMARY) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
}

static uint16_t channel_ctrl(uint8_t channel) {
    return (channel == ATA_PRIMARY) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;
}

/* 选完驱动器后要等 400ns 让状态寄存器稳定。
 * 读一次备用状态端口大约 100ns，所以读四次就够了。
 */
static void delay_400ns(uint8_t channel) {
    for (int i = 0; i < 4; i++) {
        inb(channel_ctrl(channel));
    }
}

/* 等待 BSY 清零。成功返回 true */
static bool wait_not_busy(uint8_t channel) {
    uint16_t io = channel_io(channel);

    for (uint32_t i = 0; i < ATA_TIMEOUT; i++) {
        if ((inb(io + REG_STATUS) & SR_BSY) == 0) {
            return true;
        }
    }
    last_error = "timeout waiting for BSY to clear";
    return false;
}

/* 等待 DRQ 置位（数据可以传输了） */
static bool wait_drq(uint8_t channel) {
    uint16_t io = channel_io(channel);

    for (uint32_t i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t status = inb(io + REG_STATUS);

        if (status & SR_BSY) {
            continue;
        }
        if (status & (SR_DF | SR_ERR)) {
            last_error = "device reported an error";
            return false;
        }
        if (status & SR_DRQ) {
            return true;
        }
    }
    last_error = "timeout waiting for DRQ";
    return false;
}

/* 读 16 位数据寄存器 256 次 = 一个扇区 */
static void read_sector_words(uint16_t io, uint16_t* out) {
    for (int i = 0; i < 256; i++) {
        out[i] = inw(io + REG_DATA);
    }
}

static void write_sector_words(uint16_t io, const uint16_t* in) {
    for (int i = 0; i < 256; i++) {
        outw(io + REG_DATA, in[i]);
    }
}

/* ------------------------------------------------------------
 * 把 IDENTIFY 返回的 256 个字里的一段字符字段提取成 C 字符串。
 * ATA 规定这些字段是"字内高低字节交换"的，所以要手动换回来。
 * ------------------------------------------------------------ */
static void extract_string(const uint16_t* id, int first_word, int word_count,
                           char* out, int out_size) {
    int pos = 0;

    for (int w = 0; w < word_count && pos < out_size - 1; w++) {
        uint16_t v = id[first_word + w];
        out[pos++] = (char)(v >> 8);      /* 高字节在前 */
        if (pos < out_size - 1) {
            out[pos++] = (char)(v & 0xFF);
        }
    }
    out[pos] = '\0';

    /* 去掉尾部的空格 */
    while (pos > 0 && out[pos - 1] == ' ') {
        out[--pos] = '\0';
    }
}

/* ------------------------------------------------------------
 * 识别一个设备
 * ------------------------------------------------------------ */
static bool identify_device(uint8_t channel, uint8_t drive, ata_device_t* dev) {
    uint16_t io = channel_io(channel);

    memset(dev, 0, sizeof(*dev));
    dev->channel = channel;
    dev->drive   = drive;

    /* 选择驱动器：0xA0 = 主盘，0xB0 = 从盘 */
    outb(io + REG_DRIVESEL, (uint8_t)(0xA0 | (drive << 4)));
    delay_400ns(channel);

    /* IDENTIFY 之前要把其余寄存器清干净 */
    outb(io + REG_SECCOUNT, 0);
    outb(io + REG_LBA0, 0);
    outb(io + REG_LBA1, 0);
    outb(io + REG_LBA2, 0);

    outb(io + REG_COMMAND, CMD_IDENTIFY);
    delay_400ns(channel);

    /* 状态寄存器读回 0 说明这个位置根本没有设备 */
    uint8_t status = inb(io + REG_STATUS);
    if (status == 0) {
        return false;
    }

    if (!wait_not_busy(channel)) {
        return false;
    }

    /* LBA1/LBA2 非 0 表示这不是 ATA 设备（可能是 ATAPI 光驱） */
    if (inb(io + REG_LBA1) != 0 || inb(io + REG_LBA2) != 0) {
        last_error = "not an ATA device (ATAPI?)";
        return false;
    }

    if (!wait_drq(channel)) {
        return false;
    }

    uint16_t id[256];
    read_sector_words(io, id);

    dev->present       = true;
    dev->signature     = id[0];
    dev->lba_supported = (id[49] & 0x0200) != 0;

    /* 字 60-61 是 LBA28 的扇区总数（低 16 位在前） */
    uint32_t lba28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);

    /* 字 100-103 是 LBA48 的扇区总数；超过 28 位寻址范围时才用得上 */
    uint32_t lba48_low  = (uint32_t)id[100] | ((uint32_t)id[101] << 16);
    uint32_t lba48_high = (uint32_t)id[102] | ((uint32_t)id[103] << 16);

    if (lba28 != 0) {
        dev->sectors = lba28;
    } else if (lba48_high == 0) {
        dev->sectors = lba48_low;
    } else {
        dev->sectors = 0x0FFFFFFF;   /* 超过 128GB，PIO 的 LBA28 寻址用不了 */
    }

    extract_string(id, 27, 20, dev->model, sizeof(dev->model));
    extract_string(id, 10, 10, dev->serial, sizeof(dev->serial));

    return true;
}

uint32_t ata_init(void) {
    device_count = 0;
    active       = NULL;
    last_error   = "no error";

    static const uint8_t channels[2] = { ATA_PRIMARY, ATA_SECONDARY };
    static const uint8_t drives[2]   = { ATA_MASTER, ATA_SLAVE };

    for (uint32_t c = 0; c < 2; c++) {
        for (uint32_t d = 0; d < 2; d++) {
            ata_device_t dev;

            if (identify_device(channels[c], drives[d], &dev)) {
                devices[device_count++] = dev;
            }
        }
    }

    if (device_count > 0) {
        active = &devices[0];
    }
    return device_count;
}

const ata_device_t* ata_get_device(uint32_t index) {
    if (index >= device_count) {
        return NULL;
    }
    return &devices[index];
}

const ata_device_t* ata_primary_device(void) {
    return active;
}

const char* ata_last_error(void) {
    return last_error;
}

/* ------------------------------------------------------------
 * 读写
 * ------------------------------------------------------------ */

/* 把 LBA 命令参数写进寄存器并发命令 */
static bool issue_lba28(const ata_device_t* dev, uint32_t lba, uint8_t count,
                        uint8_t command) {
    uint16_t io = channel_io(dev->channel);

    if (!wait_not_busy(dev->channel)) {
        return false;
    }

    /* 驱动器选择字节里带上 LBA 的最高 4 位 */
    outb(io + REG_DRIVESEL,
         (uint8_t)(0xE0 | (dev->drive << 4) | ((lba >> 24) & 0x0F)));
    delay_400ns(dev->channel);

    outb(io + REG_SECCOUNT, count);
    outb(io + REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(io + REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(io + REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));

    outb(io + REG_COMMAND, command);
    return true;
}

bool ata_read_sectors(uint32_t lba, uint8_t count, void* buffer) {
    const ata_device_t* dev = active;
    uint16_t io;
    uint16_t* out = (uint16_t*)buffer;

    if (!dev || !buffer || count == 0) {
        last_error = "no ATA device or invalid arguments";
        return false;
    }
    if (lba + count > dev->sectors) {
        last_error = "read past the end of the disk";
        return false;
    }

    io = channel_io(dev->channel);

    if (!issue_lba28(dev, lba, count, CMD_READ_PIO)) {
        return false;
    }

    for (uint32_t s = 0; s < count; s++) {
        if (!wait_drq(dev->channel)) {
            return false;
        }
        read_sector_words(io, out + s * 256);
    }

    stat_reads += count;
    return true;
}

bool ata_write_sectors(uint32_t lba, uint8_t count, const void* buffer) {
    const ata_device_t* dev = active;
    uint16_t io;
    const uint16_t* in = (const uint16_t*)buffer;

    if (!dev || !buffer || count == 0) {
        last_error = "no ATA device or invalid arguments";
        return false;
    }
    if (lba + count > dev->sectors) {
        last_error = "write past the end of the disk";
        return false;
    }

    io = channel_io(dev->channel);

    if (!issue_lba28(dev, lba, count, CMD_WRITE_PIO)) {
        return false;
    }

    for (uint32_t s = 0; s < count; s++) {
        if (!wait_drq(dev->channel)) {
            return false;
        }
        write_sector_words(io, in + s * 256);
    }

    /* 等最后一个扇区真正落盘 */
    outb(io + REG_COMMAND, CMD_CACHE_FLUSH);
    if (!wait_not_busy(dev->channel)) {
        return false;
    }

    stat_writes += count;
    return true;
}

uint32_t ata_sectors_read(void)    { return stat_reads; }
uint32_t ata_sectors_written(void) { return stat_writes; }
