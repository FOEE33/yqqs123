// seedump_dylib.c  挂进游戏进程内部的读取器
//
// 用 LC_LOAD_DYLIB 被 UnityFramework 加载，构造函数起一个后台线程，
// 每秒轮询一次，等到 ItemInfo 单例 + 种子表都就绪，就把全表写进
// app 自己的 Documents 目录，然后线程退出。
//
// 全程在进程内，不需要 task_for_pid / root / 终端。
// 所有内存读取都走 vm_read_overwrite（对自身 task），读失败返回错误而不是崩溃。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach-o/dyld.h>

#define RVA_SLOT_ITEMINFO    0x12088D60ULL  // ItemInfo metadata usage 槽（运行时存 Il2CppClass*）
#define OFF_CLASS_STATICFLD  0xB8ULL        // Il2CppClass.static_fields
#define OFF_SEEDS            0x18ULL        // ItemInfo.Seeds
#define OFF_BLUEPRINTS       0x10ULL        // ItemInfo.Blueprints
#define OFF_LIST_ITEMS       0x10ULL        // List<T>._items
#define OFF_LIST_SIZE        0x18ULL        // List<T>._size
#define OFF_ARRAY_DATA       0x20ULL        // 数组首元素
#define OFF_ROW_NAME         0x10ULL        // ItemInfoRow.Name
#define OFF_STR_LEN          0x10ULL
#define OFF_STR_CHARS        0x14ULL

#define MAXNAMES 4096
#define POLL_MAX 3600                       // 最多轮询 1 小时

static uint64_t g_base = 0;
static char     g_dir[1024] = {0};

static int rd(uint64_t addr, void *buf, size_t n) {
    vm_size_t out = 0;
    kern_return_t kr = vm_read_overwrite(mach_task_self(),
                                         (vm_address_t)addr,
                                         (vm_size_t)n,
                                         (vm_address_t)(uintptr_t)buf,
                                         &out);
    return (kr == KERN_SUCCESS && out == n) ? 0 : -1;
}
static uint64_t rd64(uint64_t a) { uint64_t v = 0; if (rd(a, &v, 8)) return 0; return v; }
static uint32_t rd32(uint64_t a) { uint32_t v = 0; if (rd(a, &v, 4)) return 0; return v; }

static void note(const char *msg) {
    char p[1200];
    FILE *f;
    if (!g_dir[0]) return;
    snprintf(p, sizeof(p), "%s/Documents/seedump.log", g_dir);
    f = fopen(p, "a");
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fclose(f);
}

static int read_cs_string(uint64_t sp, char *out, size_t max) {
    int32_t len;
    size_t o = 0;
    int32_t i;
    out[0] = 0;
    if (!sp) return -1;
    len = (int32_t)rd32(sp + OFF_STR_LEN);
    if (len <= 0) return 0;
    if (len > 512) return -1;
    for (i = 0; i < len; i++) {
        uint16_t c = 0;
        if (rd(sp + OFF_STR_CHARS + (uint64_t)i * 2, &c, 2)) return -1;
        if (c < 0x80) {
            if (o + 1 >= max) break;
            out[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= max) break;
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= max) break;
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    return (int)o;
}

static void find_base(void) {
    uint32_t i, n = _dyld_image_count();
    for (i = 0; i < n; i++) {
        const char *nm = _dyld_get_image_name(i);
        if (nm && strstr(nm, "UnityFramework")) {
            g_base = (uint64_t)(uintptr_t)_dyld_get_image_header(i);
            return;
        }
    }
}

static void dump_list(FILE *fp, const char *tag, uint64_t listPtr, char names[][128], int *pn) {
    int32_t size, i;
    uint64_t items;
    *pn = 0;
    if (!listPtr) { fprintf(fp, "# %s <空>\n\n", tag); return; }
    size = (int32_t)rd32(listPtr + OFF_LIST_SIZE);
    items = rd64(listPtr + OFF_LIST_ITEMS);
    if (size <= 0 || size > MAXNAMES || !items) {
        fprintf(fp, "# %s 异常 size=%d items=%llu\n\n", tag, size, (unsigned long long)items);
        return;
    }
    fprintf(fp, "# %s  %d 条\n", tag, size);
    for (i = 0; i < size; i++) {
        uint64_t row = rd64(items + OFF_ARRAY_DATA + (uint64_t)i * 8);
        char nm[128];
        if (!row) continue;
        if (read_cs_string(rd64(row + OFF_ROW_NAME), nm, sizeof(nm)) <= 0) continue;
        fprintf(fp, "%s\n", nm);
        strncpy(names[*pn], nm, 127);
        names[*pn][127] = 0;
        (*pn)++;
        if (*pn >= MAXNAMES) break;
    }
    fprintf(fp, "\n");
}

static char g_seeds[MAXNAMES][128];
static char g_bps[MAXNAMES][128];

// 返回 1 = 成功写完
static int try_dump(void) {
    uint64_t klass, sf, info, listPtr;
    char p[1200];
    FILE *fp;
    int ns = 0, nb = 0;

    if (!g_base) find_base();
    if (!g_base) return 0;

    klass = rd64(g_base + RVA_SLOT_ITEMINFO);
    // 还没被 IL2CPP 解析时槽里是 token（小数值），不能当指针用
    if (klass < 0x100000000ULL || klass > 0x0000010000000000ULL) return 0;
    sf = rd64(klass + OFF_CLASS_STATICFLD);
    if (sf < 0x100000000ULL) return 0;
    info = rd64(sf);
    if (info < 0x100000000ULL) return 0;

    listPtr = rd64(info + OFF_SEEDS);
    if (listPtr < 0x100000000ULL) return 0;

    snprintf(p, sizeof(p), "%s/Documents/seed_dump.txt", g_dir);
    fp = fopen(p, "w");
    if (!fp) { note("打不开 seed_dump.txt"); return 0; }
    fprintf(fp, "# seedump  从进程内读取\n# UnityFramework base = 0x%llx   ItemInfo = 0x%llx\n\n",
            (unsigned long long)g_base, (unsigned long long)info);
    dump_list(fp, "Seeds", listPtr, g_seeds, &ns);
    dump_list(fp, "Blueprints", rd64(info + OFF_BLUEPRINTS), g_bps, &nb);
    fclose(fp);

    snprintf(p, sizeof(p), "成功：Seeds %d 条，Blueprints %d 条 -> Documents/seed_dump.txt", ns, nb);
    note(p);
    return (ns > 0) ? 1 : 0;
}

static void *worker(void *arg) {
    int i, logged = 0;
    (void)arg;
    note("--- seedump 已加载，开始轮询 ---");
    for (i = 0; i < POLL_MAX; i++) {
        char p[1200];
        if (try_dump()) return NULL;
        if (!logged || i % 30 == 29) {
            snprintf(p, sizeof(p), "第 %d 次：ItemInfo/种子表还没就绪（base=0x%llx）",
                     i + 1, (unsigned long long)g_base);
            note(p);
            logged = 1;
        }
        sleep(1);
    }
    note("超时放弃（一直没等到种子表）");
    return NULL;
}

__attribute__((constructor))
static void seedump_init(void) {
    const char *home = getenv("HOME");
    pthread_t t;
    snprintf(g_dir, sizeof(g_dir), "%s", home ? home : "/var/mobile");
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
