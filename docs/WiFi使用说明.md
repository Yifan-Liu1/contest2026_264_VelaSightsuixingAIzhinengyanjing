# Wi-Fi 使用说明

当前固件使用单个 `wlan0`，STA 和 SoftAP 模式互斥运行。连接开发板 UART0 后，
在 CP 控制台执行 `ap_console open` 进入 NSH。

## STA 模式

连接 2.4 GHz 热点：

```sh
ifup wlan0
wapi scan wlan0                    # 看有哪些热点；encode=8000 是开放网络
wapi psk wlan0 <密码> 3            # 开放热点跳过这行
wapi essid wlan0 <2.4G_SSID> 1
renew wlan0                        # 失败就再来一次，见下
ifconfig wlan0
```

`renew wlan0` 用于通过 DHCP 获取热点分配的 IP、掩码和默认网关，不能省略。

**首次 `renew` 可能失败。** 关联完成到能收发 DHCP 之间有一段窗口，实测第一次报
`ERROR: netlib_obtain_ipv4addr() failed`、第二次成功。重试一次再判断。

### 怎么判断真的通了

看 `ifconfig wlan0` 两件事：状态是 `RUNNING`（不是 `UP` 也不是 `DOWN`），以及地址
不再是 `10.0.0.2`——那个是 NuttX 的默认静态地址，没配网时就长这样。

```
wlan0	Link encap:Ethernet HWaddr c8:47:8c:55:6b:ea at RUNNING mtu 1500
	inet addr:10.192.105.127 DRaddr:10.192.104.1 Mask:255.255.254.0
```

**不要用 `ping` 当判据。** 实测环境屏蔽 ICMP：`ping 8.8.8.8` 与
`ping <解析出的 IP>` 都 100% 丢包，而同一时刻 DNS 和 TLS 都是通的。用下面两个之一：

```sh
ping -c 2 www.baidu.com    # 只看它是否打印出解析到的 IP，即 DNS 通了；丢包可以忽略
```

或者进 agent 跑一次真实的 HTTPS：

```
ai_agent
net_test
```

期望输出：

```
[vela_tls] Handshake OK: TLSv1.2 / TLS-DHE-RSA-WITH-AES-256-CBC-SHA
SUCCESS! HTTP Status: 200
```

`net_status` 的 `Network connected: yes` **不能**作为判据：它只检查有没有配上地址，
接口 DOWN 时也会这么说。

## SoftAP 模式

以下配置已在 BK7258 实板验证手机可发现并连接热点：

```sh
wapi disconnect wlan0
ifdown wlan0
ifup wlan0
wapi mode wlan0 3
wapi freq wlan0 6 1
wapi psk wlan0 12345678 3
wapi essid wlan0 VelaSight_AP 1
dhcpd_start wlan0
```

其中：

- `3` 表示 `WAPI_MODE_MASTER` 或 WPA-CCMP。
- channel 固定为 6，可按现场 2.4 GHz 信道占用改为 1 至 13。
- WPA2 密码必须为 8 至 63 字节。
- `dhcpd_start` 会将板端配置为 `192.168.10.1/24`。
- DHCP 地址池为 `192.168.10.2` 起，最多分配 4 个租约。

检查运行状态：

```sh
ifconfig wlan0
wapi show wlan0
```

成功时 `ifconfig` 必须显示 `RUNNING`，`wapi show` 应显示
`WAPI_MODE_MASTER`、目标 ESSID、channel 和 AP MAC。只有 `UP` 而没有
`RUNNING` 表示配置已保存，但 CP 尚未成功启动 beacon。

手机连接：

```text
SSID: VelaSight_AP
密码: 12345678
IP 设置: DHCP/自动
```

停止 SoftAP：

```sh
dhcpd_stop
wapi disconnect wlan0
ifdown wlan0
```

切回 STA：

```sh
ifup wlan0
wapi mode wlan0 2
wapi psk wlan0 <路由器密码> 3
wapi essid wlan0 <2.4G_SSID> 1
renew wlan0
ifconfig wlan0
```

## 故障排查

- `dhcpd_start: command not found`：当前 AP 分区不是启用 SoftAP DHCP 的最新
  `nsh` 镜像。`help` 的 `Builtin Apps` 必须包含 `dhcpd_start` 和 `dhcpd_stop`。
- 手机扫描不到热点：先检查 `ifconfig wlan0` 是否为 `RUNNING`；`wapi show`
  只能证明配置已保存，不能单独证明热点已经发 beacon。
- 重复启动 DHCP 前先执行 `dhcpd_stop`，避免旧服务仍占用 UDP 端口 67。
- SoftAP 模式不支持与 STA 并发；切换角色前必须停止当前角色。

## 注意

- 不要把真实 Wi-Fi 密码提交到仓库或测试日志中。开放热点不涉及这个问题。
- 示例 SoftAP 密码仅用于开发测试，产品使用时必须替换。
- 当前的 HTTPS 是加密但未验证证书的（无 RTC/SNTP，且 `VERIFY_OPTIONAL`），详见
  `docs/8.16基础适配门禁验收记录.md` 第 7 节。
