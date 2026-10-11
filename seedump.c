// seedump.c  独立 iOS 命令行工具（DumpDecrypter 路子）
//
// 用 task_for_pid 附加到运行中的元气骑士，直接读内存里的 ItemInfo.Seeds / Blueprints，
// 把全部种子名 / 蓝图名写成文本文件。不改游戏包、不 hook、不需要 Frida。
//
// 用法:  sudo ./seedump [输出路径]        默认 /var/mobile/seed_dump.txt
//        sudo ./seedump -p 1234 out.txt   指定 pid
//
// 偏移来源: 8.6.0 (1519) 的 UnityFramework 反汇编 + Il2CppDumper 符号表
//   ItemInfo.get_Info():
//     adrp x19,#0x12088000 / add #0xd60 ; ldr x0,[x19]      -> Il2CppClass*
//     ldr  x8,[x0,#0xb8]                 ; ldr x0,[x8]      -> static_fields[0] = ItemInfo 单例

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/task_info.h>
#include <mach-o/dyld_images.h>

#define RVA_SLOT_ITEMINFO    0x12088D60ULL  // ItemInfo 的 metadata usage 槽（运行时存 Il2CppClass*）
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

static task_t g_task = MACH_PORT_NULL;

// iOS SDK 把 mach_vm.h 直接 #error 掉了，改用一直可用的 vm_read_overwrite
static int rd(uint64_t addr, void *buf, size_t n) {
    vm_size_t out = 0;
    kern_return_t kr = vm_read_overwrite((vm_map_t)g_task,
                                         (vm_address_t)addr,
                                         (vm_size_t)n,
                                         (vm_address_t)(uintptr_t)buf,
                                         &out);
    return (kr == KERN_SUCCESS && out == n) ? 0 : -1;
}
static uint64_t rd64(uint64_t a) { uint64_t v = 0; if (rd(a, &v, 8)) return 0; return v; }
static uint32_t rd32(uint64_t a) { uint32_t v = 0; if (rd(a, &v, 4)) return 0; return v; }

static void rd_cstr(uint64_t a, char *out, size_t max) {
    size_t i;
    out[0] = 0;
    if (!a) return;
    for (i = 0; i + 1 < max; i++) {
        char c = 0;
        if (rd(a + i, &c, 1)) break;
        out[i] = c;
        out[i + 1] = 0;
        if (c == 0) return;
    }
}

// IL2CPP System.String -> UTF-8
static int rd_string(uint64_t sp, char *out, size_t max) {
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
        if (rd(sp + OFF_STR_CHARS + (uint64_t)i * 2, &c, 2)) break;
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

static pid_t find_pid(const char *want) {
    int mib[4];
    size_t len = 0, n, i;
    char *buf;
    struct kinfo_proc *kp;
    pid_t found = -1;

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_ALL;
    mib[3] = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len == 0) return -1;
    buf = (char *)malloc(len);
    if (!buf) return -1;
    if (sysctl(mib, 4, buf, &len, NULL, 0) != 0) { free(buf); return -1; }

    kp = (struct kinfo_proc *)buf;
    n = len / sizeof(struct kinfo_proc);
    for (i = 0; i < n; i++) {
        const char *nm = kp[i].kp_proc.p_comm;
        if (nm && strstr(nm, want)) { found = kp[i].kp_proc.p_pid; break; }
    }
    free(buf);
    return found;
}

// 在目标进程里找某个 image 的加载基址（UnityFramework 的 vmaddr=0，所以基址就是 slide）
static uint64_t find_image_base(const char *want) {
    task_dyld_info_data_t d;
    mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
    uint32_t count, i;
    uint64_t ai, arr;
    memset(&d, 0, sizeof(d));
    if (task_info(g_task, TASK_DYLD_INFO, (task_info_t)&d, &cnt) != KERN_SUCCESS) {
        printf("[x] task_info(TASK_DYLD_INFO) 失败\n");
        return 0;
    }
    ai = (uint64_t)d.all_image_info_addr;
    if (!ai) { printf("[x] all_image_info_addr 为空\n"); return 0; }
    count = rd32(ai + 4);
    arr = rd64(ai + 8);
    if (!count || !arr || count > 200000) { printf("[x] 镜像表异常 count=%u arr=%llu\n", count, (unsigned long long)arr); return 0; }
    for (i = 0; i < count; i++) {
        uint64_t e = arr + (uint64_t)i * 24;
        uint64_t loadAddr = rd64(e);
        uint64_t pathPtr = rd64(e + 8);
        char path[1024];
        rd_cstr(pathPtr, path, sizeof(path));
        if (strstr(path, want)) return loadAddr;
    }
    printf("[x] 目标进程里没找到 %s\n", want);
    return 0;
}

// 读 ItemInfo.Seeds / Blueprints
static int dump_list(FILE *fp, const char *tag, uint64_t listPtr, char names[][128], int *pn) {
    int32_t size;
    uint64_t items;
    int32_t i;
    *pn = 0;
    if (!listPtr) { fprintf(fp, "# %s <空>\n", tag); return 0; }
    size = (int32_t)rd32(listPtr + OFF_LIST_SIZE);
    items = rd64(listPtr + OFF_LIST_ITEMS);
    if (size <= 0 || size > MAXNAMES) { fprintf(fp, "# %s 大小异常 %d\n", tag, size); return 0; }
    if (!items) { fprintf(fp, "# %s _items 为空\n", tag); return 0; }
    fprintf(fp, "# %s  %d 条\n", tag, size);
    for (i = 0; i < size; i++) {
        uint64_t row = rd64(items + OFF_ARRAY_DATA + (uint64_t)i * 8);
        char nm[128];
        if (!row) continue;
        if (rd_string(rd64(row + OFF_ROW_NAME), nm, sizeof(nm)) <= 0) continue;
        fprintf(fp, "%s\n", nm);
        strncpy(names[*pn], nm, 127);
        names[*pn][127] = 0;
        (*pn)++;
        if (*pn >= MAXNAMES) break;
    }
    fprintf(fp, "\n");
    return *pn;
}

int main(int argc, char *argv[]) {
    const char *outpath = "/var/mobile/seed_dump.txt";
    const char *procname = "SoulKnight";
    long wantpid = -1;
    pid_t pid;
    kern_return_t kr;
    uint64_t base, slot, klass, sf, info;
    FILE *fp;
    static char seeds[MAXNAMES][128];
    static char bps[MAXNAMES][128];
    int ns = 0, nb = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) { wantpid = strtol(argv[++i], NULL, 10); }
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) { procname = argv[++i]; }
        else { outpath = argv[i]; }
    }

    pid = (wantpid > 0) ? (pid_t)wantpid : find_pid(procname);
    if (pid <= 0) { printf("[x] 没找到进程 %s  先把游戏开着\n", procname); return 1; }
    printf("[+] pid = %d\n", pid);

    kr = task_for_pid(mach_task_self(), pid, &g_task);
    if (kr != KERN_SUCCESS) {
        printf("[x] task_for_pid 失败 kr=%d\n", kr);
        printf("    需要用 root 跑，并用 ldid 签上 task_for_pid-allow（见 README）\n");
        return 2;
    }
    printf("[+] 拿到 task port\n");

    base = find_image_base("UnityFramework");
    if (!base) return 3;
    printf("[+] UnityFramework 基址 = 0x%llx\n", (unsigned long long)base);

    slot = base + RVA_SLOT_ITEMINFO;
    klass = rd64(slot);
    printf("[+] ItemInfo 类指针 = 0x%llx  (槽 0x%llx)\n", (unsigned long long)klass, (unsigned long long)slot);
    if (!klass) { printf("[x] 槽里还是空的：ItemInfo 类还没被 IL2CPP 初始化，进游戏开一次花园/背包再跑\n"); return 4; }

    sf = rd64(klass + OFF_CLASS_STATICFLD);
    printf("[+] static_fields = 0x%llx\n", (unsigned long long)sf);
    if (!sf) return 4;

    info = rd64(sf);
    printf("[+] ItemInfo 单例 = 0x%llx\n", (unsigned long long)info);
    if (!info) { printf("[x] 单例还没建：进游戏开一次花园/背包（触发道具表加载）再跑\n"); return 5; }

    fp = fopen(outpath, "w");
    if (!fp) { printf("[x] 打不开输出文件 %s\n", outpath); return 6; }

    fprintf(fp, "# seedump    元气骑士 ItemInfo 全表\n");
    fprintf(fp, "# UnityFramework base = 0x%llx   ItemInfo = 0x%llx\n\n", (unsigned long long)base, (unsigned long long)info);
    dump_list(fp, "Seeds", rd64(info + OFF_SEEDS), seeds, &ns);
    dump_list(fp, "Blueprints", rd64(info + OFF_BLUEPRINTS), bps, &nb);
    fflush(fp);
    fclose(fp);

    printf("[+] Seeds      %d 条\n", ns);
    printf("[+] Blueprints %d 条\n", nb);
    printf("[+] 已写入 %s\n", outpath);
    printf("\n---- Seeds ----\n");
    for (i = 0; i < ns; i++) printf("%s\n", seeds[i]);
    return 0;
}
