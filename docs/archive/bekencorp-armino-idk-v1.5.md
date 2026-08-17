# 使用 Podman 构建 BK7258

> 归档状态：早期容器构建记录。当前完整 AP/CP 构建、打包和哈希校验流程以
> `docs/固件构建步骤.md` 为准。

`bk_sdk_project.py`已支持通过`EXTERNAL_AP_BIN`链接外部OpenVela AP固件。Podman使用与Docker相同的ARMINO官方镜像，因此仍然使用镜像内的官方工具链，不需要修改宿主机GCC或工具链路径。

### 1. 确认镜像和AP固件

```bash
podman images
```

应能看到类似：

```text
localhost/bekencorp/armino-idk  1.5
```

确认待链接的OpenVela AP固件存在：

```bash
cd "/home/mi/vela_competition/bk_avdk_smp"
ls -lh build/openvela-ap.bin
```

### 2. 使用外部AP构建app_ab

在`bk_avdk_smp`根目录执行：

```bash
podman run --rm \
  --userns=keep-id \
  -v "$PWD:/armino" \
  -w /armino \
  localhost/bekencorp/armino-idk:1.5 \
  make -C projects/app_ab bk7258 \
  SDK_DIR=/armino \
  EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin
```

单行版本：

```bash
podman run --rm --userns=keep-id -v "$PWD:/armino" -w /armino localhost/bekencorp/armino-idk:1.5 make -C projects/app_ab bk7258 SDK_DIR=/armino EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin
```

宿主机和容器内的路径对应关系：

```text
宿主机：/home/mi/vela_competition/bk_avdk_smp/build/openvela-ap.bin
容器内：/armino/build/openvela-ap.bin
```

`EXTERNAL_AP_BIN`必须使用容器内路径，不能使用宿主机绝对路径。

不要再使用：

```bash
./dbuild.sh make -C projects/app_ab ...
```

`dbuild.sh`内部调用`docker`，仅安装Podman时会报：

```text
You don't have docker installed in your path.
```

### 3. 最终固件和烧录

最终完整烧录包：

```text
projects/app_ab/build/bk7258/app_ab/package/all-app.bin
```

烧录命令：

```bash
./bk_loader download \
  -p 0 \
  -b 115200 \
  -s 0 \
  -i "/home/mi/vela_competition/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/all-app.bin"
```
