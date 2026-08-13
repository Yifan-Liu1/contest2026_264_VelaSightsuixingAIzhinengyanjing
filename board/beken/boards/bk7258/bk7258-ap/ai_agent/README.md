# ai_agent on bk7258-ap

What this board needs in order to run `packages/ai_agent`, and which parts of
that belong here rather than upstream.

Build config: `../configs/ai_agent/defconfig` (the current `nsh` baseline --
which already carries the Wi-Fi driver and the IPv4 stack -- plus mbedTLS,
cJSON and the agent itself).

Two things `nsh` has and this config deliberately does not: the V4L2 M2M JPEG
codec (`CONFIG_BK7258_JPEG_ENC`, `/dev/video1`) and on-device libjpeg
(`CONFIG_LIB_JPEG_TURBO`), and with them the `jpeg_test` command, whose Kconfig
`depends on BK7258_JPEG_ENC`. **The camera's JPEG path does not need any of
them** -- capture-side encoding lives in the chip-level
`board/beken/chips/bk7258/bk7258_jpeg_enc.c`, which is compiled
unconditionally, and `bk7258_jpeg_enc_write_header` is present in this config's
`System.map`. `jpeg_test` and the M2M codec exist to exercise the encoder as a
standalone transcoder; the agent has no use for that, so the config leaves them
out rather than spending flash on them.

```sh
cd <openvela work tree root>
sh contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/boards/bk7258/bk7258-ap/ai_agent/apply.sh
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent --cmake -j8
```

`-Werror` cannot be used with this config: `packages/ai_agent` itself does not
build warning-free (see "Upstream defects" below). The `nsh` config keeps
`-Werror`.

## Measured footprint

From the linker's own report (`-Wl,--print-memory-usage`), both configs
rebuilt clean at commit `422bef0`, with all six `app/` linkfiles in place (as
a fresh `repo sync` produces them):

| Region | `nsh` | `ai_agent` | Region size |
|---|---|---|---|
| FLASH | 485184 B (43.55%) | 691008 B (62.02%) | 1088 KB (`FLASH` 0x110000) |
| RAM | 122160 B (35.51%) | 186112 B (54.09%) | 336 KB (`RAM` 0x54000) |
| PSRAM_SECTION | 0 B | 0 B | 6 MB |

`ai_agent` leaves 413 KB of flash spare. The gap to `nsh` is 201 KB, but it is
not a clean "cost of the agent": this config adds mbedTLS, cJSON, the agent and
`agent_camera`, and drops `jpeg_test`. The Wi-Fi driver and the IPv4 stack are
in both, which is why the gap is much smaller than the 371 KB this file used to
record.

Two things make these numbers easy to get wrong, both worth checking before
quoting them: whether the `app/` linkfiles are present (a missing one silently
removes that app, `docs/reference/platform.md` §6), and whether the build
directory was wiped after a defconfig change.

RAM here is the static footprint only, and it is the interesting half: those
186 KB leave the SRAM heap with a 158 KB arena, which is *not* enough to
start the agent. See "`ai_agent` itself" below.

## Camera path

`camera_capture` (`src/tools/tool_camera.c`) is the only agent tool that
touches this board's camera. It opens `/dev/video0`, sets a compressed
format, uses two `V4L2_MEMORY_MMAP` buffers, waits with `poll()` and hands
the frame to a Vision LLM.

What already matches:

- **MMAP buffers land in PSRAM.** `v4l2_cap.c` calls
  `imgdata_ops_s.alloc()` when one exists, and this board's implementation
  takes the buffer from `PSRAM_POOL_DISPLAY` (`bk7258_camera_imgdata.c`).
  That is what makes a 160 KB × 2 buffer heap possible on a board whose AP
  kernel heap is smaller than that, and it is also required: YUV_BUF can
  only write into `0x60000000..0x61000000` (invariant 5 in
  `docs/reference/camera.md`).
- **`sizeimage` decides the buffer size.** For a compressed format
  `get_bufsize()` returns `sizeimage` rather than `width*height`, so the
  tool's 160 KB hint is what gets allocated per buffer.
- **The same MMAP + `poll` + `DQBUF` shape is already proven on this board**
  by `camera_preview`, which uses `V4L2_MEMORY_MMAP` too. Only the pixel
  format differs (UYVY there, JPEG here).

What did not match, and how it was fixed. Both were found by running the
sequence on hardware, and both are now measured rather than argued:

- **The driver rejected the tool's buffer.** `set_buf()` required the buffer
  to be at least `width*height*2`, i.e. a whole raw frame, for every format.
  For a compressed format that is the wrong number: the size is the
  application's choice, because `get_bufsize()` returns `sizeimage` when one
  was set and only computes `width*height` when it was not. The agent asks
  for 160 KB, `480*480` is 225 KB, so every buffer was refused:

  ```
  set_buf: rejected addr=0x601901a0 size=163840 (need >=230400 bytes ...)
  start_capture: frame_buf is NULL, -EINVAL
  agent_camera: no frame within 5000 ms
  ```

  Fixed in `bk7258_camera_imgdata.c`: for JPEG the capacity comes from
  `set_buf()`'s own `size` argument, floored to a whole number of DMA
  transfer steps (the drain channel loops in `transfer_len` steps, so a
  window that is not a multiple of one would let the last step write past
  its end) and required to be at least two steps. For UYVY nothing changed
  -- whole-frame direct capture still needs exactly `width*height*2`.

  This also removed a latent overrun: `frame_bytes` was both the accept
  threshold *and* the DMA wrap bound, so a buffer smaller than
  `width*height` would previously have been drained as if it were that
  large.

- **Geometry.** The tool asks for 320x180 ("low", its default) or 1280x720
  ("high"). This driver programs 480x480 / 640x480 / 864x480 and
  `bk7258_gc2145_find_mode()` matches exactly, so `VIDIOC_S_FMT` refuses
  both and the tool gives up with no way to discover what it should have
  asked for. Fixed by patch 0001: when the requested geometry is refused,
  enumerate `VIDIOC_ENUM_FRAMESIZES` and retry at the closest size the
  driver offers. The requested geometry is still tried first, so devices
  that support it are unaffected.

### Measured on hardware

Firmware built from this config, packaged per `docs/reference/camera.md`
10.2 with the sha256 of `nuttx.bin`, `openvela-ap.bin` and `app1.bin`
verified equal, flashed with `autoflash.sh -b 1500000`.

The V4L2 path works end to end. Auto geometry, one frame -- **this transcript
predates the header fix below**, which is why it reports `SOS=NO`; the ioctl
sequence, the buffer sizes and the frame rates in it are still current:

```
agent_camera: auto-selected 480x480
agent_camera: format JPEG 480x480 sizeimage=163840
agent_camera: buffer[0] len=163840 at 0x601901a0
set_buf: addr=0x601901a0 frame_bytes=163840 (buffer size given=163840)
agent_camera: frame 0: index=0 bytesused=43263 flags=0x00000000
agent_camera: frame 0: index=0 bytesused=42735 flags=0x00000000
agent_camera: structure SOF=yes DQT=yes DHT=yes SOS=NO EOI=yes
agent_camera: NOT a decodable JPEG -- no SOS segment ...
agent_camera: OK
```

33-frame session: 29.78 fps, no dropped frames, no timeouts. **Every fps and
millisecond figure in this file was measured before the 2026-08-13 timebase
fix and is therefore roughly 2x too high** — the real rate at 480x480 is about
16 fps (`docs/reference/camera.md` §6.8 and §14.1). The frame counts, byte
counts and pass/fail results are unaffected; only the time axis was wrong.

> **The bytes were not a decodable JPEG until the header was rebuilt, and
> this README claimed otherwise for a while.** The encoder emits SOI / APP0 /
> SOF0 / DQT×2 / DHT×3 and then entropy data, and **the AC Huffman table it
> writes into the stream is not the table it encoded with** — its two DC
> tables are byte-identical to the JPEG Annex K standard tables, its AC table
> is not, and it emits neither the chroma AC table nor an SOS segment.
> libjpeg's verdict on the raw stream: `Invalid JPEG file structure: missing
> SOS marker`.
>
> Fixed in the driver: `bk7258_jpeg_enc_write_header()` rewrites the header
> in place with the four standard tables plus an SOS, keeping the hardware's
> SOF0 and DQT (both correct). Zero-copy — the DMA writes 256 bytes into the
> buffer and a COM segment absorbs the length difference, so the entropy data
> is never moved and it runs in the ISR. Design and evidence:
> `docs/reference/camera.md` 14.7-14.9.
>
> Verified from the board: `identify` reports
> `JPEG 480x480 8-bit sRGB` with no warnings, PIL decodes, and the render is
> a clear photograph. 30-frame session: 31.56 fps, 30/30 frames structurally
> complete, `hdr_fail=0 short=0`.
>
> So `camera_capture` now hands a Vision LLM a file standard decoders accept.
>
> `SOI` + `EOI` present was the judgement used here, and it is not
> sufficient — `agent_camera`'s marker check and the driver were both
> applying the same inadequate test. `ffmpeg` makes this worse by
> "succeeding": it emits a 480x480 PNG that is uniformly **RGB(0,135,0)**,
> which is just YUV(0,0,0) through BT.601, i.e. no scan data decoded at all.
> That is the lesson worth keeping from this retraction: the acceptance test
> has to be a real decoder, not a marker scan and not a tool that exits 0.

The patch, before and after, on the tool's own default request:

| Command | Result |
|---|---|
| `agent_camera low` (negotiates, = patched tool) | `S_FMT JPEG 320x180 failed: 22` → `320x180 refused, using enumerated 480x480` → `bytesused=13651` → `OK` |
| `agent_camera low strict` (= unpatched tool) | `S_FMT JPEG 320x180 failed: 22`, `S_FMT ENTR 320x180 failed: 22`, `FAILED (-22)` |

So the unpatched `camera_capture` cannot work on this board at all, and the
patched one captures a valid JPEG.

Three frames in one session, first one saved: `bytesused` 13531 / 12535 /
39915, `wrote 13531 bytes to /mnt/cap.jpg`, `ls -l /mnt` reports `13531
cap.jpg`. So streaming, buffer rotation and the file write all work. Those
particular bytes were captured before the header fix, so they were not
decodable; the path is, since `bk7258_jpeg_enc_write_header()`.

The start-up transient, diagnosed: every session begins with exactly
`err=3 resets=3 short=1` and then runs clean — the counts do not grow with
session length (measured at 3, 13 and 33 frames). In JPEG mode the stream is
driven by enabling the encoder, and this board's sensor streams continuously
from init, so the encoder always starts at an arbitrary phase and the first
frame is partial; that is precisely what it reports as `frame_err` (the
reference names the handler `dvp_camera_sensor_ppi_err_handler`, i.e. "the
picture is not the size I was told"). **It never reaches the application** —
a frame without an EOI is completed with an error and not handed up, so the
first frame dequeued is always whole: 33-frame session, 31 frames delivered with
no error flag, zero timeouts, 29.78 fps. That
is what matters here, because `camera_capture` takes exactly one frame.

Waiting for a VSYNC boundary before enabling the encoder was tried and
reverted: the premise is circular (in JPEG mode VSYNC events only start once
the encoder is enabled), the wait timed out in all three sessions, and its
only effect was cost — 29.72 → 25.99 fps. See `docs/reference/camera.md`
14.4.

### `ai_agent` itself

Starts and runs on the board, and reaches its own prompt in 929 ms:

```
[agent] [boot +929ms] AI Agent ready. Type 'help' in NSH for commands.
vela> heap_info
Heap: arena=6449416 fordblks(free)=6110144 uordblks(used)=339272
vela> net_status
Network connected: yes
```

**That arena is 6.4 MB because the heap now includes PSRAM, and without it the
agent does not start at all.** When the upstream sync brought the Wi-Fi driver
and the IPv4 stack in, static RAM went from 97 KB to 186 KB and the SRAM heap
was left with a 158 KB arena, 27 KB free and a 26312-byte largest block --
smaller than the agent task's 32768-byte stack (`AGENT_AI_AGENT_STACK` in
`include/agent_config.h`). The spawn failed and NSH reported it as
`command not found`, which looks exactly like a missing builtin even though
`help` lists it. `bk7258_psram_initialize()` now donates the unused tail of
`PSRAM_SECTION` to the system heap (commit `b96373d`); the layout and what it
costs -- PSRAM is non-cacheable, so some buffers that used to sit in SRAM got
slower -- are in `docs/reference/platform.md` §4.

Flash, at 61%, is not the constraint, and since `b96373d` neither is the heap.

Without a mounted `/mnt` the agent still starts; it only loses persistence
(`[skills] Cannot write skill: /mnt/ai_agent/skills/...`). Be careful about
what is actually missing there: the agent *does* create its data directory
tree, recursively -- `mkdirs()` in `src/infra/config_store.c` and
`ensure_dir()` in `src/core/memory_store.c` both walk the path. What it cannot
do is create a filesystem, and `bk7258_ramdisk.c` registers `/dev/ram0`
deliberately unformatted, so

```sh
mkfatfs /dev/ram0
mount -t vfat /dev/ram0 /mnt
```

is the whole prerequisite -- the `ai_agent/` tree underneath appears by itself
on the next start. (`mkdir /mnt/ai_agent` by hand, which this file used to
ask for, was never the missing step.)

One genuine upstream bug lives next to this: `src/agent_main.c` Phase 0
hardcodes `/data/agent` and even mounts a tmpfs on `/data`, instead of using
`AGENT_DATA_DIR`. On this board that creates five directories nobody reads,
and it is harmless only because `config_store_init()` gets the real path
right. Worth folding into the next upstream patch.

### Using the JPEG path

Three routes, all exercised on hardware. Resolutions are 480x480 / 640x480 /
864x480 only — the sensor driver matches exactly and rejects anything else.

```sh
agent_camera                      # one frame, prints the segment structure
agent_camera b64                  # one frame as base64, to pull it off-board
nxcamera                          # or use the stock tool, fourcc "JPEG":
  input /dev/video0
  output /mnt/n.jpg 1
  stream 480 480 30 JPEG
  q
```

For your own application the sequence is the one in
`app/agent_camera/agent_camera_main.c`, which is also what
`packages/ai_agent`'s `tool_camera.c` does: `S_FMT`
(`V4L2_PIX_FMT_JPEG`, set `sizeimage`) → `REQBUFS` 2 MMAP → `QUERYBUF` +
`mmap` → `QBUF` → `STREAMON` → `poll` → `DQBUF`. `bytesused` is the length of
a complete JPEG file starting at byte 0 of the buffer: the driver has already
written a standard header in front of the entropy data, so nothing needs to be
added afterwards. Details, including why `sizeimage` matters, are in
`docs/reference/camera.md` 14.3.

### Reproducing any of this: `app/agent_camera`

`camera_capture` can only be reached through the LLM (the agent has no CLI
command that invokes a tool directly), so it cannot be triggered on a board
with no network. `app/agent_camera` exists to run the half that the board is
responsible for: the tool's ioctl sequence verbatim, then SOI/EOI and byte
count validation instead of an LLM post. A failure therefore separates into
"capture path" versus "geometry" rather than one opaque error.

```sh
agent_camera caps                 # what the driver enumerates
agent_camera                      # auto geometry, one frame, structure checked
agent_camera low                  # the tool's default request, negotiated
agent_camera low strict           # the same request without negotiation
agent_camera b64                  # print the frame as base64 to retrieve it
agent_camera n=5 out=/mnt/cap.jpg # five frames, first saved (needs a mount)
```

To get a frame onto the host and check it with a real decoder — which is the
only way to catch what the marker check missed — see
`docs/reference/camera.md` 14.5.

Its negotiation is the same algorithm as patch 0001, so `low` versus
`low strict` is a before/after of that patch against the real driver.

## Network

**The AP has a real network device**, so the claim this section used to make --
no netdev, `CONFIG_NET` with loopback only, porting the host driver is out of
scope -- is obsolete. `board/beken/chips/bk7258/bk7258_wifi.c` (~2000 lines) is
a `netdev_lowerhalf_s` STA driver registered as `NET_LL_IEEE80211`, with
`.connect` / `.disconnect`, `SIOCSIWESSID` plus PSK, paged scan results and
`CONNECTED` / `DISCONNECTED` events. The CP half of the transport is
`external/bk_avdk_smp/cp/components/controller_if/cif_wifi_dp.c`. Commits
`d093016` and `e618a28`.

The division of labour is the one that was predicted: CP still owns the radio,
WPA, association, EAPOL and the PHY, and the AP owns STA IPv4 and ARP over the
mailbox transport.

The config carries `CONFIG_BK7258_WIFI`, IPv4, ARP, TCP, WAPI, ping and DHCP
renewal. `CONFIG_NETINIT_NETLOCAL=y`, so nothing joins a network at boot --
either `wapi` from NSH, or the agent's own `set_wifi <ssid> <password>`, which
saves the credentials so that `wifi_reconnect` works after a reboot.

How far this is actually proven, in this repo:

| Step | Evidence |
|---|---|
| scan | `d093016`: "successful on-board Wi-Fi scan" |
| association | **not proven.** On a freshly flashed board `ifconfig` reports `wlan0 … at DOWN` with the NuttX default static address `10.0.0.2/24` — nothing has joined a network. Provisioning is manual, see `WiFi使用说明.md` |
| DHCP, DNS, ping | not recorded |
| TLS / HTTPS | fails at DNS: `net_test` → `[vela_tls] net_connect www.baidu.com:443 ret=0x52` (`MBEDTLS_ERR_NET_UNKNOWN_HOST`), because the step above has not happened. The entropy blocker that used to fail this earlier is fixed -- see below |
| `ask`, Vision LLM | unverified. `camera_capture` hands its JPEG to a Vision LLM, so the camera path's last hop depends on the rows above |

**Do not read the agent's `net_status` as evidence of association.** It prints
`Network connected: yes / IP: 10.0.0.2` on a board whose `wlan0` is `DOWN`,
because `network_is_connected()` only checks that an address is configured.
This file previously cited that line as proof of association; it is not.

**TLS needs an entropy device, and this config had none.** `agent_secure_random()`
(`include/agent_compat.h`) reads `/dev/urandom` or `/dev/random`; with neither
present, mbedTLS refused to seed and every handshake failed before touching the
network:

```
[vela_tls] CRITICAL: No secure entropy source available
[vela_tls] ctr_drbg_seed ret=0x34          (MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED)
FAILED! TLS Error: 0x2                     (VELA_TLS_ERR_HANDSHAKE)
```

Fixed in the defconfig with `CONFIG_CRYPTO=y` + `CONFIG_CRYPTO_RANDOM_POOL=y` +
`CONFIG_DEV_URANDOM=y` + `CONFIG_DEV_URANDOM_RANDOM_POOL=y`, i.e. NuttX's
BLAKE2Xs entropy pool (fed by IRQ timing) behind `/dev/urandom` rather than the
default xorshift128. `CONFIG_CRYPTO=y` is the part that is easy to miss:
`CRYPTO_RANDOM_POOL` sits inside `if CRYPTO`, so without it the line is silently
dropped and the choice falls back to xorshift128 -- the same class of trap as
`docs/reference/platform.md` §6. Verified on hardware: `/dev/urandom` is present
in `ls /dev`, and `net_test` now gets as far as a DNS lookup instead of dying at
seeding. Cost: +3 KB flash, +1 KB RAM.

**This is not a hardware TRNG.** The pool's own Kconfig says it "may not
actually be cryptographically secure if not enough entropy is made available".
BK7258 has a TRNG; wiring it up and selecting `ARCH_HAVE_RNG` +
`DEV_URANDOM_ARCH` is the real fix, and should happen before anything ships.

Until a network is joined the agent degrades cleanly rather than hanging:
`[agent] Network timeout — net services not started.`

## Upstream defects found while porting

To be sent as pull requests against `open-vela/packages_ai_agent`
(`dev-ai-contest-2026`). Until they are merged, `apply.sh` puts them in the
work tree; patches here are archived copies, not a fork.

| File | Defect | Status |
|---|---|---|
| `src/tools/tool_camera.c` | Hardcoded geometry with no negotiation; 6 `%u` conversions applied to `uint32_t` (wrong on this ABI, and fatal under `-Werror=format`) | patch 0001 |
| `src/core/agent_loop.c:665` | `-Wformat-truncation`: `%s` may write up to 511 bytes into a 473..483 byte region | not patched |
| `src/tools/skill_loader.c:448` | `%x` applied to `uint32_t` | not patched |

The two unpatched ones only block `-Werror`; they are reported here so the
next person does not rediscover them.
