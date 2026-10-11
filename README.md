# seedump  元气骑士 ItemInfo 全表读取器

独立 iOS 命令行工具。`task_for_pid` 附加到运行中的**元气骑士**，
直接读内存里的 `ItemInfo.Seeds` / `ItemInfo.Blueprints`，写出全部种子名和蓝图名。

- 不改游戏包、不 hook、不需要 Frida、不需要电脑
- 目标版本：8.6.0 (1519)，bundle id `com.ChillyRoom.DungeonShooter`
- 所有偏移都是从这份二进制反汇编 + Il2CppDumper 符号表里取的硬偏移：
  - `ItemInfo.get_Info()` 里 `adrp #0x12088000 / add #0xd60`  槽 `0x12088D60`
  - `ldr x8,[class,#0xB8]`  `static_fields`；`ldr x0,[x8]`  单例
  - `ItemInfo.Seeds` @ `+0x18`，`Blueprints` @ `+0x10`
  - `List<T>`: `_items` @ `+0x10`，`_size` @ `+0x18`，数组数据 @ `+0x20`
  - `ItemInfoRow.Name` @ `+0x10`；`System.String`: 长度 @ `+0x10`，UTF-16 @ `+0x14`

## 1. 编译（GitHub Actions，不用你有 Mac）

新建一个仓库（**建议 public**，私有仓库的 macOS runner 会烧额度），把本目录整个推上去。
`Actions` 会自动跑 `.github/workflows/build.yml`，产出 artifact `seedump-arm64`。

## 2. 装到设备

从 artifact 里取出 `seedump` 和 `ents.plist`，用 Filza 放进设备（例如 `/var/mobile/`），然后开终端：

```
cd /var/mobile
chmod +x seedump
ldid -Sents.plist seedump          # 关键：签上 task_for_pid-allow
```

`ldid` 没装就先在 Sileo 里装 `ldid`。

## 3. 跑

1. **先把游戏打开并进到主界面**，然后随便开一次花园 / 背包（触发道具表加载，让 `ItemInfo` 单例建出来）
2. 切到终端，切到 root（`sudo -s`，或者用 Filza 的 root 终端）：

```
sudo ./seedump
```

默认写到 `/var/mobile/seed_dump.txt`。也可以 `sudo ./seedump -p 1234 /var/mobile/out.txt`。

用 Filza 打开 `/var/mobile/seed_dump.txt`，把内容发我。

## 4. 常见报错

| 输出 | 原因 | 处理 |
|---|---|---|
| `task_for_pid 失败 kr=5` | 权限不够 | 用 root 跑；确认 `ldid -Sents.plist` 签过；部分越狱还要装 `AppSync`/`amfid` 补丁类插件 |
| `没找到进程 SoulKnight` | 游戏没开 | 先开游戏 |
| `槽里还是空的` / `单例还没建` | 道具表还没加载 | 进游戏开一次花园或背包，再跑 |
| `目标进程里没找到 UnityFramework` | 读 dyld 镜像表失败 | 把完整输出发我 |
