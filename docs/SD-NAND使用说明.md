# SD-NAND 使用说明

板载 SD-NAND 是持久存储，默认挂载到 `/mnt/sdnand`。系统启动约 5 秒后会自动
挂载已有 FAT 文件系统；`/mnt` 本身是 PSRAM 临时盘，重启后内容会丢失。

进入 AP 的 NSH 后查看状态和容量：

```sh
sdnand_init status
df -h
ls -l /mnt/sdnand
```

正常状态应显示 `mounted=yes`。持久数据建议分别放在：

```text
/mnt/sdnand/ai_agent    AI Agent 数据
/mnt/sdnand/captures    照片、录音等文件
```

常用文件操作：

```sh
printf "hello\n" > /mnt/sdnand/test.txt
cat /mnt/sdnand/test.txt
mv /mnt/sdnand/test.txt /mnt/sdnand/demo.txt
rm /mnt/sdnand/demo.txt
```

手工挂载或卸载：

```sh
sdnand_init unmount
sdnand_init mount
```

## 格式化警告

以下命令会清空整块 SD-NAND，并重建 FAT32，仅在确认不需要原有数据时执行：

```sh
sdnand_init provision --confirm
```

普通启动挂载失败时只会报错，不会自动格式化。当前存储使用 1-bit PIO 和 CMD24
单块写，不支持 DMA、4-bit 或 CMD25 多块写。
