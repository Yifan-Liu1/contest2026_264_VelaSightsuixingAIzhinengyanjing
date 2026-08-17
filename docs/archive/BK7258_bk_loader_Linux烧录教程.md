# BK7258 使用 bk_loader 在 Linux 下烧录固件

> 归档状态：早期手工烧录说明，仅用于故障排查和历史参考。当前交付流程优先使用
> 仓库根目录 `autoflash.sh`，并以其参数和校验结果为准。

本文介绍如何在 Linux 环境中使用 `bk_loader` 命令行工具，为 BK7258 开发板烧录 Armino SMP 固件。

本文中的烧录命令已在以下环境验证通过：

- 开发板：BK7258
- 烧录工具：`bk_loader` 2.1.11.8
- 操作系统：Linux
- 串口：`/dev/ttyUSB0`
- 波特率：115200
- 固件：`all-app.bin`

## 1. 准备工作

需要准备以下内容：

- BK7258 开发板
- USB 转串口工具，推荐 CH340
- Linux 电脑
- Armino SMP SDK 及已编译完成的固件
- `bk_loader` Linux 可执行文件

烧录时使用开发板的 `DL_UART0` 接口。烧录完成后，启动日志和 Armino CLI 通常也通过这个串口输出。

## 2. 获取烧录工具

将 `bk_loader` 放在一个便于访问的目录中，例如：

```bash
mkdir -p ~/tools/bk_loader
cp /path/to/bk_loader ~/tools/bk_loader/
cd ~/tools/bk_loader
```

如果工具位于当前目录，直接进入该目录即可：

```bash
cd ~/下载
```

检查文件类型：

```bash
file ./bk_loader
```

正常情况下应显示 Linux x86-64 ELF 可执行文件。

## 3. 添加执行权限

首次使用时，为 `bk_loader` 添加执行权限：

```bash
chmod +x ./bk_loader
```

检查工具是否可以运行：

```bash
./bk_loader --help
./bk_loader --version
```

常用子命令如下：

```text
download    烧录固件
erase       擦除 Flash
read        读取 Flash
tool        工具转换功能
```

## 4. 编译 Armino 固件

Armino 编译完成后，BK7258 AP 工程的完整烧录镜像通常位于：

```text
build/bk7258/app/package/all-app.bin
```

本教程使用的固件路径为：

```text
/home/mi/vela_competition/bk_avdk_smp/build/bk7258/app/package/all-app.bin
```

先确认固件存在：

```bash
ls -lh "/home/mi/vela_competition/bk_avdk_smp/build/bk7258/app/package/all-app.bin"
```

如果找不到该文件，需要先完成工程配置和编译。不要把 SDK 工程目录或其他单独的 bin 文件误作为 `all-app.bin` 烧录，除非工程文档明确要求使用其他镜像。

## 5. 检查串口

连接开发板后，查看 Linux 是否识别到 USB 串口：

```bash
ls -l /dev/ttyUSB* /dev/ttyACM*
```

本教程验证时识别到：

```text
/dev/ttyUSB0
```

`bk_loader` 的 `-p` 参数填写串口编号，而不是完整设备路径：

| Linux 设备 | `-p` 参数 |
| --- | --- |
| `/dev/ttyUSB0` | `-p 0` |
| `/dev/ttyUSB1` | `-p 1` |
| `/dev/ttyACM0` | 需根据工具支持情况确认，优先使用 `/dev/ttyUSB0` |

如果没有识别到串口，可以重新插拔 USB 转串口工具，或执行：

```bash
dmesg | tail -n 30
```

## 6. 连接开发板

将 USB 转串口连接到开发板的 `DL_UART0`：

- USB 转串口 TX 连接开发板 RX
- USB 转串口 RX 连接开发板 TX
- GND 连接开发板 GND
- 使用 3.3 V 电平串口

烧录前让开发板进入下载模式。不同开发板的按键名称可能不同，通常需要按住下载键后按一下复位键，或者按开发板说明进行上电操作。

## 7. 烧录固件

### 7.1 已验证命令

以下命令已经在 Linux 环境中验证通过：

```bash
cd ~/下载

./bk_loader download \
  -p 0 \
  -b 115200 \
  -s 0 \
  -i '/home/mi/vela_competition/bk_avdk_smp/build/bk7258/app/package/all-app.bin'
```

参数说明：

- `download`：执行固件烧录
- `-p 0`：使用 `/dev/ttyUSB0`
- `-b 115200`：烧录串口波特率为 115200
- `-s 0`：从 Flash 地址 `0x00000000` 开始烧录
- `-i`：指定输入 bin 文件

如果 `bk_loader` 不在 `~/下载`，将命令中的 `./bk_loader` 替换为实际路径，例如：

```bash
~/tools/bk_loader/bk_loader download \
  -p 0 \
  -b 115200 \
  -s 0 \
  -i '/home/mi/vela_competition/bk_avdk_smp/build/bk7258/app/package/all-app.bin'
```

### 7.2 使用整片擦除

如果设备原有固件异常，或者普通烧录后启动不正常，可以增加 `-c` 参数进行整片擦除：

```bash
./bk_loader download \
  -p 0 \
  -b 115200 \
  -s 0 \
  -c \
  -i '/home/mi/vela_competition/bk_avdk_smp/build/bk7258/app/package/all-app.bin'
```

整片擦除会删除 Flash 中已有的内容，可能包括设备配置、校准数据、密钥或其他用户数据。非必要时不要使用 `-c`。

## 8. 烧录完成后的启动日志

烧录完成后：

1. 退出下载模式（如果开发板有相关按键）。
2. 断电再上电，或按一下复位键。
3. 使用串口工具连接 `DL_UART0`。
4. 设置串口参数：

```text
波特率：115200
数据位：8
停止位：1
校验位：无
流控：无
```

正常启动后应能看到开机日志。在串口终端中输入：

```text
help
```

可以查看 Armino CLI 支持的命令列表。

## 9. 常见问题

### 9.1 `Permission denied`

如果运行工具时报权限错误，执行：

```bash
chmod +x ./bk_loader
```

如果访问串口时报权限错误，检查当前用户是否属于 `dialout` 组：

```bash
groups
```

没有 `dialout` 时执行：

```bash
sudo usermod -aG dialout "$USER"
```

注销并重新登录后再试。也可以临时使用 `sudo` 验证：

```bash
sudo ./bk_loader download \
  -p 0 \
  -b 115200 \
  -s 0 \
  -i '/home/mi/vela_competition/bk_avdk_smp/build/bk7258/app/package/all-app.bin'
```

### 9.2 卡在 `Getting Bus...` 或连接失败

依次检查：

- 是否连接到了 `DL_UART0`
- TX 和 RX 是否交叉连接
- GND 是否连接
- 串口电平是否为 3.3 V
- 开发板是否进入下载模式
- 是否有其他串口工具占用 `/dev/ttyUSB0`
- 烧录过程中是否按过一次开发板复位键

可以先关闭串口监视工具，再重新执行烧录命令。必要时重新插拔 USB 转串口工具，并确认串口编号没有变成 `/dev/ttyUSB1`。

### 9.3 烧录成功但没有启动日志

检查以下内容：

- 是否已经退出下载模式
- 是否重新上电或复位
- 串口工具是否连接到 `DL_UART0`
- 串口波特率是否为 115200
- 是否烧录了正确的 `all-app.bin`
- 是否误用了其他芯片或其他工程生成的固件

## 10. 查看命令帮助

需要确认本机工具参数时，执行：

```bash
./bk_loader download --help
./bk_loader erase --help
./bk_loader read --help
./bk_loader --version
```

本教程使用的 `bk_loader` 版本为 `2.1.11.8`。不同版本的参数可能存在差异，实际使用时以本机 `--help` 输出为准。
