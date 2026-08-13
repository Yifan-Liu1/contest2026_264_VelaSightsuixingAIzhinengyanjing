# Wi-Fi 使用说明

当前固件使用手工配网。进入 AP 的 NSH 后执行：

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

## 怎么判断真的通了

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

## 注意

- 不要把真实 Wi-Fi 密码提交到仓库或测试日志中。开放热点不涉及这个问题。
- 当前的 HTTPS 是加密但未验证证书的（无 RTC/SNTP，且 `VERIFY_OPTIONAL`），详见
  `8.16基础适配门禁验收记录.md` 第 7 节。
