#include "../include/hneofs.h"
#include "../include/ata.h"
#include "../include/string.h"

static hneofs_super_t  superblock;
static bool            mounted = false;

/* 文件表常驻内存：8 个扇区 = 4KB = 64 个目录项 */
static hneofs_file_t   file_table[HNEOFS_MAX_FILES];

/* 一次 ATA 读写最多搬多少扇区 */
#define IO_CHUNK 32

/* 槽位是否被占用：名字为空表示这是删除留下的空洞 */
static inline bool slot_used(uint32_t i) {
    return file_table[i].name[0] != '\0';
}

/* ------------------------------------------------------------
 * 底层读写：按扇区为单位搬运，自动分块
 * ------------------------------------------------------------ */
static bool read_sectors(uint32_t lba, uint32_t count, void* buffer) {
    uint8_t* p = (uint8_t*)buffer;

    while (count > 0) {
        uint32_t chunk = (count > IO_CHUNK) ? IO_CHUNK : count;

        if (!ata_read_sectors(lba, (uint8_t)chunk, p)) {
            return false;
        }
        lba   += chunk;
        p     += chunk * HNEOFS_BLOCK_SIZE;
        count -= chunk;
    }
    return true;
}

static bool write_sectors(uint32_t lba, uint32_t count, const void* buffer) {
    const uint8_t* p = (const uint8_t*)buffer;

    while (count > 0) {
        uint32_t chunk = (count > IO_CHUNK) ? IO_CHUNK : count;

        if (!ata_write_sectors(lba, (uint8_t)chunk, p)) {
            return false;
        }
        lba   += chunk;
        p     += chunk * HNEOFS_BLOCK_SIZE;
        count -= chunk;
    }
    return true;
}

/* 按字节偏移写：跨到部分扇区时先读出来改再写回去 */
static bool write_bytes(uint32_t base_lba, uint32_t offset,
                        const void* data, uint32_t size) {
    uint8_t  sector[HNEOFS_BLOCK_SIZE];
    const uint8_t* src = (const uint8_t*)data;
    uint32_t done = 0;

    while (done < size) {
        uint32_t pos       = offset + done;
        uint32_t lba       = base_lba + pos / HNEOFS_BLOCK_SIZE;
        uint32_t in_sector = pos % HNEOFS_BLOCK_SIZE;
        uint32_t chunk     = HNEOFS_BLOCK_SIZE - in_sector;

        if (chunk > size - done) {
            chunk = size - done;
        }

        if (chunk == HNEOFS_BLOCK_SIZE) {
            if (!write_sectors(lba, 1, src + done)) {
                return false;
            }
        } else {
            if (!read_sectors(lba, 1, sector)) {
                return false;
            }
            for (uint32_t i = 0; i < chunk; i++) {
                sector[in_sector + i] = src[done + i];
            }
            if (!write_sectors(lba, 1, sector)) {
                return false;
            }
        }
        done += chunk;
    }
    return true;
}

/* ------------------------------------------------------------
 * 挂载
 * ------------------------------------------------------------ */
bool hneofs_mount(void) {
    uint8_t sector[HNEOFS_BLOCK_SIZE];

    mounted = false;

    if (!read_sectors(HNEOFS_START_LBA, 1, sector)) {
        return false;
    }
    memcpy(&superblock, sector, sizeof(superblock));

    if (superblock.magic != HNEOFS_MAGIC) {
        return false;
    }
    if (superblock.version != HNEOFS_VERSION) {
        return false;
    }

    for (uint32_t i = 0; i < HNEOFS_TABLE_SECTORS; i++) {
        if (!read_sectors(superblock.table_lba + i, 1, sector)) {
            return false;
        }
        memcpy((uint8_t*)file_table + i * HNEOFS_BLOCK_SIZE,
               sector, HNEOFS_BLOCK_SIZE);
    }

    if (superblock.file_count > HNEOFS_MAX_FILES) {
        superblock.file_count = HNEOFS_MAX_FILES;
    }

    mounted = true;
    return true;
}

bool hneofs_mounted(void) { return mounted; }

const hneofs_super_t* hneofs_super(void) {
    return mounted ? &superblock : NULL;
}

uint32_t hneofs_count(void) {
    return mounted ? superblock.file_count : 0;
}

const hneofs_file_t* hneofs_file(uint32_t index) {
    if (!mounted || index >= superblock.file_count) {
        return NULL;
    }
    if (!slot_used(index)) {
        return NULL;   /* 删除留下的空洞 */
    }
    return &file_table[index];
}

hneofs_file_t* hneofs_file_mut(uint32_t index) {
    if (!mounted || index >= superblock.file_count) {
        return NULL;
    }
    if (!slot_used(index)) {
        return NULL;
    }
    return &file_table[index];
}

/* ------------------------------------------------------------
 * 目录遍历
 * ------------------------------------------------------------ */
int32_t hneofs_find_child(uint32_t dir_index, const char* name) {
    if (!mounted || !name) {
        return -1;
    }

    for (uint32_t i = 0; i < superblock.file_count; i++) {
        if (!slot_used(i)) {
            continue;
        }
        if (file_table[i].parent != dir_index) {
            continue;
        }
        if (strcmp(file_table[i].name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t hneofs_child_at(uint32_t dir_index, uint32_t n) {
    uint32_t seen = 0;

    if (!mounted) {
        return -1;
    }

    for (uint32_t i = 0; i < superblock.file_count; i++) {
        if (!slot_used(i) || file_table[i].parent != dir_index) {
            continue;
        }
        if (seen == n) {
            return (int32_t)i;
        }
        seen++;
    }
    return -1;
}

uint32_t hneofs_child_count(uint32_t dir_index) {
    uint32_t n = 0;

    if (!mounted) {
        return 0;
    }

    for (uint32_t i = 0; i < superblock.file_count; i++) {
        if (slot_used(i) && file_table[i].parent == dir_index) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------
 * 路径解析
 *
 * 从根目录开始，一段一段往下比对名字。允许前导 '/'，也允许末尾的 '/'。
 * 相对路径同样从根开始（内核 Shell 没有工作目录的概念）。
 * ------------------------------------------------------------ */
int32_t hneofs_resolve(const char* path) {
    uint32_t parent = HNEOFS_ROOT;
    const char* p = path;

    if (!mounted || !path) {
        return -1;
    }

    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        return -1;   /* 根目录本身没有目录项 */
    }

    for (;;) {
        char comp[HNEOFS_NAME_MAX];
        int n = 0;
        int32_t idx;

        while (*p && *p != '/') {
            if (n >= HNEOFS_NAME_MAX - 1) {
                return -1;   /* 单段太长 */
            }
            comp[n++] = *p++;
        }
        comp[n] = '\0';

        while (*p == '/') {
            p++;
        }

        if (n == 0) {
            return -1;   /* 出现了空分量，比如 "a//b" */
        }

        idx = hneofs_find_child(parent, comp);
        if (idx < 0) {
            return -1;
        }

        if (*p == '\0') {
            return idx;   /* 最后一段，找到了 */
        }

        /* 还有下一段，那这一段必须是目录 */
        if (file_table[idx].type != HNEOFS_TYPE_DIR) {
            return -1;
        }
        parent = (uint32_t)idx;
    }
}

const hneofs_file_t* hneofs_lookup(const char* path) {
    int32_t idx = hneofs_resolve(path);

    return (idx < 0) ? NULL : &file_table[idx];
}

/* 把路径拆成"父目录下标 + 最后一段名字"。
 * 创建、删除这类操作都要先拆，因为父目录本身必须已经存在。
 */
static int32_t split_parent(const char* path, uint32_t* parent, char* name) {
    const char* last = NULL;
    const char* p;

    if (!mounted) {
        return HNEOFS_ERR_MOUNT;
    }
    if (!path || path[0] == '\0') {
        return HNEOFS_ERR_NAME;
    }

    for (p = path; *p; p++) {
        if (*p == '/') {
            last = p;
        }
    }

    if (!last) {
        /* 没有斜杠：直接建在根目录下 */
        *parent = HNEOFS_ROOT;
        if (!hneofs_name_ok(path)) {
            return HNEOFS_ERR_NAME;
        }
        strncpy(name, path, HNEOFS_NAME_MAX);
        return 0;
    }

    /* 斜杠之前是父目录路径 */
    {
        char dir[HNEOFS_PATH_MAX];
        uint32_t len = (uint32_t)(last - path);
        int32_t idx;

        if (len >= sizeof(dir)) {
            return HNEOFS_ERR_NAME;
        }
        memcpy(dir, path, len);
        dir[len] = '\0';

        if (len == 0) {
            *parent = HNEOFS_ROOT;      /* 形如 "/name" */
        } else {
            idx = hneofs_resolve(dir);
            if (idx < 0) {
                return HNEOFS_ERR_NOENT;
            }
            if (file_table[idx].type != HNEOFS_TYPE_DIR) {
                return HNEOFS_ERR_NOTDIR;
            }
            *parent = (uint32_t)idx;
        }
    }

    if (!hneofs_name_ok(last + 1)) {
        return HNEOFS_ERR_NAME;
    }
    strncpy(name, last + 1, HNEOFS_NAME_MAX);
    return 0;
}

/* ------------------------------------------------------------
 * 读取
 * ------------------------------------------------------------ */
int32_t hneofs_read_at(const hneofs_file_t* f, uint32_t offset,
                       void* buffer, uint32_t size) {
    uint8_t  sector[HNEOFS_BLOCK_SIZE];
    uint8_t* out = (uint8_t*)buffer;
    uint32_t done = 0;

    if (!f || !buffer) {
        return -1;
    }
    if (offset >= f->size) {
        return 0;
    }
    if (offset + size > f->size) {
        size = f->size - offset;
    }

    while (done < size) {
        uint32_t pos       = offset + done;
        uint32_t lba       = f->start_lba + (pos / HNEOFS_BLOCK_SIZE);
        uint32_t in_sector = pos % HNEOFS_BLOCK_SIZE;
        uint32_t chunk     = HNEOFS_BLOCK_SIZE - in_sector;

        if (chunk > size - done) {
            chunk = size - done;
        }
        if (!read_sectors(lba, 1, sector)) {
            return -1;
        }
        memcpy(out + done, sector + in_sector, chunk);
        done += chunk;
    }

    return (int32_t)done;
}

int32_t hneofs_read_file(const hneofs_file_t* f, void* buffer, uint32_t buffer_size) {
    if (!f) {
        return -1;
    }
    if (f->size > buffer_size) {
        return -1;
    }
    return hneofs_read_at(f, 0, buffer, f->size);
}

/* ------------------------------------------------------------
 * 空闲空间：从现有文件的区间推导，不单独维护位图
 * ------------------------------------------------------------ */

/* 一个文件占多少扇区（空文件也占 1 个，方便定位） */
static uint32_t file_sectors(const hneofs_file_t* f) {
    uint32_t n = (f->size + HNEOFS_BLOCK_SIZE - 1) / HNEOFS_BLOCK_SIZE;

    return (n == 0) ? 1 : n;
}

static uint32_t data_end_lba(void) {
    return HNEOFS_START_LBA + superblock.total_blocks;
}

/* 找一段能放下 need 个扇区的连续空间，返回起始 LBA；找不到返回 0 */
static uint32_t alloc_extent(uint32_t need) {
    uint32_t start[HNEOFS_MAX_FILES];
    uint32_t len[HNEOFS_MAX_FILES];
    uint32_t count = 0;

    if (need == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < superblock.file_count; i++) {
        if (!slot_used(i)) {
            continue;
        }
        /* 目录不占数据区 */
        if (file_table[i].type != HNEOFS_TYPE_FILE) {
            continue;
        }
        start[count] = file_table[i].start_lba;
        len[count]   = file_sectors(&file_table[i]);
        count++;
    }

    /* 按起点插入排序（项数最多 64，直接排） */
    for (uint32_t i = 1; i < count; i++) {
        uint32_t s = start[i];
        uint32_t l = len[i];
        uint32_t j = i;

        while (j > 0 && start[j - 1] > s) {
            start[j] = start[j - 1];
            len[j]   = len[j - 1];
            j--;
        }
        start[j] = s;
        len[j]   = l;
    }

    {
        uint32_t cursor = superblock.data_lba;

        for (uint32_t i = 0; i < count; i++) {
            if (start[i] + len[i] <= cursor) {
                continue;                  /* 已被前一个区间盖住 */
            }
            if (start[i] > cursor && start[i] - cursor >= need) {
                return cursor;             /* 空隙够大 */
            }
            cursor = start[i] + len[i];
        }

        if (data_end_lba() >= cursor && data_end_lba() - cursor >= need) {
            return cursor;                 /* 尾部剩下的空间 */
        }
    }

    return 0;
}

uint32_t hneofs_free_bytes(void) {
    uint32_t used = 0;

    if (!mounted) {
        return 0;
    }

    for (uint32_t i = 0; i < superblock.file_count; i++) {
        if (slot_used(i) && file_table[i].type == HNEOFS_TYPE_FILE) {
            used += file_sectors(&file_table[i]);
        }
    }

    {
        uint32_t data_sectors = (data_end_lba() > superblock.data_lba)
                              ? (data_end_lba() - superblock.data_lba) : 0;

        if (used >= data_sectors) {
            return 0;
        }
        return (data_sectors - used) * HNEOFS_BLOCK_SIZE;
    }
}

/* ------------------------------------------------------------
 * 目录项维护
 * ------------------------------------------------------------ */
bool hneofs_sync(void) {
    /* 4KB 的局部数组会占掉半个内核栈（任务栈才 8KB），而且 GCC 在 PE
     * 目标上会给超过 4KB 的栈帧插一条 __chkstk_ms 调用，裸机链接时
     * 根本没有这个函数。放成 static，两个问题一起解决。
     */
    static uint8_t table_bytes[HNEOFS_TABLE_SECTORS * HNEOFS_BLOCK_SIZE];

    if (!mounted) {
        return false;
    }

    superblock.free_lba = superblock.data_lba;

    memcpy(table_bytes, file_table, sizeof(table_bytes));

    if (!write_sectors(superblock.table_lba, HNEOFS_TABLE_SECTORS, table_bytes)) {
        return false;
    }
    if (!write_sectors(HNEOFS_START_LBA, 1, &superblock)) {
        return false;
    }
    return true;
}

bool hneofs_name_ok(const char* name) {
    uint32_t n = 0;

    if (!name || name[0] == '\0') {
        return false;
    }
    while (name[n]) {
        if (name[n] == '/') {
            return false;   /* 单个分量里不允许出现分隔符 */
        }
        n++;
        if (n >= HNEOFS_NAME_MAX) {
            return false;
        }
    }
    return true;
}

/* 找一个空槽；没有就返回 -1 */
static int32_t find_free_slot(void) {
    for (uint32_t i = 0; i < superblock.file_count; i++) {
        if (!slot_used(i)) {
            return (int32_t)i;   /* 复用删除留下的空洞 */
        }
    }
    if (superblock.file_count < HNEOFS_MAX_FILES) {
        return (int32_t)superblock.file_count;
    }
    return -1;
}

/* 在指定目录下建一个条目 */
static int32_t create_entry(uint32_t parent, const char* name, uint32_t type) {
    int32_t slot;
    uint32_t lba = 0;
    hneofs_file_t* f;

    if (!mounted) {
        return HNEOFS_ERR_MOUNT;
    }
    if (!hneofs_name_ok(name)) {
        return HNEOFS_ERR_NAME;
    }
    if (hneofs_find_child(parent, name) >= 0) {
        return HNEOFS_ERR_EXIST;
    }

    slot = find_free_slot();
    if (slot < 0) {
        return HNEOFS_ERR_FULL;
    }

    /* 只有普通文件才需要数据区；目录本身不占空间，子项靠 parent 串起来 */
    if (type == HNEOFS_TYPE_FILE) {
        lba = alloc_extent(1);
        if (lba == 0) {
            return HNEOFS_ERR_NOSPC;
        }
    }

    f = &file_table[slot];
    memset(f, 0, sizeof(*f));
    strncpy(f->name, name, HNEOFS_NAME_MAX);
    f->start_lba = lba;
    f->size      = 0;
    f->type      = type;
    f->parent    = parent;

    if ((uint32_t)slot >= superblock.file_count) {
        superblock.file_count = (uint32_t)slot + 1;   /* 抬高水位 */
    }

    if (!hneofs_sync()) {
        return HNEOFS_ERR_IO;
    }
    return slot;
}

int32_t hneofs_create(const char* path) {
    uint32_t parent;
    char name[HNEOFS_NAME_MAX];
    int32_t rc = split_parent(path, &parent, name);

    if (rc < 0) {
        return rc;
    }
    return create_entry(parent, name, HNEOFS_TYPE_FILE);
}

int32_t hneofs_mkdir(const char* path) {
    uint32_t parent;
    char name[HNEOFS_NAME_MAX];
    int32_t rc = split_parent(path, &parent, name);

    if (rc < 0) {
        return rc;
    }
    return create_entry(parent, name, HNEOFS_TYPE_DIR);
}

bool hneofs_unlink(const char* path) {
    int32_t idx = hneofs_resolve(path);
    hneofs_file_t* f;

    if (idx < 0) {
        return false;
    }

    f = &file_table[idx];

    /* 目录必须空着才能删 */
    if (f->type == HNEOFS_TYPE_DIR && hneofs_child_count((uint32_t)idx) > 0) {
        return false;
    }

    /* 关键：只在原地清空，绝不搬动其它目录项。
     * 一搬动所有下标都会变，子目录的 parent 链接就全废了。
     * 代价是表里留空洞，但 find_free_slot 和 alloc_extent 都会跳过它们。
     */
    memset(f, 0, sizeof(*f));
    return hneofs_sync();
}

/* ------------------------------------------------------------
 * 写入
 * ------------------------------------------------------------ */

/* 把文件扩展到至少 need_sectors 个扇区，必要时搬到新位置 */
static bool ensure_capacity(uint32_t index, uint32_t need_sectors) {
    hneofs_file_t* f = &file_table[index];
    uint32_t have = file_sectors(f);
    uint32_t new_lba;
    uint8_t  buffer[HNEOFS_BLOCK_SIZE];

    if (need_sectors <= have) {
        return true;
    }

    new_lba = alloc_extent(need_sectors);
    if (new_lba == 0) {
        return false;
    }

    /* 老内容搬到新位置：一扇区一扇区搬，不需要整个文件的缓冲区 */
    {
        uint32_t done = 0;

        while (done < f->size) {
            if (!read_sectors(f->start_lba + done / HNEOFS_BLOCK_SIZE, 1, buffer)) {
                return false;
            }
            if (!write_sectors(new_lba + done / HNEOFS_BLOCK_SIZE, 1, buffer)) {
                return false;
            }
            done += HNEOFS_BLOCK_SIZE;
        }
    }

    f->start_lba = new_lba;
    return true;
}

int32_t hneofs_write_at(uint32_t index, uint32_t offset,
                        const void* data, uint32_t size) {
    hneofs_file_t* f;
    uint32_t need_sectors;
    uint32_t new_size;

    if (!mounted) {
        return HNEOFS_ERR_MOUNT;
    }
    if (index >= superblock.file_count || !slot_used(index) || !data) {
        return HNEOFS_ERR_NOENT;
    }
    if (file_table[index].type != HNEOFS_TYPE_FILE) {
        return HNEOFS_ERR_ISDIR;
    }
    if (size == 0) {
        return 0;
    }

    f = &file_table[index];
    new_size = offset + size;
    need_sectors = (new_size + HNEOFS_BLOCK_SIZE - 1) / HNEOFS_BLOCK_SIZE;
    if (need_sectors == 0) {
        need_sectors = 1;
    }

    if (!ensure_capacity(index, need_sectors)) {
        return HNEOFS_ERR_NOSPC;
    }

    if (!write_bytes(f->start_lba, offset, data, size)) {
        return HNEOFS_ERR_IO;
    }

    if (new_size > f->size) {
        f->size = new_size;
    }

    if (!hneofs_sync()) {
        return HNEOFS_ERR_IO;
    }
    return (int32_t)size;
}

bool hneofs_truncate(uint32_t index, uint32_t new_size) {
    hneofs_file_t* f;

    if (!mounted || index >= superblock.file_count || !slot_used(index)) {
        return false;
    }

    f = &file_table[index];
    if (f->type != HNEOFS_TYPE_FILE) {
        return false;
    }

    if (new_size > f->size) {
        uint32_t need = (new_size + HNEOFS_BLOCK_SIZE - 1) / HNEOFS_BLOCK_SIZE;

        if (!ensure_capacity(index, need == 0 ? 1 : need)) {
            return false;
        }
    }

    f->size = new_size;
    return hneofs_sync();
}

int32_t hneofs_write_file(const char* path, const void* data, uint32_t size) {
    int32_t index;
    int32_t rc;

    if (!mounted) {
        return HNEOFS_ERR_MOUNT;
    }

    index = hneofs_resolve(path);
    if (index < 0) {
        index = hneofs_create(path);
        if (index < 0) {
            return index;
        }
    } else if (file_table[index].type != HNEOFS_TYPE_FILE) {
        return HNEOFS_ERR_ISDIR;
    }

    rc = hneofs_write_at((uint32_t)index, 0, data, size);
    if (rc < 0) {
        return rc;
    }

    /* 写短了就把尾巴截掉 */
    hneofs_truncate((uint32_t)index, size);
    return (int32_t)size;
}
