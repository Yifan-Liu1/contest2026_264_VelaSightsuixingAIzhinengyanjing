# AP 模式简易配网 Web

板子进入 SoftAP 后调用一个入口，手机连上热点打开网页填 SSID 和密码，凭据原子写入
SD-NAND，应用通过回调知道「第 N 次保存成功」。

**本服务不切换 Wi-Fi 模式。** 不调用 `wapi`、不 `ifup/ifdown`、不关联、不启停 DHCPD。
进 SoftAP 和切回 STA 全部由应用或人工完成——只有应用知道什么时候可以让网络断开。

## 应用侧用法

```c
#include "velasight_provisioning.h"

static void on_saved(int status, uint32_t generation, void *arg)
{
  if (status < 0)
    {
      /* 写盘失败，凭据没存下来 */
      return;
    }

  /* 页面已经发完、连接已经关闭，这里可以安全地切回 STA */
  struct velasight_prov_credentials_s cred;

  if (velasight_provisioning_load(&cred) == 0)
    {
      /* cred.ssid / cred.password / cred.open_network */
      velasight_provisioning_stop();   /* 从回调里调用是安全的 */
      /* 这里做你自己的 STA 切换与关联 */
    }
}

struct velasight_prov_config_s cfg =
{
  .port       = 0,          /* 0 → 80 */
  .one_shot   = false,      /* true = 成功保存一次后自动停止 */
  .store_path = NULL,       /* NULL → CONFIG_VELASIGHT_PROVISION_STORE */
  .on_saved   = on_saved,
  .cb_arg     = NULL,
};

velasight_provisioning_start(&cfg);
```

| 函数 | 说明 |
|---|---|
| `velasight_provisioning_start(cfg)` | 绑定端口并启动监听线程。已在运行返回 `-EALREADY`；`cfg` 可为 NULL 表示全默认 |
| `velasight_provisioning_stop()` | 关闭监听并回收线程。未运行返回 `-EALREADY`。可在回调里调用 |
| `velasight_provisioning_is_running()` | 是否在监听 |
| `velasight_provisioning_generation()` | 本次运行内成功保存的次数，未保存过为 0 |
| `velasight_provisioning_load(out)` | 读取凭据。`-ENOENT` 未配网，`-EBADMSG` 记录损坏 |
| `velasight_provisioning_load_from(path, out)` | 指定路径读取 |

回调只带状态码和序号，不带凭据：要凭据请显式 `load()`，这样密码不会顺着每一个只想收
通知的回调链路走一遍。回调固定在**响应发完并关闭连接之后**触发——反过来会在应用切走
Wi-Fi 时截断响应，手机上看到的是「提交失败」而实际已经存了。

监听线程是 pthread，pthread 不会比创建它的任务活得更久，所以应用要在自己的长驻任务里
调 `start()`。

## 手工上板验证

先切 SoftAP（详见 `docs/WiFi使用说明.md`）：

```sh
ifdown wlan0
ifup wlan0
wapi mode wlan0 3
wapi freq wlan0 6 1
wapi psk wlan0 <8-63字节密码> 3
wapi essid wlan0 VelaSight_AP 1
dhcpd_start wlan0            # 板子成为 192.168.10.1
```

再起配网页面：

```sh
provision_web run            # 前台运行，Ctrl-C 停止
provision_web run 8080       # 换端口
provision_web run --one-shot # 成功保存一次后返回
```

手机连上 `VelaSight_AP` 后浏览器打开 `http://192.168.10.1/`，填 SSID 和密码提交。

查看已存内容（**不打印密码**）：

```sh
provision_web show
# provision_web: ssid=AIPC security=wpa2 password=set generation=1
provision_web path
# /mnt/sdnand/prov/wifi.bin
```

没有手机或 Wi-Fi 客户端时，用板子自己的回环把整条链路走一遍（会用测试网络覆盖已存
记录，不需要进 SoftAP）：

```sh
provision_web selftest
# provision_web selftest: PASS, 14 checks, 0 failures
reboot
provision_web show      # 证明记录跨重启存活
```

验完切回 STA，仍然按 `docs/WiFi使用说明.md`：

```sh
dhcpd_stop
ifdown wlan0
ifup wlan0
wapi mode wlan0 2
wapi psk wlan0 <路由器密码> 3
wapi essid wlan0 <2.4G_SSID> 1
renew wlan0
```

## HTTP 表面

| 请求 | 响应 |
|---|---|
| `GET /`、`GET /index.html` | 200 表单页 |
| `POST /save`（`application/x-www-form-urlencoded`） | 200 成功页 / 400 校验失败 / 500 写盘失败 |
| 其他方法 | 405 |
| 其他路径 | 404 |
| 缺 `Content-Length` | 411 |
| body > 512 字节 | 413 |
| 非表单 Content-Type | 415 |
| 请求头 > 2048 字节 | 431 |
| `Transfer-Encoding: chunked` | 400 |

读超时 5 秒，一次只服务一个连接，每个连接答完即关闭。成功页只回显经过 HTML 转义的
SSID；**密码永不回显、永不进日志**。

校验规则：SSID 1–32 字节且不含控制字符；密码留空表示开放网络，否则必须 8–63 字节的
可打印 ASCII。表单缺 `ssid` 直接 400；`ssid` 或 `password` 重复出现按歧义拒绝，不猜。

## NAND 记录

默认 `/mnt/sdnand/prov/wifi.bin`，固定 114 字节，小端：

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 4 | 魔数 `VSWP` |
| 4 | 2 | 版本，当前 1 |
| 6 | 2 | 标志位，bit0 = 开放网络 |
| 8 | 4 | generation |
| 12 | 1 | ssid_len，1..32 |
| 13 | 1 | psk_len，0 或 8..63 |
| 14 | 32 | SSID，补零 |
| 46 | 63 | 密码，补零 |
| 109 | 1 | 保留，必须 0 |
| 110 | 4 | CRC32（IEEE，覆盖 0..109） |

落盘顺序是同目录的 `vpsave.tmp` → `fflush` → `fsync` → `close` → `rename` → `sync()`。
若目标文件缺失但 scratch 是完整且 CRC 有效的记录，下一次读取会先将 scratch 提升为正式
记录；损坏 scratch 按 `-EBADMSG` 拒绝，不会被提升。
魔数、版本、两个长度、开放标志与 `psk_len` 的自洽性、补零区、保留字节和 CRC 逐项校验，
任何一项不过按 `-EBADMSG` 处理，不做「尽力恢复」。

只保留最新一份，提交即替换，generation 递增。凭据不镜像到 kvdb——需要的话由应用读出后
自己写。

## 三条平台约束（都是上板实测撞出来的）

改动这三处之前先读这一节，否则会重现同样的故障：

1. **`/mnt` 必须保持伪文件系统。** PSRAM ramdisk 挂在 `/mnt/ram`，不是 `/mnt`。
   NuttX 不允许在真实文件系统内部再挂载，`/mnt` 上挂了 littlefs 之后 SD-NAND 的
   `/mnt/sdnand` 会以 `-ENOTDIR` 挂载失败，持久存储静默消失。
2. **`CONFIG_NET_TCPBACKLOG=y` 是必需项。** 没有它 listening socket 上不会有
   `POLLIN`，`accept()` 也无超时，可停止的监听线程无法实现；症状是打印了 listening
   之后什么都不回。本模块 Kconfig 已 `depends on` 它，配置错误会在编译期暴露。
3. **SD-NAND 是不带长名的 VFAT。** `CONFIG_FAT_LFN` 未开，路径每一段必须符合 8.3。
   `velasight/`、`wifi-provision.bin` 都会被 `-EINVAL` 拒绝，把 `.tmp` 追加到
   `wifi.bin` 得到两个点也非法——所以临时文件是固定名 `vpsave.tmp`。主机测试会逐段
   校验默认路径的 8.3 合规性。

VFAT 上 `rename` 不是单步原子：NuttX 的 VFS 先 unlink 目标，FAT 的 `rename` 拒绝覆盖。
所以存在「旧记录已删、新记录未就位」的极短窗口，此时读到 `-ENOENT`；读不到截断记录，
因为长度和 CRC 都会拒绝。

## 安全边界

- **服务是明文 HTTP 且无鉴权。** 热点覆盖内的任何设备都能提交。缓解手段：SoftAP 带
  WPA2 密码、只在配网期间开启、配完 `stop()`，或者用 `one_shot`。
- **密码在 NAND 上是明文。** VFAT 没有可信的权限模型，这颗芯片也没有已确认可用的设备
  密钥来源，所以不假装做静态加密。可控的部分是：不写日志、不回显、只走公共 API 读取。
  物理拿到 SD-NAND 就能读到密码。
- 不要把真实密码写进仓库、提交信息或测试日志。

## 测试

```sh
cd app/provisioning_web/tests && make        # 表单/记录/HTTP/服务器
cd app/web_tool/host/tests   && make         # 全量回归，已包含上面这一组
```

`vp_form.c`、`vp_store.c`、`vp_http.c` 不含任何 NuttX 头文件，`vp_server.c` 只用 POSIX
socket，所以连 accept 循环、回调时序和 one-shot 都能在主机上跑完，剩给真板的只有
「SoftAP 和 SD-NAND 是否真的在」。

## 文件

| 文件 | 职责 |
|---|---|
| `include/velasight_provisioning.h` | 公共 API 与凭据结构 |
| `vp_form.c/.h` | URL 解码与字段校验 |
| `vp_store.c/.h` | 记录编解码、CRC32、原子落盘 |
| `vp_http.c/.h` | 请求解析、页面生成、HTML 转义 |
| `vp_server.c` | 监听线程、连接处理、生命周期与回调时序 |
| `provisioning_web_main.c` | `provision_web` 命令 |

设计文档：`docs/superpowers/specs/2026-08-18-ap-provisioning-web-design.md`
