# Vermeer（Redmi K70）XNU 探针固件

这个仓库只做一件事：用 GitHub Actions 构建一个**读取硬件状态、但不写外设寄存器的探针** `boot.img`，
通过 `fastboot boot` 临时加载到 Redmi K70（`vermeer` / SM8550）上，
把生成 vermeer ADT 所需的实测值显示在屏幕上。

> **不使用 `fastboot flash`，不主动写任何分区。** 探针不调用
> `ExitBootServices`，不写外设寄存器，也不读写磁盘；它只通过
> GOP/DEBUG 向帧缓冲输出结果，并通过 UEFI 变量服务写
> `Boot####` / `BootOrder`，让 BDS 启动探针本身。

---

## 1. 它采集什么

| 屏幕字段 | 含义 | 用途 |
| --- | --- | --- |
| `EL` | UEFI 运行所在的异常级别 | 确认启动环境 |
| `MIDR` | CPU 标识 | 确认 SM8550 / Kryo |
| `MPIDR` | 启动 CPU 的完整 MPIDR | 参考 |
| `AFF` | MPIDR 的 Aff0–Aff2 位 | `make_vermeer_adt.py --boot-mpidr` |
| `CNTFRQ` | 通用定时器频率 | 必须是 `19200000` |
| `MMFR0` / `PFR0` | AArch64 特性寄存器 | 交叉检查 |
| `GOP FB` / `GOP SZ` | GOP 帧缓冲基址 / 大小 | 显示交接 |
| `GOP RES` | 分辨率 | 预期 `1440x3200` |
| `GOP STR` | 每行像素数 | 预期 `1440` |
| `GOP FMT` | 像素格式 | 记录即可 |
| `WIN BASE` / `WIN LEN` | 最大连续空闲窗口 / DarwinBoot 候选起点 | `--owned-memory-size` |
| `RAM TOP` / `RAM MB` | 最高连续常规内存顶端 | `--dram-size` 的参考 |

`WIN BASE` 的算法与 DarwinBoot 一致：从最大连续
`EfiConventionalMemory` 段开始，按 2 MiB 对齐再加 `0x4000`。
`WIN LEN` 是从该起点到这段内存末尾的字节数。

---

## 2. 构建前准备

- Redmi K70，代号必须是 `vermeer`（K70 Pro 是 `manet`，不能混用）。
- Bootloader 已解锁（`fastboot oem device-info` 显示 `unlocked: true`）。
- 一台能运行 `fastboot` 的电脑。本工作区已有
  `tools/platform-tools/fastboot.exe`。
- 一个 GitHub 账号。**不需要 Mac、本地 clang 或 Python。**

---

## 3. 创建仓库并上传

1. 在 GitHub 新建一个 **Public** 仓库，例如 `vermeer-probe`，默认分支 `main`。
2. 打开本机的 `D:\UniXNUvermeer\probe-ci` 文件夹。
3. 全选**文件夹里的内容**（包括 `.github`、`probe`、`tools`、`dtb`、
   `README.md`），拖到 GitHub 空仓库的 “uploading an existing file” 页面。
   - 必须让 `.github/workflows/build-probe.yml` 位于仓库根目录。
   - **不要**把 `probe-ci` 这一层文件夹本身拖进去，否则路径会变成
     `probe-ci/.github/...`，Actions 找不到工作流。
4. 提交到 `main`。

首次 push 会自动触发一次构建（只上传 artifact）。要生成 Release，
在 **Actions → Build Vermeer probe firmware → Run workflow** 手动再跑一次。

---

## 4. 构建

工作流做这些事：

1. `git clone --depth 1 --branch vermeer-zh-v2`，并校验 HEAD 必须是
   `05e5980445e0da43aec5c1c042bd8f966f2dbffd`；
2. 初始化子模块，安装 `clang` / `lld` / `gcc-aarch64-linux-gnu` / Python 依赖；
3. 运行 `tools/prepare_probe.py`，把上游 `vermeerPkg` 复制成独立的
   `vermeerProbePkg`，删除 `UniXNUPkg`、`DarwinBootPkg`、`UniXNULib`，
   关闭中文 UI，加入探针；
4. `python3 build_uefi.py -d vermeerProbe -r DEBUG -c -m 0`；
5. 校验 `Mu-vermeerProbe.img` 的 boot header、gzip、FD、DTB 和探针模块；
6. 上传 artifact；手动运行时再发布 Release。

首次构建通常需要 15–30 分钟（子模块较大）。

### 下载

- **Artifact**：Actions → 对应 run → Artifacts → `vermeer-probe-N`。
- **Release**：Releases → `probe-N` → `Mu-vermeerProbe.img`。

解压后目录里应有：

```
Mu-vermeerProbe.img
SHA256SUMS.txt
build-info.txt
```

校验：

```powershell
Get-FileHash .\Mu-vermeerProbe.img -Algorithm SHA256
Get-Content .\SHA256SUMS.txt
```

两个 SHA-256 必须一致。`build-info.txt` 里记录了上游 commit、run 链接和
构建时间。

---

## 5. 启动探针

设备进入 fastboot（关机后按住**音量下 + 电源**，或
`adb reboot bootloader`），然后：

```powershell
cd D:\UniXNUvermeer
.\tools\platform-tools\fastboot.exe devices
.\tools\platform-tools\fastboot.exe boot .\Mu-vermeerProbe.img
```

预期输出：

```text
Sending 'boot.img' (...) OKAY
Booting                        OKAY
```

**不要**执行 `fastboot flash`。`fastboot boot` 只把镜像加载到内存。

启动后屏幕会出现大号 DEBUG 文本（字号 6：约 36×72 像素，40 列 × 44 行）：

```text
VERMEER PROBE v1

EL 2
MIDR 0x...
MPIDR 0x...
AFF 0x...
CNTFRQ 19200000 Hz
MMFR0 0x...
PFR0 0x...

GOP FB 0x...
GOP SZ 0x...
GOP RES 1440x3200
GOP STR 1440
GOP FMT 1

MAP DESC ...
WIN BASE 0x...
WIN LEN 0x...
WIN MB ...
RAM TOP 0x...
RAM MB ...

PHOTO THIS SCREEN
ANY KEY RETURNS
PWR+VOL- FASTBOOT
AUTO EXIT 300s
```

屏幕底部还有一条**白色横条**，它是独立于 DEBUG 文本的“应用确实运行了”
标记。

### 拍照清单

至少拍清楚这几行：

- `EL`、`MIDR`、`MPIDR`、`AFF`、`CNTFRQ`；
- `GOP FB`、`GOP SZ`、`GOP RES`、`GOP STR`、`GOP FMT`；
- `WIN BASE`、`WIN LEN`、`WIN MB`、`RAM TOP`、`RAM MB`。

如果某行被截断，调大照片分辨率或分两张拍。

### 退出

- 按任意键：探针返回 BDS（BDS 可能再次启动它）；
- **电源 + 音量下**：回到 fastboot；
- 长按电源：重启回 Android。

`fastboot boot` 没有写分区，所以正常重启一定回到原系统。

---

## 6. 怎么用这些值

### ADT

在 `mansar1337/UniXNU-vermeer` 的 `tools/make_vermeer_adt.py` 里：

```powershell
python make_vermeer_adt.py `
  --output vermeer-adt.bin `
  --dram-size 0x400000000 `
  --boot-mpidr 0x0 `
  --counter-hz 19200000 `
  --owned-memory-size 0x80000000
```

- `--boot-mpidr` 用屏幕上的 `AFF`。生成器只接受 `0x0..0x700`、步长
  `0x100`；如果实测不是这个形式，先停下来检查。
- `--counter-hz` 必须是 `19200000`；不是就说明定时器来源和预期不同，
  不要继续。
- `--owned-memory-size` 取 `WIN LEN` 以内的值，要求
  `256 MiB..8 GiB`、2 MiB 对齐。`0x80000000`（2 GiB）是保守起点。
- `--dram-size`：K70 16 GB 用 `0x400000000`（从 `0x80000000` 起 16 GiB）。
  `RAM TOP` / `RAM MB` 可以用来交叉验证；如果 `RAM TOP` 明显低于
  16 GiB，说明顶部有保留区，以设备实际内存为准。

### 判定

- `CNTFRQ == 19200000` 且 `AFF` 在允许范围：可以生成 ADT；
- `GOP RES == 1440x3200`、`GOP STR == 1440`：显示交接符合预期；
- `WIN LEN` 至少 256 MiB：DarwinBoot 有可用窗口；
- 任何一项不符：先把屏幕照片和 `build-info.txt` 发回来，不要直接改
  DarwinBoot。

---

## 7. 这个探针是怎么做出来的

### 上游基线

- 仓库：`https://github.com/yyx2025/uefi-vermeer`
- tag：`vermeer-zh-v2`
- commit：`05e5980445e0da43aec5c1c042bd8f966f2dbffd`

必须按 tag 对应的 commit 构建；分支 HEAD 已经在该 tag 之后。

### `prepare_probe.py` 的改动

- 复制 `Platforms/Xiaomi/vermeerPkg` → `vermeerProbePkg`；
- 把 `.dsc/.fdf/.inf/.dec/.inc/.py` 里的 `vermeerPkg` 改成
  `vermeerProbePkg`；
- 从 DSC/FDF 删除 `UniXNUPkg`、`UniXNULib`、`DarwinBootPkg` 引用，
  这样 CI 不需要再检出 UniXNU；
- 修正 `build_uefi.py` 的参数传递：上游用真值判断，`base = 0` /
  `header_version = 0` 会被丢掉，mkbootimg 退回 `base 0x10000000`，
  内核地址变成 `0x10008000`；改成 `value is not None` 后才与
  G1 镜像的 `0x00008000` 一致；
- `VERMEER_CHINESE_UI = 0`，避免中文 UI 与探针抢 BootOrder；
- 加入 `VermeerProbeDxe`（注册 `Boot####`）和 `VermeerProbe`
  （UEFI 应用）；
- `PcdFrameBufferFontScale` 3 → 6，`PcdFrameBufferDelay` 30000 → 2000，
  并显式设置 `PcdFrameBufferFreezeWhenFull = FALSE`；
- 把自带的 `dtb/vermeer.dtb` 复制成 `Resources/DTBs/vermeerProbe.dtb`；
- 把 `Resources/ramdisk` 清空为 0 字节，保证 `ramdisk_size = 0`。

脚本对每个替换都做数量校验；上游布局一变就会直接失败，不会悄悄构建成
原版 vermeer 固件。

### 显示路径

探针**不用** GraphicsConsole 的 `Print`。它用 `DEBUG ((DEBUG_ERROR, ...))`，
由平台的 `SerialPortLib = FrameBufferSerialPortLib` 直接画到
“Display Reserved” 帧缓冲上。这样字号由 `PcdFrameBufferFontScale` 控制，
在 1440 宽的屏上是 6 倍字体（约 36×72 像素）。

`VermeerProbeDxe` 复制的是 `VermeerChineseUiDxe` 已经在本机验证过的机制：
写一个指向本 FV 应用的 `Boot####`，再把它放到 `BootOrder` 首位。它的
DEPEX 是变量服务，所以 DXE 会在 BDS 架构协议安装之前派发它。

### DTB 来源

`dtb/vermeer.dtb` 不是上游仓库里的文件（上游 `Resources/DTBs/` 没有
`vermeer.dtb`），而是从 release 镜像 `vermeer-zh-boot-touch.img` 里提取的：

- 提取偏移：`1,037,325`（`4096 + gzip 负载长度 1,033,229`）；
- 长度：`511,734` 字节；
- FDT `totalsize` 与文件长度一致；
- SHA-256：`82EC568163AD66733D2A140B2C7190C9ADA58E53A1DC0EE3C0FB4765AD072E8B`。

没有这个 DTB，`build_uefi.py` 会拒绝打包（它要找
`Resources/DTBs/vermeerProbe.dtb`）。

---

## 8. 故障排查

| 现象 | 处理 |
| --- | --- |
| `prepare_probe.py: expected exactly one ...` | 上游 tag 内容变了。把完整报错发回来，不要跳过检查。 |
| `setup_env.sh` 失败 | 多为 apt/网络问题，重跑 workflow。 |
| `build_uefi.py` 失败 | 下载该 run 的日志；重点看 `DeviceBuild.py` 的 `stuart` 输出。 |
| `verify_boot_image.py` 失败 | 镜像结构不对，**不要**拿去 `fastboot boot`；把报错发回来。 |
| 报 `page size is 4096 (got ...)` | 日志上方的 `header words` 就是镜像真实头部：`got 2048` 说明 `--pagesize` 没传，`got 0` 说明写成了 v3 头。把这段贴回来。 |
| `fastboot boot` 后黑屏 | 先确认预编译中文 UI 镜像仍能亮屏；再重跑探针。黑屏但设备有反应时，长按电源回 Android。 |
| 只有固件 DEBUG 文本，没有探针 | 应用可能没被 BDS 启动。看屏幕里有没有 `VermeerProbeDxe: Boot#### first`。 |
| `VermeerProbeDxe: probe application not found` | FDF 没把应用放进 FV，或 FILE_GUID 不匹配。检查 `prepare_probe.py` 是否完整执行。 |
| 屏幕文字太小/太大 | 改 `vermeerProbe.dsc` 里的 `PcdFrameBufferFontScale` 后重跑；6 是当前值。 |
| 按键没反应 | 探针会在 300 秒后自动返回 BDS；也可以电源 + 音量下回 fastboot。 |

---

## 9. 安全边界

- `fastboot boot` 不写 boot / dtbo / userdata / super 等任何分区；
- 探针不调用 `ExitBootServices`，不写外设寄存器，不读写磁盘；
- 探针读取寄存器、GOP 和 UEFI 内存映射；结果只通过 GOP/DEBUG 写帧缓冲；
- 为让 BDS 启动自己，`VermeerProbeDxe` 会通过 UEFI 变量服务写
  `Boot####` / `BootOrder`；这是启动项注册，不等于 `fastboot flash` 写分区，重启后由 fastboot 重新加载原系统；
- 如果设备进入异常状态，长按电源 10 秒强制重启即可回到 Android。
