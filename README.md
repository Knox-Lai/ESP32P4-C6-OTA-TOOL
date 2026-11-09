# ESP32-P4 Ethernet OTA 工具 —— 固件工程

当前阶段：**阶段 2 + 3 + 4 已实机跑通；阶段 5（OTA 协议）工具侧完成，本机对拼验证通过**
（阶段 6 设备端 Bootloader 由设备固件团队实现，这里只提供协议与 PC 参考实现）

## 目录

```
firmware/
├── CMakeLists.txt            # 含 MINIMAL_BUILD ON（关键，见下）
├── sdkconfig.defaults
├── partitions_p4.csv         # 自定义分区表：偏移 0x10000，app 3MB + storage 11MB
├── build.bat                 # 双击/命令行入口（绕过 PowerShell 执行策略）
├── build.ps1                 # 实际干活的脚本（激活环境 + 编译，可选 -Flash/-Port）
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml     # espressif/ip101（PHY 驱动）
    ├── app_config.h          # 所有可调参数：引脚 / 网段 / 地址池 / 发现周期 / OTA 超时
    ├── net_eth.c/.h          # EMAC + IP101 + esp_netif 装配
    ├── net_dhcps.c/.h        # DHCP Server 配置 + 启动自检 + 租约日志
    ├── ota_frame.c/.h        # 通用帧格式（12B 头 + CRC32），UDP/TCP 共用
    ├── ota_proto.c/.h        # 5 种升级载荷的打包/解析（BEGIN/BEGIN_ACK/DATA_ACK/RESULT）
    ├── ota_sha256.c/.h       # 纯软件 SHA-256（整包权威校验，不依赖 mbedtls）
    ├── ota_image.c/.h        # 镜像抽象 + 合成测试镜像（阶段 7 换成读 .fwp）
    ├── ota_client.c/.h       # 升级会话引擎：连接/协商/滑窗/重传/校验/重启/版本核对
    ├── ota_shell.c/.h        # 串口命令行：dev / status / ota <n> [KB]
    ├── dev_table.c/.h        # 设备表（16 槽）+ 身份字段序列化
    ├── dev_discovery.c/.h    # UDP 发现：租约被动发现 + 单播识别 + 心跳
    └── app_main.c            # 启动顺序编排 + 周期状态自检
```

PC 侧配套：`../tools/ota_sim.py`（设备模拟器：UDP 4010 发现 + TCP 4011 升级）

## 编译与烧写

**这台机器装了 3 套 IDF，必须先激活对的那一套**（否则会报 `xtensa-esp32-elf-gcc was not found`，详见下节）：

| 用途 | 路径 |
|---|---|
| **本工程使用** | IDF `D:\esp\.espressif\v6.1\esp-idf` |
| 配套工具链 | `C:\Espressif\tools`（`riscv32-esp-elf\esp-15.2.0_20251204`） |
| 配套 Python | `C:\Espressif\tools\python\v6.1\venv` |
| 环境激活脚本 | `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` |

推荐直接用工程自带脚本（自动激活环境、只在目标不对时才 set-target）：

```powershell
cd C:\Users\EL\Desktop\ESP32P4\ESP32P4-colorscaping\firmware
.\build.bat                 # 只编译（务必用 .bat，见下）
.\build.bat -Flash          # 编译 + 烧写 + 串口监视 (COM69)
.\build.bat -Flash -Port COM12
```

> **为什么用 `build.bat` 而不是直接 `.\build.ps1`**：本机 PowerShell 执行策略是 Restricted，
> 直接跑 `.ps1` 会报 “禁止运行脚本”，连 IDF 激活脚本都 dot-source 不了。
> `.bat` 用 `-ExecutionPolicy Bypass` 启动 PowerShell，不改系统策略。

手工等价命令：

```powershell
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"   # 注意前面的点，必须 dot-source
cd C:\Users\EL\Desktop\ESP32P4\ESP32P4-colorscaping\firmware
idf.py set-target esp32p4   # 已有正确 sdkconfig 时可跳过；会清空 sdkconfig 与 build
idf.py build
idf.py -p COM69 flash monitor
```

> 不要用 `D:\esp\v6.0.2\esp-idf\export.bat`：环境变量里的 `IDF_PATH`/`IDF_TOOLS_PATH`
> 是旧的（指向 v6.0.2 + `D:\Espressif`），会让 v6.1 去配一个 6.0 的 python env。

首次构建会从组件仓库拉取 `espressif/ip101`（IP101 PHY 驱动自 IDF v6.0 起已移出 IDF 主仓库），已拉过一次，`dependencies.lock` 里锁的是 `ip101 1.1.0` + `idf 6.1.0`。

需要 IDF **v6.1.0**（与开发板自带示例一致；`esp_eth_mac_new_esp32()` / `esp_eth_phy_new_ip101()` 在 v6.x 下可用）。

### 为什么报 `xtensa-esp32-elf-gcc was not found`

ESP32-P4 是 **RISC-V**，编译器是 `riscv32-esp-elf-gcc`；报 xtensa 说明构建系统当时把目标当成了默认的 `esp32`。
该文案来自 VS Code 的 ESP-IDF 插件（v2.3.0，`IdfToolNotFound`），它的工具名是按目标名拼的：

```js
case "esp32": case "esp32s2": case "esp32s3": return `xtensa-${target}-elf-gcc`;
default:                                     return `riscv32-esp-elf-gcc`;
```

触发条件是**插件的工作区根目录里读不到 `sdkconfig`**（当时工作区是 `ESP32P4-colorscaping/`，而 `sdkconfig` 在 `firmware/` 下），目标回落成 `esp32`。
**解决：把 `firmware/` 单独作为文件夹打开**（或用上面脚本从命令行编），不要用上层目录当工作区。

### 两个真正卡住构建的坑（都已修）

**坑 1：分区表偏移 0x8000 装不下 P4 的 bootloader。**

这是最初编译失败的真实原因，报错发生在 bootloader 打包阶段、`sections.ld` 之后：

```
Error: Bootloader binary size 0x6140 bytes is too large for partition table offset 0x8000.
Bootloader binary can be maximum 0x6000 (24576) bytes unless the partition table offset
is increased in the Partition Table section of the project configuration menu.
```

P4 的 bootloader 实测 **0x6140 = 24896 字节**，而 0x8000 偏移只给它留 0x6000 = 24576 字节，**超 320 字节**。
开发板自带 `ethernet` 示例里 `CONFIG_PARTITION_TABLE_OFFSET=0x10000` 就是为了这个，不是随手填的。

修正后同一项检查变成（实测输出）：

```
Bootloader binary size 0x6140 bytes. 0x7ec0 bytes (57%) free.
```

**坑 2：没开 MINIMAL_BUILD，整个 IDF 都编进镜像。**

IDF 默认编译**全部组件**（wifi / bt / openthread / lvgl…），ninja 任务 747 个，构建 4 分钟以上，
镜像体积也会被撑到 MB 级。demo 工程顶层 CMakeLists 有一行我们漏了：

```cmake
idf_build_set_property(MINIMAL_BUILD ON)
```

加上后只编 main 及其依赖，任务数降到 185 个，镜像 **0x5b080 = 364 KB**。

### 当前分区布局（`partitions_p4.csv`，偏移 0x10000）

| 分区 | 偏移 | 大小 | 用途 |
|---|---|---|---|
| （分区表） | 0x10000 | 4K | 占用 0x10000~0x11000 |
| nvs | 0x11000 | 24K | 参数存储 |
| phy_init | 0x17000 | 4K | PHY 校准数据 |
| factory | 0x20000 | **3M** | 应用本体（当前用 364KB，余 88%） |
| storage | 0x320000 | **11M** | 固件仓库（阶段 7 导入的待升级固件） |

改分区表涉及链接脚本，`idf.py build` 会自动重新 configure + 重新生成 `sections.ld`，不需要 `fullclean`。

> 早期那个 `partitions.csv`（0x8000 布局）已作废，不要再用；`sdkconfig` 指向的是 `partitions_p4.csv`。

## 设备发现（阶段 4，UDP 4010）

### 为什么不是"纯广播扫描"

工具自己就是 DHCP Server，`IP_EVENT_ASSIGNED_IP_TO_CLIENT` 已经把
**MAC + 分配到的 IP + 主机名** 直接送到手上。所以主路径是：

```
DHCP 租约事件  →  设备表插入一行  →  ≤0.5 s 内单播 QUERY_INFO  →  设备回 DISCOVER_RSP  →  身份补全
```

广播 `DISCOVER` 只作为兜底，覆盖两类情况：设备用**静态 IP**（不在租约表里）、
工具重启后**租约表丢失**（DHCP Server 不持久化租约）。

### 帧格式（`main/ota_frame.h`，UDP 与后续 TCP 共用）

```
 0        1        2        3        4        5        6        7
+--------+--------+--------+--------+--------+--------+--------+--------+
|  MAGIC(2B,BE 0x4F54)   | VER(1B)| CMD(1B)|       LEN(2B,BE)        |
+--------+--------+--------+--------+--------+--------+--------+--------+
|                     SEQ(4B,BE)        |        FLAGS(2B,BE)        |
+--------+--------+--------+--------+--------+--------+--------+--------+
|              PAYLOAD (LEN 字节)          |      CRC32(4B, BE)       |
+----------------------------------------------------------------------+
```

头固定 12 字节，CRC32 覆盖「头 + 载荷」，**与 `binascii.crc32()` 完全等价**，
所以 PC 模拟器不需要自己实现 CRC。

身份载荷固定 **44 字节**：

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 16 | UID（芯片 ID / 序列号） |
| 16 | 16 | MODEL（ASCII，补 0） |
| 32 | 2 | HW_VER（高字节主、低字节次，0x0102 = 1.2） |
| 34 | 4 | FW_VER（major.minor.patch.build） |
| 38 | 4 | FLASH_SIZE（字节） |
| 42 | 2 | CAPS（bit0 续传 / bit1 SHA-256 / bit2 双分区） |

### 联调：用 PC 冒充设备

```bash
cd ESP32P4-colorscaping\tools
python ota_sim.py --selftest          # 先跑协议自检（不联网）
python ota_sim.py --model P4-COLOR-4CH --hw 1.2 --fw 1.0.3.12
python ota_sim.py --announce 3        # 每 3 s 主动上报（模拟静态 IP 设备）
```

⚠ **Windows 第一次运行会弹防火墙提示，必须点"允许访问"**。
点"取消"的话 P4 发的 UDP 会被静默丢弃，现象是串口一直打「设备表为空」，
很容易误判成固件问题。事后要补：Windows 安全中心 → 允许应用通过防火墙 → 勾选 Python。

### 期望日志

```
I (xxx) dev_disc: 发现模块就绪：UDP 4010，广播周期 5000 ms，保活 5000 ms
I (xxx) dev_disc: 已广播 DISCOVER 到 10.77.0.255:4010
I (xxx) dev_disc: 租约入表：10.77.0.200  54:ee:75:a3:47:aa  host='DG0431'，立即探测身份
I (xxx) dev_disc: 识别到设备 10.77.0.200  model='P4-COLOR-4CH' hw=1.2 fw=1.0.3.12 flash=4096KB caps=0x0003 uid=...
I (xxx) dev_disc: 设备表：1 台
I (xxx) dev_disc:   [0] 10.77.0.200 54:ee:75:a3:47:aa DG0431     ONLINE  model='P4-COLOR-4CH' hw=1.2 fw=1.0.3.12 4096KB
```

拔掉设备后约 15 s（3 次心跳丢失）出现 `设备离线：...`，之后降级为 30 s 探一次，
设备恢复后自动回到 `ONLINE`。

> **正常现象（很重要，别当成故障）**：PC 自己不跑模拟器时，设备表里会出现一条
> `无应答 / 仅 DHCP 租约，未响应发现协议` 的条目——那是 PC 的 DHCP 租约
> （有 MAC、有 IP、有主机名），但 PC 上没人监听 UDP 4010。
>
> 这种情况分两档处理，不会刷 W 级警告：
> - **未识别设备**：1 s 快探 3 次，之后 **10 s 慢探**，只打一条 INFO 提示；
> - **已识别设备**：5 s 心跳，3 次丢失才报 `设备离线`（W 级）。
>
> 在 PC 上启动 `ota_sim.py` 后，同一个槽会在 ≤5 s（下一次兜底广播 DISCOVER 的应答）内
> 变成 `ONLINE` 并补全型号版本。

### 排除“一直身份未知”的两种可能

设备表里长期只有 `仅 DHCP 租约` 条目时，看这两处就能分清是**对端没回**还是**包没进来**：

1. **P4 侧** —— 日志里的 `累计收包 N`。N 一直为 0 且模拟器在跑，
   说明 PC→P4 方向不通（防火墙 / 路由），问题不在固件。
2. **PC 侧** —— 模拟器窗口里有没有 `<- QUERY_INFO` 或 `<- DISCOVER`。
   如果 10 s 内一行都没有，模拟器会自己打出排查提示（防火墙、`route print 10.77.0.1`）。


## 期望日志（DHCP 部分）

```
I (312) app: ===== ESP32-P4 Ethernet OTA 工具 / DHCP Server POC =====
I (312) app: PHY=IP101 addr=1 MDC=GPIO31 MDIO=GPIO52 nRST=GPIO51
I (312) app: 工具地址 10.77.0.1/24，地址池 10.77.0.200 ~ 10.77.0.230
I (322) net_eth: P4 ETH MAC = xx:xx:xx:xx:xx:xx
I (322) net_eth: netif 就绪：ETH_OTA，静态地址 10.77.0.1/255.255.255.0
I (332) net_dhcps: 地址池 10.77.0.200 ~ 10.77.0.230，租约 3600 s（单位 60 s × 60）
I (342) net_eth: EMAC 已启动
I (352) net_dhcps: DHCP Server 已启动（状态 = STARTED）
I (362) net_eth: PHY ID = 0x02430xxx（IP101，PHYID1=0x0243）

I (5342) net_eth: 链路 UP：100M full
I (5342) net_dhcps: 新租约 #1 -> 10.77.0.200  client xx:xx:xx:xx:xx:xx  host='DESKTOP-XXXX'
I (10342) app: link=UP 100M/full | ip=10.77.0.1 | dhcps=STARTED | lease=1 | heap=xxxxx
```

判定标准（详见 `../docs/dhcp-verify.md`）：

1. 日志出现 `DHCP Server 已启动（状态 = STARTED）`；
2. 插线后出现 `链路 UP` 且有 `新租约`；
3. PC 侧 `ipconfig /all` 的 **DHCP 服务器 = 10.77.0.1**；
4. PC 的 `arp -a` 中 10.77.0.1 的 MAC 与日志打印的 `P4 ETH MAC` 一致。

## 可调参数（`main/app_config.h`）

| 宏 | 默认 | 说明 |
|---|---|---|
| `APP_ETH_MDC_GPIO` / `APP_ETH_MDIO_GPIO` | 31 / 52 | SMI 引脚，板载实测值 |
| `APP_PHY_RST_GPIO` / `APP_PHY_ADDR` | 51 / 1 | PHY 复位脚与地址 |
| `APP_NET_A/B/C` | 10.77.0 | 网段；改这里即同时换掉工具 IP、DHCP 池、广播地址（旧网段 192.168.4 已留档在注释里） |
| `APP_TOOL_HOST` | 1 | 工具自身 IP = 10.77.0.1 |
| `APP_POOL_START/END` | 200 / 230 | DHCP 地址池（31 个地址），数量 ≤ 100 |
| `APP_LEASE_SECONDS` | 3600 | 租约时长 |
| `APP_MONITOR_PERIOD_MS` | 5000 | 状态自检周期 |
| `APP_DISCOVERY_PORT` | 4010 | 设备发现 UDP 端口，PC 模拟器必须一致 |
| `APP_DEV_MAX` | 16 | 设备表槽位数（满了回收最久离线的槽） |
| `APP_DISCOVER_BROADCAST_MS` | 5000 | 兜底广播周期 |
| `APP_IDENTIFY_RETRY_MS` | 1000 | 未识别设备的快探间隔 |
| `APP_IDENTIFY_FAST_TRIES` | 3 | 快探次数（约 3 s），之后转慢探 |
| `APP_IDENTIFY_SLOW_MS` | 10000 | 未识别设备的慢探间隔（PC/非 OTA 设备走这档） |
| `APP_POLL_INTERVAL_MS` | 5000 | 已识别设备心跳间隔 |
| `APP_DEV_OFFLINE_MISS` | 3 | 连续几次无应答判离线 |
| `APP_OFFLINE_PROBE_MS` | 30000 | 已识别设备判离线后的降级探测间隔（复活通道） |
| `APP_OTA_PORT` | 4011 | 升级 TCP 端口（设备 listen，工具 connect） |
| `APP_OTA_CONNECT_TIMEOUT_MS` | 3000 | 建连超时 |
| `APP_OTA_T_ACK_MS` | 2000 | 等 BEGIN_ACK / DATA_ACK 的超时 |
| `APP_OTA_T_RESULT_MS` | 30000 | 等 RESULT 的超时（设备要算整包 CRC + SHA-256） |
| `APP_OTA_N_RETRY` | 3 | 单帧重传次数 |
| `APP_OTA_DEFAULT_CHUNK` / `APP_OTA_WINDOW` | 1024 / 4 | 默认分片与窗口上限（实际以设备上报的为准） |
| `APP_OTA_T_REBOOT_MS` | 60000 | REBOOT 后等目标版本上线的上限 |
| `APP_OTA_SYNTH_SIZE_KB` | 256 | 命令行默认生成的合成镜像大小 |

## 升级会话（阶段 5，TCP 4011）

工具是 **TCP 客户端**，设备 `listen(4011)`。一次会话：

```
connect → OTA_BEGIN(64B)  ──►  ◄── BEGIN_ACK(12B，设备报 chunk/window)
        → OTA_DATA(4+N) ×窗口 ──►  ◄── DATA_ACK(5B，累计 next_offset)
        → OTA_END          ──►  ◄── OTA_RESULT(6B)
        → REBOOT           ──►  （设备不回应答，直接重启）
        → 等设备重新上线  ◄── QUERY_INFO 回报的版本 == 目标版本 → 成功
```

**成功只看最后一步**：`OTA_RESULT(OK)` 只说明"写完了、校验过了"，跳转失败 / 看门狗复位 /
回滚都会让它变成假成功。所以 `do_verify()` 必须等到设备自报目标版本才算过。

### 串口命令（UI 出来前的入口）

```
help            命令列表
dev             设备表（编号与日志里的 [n] 一致）
status          当前/上一次会话：状态、进度、错误码、用时
ota <n> [KB]    对第 n 台发起升级，合成镜像，默认 256 KB（16~2048）
```

`ota 0 256` 之后：

```
I (xxxx) ota_client: 开始升级 54:ee:75:a3:47:aa（SIM-LED-4CH，fw=1.0.0.1，256 KB）
I (xxxx) ota_image: 镜像 'synthetic-256K' model='SIM-LED-4CH' hw=1.0~1.0 fw=1.0.0.1 size=256 KB crc32=0x...
I (xxxx) ota_client: 协商成功：session=0x2b9c53e2 chunk=1024 window=4 续传起点=0
I (xxxx) ota_client: 进度 10%（26214/262144 B）
...
I (xxxx) ota_client: 设备校验通过
I (xxxx) ota_client: 已请求设备重启，等它重新上线…
I (xxxx) ota_client: 设备已用新固件上线：fw=1.0.0.1
I (xxxx) ota_client: 会话结束：成功 —— 升级成功（设备已用新固件上线）
```

PC 侧同时启动模拟器即可（模拟器现在也扮演设备侧 TCP 服务）：

```powershell
cd ..\tools
python ota_sim.py --fw 1.0.0.0
# 故障注入验证：
python ota_sim.py --drop-ack 2        # 丢 ACK → 工具超时重发 → 设备幂等补 ACK
python ota_sim.py --fail-at 65536     # 写 Flash 失败 → 工具报 0x08
python ota_sim.py --bad-model         # 型号不符 → 工具报 0x01
python ota_sim.py --chunk 256 --window 2
```

### 设备端实现时必须注意的两条

1. **重复 `OTA_DATA` 必须幂等**：`offset < next_offset` 时丢弃数据、立刻回
   `DATA_ACK(next_offset, OK)`。工具的重传是"整窗从确认点重发"，重复帧是常态。
   不做这条的后果实测过：丢一个 ACK 就永久失败（设备回 `E_OFFSET`）。
2. **`LEN` 上限是 1028 不是 1024**：`OTA_DATA` 载荷 = 4 B offset + N，N 最大 1024。

## 三个容易踩的坑（代码里已规避）

1. **netif 必须带 `ESP_NETIF_DHCP_SERVER` 标志。**
   默认的 `ESP_NETIF_INHERENT_DEFAULT_ETH()` 是 `DHCP_CLIENT`，此时 `dhcps` 句柄为 NULL，
   所有 dhcps API 返回 `ESP_ERR_ESP_NETIF_INVALID_PARAMS`。

2. **静态 IP 用 `base.ip_info` 注入，不要再调 `esp_netif_set_ip_info()`。**
   在 DHCP_SERVER 模式下该函数要求 dhcps 处于 STOPPED，否则返回
   `ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED`。

3. **`esp_netif_dhcps_start()` 可能"假成功"。**
   netif 未 up 时它返回 `ESP_OK` 但只是把状态置回 INIT。
   所以 `net_dhcps_ensure_started()` 一定会回读 `esp_netif_dhcps_get_status()` 确认 `STARTED`，
   `app_main` 的监控任务每 5 s 兜底重试。

4. **地址池 ≠ 可挂设备数。**
   租约表大小由 `CONFIG_LWIP_DHCPS_MAX_STATION_NUM` 决定，IDF 默认只有 **8**；
   池里写 100~150（51 个地址）也不会让你同时挂 51 台。
   已在 `sdkconfig.defaults` 调到 16，需要更多就继续调大。

## 下一步

- 阶段 6：设备端 Bootloader（**设备固件团队实现**，本工程只提供协议与 `ota_sim.py` 参考）
- 阶段 7：固件导入到 `storage` 分区（SPIFFS/FAT，11 MB）+ `.fwp` 包头解析，
  把 `ota_image_make_synthetic()` 换成读文件即可，`ota_client.c` 一行不用改
- 阶段 8：LVGL 设备列表页（数据源就是 `dev_table_snapshot()` 与 `ota_client_get_status()`，
  串口命令可以直接删掉）
