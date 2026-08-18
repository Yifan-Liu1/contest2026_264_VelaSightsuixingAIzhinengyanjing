# SD-NAND 使用说明

板载 SD-NAND 是持久存储，默认挂载到 `/mnt/sdnand`。系统启动约 5 秒后会自动
挂载已有 FAT 文件系统。`/mnt` 本身保持伪文件系统——NuttX 不允许在一个真实文件
系统内部再挂载，所以把 PSRAM 临时盘挂在 `/mnt` 上会让 `/mnt/sdnand` 挂载失败
（`error=-20`）。临时盘现在挂在 `/mnt/ram`，重启后内容会丢失。

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
echo hello > /mnt/sdnand/test.txt
cat /mnt/sdnand/test.txt
mv /mnt/sdnand/test.txt /mnt/sdnand/demo.txt
rm /mnt/sdnand/demo.txt
```

当前精简 NSH 的 `printf` 默认不追加换行。例如
`printf hellon > /mnt/sdnand/test.txt` 写入的是字面值 `hellon`，不是 `hello` 加
换行。通过 `printf` 生成的无末尾换行文件使用 `cat` 时，内容可能在下一次 `nsh>`
提示符刷新时显示为空白；这是控制台显示现象，不影响文件内容或 SD-NAND 读写。
普通文本行建议使用 `echo` 写入；需要核对实际字节时使用：

```sh
hexdump /mnt/sdnand/test.txt
```

该 NSH 的 `hexdump` 不支持 GNU/Linux 的 `-C` 参数。

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
