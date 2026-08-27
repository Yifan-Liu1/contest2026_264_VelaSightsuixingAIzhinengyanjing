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
./contest2026_264_VelaSightsuixingAIzhinengyanjing/external/prepare.sh install
./contest2026_264_VelaSightsuixingAIzhinengyanjing/external/prepare.sh check
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent --cmake -j8
```

With the managed complete-file overlay installed, this config builds clean under
`-Werror`; the standard product build does not force that flag. The `nsh`
config also keeps its `-Werror` gate.

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
  asked for. The complete target file
  `external/packages/ai_agent/src/tools/tool_camera.c` fixes this: when the
  requested geometry is refused, it enumerates `VIDIOC_ENUM_FRAMESIZES` and
  retries at the closest size the driver offers. The requested geometry is
  still tried first, so devices that support it are unaffected.

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

The complete-file overlay, before and after, on the tool's own default request:

| Command | Result |
|---|---|
| `agent_camera low` (negotiates, overlay installed) | `S_FMT JPEG 320x180 failed: 22` → `320x180 refused, using enumerated 480x480` → `bytesused=13651` → `OK` |
| `agent_camera low strict` (fixed-baseline behavior) | `S_FMT JPEG 320x180 failed: 22`, `S_FMT ENTR 320x180 failed: 22`, `FAILED (-22)` |

So the fixed-baseline `camera_capture` cannot work on this board at all, and
the overlay-managed implementation captures a valid JPEG.

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

**`/mnt` is mounted by bring-up now**, so persistence works out of the box:
`bk7258_ramdisk.c` registers `/dev/ram0` and then mounts it with littlefs and
`-o autoformat`, which formats the device when it finds no valid superblock --
exactly right for a RAM disk whose contents are gone after each reset. Measured
on hardware:

```
ramdisk: /dev/ram0 registered, 2048 KB at 0x60720200 (PSRAM)
ramdisk: /mnt mounted (littlefs on /dev/ram0)
...
[cfgstore] Config store ready at /mnt/ai_agent/config/config.json
[memory] Created file: /mnt/ai_agent/config/SOUL.md
[skills] Installed built-in skill: /mnt/ai_agent/skills/weather.md   (x10)
```

No `Cannot write skill` line anywhere, and the agent built the whole tree
itself -- `mkdirs()` in `src/infra/config_store.c` and `ensure_dir()` in
`src/core/memory_store.c` were always able to do that; what they could not do
was create a filesystem. `mkdir /mnt/ai_agent` by hand, which this file used to
ask for, was never the missing step.

One geometry detail worth keeping: littlefs needs its block size to be a
multiple of its program size, and NuttX derives both from the block driver --
program size is `CONFIG_FS_LITTLEFS_PROGRAM_SIZE_FACTOR` (4) x the 512-byte
sector, so the default block size (1 x 512) is *smaller* than the program size
and the mount fails with `-28` (`ENOSPC`), which is not an obvious way to say
"your geometry is inconsistent". `CONFIG_FS_LITTLEFS_BLOCK_SIZE_FACTOR=8` gives
4096-byte blocks and it mounts.

This is not persistence. The backing store is PSRAM, so the tree is rebuilt
from scratch on every boot; what it buys is that the agent's paths are writable
at all. Real persistence needs a flash partition, and when it arrives only the
device name in `bk7258_ramdisk.c` has to change.

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

Its negotiation is the same algorithm delivered by
`external/packages/ai_agent/src/tools/tool_camera.c`, so `low` versus
`low strict` compares the installed overlay with the fixed-baseline behavior
against the real driver.

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

How far this is proven, all of it measured on hardware on 2026-08-13:

| Step | Evidence |
|---|---|
| scan | `wapi scan wlan0` → 22 BSSes with bssid / frequency / signal / encoding / SSID |
| association | `wapi essid wlan0 <ssid> 1` → `ifconfig` goes from `UP` to `RUNNING` |
| DHCP | `renew wlan0` → `inet addr:10.192.105.127 DRaddr:10.192.104.1 Mask:255.255.254.0`. **The first `renew` after association can time out; retry once** |
| DNS | `ping www.baidu.com` prints `PING 220.181.111.232`, i.e. the query went out and an answer came back |
| TCP + TLS | `[vela_tls] Handshake OK: TLSv1.2 / TLS-DHE-RSA-WITH-AES-256-CBC-SHA` |
| HTTPS | `net_test` → `SUCCESS! HTTP Status: 200` |
| ICMP to the internet | fails, and it is not a defect: `ping 8.8.8.8` loses 100% while DNS and TLS work in the same session, so that network blocks ICMP. Do not use ping as the connectivity test here |

**Do not read the agent's `net_status` as evidence of association.** It prints
`Network connected: yes / IP: 10.0.0.2` on a board whose `wlan0` is `DOWN`,
because `network_is_connected()` only checks that an address is configured. Look
at `ifconfig` for `RUNNING`, or just try DNS.

**The HTTPS above is encrypted but not authenticated.** Two reasons, both
visible in the handshake log:

```
[vela_tls] Handshake start: Host=www.baidu.com, UNIX=328
[vela_tls] Clock too old, forcing to 2026
```

`UNIX=328` is seconds since boot -- there is no RTC and no SNTP, so upstream
forces the clock to 2026 or every certificate would look not-yet-valid. On top
of that `vela_tls.c` sets `MBEDTLS_SSL_VERIFY_OPTIONAL`, so a failed chain check
is logged and ignored, and no CA bundle is installed anyway. Closing this needs
SNTP first, then a root certificate for whichever endpoint the product uses,
then `VERIFY_REQUIRED` -- in that order, because doing the last one first turns
the network off.

**TLS needs an entropy device, and this config had none.** `agent_secure_random()`
(`include/agent_compat.h`) reads `/dev/urandom` or `/dev/random`; with neither
present, mbedTLS refused to seed and every handshake failed before touching the
network:

```
[vela_tls] CRITICAL: No secure entropy source available
[vela_tls] ctr_drbg_seed ret=0x34          (MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED)
FAILED! TLS Error: 0x2                     (VELA_TLS_ERR_HANDSHAKE)
```

Fixed in two steps. First the software entropy pool (`CONFIG_CRYPTO=y` +
`CONFIG_CRYPTO_RANDOM_POOL=y` + `CONFIG_DEV_URANDOM=y` +
`CONFIG_DEV_URANDOM_RANDOM_POOL=y`), then the real thing: `CONFIG_BK7258_TRNG=y`
drives the chip's hardware TRNG (`board/beken/chips/bk7258/bk7258_trng.c`),
registers `/dev/random` from it, and seeds that pool with 32 words of hardware
randomness at bring-up. Measured:

```
trng: dev_id=0x54524e47 version=0x00010001, 8/8 reads distinct, 32 words seeded into the entropy pool
/dev:  random   urandom
```

`dev_id` reads ASCII `"TRNG"`, so the block answering is the intended one. The
`8/8 reads distinct` line is a deliberate boot self-test: this block has two
documented silent failure modes -- a dead register returning a constant, and
the vendor's own warning that with the clock gate left enabled the first read
of each session repeats -- and both look identical, "the output is a constant".
Counting distinct values catches both; it says nothing about statistical
quality, which a boot test cannot establish anyway.

`CONFIG_CRYPTO=y` is the part that is easy to miss: `CRYPTO_RANDOM_POOL` sits
inside `if CRYPTO`, so without it the line is silently dropped and the
`/dev/urandom` choice falls back to xorshift128 -- the same class of trap as
`docs/reference/platform.md` §6. Verified after each step: `net_test` stopped
failing at seeding and now reaches a DNS lookup instead.

Cost of both steps together: about 24 KB of flash (most of it littlefs, which
arrived in the same config change) and 1 KB of RAM.

Still open: the pool is seeded once, at bring-up. Over a long run it is topped
up only by interrupt timing, so a periodic re-seed from the TRNG is worth
adding.

Until a network is joined the agent degrades cleanly rather than hanging:
`[agent] Network timeout — net services not started.`

## Enabling it on your own board

Three steps, in this order. The first two make the LLM reachable; skipping
either leaves an agent that starts and then cannot call anything.

```sh
cd <openvela work tree root>

# 1. Install and verify the complete-file overlays from the OpenVela root
./contest2026_264_VelaSightsuixingAIzhinengyanjing/external/prepare.sh install
./contest2026_264_VelaSightsuixingAIzhinengyanjing/external/prepare.sh check

# 2. Your own API key, never committed
cp contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/boards/\
bk7258/bk7258-ap/ai_agent/agent_secrets.h.example \
   packages/ai_agent/include/agent_secrets.h
$EDITOR packages/ai_agent/include/agent_secrets.h     # fill in the key

# 3. Build and package; add --flash and serial options when required
./contest2026_264_VelaSightsuixingAIzhinengyanjing/build_and_flash.sh
```

`agent_secrets.h` is picked up by `__has_include` in `agent_config.h`. Two
things bite here: ninja does not know that a *previously absent* header has
appeared, so the first build after creating it needs `touch` on a file that
includes it (or delete the `.o`); and the key ends up in plaintext in the
firmware, so a `.bin` built this way must not be handed around. The secret is
an unrelated local file and is deliberately not part of the overlay.

**MiMo has two credential sets and mixing them up looks like a bad key.**
Measured with a real key: `tp-` prefix against `api.xiaomimimo.com` returns
`401 Invalid API Key` with both `Authorization: Bearer` and `api-key:` headers;
the same key against `token-plan-cn.xiaomimimo.com` returns HTTP 200. Also
note the agent's built-in `mimo` presets name `mimo-v2-flash` and
`mimo-v2-omni`, both retired on 2026-06-30 -- the only model that answers now
is `mimo-v2.5`.

| Credential | BASE_URL | key |
|---|---|---|
| 按量付费 | `api.xiaomimimo.com/v1` | `sk-…` |
| Token Plan | `token-plan-cn.xiaomimimo.com/v1` | `tp-…` |

### Measured latency

All on hardware except the two model rows, which were measured from a host on
the same key with a frame captured from this board:

| Step | Measured |
|---|---|
| Text `ask` round trip, on board | `llm_ms=5091` (host: 4.6 s for the same 15 KB request) |
| Vision, 480x480 JPEG (43859 B → 58480 B base64) | **14.4–14.8 s**, `image_tokens=225` |
| Vision, 240x240 JPEG (8941 B → 11924 B base64) | **3.6 s**, `image_tokens=64`, answer still correct |
| JPEG out of the preview stream | 313 ms (copy 27 ms + software encode 286 ms) |
| Expression render + panel push | 24–26 ms |

The 4x difference between 480x480 and 240x240 is the single biggest lever on
the recognition loop, and it costs nothing in answer quality for a face that
fills the frame.

## Upstream defects found while porting

These fixes should still be submitted as pull requests against
`open-vela/packages_ai_agent` (`dev-ai-contest-2026`). Until they are merged,
the complete desired files live at matching relative paths under
`external/packages/ai_agent/`; `external/prepare.sh` is the only installation
and verification entry point. No board patch files are required or retained.

| File | Defect | Complete-file overlay status |
|---|---|---|
| `src/tools/tool_camera.c` | Hardcoded geometry with no negotiation; 6 `%u` conversions applied to `uint32_t` (wrong on this ABI, and fatal under `-Werror=format`) | `external/packages/ai_agent/src/tools/tool_camera.c` |
| `src/core/agent_loop.c:665` | `-Wformat-truncation`: `%s` may write up to 511 bytes into a 473..483 byte region. A cut would land mid-message, i.e. mid UTF-8 sequence, putting an invalid byte on the wire to the channel | `external/packages/ai_agent/src/core/agent_loop.c` |
| `src/core/agent_loop.c` (`calc_elapsed_ms`) | **The LLM watchdog times calls with `gettimeofday()`, and `vela_tls.c` sets the wall clock forward during the first handshake** (`Clock too old, forcing to 2026`). The first `ask` after boot therefore measures ~2.7e9 ms and is declared timed out even when the answer arrives. The helper already guards against the clock going *backwards*; forwards is the case that actually happens here. Should use `CLOCK_MONOTONIC` | `external/packages/ai_agent/src/core/agent_loop.c`; the `net_test`-first workaround is no longer needed |
| `src/llm/llm_proxy.c` (endpoint + diagnostics) | `AGENT_LLM_API_HOST` is an unconditional `#define`, so `agent_secrets.h` can supply a key and model but not a host -- a board configured entirely at build time has nowhere to send the request. Also, "Failed to parse API JSON" logged nothing about what arrived, which hid a captive-portal redirect page behind what looked like a model problem | `external/packages/ai_agent/src/llm/llm_proxy.c` |
| agent response cache | An identical question replays the cached answer, including a cached *error* string: after a failed call, re-asking the same question prints the old failure with `Cache hit, skipping LLM call` and never retries. Cost us a wrong conclusion once | Not changed; design question outside the external overlay, so vary the question when retesting |
| `src/tools/skill_loader.c:448` | `%x` applied to `uint32_t` (wrong on this ABI, fatal under `-Werror=format`) | `external/packages/ai_agent/src/tools/skill_loader.c` |
| `include/agent_config.h` | `AGENT_LLM_TIMEOUT_SEC` was an unconditional `#define`, so `agent_secrets.h` could not raise it for a slow link. An earlier work tree contained the change but its delivery archive did not, so a fresh checkout failed to reproduce the flashed tree | `external/packages/ai_agent/include/agent_config.h` |
| `include/agent_config.h` | WebSocket TTS hardcoded 24 kHz although BK7258 cannot produce that rate; make the rate overridable and default to 16 kHz | `external/packages/ai_agent/include/agent_config.h` |
| `include/agent_config.h` | No control over how much PCM is buffered before playback starts, so a network stall in the first ~200-300ms of a sentence drained the queue and produced audible underruns; added `AGENT_TTS_PREROLL_MS` (default 250ms) | `external/packages/ai_agent/include/agent_config.h` |
| `CMakeLists.txt` | The Vela build omitted the local VelaClaw client implementation from its source list | `external/packages/ai_agent/CMakeLists.txt` |
| LLM/config/vision paths | Oversized token budgets, discarded MiMo thinking, unconstrained structured replies and repeated config-file parsing inflated voice latency and made JSON replies unreliable | external overlay (matching complete files under `include/`, `src/llm/` and `src/tools/`) |
| `src/voice/volc_asr.c`, `volc_tts.h`, `volc_tts_ws.c` | Handle WebSocket control frames and partial reads correctly; distinguish empty ASR input; add cancellable TTS, unique request IDs and invalidatable credential caching | complete files under `external/packages/ai_agent/src/voice/` |
| `src/voice/volc_tts_ws.c` | The ws_binary TTS request sent the output rate under the key `sample_rate`, which the service silently ignores -- it names the field `rate` -- so playback always ran at the service's own 24kHz default regardless of `AGENT_TTS_WS_SAMPLE_RATE` | `external/packages/ai_agent/src/voice/volc_tts_ws.c` |
| `src/voice/audio_playback.c` | `media_player_start()` was called before any PCM was queued, so a network stall in the first ~200-300ms of a sentence emptied the queue and produced audible underruns; buffer `AGENT_TTS_PREROLL_MS` of PCM before starting. `audio_playback_close()` also now logs the real playback duration (bytes / actual rate) instead of only a byte count | `external/packages/ai_agent/src/voice/audio_playback.c` |

With the managed complete files installed, `configs/ai_agent` **builds clean
under `-Werror`**; the note elsewhere that it cannot is obsolete. The response
cache remains a design question rather than a defect: vary the question when
retesting.

`external/prepare.sh check` is the reproducibility gate: it read-only verifies
every managed `packages/ai_agent` target against the complete-file overlay.
When another public-stack file changes, implement and test it in the real
repository, copy the complete final file to the same relative path under
`external/packages/ai_agent/`, and still send the final fix upstream.
