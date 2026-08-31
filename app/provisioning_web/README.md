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

  /* 页面已经发完、连接已经关闭。可以继续保持 SoftAP，也可以按产品
   * 状态在这里请求切回 STA。 */
  struct velasight_prov_credentials_s cred;

  if (velasight_provisioning_load(&cred) == 0)
    {
      /* cred.ssid / cred.password / cred.open_network
       * cred.cloud_host / cred.cloud_port / cred.cloud_path
       *   —— 空 host 或 0 端口表示「用编译期默认」，不是「未配置」 */
      /* 这里不必停止服务；用户可以返回表单继续提交。 */
    }
}

struct velasight_prov_config_s cfg =
{
  .port       = 0,          /* 0 → 80 */
   .one_shot   = false,      /* false = 保存后继续提供配网页面 */
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
  provision_web run --one-shot # 测试用：成功保存一次后返回
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
| `GET /`、`GET /index.html` | 200 设置页 |
| `POST /`、`POST /index.html`（`application/x-www-form-urlencoded`） | 200 设置页 + 已保存提示 / 400 设置页 + 待修正提示 / 500 写盘失败 |
| `GET /history`、`GET /history/<key>`、`GET /history/<key>/download` | 配置了历史 provider 时 200，否则 404 |
| 其他方法 | 405 |
| 其他路径 | 404 |
| 缺 `Content-Length` | 411 |
| body > 2048 字节 | 413 |
| 非表单 Content-Type | 415 |
| 请求头 > 2048 字节 | 431 |
| `Transfer-Encoding: chunked` | 400 |

没有独立的 `/save`：表单回帖到自己所在的地址，所以整个服务只有一个 URL，底部标签栏
指向的永远是用户实际所在的页面。代价是提交结果页刷新会触发浏览器的重复提交确认——换成
303 重定向可以消掉它，但那样保存结果和改动清单就没地方显示了。

读超时 5 秒，一次只服务一个连接，每个连接答完即关闭。页面回显经过 HTML 转义的
Wi-Fi 名称和社交云端点；**密码与三个密钥永不回显、永不进日志**，页面上只显示
"已填写／还没填写"。端点是唯一按明文渲染的已存值，理由见下。

校验规则：Wi-Fi 名称 1–32 字节且不含控制字符；密码 8–63 字节可打印 ASCII。
表单缺 `ssid` 直接 400；同一字段重复出现按歧义拒绝，不猜。

### 社交云端点

社交模式对接的是项目组自建的 `/contest/v1` 服务，与上面的 MiMo、Volcengine 是三套
无关的东西。端点拆成三个字段而不是一条 URL，因为三者各自被独立消费，再把 URL 拆回来
只会多一处让 scheme、默认端口和结尾斜杠互相矛盾的地方：

| 字段 | 含义 | 校验 |
|---|---|---|
| `cloud_host` | 域名或点分十进制，不带 scheme、端口、路径 | 仅 `[A-Za-z0-9.-]`，首尾不能是 `.` 或 `-` |
| `cloud_port` | TCP 端口 | 纯数字 1–65535；`0` 按非法拒绝 |
| `cloud_path` | `/contest/v1` 之前的前缀 | 以 `/` 开头、结尾不加 `/`、无连续 `//`，字符集为 URI unreserved 加 `/` `%` |

路径前缀不是可选的装饰。云端接口文档的示例是按 `127.0.0.1:18080/contest/v1` 写的，而
实际部署应答在 `/hlthopen/public/contest/v1`，不带前缀的路径会返回空的 204 —— 设备侧
会把它报成协议错误，让看日志的人去找一个并不存在的 JSON 解析 bug。

`cloud_host` 的字符集比密钥字段严格，有两个独立原因：这个值会被拼进 `Host` 头并交给
`connect()`，空格或斜杠会劈开请求头或悄悄改变实际连接的主机；同时它排除了 HTML 转义
会展开的那五个字符（`& < > " '`），这让 `vp_http_page()` 的转义暂存可以按原长而不是
六倍来分配。`cloud_path` 同理。**放宽这两个校验会溢出那两个缓冲区**，
`test_vp_form.c` 里有一条专门盯着这件事的断言。

端点按明文渲染是对"密钥只显示已填写"那条规则的有意例外：地址不是秘密，而且这是页面
唯一有用的形态 —— 排查社交模式连不上时，需要看到设备实际在用哪个主机，显示"已填写"
等于什么都没说。

### 空字段的含义

凭据类字段留空表示"不改动"，端点类字段留空表示"回到出厂默认"。这是本模块最容易踩的
地方，两类的差别来自表单是否回填：

| 字段 | 留空提交 |
|---|---|
| `ssid` | 拒绝。名称是回填的，清空它是明确动作 |
| `password` | 沿用已存密码与"有无密码"标志（成对拷贝）；没有已存记录时拒绝 |
| `no_password`（复选框） | 勾选且密码栏为空时才把网络记成无密码，**这是清空已存密码的唯一通路** |
| `mimo_apikey` / `volc_appid` / `volc_token` | 沿用已存值。这三项不回填，所以空框只能是"没动它" |
| `cloud_host` / `cloud_port` / `cloud_path` | 清空为默认。这三项会回填，所以空框是明确的清除动作 |

端点这一栏还要区分"提交了空值"和"字段整个没出现"：前者是用户清空了输入框，后者是客户端
根本没提供这个框（缓存的旧表单、手写的请求）。前者清除，后者沿用已存值，判断依据是
`struct vp_form_submit_s` 里的 `have_cloud_*`，而不是值本身是否为空。

密码与 `open_network` 必须成对搬运：`vp_credentials_validate()` 要求两者一致，只搬一个
会产出自相矛盾的记录，读取方信哪个字段就得到哪种网络。合并必须发生在校验之前，否则
"空密码 + 未勾选"这一步会在合并有机会补齐之前被判非法。

## NAND 记录

默认 `/mnt/sdnand/prov/vela.cfg`，固定 980 字节，小端。该文件是配网网页的唯一配置源：
Wi-Fi 名称、Wi-Fi 密码、MiMo API key、闲时语音助手用的 Volcengine（字节跳动语音开放
平台）app_id/token，以及社交模式的云端点全部在此文件中读写。KVDB 是废弃功能，产品
链路不使用。

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 4 | 魔数 `VSWP` |
| 4 | 2 | 版本，当前 4 |
| 6 | 2 | 标志位，bit0 = 开放网络 |
| 8 | 4 | generation |
| 12 | 1 | ssid_len，1..32 |
| 13 | 1 | psk_len，0 或 8..63 |
| 14 | 32 | SSID，补零 |
| 46 | 63 | 密码，补零 |
| 109 | 512 | MiMo API key，补零 |
| 621 | 64 | Volcengine app_id，补零 |
| 685 | 128 | Volcengine token，补零 |
| 813 | 96 | 社交云 host，补零 |
| 909 | 64 | 社交云路径前缀，补零 |
| 973 | 2 | 社交云端口，0 表示用默认 |
| 975 | 1 | 保留，必须 0 |
| 976 | 4 | CRC32（IEEE，覆盖 0..975） |

Volcengine 的 app_id 和 token 缺一个都视为未配置：闲时语音助手会在两者都非空才尝试
识别/合成，否则报"语音服务凭据未配置"。两者都是可选字段，不填不影响 Wi-Fi 和文字/
图片问答（那两项只依赖 MiMo API key）。

社交云的三个字段可以全为空/0，那不是"未配置"而是"用编译期默认值"。默认值由
`CONFIG_VELASIGHT_PROVISION_CLOUD_HOST` / `_PATH` / `_PORT` 三个 Kconfig 项给出，
默认指向接口文档写明的部署。设置页把同一组宏当作输入框的灰字占位符，所以页面宣称的
出厂默认和 `vs_cloud_init()` 实际回退的地址是同一个值，不会各说各话。

v3 在 MiMo key 和保留字节之间插入了两个 Volcengine 字段；v4 在同一位置又插入了三个
社交云端点字段。每一次都是破坏性变更：旧版本的记录读不出来，`vp_record_decode()` 按
"结构不对就是坏的"处理，不做部分恢复，跨版本升级的设备需要重新走一次配网。端点字段
放在凭据之后而不是中间，是为了让 813 之前的偏移与 v3 保持一致 —— 排查迁移问题时
两个版本的十六进制转储在共有字段上能对齐。

记录里的端点如果通不过当前的校验（例如 host 里有斜杠），`vp_record_decode()` 返回
`-EBADMSG` 而不是把它交出去。理由和凭据字段一样：结构完好但内容是运行代码会拒绝的
值，交出去只会让调用方拿到一个用不了的东西。

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
cd app/provisioning_web/tests && make        # 表单/记录/HTTP/服务器/端点
cd app/web_tool/host/tests   && make         # 全量回归，已包含上面这一组
```

单跑某一组：`make form` / `store` / `http` / `server` / `endpoint`。

`vp_form.c`、`vp_store.c`、`vp_http.c` 不含任何 NuttX 头文件，`vp_server.c` 只用 POSIX
socket，所以连 accept 循环、回调时序和 one-shot 都能在主机上跑完，剩给真板的只有
「SoftAP 和 SD-NAND 是否真的在」。

`test_vp_endpoint.c` 是个例外，用它之前要知道它的局限：`app/velasight/vs_cloud.c` 在
主机上编不了（依赖 `vela_tls.h`、cJSON、BK7258 PSRAM 头），所以那份文件里的 `resolve()`
是 `cloud_resolve_endpoint()` 的**镜像**而不是它本身，只改一边不会被测出来。它仍然值得
留着：记录编解码和校验器用的是真的，而优先级表是最容易搞错又最难在板子上观察的部分
——设备实际用出厂端点、页面却显示配网端点，在有人去比对之前没有任何症状。

## 文件

| 文件 | 职责 |
|---|---|
| `include/velasight_provisioning.h` | 公共 API 与凭据结构 |
| `vp_form.c/.h` | URL 解码与字段校验 |
| `vp_store.c/.h` | 记录编解码、CRC32、原子落盘 |
| `vp_http.c/.h` | 请求解析、页面生成、HTML 转义 |
| `vp_server.c` | 监听线程、连接处理、生命周期与回调时序 |
| `provisioning_web_main.c` | `provision_web` 命令 |
| `tests/test_vp_endpoint.c` | 社交云端点优先级表（镜像，见「测试」一节） |

设计文档：`docs/superpowers/specs/2026-08-18-ap-provisioning-web-design.md`
