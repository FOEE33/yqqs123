// seedump_dylib.c  挂进游戏进程内部，用游戏自己的接口做三件事
//   1) 种子：每 3 秒检查，不足就补满（无限种子，含未获得的）
//   2) 蓝图：对 ItemInfo.Blueprints 逐条 GetBlueprint + ResearchBlueprint（真正解锁）
//   3) 锻造：从 blueprint_weapon_* / blueprint_evolution_weapon_* 切出武器名，
//            调 UIForge.ForceUnlockForgeByAddObtainTimes（解锁锻造）
// 全程在进程内、都在 Unity 主线程上跑，不需要 task_for_pid / root / 终端。
// 所有内存读取走 vm_read_overwrite（对自己 task），读失败返回错误而不是崩溃。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach-o/dyld.h>
#include <dispatch/dispatch.h>

#define RVA_SLOT_ITEMINFO 0x12088D60ULL  // ItemInfo 的 metadata usage 槽
#define OFF_CLASS_STATICFLD 0xB8ULL
#define OFF_SEEDS          0x18ULL
#define OFF_BLUEPRINTS     0x10ULL
#define OFF_LIST_ITEMS     0x10ULL
#define OFF_LIST_SIZE      0x18ULL
#define OFF_ARRAY_DATA     0x20ULL
#define OFF_ROW_NAME       0x10ULL
#define OFF_STR_LEN        0x10ULL
#define OFF_STR_CHARS      0x14ULL

#define RVA_GET_DATA     0x6A67A94ULL   // ItemData.get_data()
#define RVA_ADD_SEED     0x6A6A394ULL   // ItemData.AddSeed(string,int,bool)
#define RVA_GET_SEEDCNT  0x6A6F1E0ULL   // ItemData.GetSeedCount(string)
#define RVA_GET_BP       0x6A6A568ULL   // ItemData.GetBlueprint(string,bool,bool)
#define RVA_RESEARCH_BP  0x6A6F8FCULL   // ItemData.ResearchBlueprint(string,bool,bool,bool)
#define RVA_FORCE_FORGE  0x32FECB0ULL   // UIForge.ForceUnlockForgeByAddObtainTimes(string) static
#define RVA_SUBSTRING    0xC73D1C4ULL   // String.Substring(int)

#define SEED_VAL        2011999999
#define LOW_WATER       1000
#define REFILL_SEC      3
#define UNLOCK_DELAY    8               // 道具表就绪后再等几秒才开始解锁

#define MAXROWS 4096

static uint64_t g_base = 0;
static char     g_dir[1024] = {0};
static uint64_t g_seed[MAXROWS];  static int g_nseed = 0;
static uint64_t g_bp[MAXROWS];    static int g_nbp = 0;

static int rd(uint64_t a, void *b, size_t n) {
    vm_size_t out = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)a, (vm_size_t)n,
                          (vm_address_t)(uintptr_t)b, &out) != KERN_SUCCESS) return -1;
    return (out == n) ? 0 : -1;
}
static uint64_t rd64(uint64_t a){ uint64_t v=0; if(rd(a,&v,8)) return 0; return v; }
static uint32_t rd32(uint64_t a){ uint32_t v=0; if(rd(a,&v,4)) return 0; return v; }
static uint16_t rd16(uint64_t a){ uint16_t v=0; if(rd(a,&v,2)) return 0; return v; }

static void note(const char *msg) {
    char p[1200]; FILE *f;
    if (!g_dir[0]) return;
    snprintf(p, sizeof(p), "%s/Documents/seedump.log", g_dir);
    f = fopen(p, "a"); if (!f) return;
    fprintf(f, "%s\n", msg); fclose(f);
}
static void note2(const char *fmt, long a) {
    char m[240]; snprintf(m, sizeof(m), fmt, a); note(m);
}

static int cs_len(uint64_t sp){ return (int)(int32_t)rd32(sp + OFF_STR_LEN); }
static int cs_utf8(uint64_t sp, char *out, size_t max) {
    int len, i; size_t o = 0;
    out[0] = 0; if (!sp) return -1;
    len = cs_len(sp); if (len <= 0) return 0; if (len > 512) return -1;
    for (i = 0; i < len && o + 4 < max; i++) {
        uint16_t c = rd16(sp + OFF_STR_CHARS + (uint64_t)i * 2);
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0|(c>>6)); out[o++] = (char)(0x80|(c&0x3F)); }
        else { out[o++] = (char)(0xE0|(c>>12)); out[o++] = (char)(0x80|((c>>6)&0x3F)); out[o++] = (char)(0x80|(c&0x3F)); }
    }
    out[o] = 0; return (int)o;
}
// 判断托管字符串是否以某个 ASCII 前缀开头
static int cs_starts(uint64_t sp, const char *pre) {
    int i, n = (int)strlen(pre);
    if (!sp || cs_len(sp) < n) return 0;
    for (i = 0; i < n; i++)
        if (rd16(sp + OFF_STR_CHARS + (uint64_t)i * 2) != (uint16_t)(unsigned char)pre[i]) return 0;
    return 1;
}

static void find_base(void) {
    uint32_t i, n = _dyld_image_count();
    for (i = 0; i < n; i++) {
        const char *nm = _dyld_get_image_name(i);
        if (nm && strstr(nm, "UnityFramework")) { g_base = (uint64_t)(uintptr_t)_dyld_get_image_header(i); return; }
    }
}

static int collect_rows(uint64_t listPtr, uint64_t *out, int max) {
    int32_t n, i; uint64_t items; int k = 0;
    if (listPtr < 0x100000000ULL) return 0;
    n = (int32_t)rd32(listPtr + OFF_LIST_SIZE);
    items = rd64(listPtr + OFF_LIST_ITEMS);
    if (n <= 0 || n > max || items < 0x100000000ULL) return 0;
    for (i = 0; i < n; i++) {
        uint64_t row = rd64(items + OFF_ARRAY_DATA + (uint64_t)i * 8);
        uint64_t sp;
        if (row < 0x100000000ULL) continue;
        sp = rd64(row + OFF_ROW_NAME);
        if (sp < 0x100000000ULL) continue;
        out[k++] = sp;
    }
    return k;
}

// 返回 1 = 道具表就绪
static int snapshot(void) {
    uint64_t klass, sf, info;
    if (!g_base) find_base();
    if (!g_base) return 0;
    klass = rd64(g_base + RVA_SLOT_ITEMINFO);
    if (klass < 0x100000000ULL || klass > 0x0000010000000000ULL) return 0;
    sf = rd64(klass + OFF_CLASS_STATICFLD);
    if (sf < 0x100000000ULL) return 0;
    info = rd64(sf);
    if (info < 0x100000000ULL) return 0;
    g_nseed = collect_rows(rd64(info + OFF_SEEDS), g_seed, MAXROWS);
    g_nbp   = collect_rows(rd64(info + OFF_BLUEPRINTS), g_bp, MAXROWS);
    return (g_nseed > 0) ? 1 : 0;
}

// ============ 主线程：干正事 ============
static void work_on_main(void *ctx) {
    void   *(*get_data)(void) = (void *(*)(void))(uintptr_t)(g_base + RVA_GET_DATA);
    int32_t (*get_cnt)(void *, void *) = (int32_t (*)(void *, void *))(uintptr_t)(g_base + RVA_GET_SEEDCNT);
    void    (*add_seed)(void *, void *, int32_t, int32_t) = (void (*)(void *, void *, int32_t, int32_t))(uintptr_t)(g_base + RVA_ADD_SEED);
    void    (*get_bp)(void *, void *, int32_t, int32_t) = (void (*)(void *, void *, int32_t, int32_t))(uintptr_t)(g_base + RVA_GET_BP);
    void    (*research_bp)(void *, void *, int32_t, int32_t, int32_t) = (void (*)(void *, void *, int32_t, int32_t, int32_t))(uintptr_t)(g_base + RVA_RESEARCH_BP);
    void    (*force_forge)(void *) = (void (*)(void *))(uintptr_t)(g_base + RVA_FORCE_FORGE);
    void   *(*substr)(void *, int32_t) = (void *(*)(void *, int32_t))(uintptr_t)(g_base + RVA_SUBSTRING);

    static int done_unlock = 0;
    void *item;
    char msg[300], buf[200];
    int i, need = 0, fixed = 0;

    item = get_data();
    if (!item) { note("work: ItemData.get_data() 返回空"); return; }

    // ---- 1. 种子：摸底 + 补满 ----
    for (i = 0; i < g_nseed; i++)
        if (get_cnt(item, (void *)(uintptr_t)g_seed[i]) < LOW_WATER) need++;
    snprintf(msg, sizeof(msg), "种子：共 %d 个，不足 %d 的有 %d 个；样本[0]=%d",
             g_nseed, LOW_WATER, need, (int)get_cnt(item, (void *)(uintptr_t)g_seed[0]));
    note(msg);
    for (i = 0; i < g_nseed; i++) {
        void *nm = (void *)(uintptr_t)g_seed[i];
        if (get_cnt(item, nm) < LOW_WATER) { add_seed(item, nm, SEED_VAL, 1); fixed++; }
    }
    if (fixed) note2("  -> 种子补满 %ld 个", (long)fixed);

    // ---- 2/3. 蓝图 + 锻造：只做一次 ----
    if (done_unlock || g_nbp <= 0) return;
    done_unlock = 1;
    note2("开始解锁蓝图，共 %ld 条", (long)g_nbp);

    {
        int nbp = 0, nforge = 0;
        for (i = 0; i < g_nbp; i++) {
            void *bp = (void *)(uintptr_t)g_bp[i];
            get_bp(item, bp, 0, 1);            // 获得蓝图
            research_bp(item, bp, 0, 1, 0);    // 研究蓝图（needRecord=false, save=true）
            nbp++;
        }
        note2("蓝图完成：%ld 条", (long)nbp);

        for (i = 0; i < g_nbp; i++) {
            uint64_t bp = g_bp[i];
            void *wn = NULL;
            if (cs_starts(bp, "blueprint_weapon_"))              wn = substr((void *)(uintptr_t)bp, 10);
            else if (cs_starts(bp, "blueprint_evolution_weapon_")) wn = substr((void *)(uintptr_t)bp, 20);
            if (!wn) continue;
            if (cs_utf8((uint64_t)(uintptr_t)wn, buf, sizeof(buf)) > 0) {
                force_forge(wn);
                nforge++;
            }
        }
        note2("锻造解锁完成：%ld 把", (long)nforge);
    }
    note("解冻流程结束");
}

static void *worker(void *arg) {
    int i;
    (void)arg;
    note("--- seedump 已加载，等待道具表 ---");
    for (i = 0; i < 3600; i++) {
        if (snapshot() && g_nbp > 0) break;
        if (i % 30 == 0) note2("等待道具表，第 %ld 次", (long)(i + 1));
        sleep(1);
    }
    if (g_nseed <= 0) { note("超时：一直没等到道具表"); return NULL; }
    note2("道具表就绪：Seeds %ld 条", (long)g_nseed);
    note2("                Blueprints %ld 条", (long)g_nbp);

    sleep(UNLOCK_DELAY);                       // 等游戏完全加载完再动手
    dispatch_async_f(dispatch_get_main_queue(), NULL, work_on_main);

    for (;;) {                                 // 之后只维护种子
        sleep(REFILL_SEC);
        dispatch_async_f(dispatch_get_main_queue(), NULL, work_on_main);
    }
    return NULL;
}

__attribute__((constructor))
static void seedump_init(void) {
    const char *home = getenv("HOME");
    pthread_t t;
    snprintf(g_dir, sizeof(g_dir), "%s", home ? home : "/var/mobile");
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
