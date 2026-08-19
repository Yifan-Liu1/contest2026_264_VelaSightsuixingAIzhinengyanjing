/* web_tool console front end.
 *
 * Talks only to this backend: JSON over the WebSocket's text frames, JPEG over
 * its binary frames, both inside WSS.  It does not know that the board is
 * reached over TLS-on-TCP, or that the board is the side that dials -- which is
 * what lets that change without touching the page.
 *
 * Frames go to <img> through URL.createObjectURL, so a 27 KB JPEG stays 27 KB
 * instead of the 36 KB it would become as base64.
 */

'use strict';

import { makeKeyStore, isValidPin, MAX_ATTEMPTS, MIN_PIN_DIGITS }
  from '/static/keystore.mjs';

const $ = (id) => document.getElementById(id);

let ws = null;
let nextId = 1;
const pending = new Map();
let paused = false;
let lastObjectUrl = null;
let logLines = [];
let filterText = '';
const keystore = makeKeyStore();

/* Console state: which command is running, and where its output goes. */
let consoleRunning = null;
let grabFinish = null;
let history = [];
let historyPos = -1;

/* Whether the board's kvdb survives a reset.  It decides what a reboot means:
 * with a non-persistent store, rebooting wipes web.host/web.fp/web.token and
 * the board cannot dial back in -- it has to be re-paired over the serial
 * console.  Turning that into a surprise would be unkind. */
let boardPersistent = null;

/* ---- transport ------------------------------------------------------- */

function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(proto + '//' + location.host + '/ws');
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    addLog('me', 'console: 已连接后端（' + proto.replace(':', '') + '）');
    refreshKeyMeta();
    /* Learn whether the board's store is persistent, which changes what the
     * reboot button means.  Harmless if the board is not there yet. */
    llmPull().catch(() => {});
  };
  ws.onclose = () => {
    addLog('err', 'console: 与后端断开，2s 后重连');
    setLamp('lamp-tcp', false);
    $('tcp-text').textContent = '后端断开';
    setTimeout(connect, 2000);
  };
  ws.onmessage = (ev) => {
    if (typeof ev.data === 'string') { onText(JSON.parse(ev.data)); }
    else { onBinary(new Uint8Array(ev.data)); }
  };
}

function send(obj) {
  return new Promise((resolve, reject) => {
    if (!ws || ws.readyState !== 1) { reject(new Error('后端未连接')); return; }
    const id = nextId++;
    obj.id = id;
    pending.set(id, resolve);
    ws.send(JSON.stringify(obj));
    setTimeout(() => {
      if (pending.has(id)) { pending.delete(id); reject(new Error('后端超时')); }
    }, 40000);
  });
}

async function cmd(name, args) {
  let rsp;
  try {
    rsp = await send({ op: 'cmd', cmd: name, args: args || {} });
  } catch (e) {
    addLog('err', 'cmd ' + name + ': ' + e.message);
    return { ok: false, err: e.message };
  }
  if (rsp && rsp.ok === false) {
    /* The name, not the number: ENOENT tells an operator the key is not set,
     * -2 tells them nothing. */
    addLog('err', 'cmd ' + name + ' 失败: ' + rsp.err +
           ' (' + (rsp.errname || rsp.errno) + ')');
  }
  return rsp;
}

function onText(msg) {
  if (msg.type === 'rsp') {
    const fn = pending.get(msg.id);
    if (fn) { pending.delete(msg.id); fn(msg); }
    return;
  }
  if (msg.type === 'log') {
    const e = msg.event || {};
    if (e.dropped !== undefined) {
      addLog('drop', '--- 丢弃 ' + e.dropped + ' 行日志 ---');
      $('dropped').textContent = 'dropped=' + (msg.dropped_total || 0);
    } else if (e.exit !== undefined) {
      /* exit_unknown means the command finished but the board could not
       * retrieve its status (NuttX pclose/waitpid).  Saying "exit=-1" there
       * would make every successful command look like a failure. */
      const how = e.exit_unknown ? '结束（退出码不可知）' : 'exit=' + e.exit;
      addLog('me', '--- 命令' + (e.exit_unknown ? '' : '结束，') + how + ' ---');
      if (consoleRunning !== null) {
        addConsole('exit', '[' + how + '] ' + consoleRunning);
        consoleRunning = null;
        setConsoleState();
      }
    } else if (e.line !== undefined) {
      addLog('', e.line);
      /* Mirror into the console while one of its commands is running, so the
       * thing you typed and its output stay together. */
      if (consoleRunning !== null) { addConsole('', e.line); }
    }
    return;
  }
  if (msg.type === 'state') { renderState(msg.state); return; }
  if (msg.type === 'bootstrap') { renderBootstrap(msg); return; }
  if (msg.type === 'capture.saved') {
    addConsole(msg.ok ? 'me' : 'err',
      '抓帧已保存：' + msg.path + (msg.ok ? '' : '（' + msg.why + '）'));
    if (grabFinish !== null) {
      const finish = grabFinish;
      grabFinish = null;
      void finish(false);
    }
    return;
  }
}

function onBinary(buf) {
  const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const seq = view.getUint32(0, true);
  const digest = view.getUint32(4, true);
  const jpeg = buf.subarray(8);

  if (lastObjectUrl) { URL.revokeObjectURL(lastObjectUrl); }
  lastObjectUrl = URL.createObjectURL(new Blob([jpeg], { type: 'image/jpeg' }));
  const img = $('preview');
  img.src = lastObjectUrl;
  img.style.display = 'block';
  $('preview-none').style.display = 'none';
  $('cam-meta').textContent =
    'seq ' + seq + ', ' + jpeg.length + ' 字节, fnv1a 0x' +
    digest.toString(16).padStart(8, '0');
}

/* ---- rendering ------------------------------------------------------- */

function setLamp(id, on, idle) {
  const el = $(id);
  el.classList.toggle('on', !!on);
  el.classList.toggle('idle', !!idle);
}

function renderState(st) {
  const link = st.link || {};
  setLamp('lamp-tcp', link.connected);
  if (link.connected) {
    $('tcp-text').textContent = (link.mode === 'inbound-tls'
      ? '板子已接入 ' : 'TCP 已连 ') + (link.host || '');
  } else if (link.mode === 'inbound-tls') {
    $('tcp-text').textContent = '等待板子接入' +
      (link.rejected ? '（拒绝 ' + link.rejected + ' 次：' +
        (link.last_reject || '') + '）' : '');
  } else {
    $('tcp-text').textContent = '重连中（第 ' + (link.attempts || 0) + ' 次）' +
      (link.last_error ? ' — ' + link.last_error : '');
  }

  const tls = st.tls || {};
  $('tls-text').textContent = 'TLS' +
    (tls.fingerprint ? ' ' + tls.fingerprint.slice(0, 8) : '');
  $('lamp-tls').title = tls.fingerprint
    ? '证书指纹 ' + tls.fingerprint : 'TLS';

  const ser = st.serial || {};
  setLamp('lamp-serial', false, !ser.held);
  $('serial-text').textContent = ser.held
    ? '串口被本工具占用（serial_cmd.sh/autoflash.sh 会失败）'
    : '串口已释放' + (ser.holder ? '（被他人占用）' : '');

  $('fps-text').textContent = (st.fps || 0).toFixed(1) + ' fps';
  $('dropped').textContent = st.dropped_total
    ? 'dropped=' + st.dropped_total : '';

  const cap = st.capture || {};
  $('cap-meta').textContent = cap.recording
    ? '正在落盘 ' + cap.name + '：' + cap.frames + ' 帧，' +
      cap.rejected + ' 帧校验失败，' + cap.log_lines + ' 行日志'
    : '未在录制';

  const tb = $('sessions').tBodies[0];
  tb.innerHTML = '';
  (st.sessions || []).forEach((s) => {
    const tr = tb.insertRow();
    tr.insertCell().textContent = s.name;
    tr.insertCell().textContent = s.frames + ' 帧';
    const c = tr.insertCell();
    c.textContent = s.rejected ? s.rejected + ' rejected' : '';
    if (s.rejected) { c.className = 'masked'; }
  });

  if (st.pairing && st.pairing.commands) {
    $('pair-cmds').textContent = st.pairing.commands.join('\n');
  }
  $('pair-state').textContent = link.connected
    ? '板子已接入 ' + (link.host || '') +
      '（TLS，证书已钉扎）'
    : '板子未接入' + (link.rejected
      ? '；被拒 ' + link.rejected + ' 次：' + (link.last_reject || '')
      : '');

  /* Nothing on the board can be driven without the board.  Grey the cards
   * rather than letting buttons look live and do nothing. */
  const down = !link.connected;
  ['card-preview', 'card-audio', 'card-conv', 'card-llm', 'card-wifi',
   'card-control']
    .forEach((id) => $(id).classList.toggle('disabled', down));

  /* Read the volume once the board is actually there.  Doing it on connect
   * only would leave the control blank whenever the page was open first. */
  if (link.connected && volApplied === null && volUnity === null) {
    volRead().catch(() => {});
  }
  $('console-input').disabled = down;
  $('degraded').textContent = down
    ? (st.degraded_reason || '板子未接入：串口执行右侧配对命令')
    : '';
}

function renderBootstrap(msg) {
  addLog('me', 'bootstrap: ' + (msg.ok ? '成功（' + msg.path + '，' +
        msg.ip + '）' : '失败：' + (msg.degraded_reason || '')));
  (msg.steps || []).forEach((s) => {
    addLog('', '  ' + (s.ok ? '[ok]  ' : '[fail]') + ' ' + s.step +
           (s.ip ? ' ' + s.ip : '') + ' — ' + (s.detail || ''));
  });
}

function addLog(cls, text) {
  logLines.push({ cls, text });
  if (logLines.length > 4000) { logLines = logLines.slice(-3000); }
  if (paused) { return; }
  renderLog();
}

function renderLog() {
  const el = $('log');
  const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 20;
  const frag = document.createDocumentFragment();
  logLines
    .filter((l) => !filterText || l.text.indexOf(filterText) >= 0)
    .slice(-1500)
    .forEach((l) => {
      const span = document.createElement('span');
      if (l.cls) { span.className = l.cls; }
      span.textContent = l.text + '\n';
      frag.appendChild(span);
    });
  el.innerHTML = '';
  el.appendChild(frag);
  if (atBottom) { el.scrollTop = el.scrollHeight; }
}

/* ---- console --------------------------------------------------------- */

function addConsole(cls, text) {
  const el = $('consoleout');
  const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 20;
  const span = document.createElement('span');
  if (cls) { span.className = cls; }
  span.textContent = text + '\n';
  el.appendChild(span);
  while (el.childNodes.length > 2000) { el.removeChild(el.firstChild); }
  if (atBottom) { el.scrollTop = el.scrollHeight; }
}

function setConsoleState() {
  $('console-state').textContent = consoleRunning
    ? '正在运行：' + consoleRunning : '空闲';
}

async function runConsole(line) {
  if (!line) { return; }
  history = history.filter((h) => h !== line);
  history.push(line);
  if (history.length > 200) { history.shift(); }
  historyPos = -1;

  addConsole('me', 'nsh> ' + line);

  if (consoleRunning !== null) {
    /* The board allows one passthrough command at a time; saying so here is
     * clearer than letting the board answer "busy" a moment later. */
    addConsole('err', '已有命令在跑（' + consoleRunning + '），先停止它');
    return;
  }

  consoleRunning = line;
  setConsoleState();

  const r = await cmd('shell.exec', { cmdline: line });
  if (!r || r.ok === false) {
    addConsole('err', r && r.err === 'busy'
      ? '板子说 busy：还有命令没结束'
      : '执行失败：' + ((r && r.err) || '未知'));
    consoleRunning = null;
    setConsoleState();
  }
}

/* ---- playback volume -------------------------------------------------- */

/* Where 0 dB sits in the driver's 0..1000 scale.  Filled in from the board's
 * own answer rather than hard-coded here, because it follows from the DAC's
 * 6-bit gain field and this page should not carry a second copy of that. */
let volUnity = null;

/* The slider is a request; this is what the board reported back.  Kept apart so
 * "应用" can say whether the value actually took, and so dragging the slider
 * never makes the page claim a volume the hardware has not confirmed. */
let volApplied = null;

function volRender() {
  const want = Number($('vol-slider').value);
  const pct = Math.round(want / 10);
  const parts = [pct + '%'];

  /* Show the raw value too: the driver quantises to 6 bits, so neighbouring
   * percentages are often the same setting and the number explains why the
   * read-back does not match what was dragged. */
  parts.push('(' + want + '/1000)');
  if (volApplied !== null && volApplied !== want) {
    parts.push('板子: ' + Math.round(volApplied / 10) + '%');
  }
  $('vol-value').textContent = parts.join(' ');
}

async function volRead() {
  const r = await cmd('audio.volume');
  if (!r || !r.ok) {
    $('vol-meta').textContent = '读不到当前音量：' + ((r && r.err) || '未知');
    return;
  }

  const d = r.data;
  if (d.unity !== undefined && d.unity !== null) {
    volUnity = d.unity;
    $('vol-unity-mark').textContent = '0 dB ' + Math.round(d.unity / 10) + '%';
  }

  /* Ask about the clip while we are here.  Knowing up front that it is missing
   * is the difference between "上传语音" being an obvious next step and 应用
   * silently doing nothing audible. */
  cmd('audio.announce').then((c) => {
    if (!c || !c.ok) { return; }
    volAnnounceReady = c.data.present === true;
    $('vol-clip').textContent = volAnnounceReady
      ? '语音已就位（' + c.data.bytes + ' 字节 / ' + c.data.seconds + ' 秒）'
      : '板子上还没有语音文件';
  }).catch(() => {});

  if (d.volume === null || d.volume === undefined) {
    /* The driver has no read-back on this build.  Say so rather than showing
     * the slider's own position as if the board had confirmed it. */
    volApplied = null;
    $('vol-meta').textContent =
      '这块板子的驱动不支持读回音量，下面显示的是你设置的值，不是硬件确认的值';
    volRender();
    return;
  }

  volApplied = d.volume;
  $('vol-slider').value = String(d.volume);
  volRender();
  $('vol-meta').textContent =
    '板子当前 ' + d.percent + '%（' + d.volume + '/1000）' +
    (volUnity !== null && d.volume > volUnity
      ? '\n高于 0 dB：DAC 在放大，过高会削顶失真' : '');
}

/* Upload the confirmation clip to the card.
 *
 * Chunked because base64 inflates by a third and the protocol drops a
 * connection that carries an oversized frame rather than trying to recover, so
 * the chunk size is chosen to stay clear of the limit rather than to be fast.
 */
const VOL_CHUNK_RAW = 12 * 1024;

async function volUpload() {
  const btn = $('btn-vol-upload');

  btn.disabled = true;
  $('vol-meta').textContent = '正在取语音文件…';

  let bytes;
  try {
    const res = await fetch('/static/announce/VOLSET.PCM', { cache: 'no-store' });
    if (!res.ok) { throw new Error('HTTP ' + res.status); }
    bytes = new Uint8Array(await res.arrayBuffer());
  } catch (e) {
    /* Say which file and how to make it: the clip is generated by a script
     * that has to be run once, and "上传失败" alone would send someone
     * looking at the board. */
    $('vol-meta').textContent =
      '取不到 host/web/announce/VOLSET.PCM（' + e.message + '）\n' +
      '先在开发机上跑一次 app/web_tool/host/make_announce.sh';
    btn.disabled = false;
    return;
  }

  let sent = 0;
  while (sent < bytes.length) {
    const end = Math.min(sent + VOL_CHUNK_RAW, bytes.length);
    const slice = bytes.subarray(sent, end);

    /* btoa over a binary string: the clip is raw samples, not text, so it has
     * to go through a latin1 round trip rather than TextDecoder. */
    let s = '';
    for (let i = 0; i < slice.length; i++) {
      s += String.fromCharCode(slice[i]);
    }

    const r = await cmd('audio.announce', {
      b64: btoa(s),
      offset: sent,
      final: end >= bytes.length,
    });

    if (!r || !r.ok) {
      $('vol-meta').textContent =
        '上传中断于 ' + sent + '/' + bytes.length + ' 字节：' +
        ((r && r.err) || '未知');
      btn.disabled = false;
      return;
    }

    sent = end;
    $('vol-meta').textContent =
      '上传中 ' + Math.round(sent * 100 / bytes.length) + '%';
  }

  const chk = await cmd('audio.announce');
  const on = chk && chk.ok && chk.data.present;
  $('vol-meta').textContent = on
    ? '语音已写入板子：' + chk.data.bytes + ' 字节，约 ' +
      chk.data.seconds + ' 秒。之后点「应用」会用新音量念一遍。'
    : '上传完成但板子上查不到文件';
  btn.disabled = false;
  volAnnounceReady = on;
}

/* Whether the board has the clip.  Checked once per connection so 应用 can say
 * why nothing was heard instead of leaving the operator wondering. */
let volAnnounceReady = null;

async function volApply(value) {
  const want = value === undefined ? Number($('vol-slider').value) : value;

  $('vol-slider').value = String(want);
  volRender();
  $('vol-meta').textContent = '正在设置…';

  /* Muted is the one level where playing the confirmation would be
   * indistinguishable from the feature being broken, so it is skipped and said
   * in words instead. */
  const r = await cmd('audio.volume',
                      { volume: want, announce: want > 0 });
  if (!r || !r.ok) {
    $('vol-meta').textContent = '设置失败：' + ((r && r.err) || '未知');
    return;
  }

  const d = r.data;
  volApplied = d.volume === undefined ? null : d.volume;

  if (volApplied === null) {
    $('vol-meta').textContent =
      '已下发 ' + Math.round(want / 10) + '%，但驱动不支持读回，无法确认';
    volRender();
    return;
  }

  $('vol-slider').value = String(volApplied);
  volRender();

  /* The read-back is the answer, not the request.  Saying "已设为 71%" when the
   * DAC landed on 70% would be a small lie that makes the next measurement
   * unexplainable. */
  const note = [];
  note.push('已设为 ' + d.percent + '%（' + volApplied + '/1000）');
  if (volApplied !== want) {
    note.push('驱动量化到 6 位增益，' + want + ' 落到了 ' + volApplied);
  }
  if (volUnity !== null && volApplied > volUnity) {
    note.push('高于 0 dB：DAC 在放大，过高会削顶失真');
  }
  if (volApplied === 0) {
    note.push('数字增益为 0，等于静音');
  }

  /* The announcement is the feature: the number tells you what was set, the
   * speaker tells you whether it is loud enough in this room.  So its outcome
   * is reported as prominently as the setting's, and a failure names the
   * reason -- silence is otherwise indistinguishable from a volume of zero. */
  /* "开始播报", not "已播报": the board answers as soon as playback starts, on
   * purpose -- waiting for the clip to finish meant two seconds of not
   * answering the keepalive, and the host declared the board dead.  So this
   * cannot claim the sound came out, only that it was started. */
  if (d.announced) {
    note.push('板子正用这个音量播报（约 2 秒）');
  } else if (volApplied === 0) {
    note.push('静音状态不播报（会什么都听不到）');
  } else if (d.announce_errname === 'ENOENT') {
    note.push('板子上没有语音文件，先点「上传语音」');
    volAnnounceReady = false;
  } else if (d.announce_errname === 'EBUSY') {
    /* One clip at a time.  Volume did change; only the confirmation was
     * skipped, which is worth distinguishing from a failed setting. */
    note.push('上一次播报还没放完，这次只改了音量没有播报');
  } else if (d.announce_errname) {
    note.push('播报失败：' + d.announce_errname);
  }

  $('vol-meta').textContent = note.join('\n');
  addLog('me', 'audio.volume: ' + d.percent + '%' +
         (d.announced ? '，已播报' : ''));
}

/* ---- conversation history -------------------------------------------- */

/* Which record's transcript is open, so clicking the same row again closes it
 * rather than re-fetching what is already on screen. */
let convOpen = null;

function convDate(yyyymmdd) {
  const s = String(yyyymmdd);
  return s.length === 8
    ? s.slice(0, 4) + '-' + s.slice(4, 6) + '-' + s.slice(6)
    : s;
}

/* The analysis is hedged on purpose and the page must not un-hedge it: a cue
 * is rendered with its own confidence, and a record the model could not judge
 * says so instead of showing a blank cue that would read as "nothing
 * happened". */
function convCueText(item) {
  if (item.unable_to_judge) { return '未能判断'; }
  if (!item.cue) { return '—'; }
  return item.cue + ' ' + Number(item.confidence).toFixed(2);
}

async function convDetail(seq, box) {
  const r = await cmd('conv.get', { seq });
  if (!r || !r.ok) {
    box.textContent = '读取失败：' + ((r && r.err) || '未知');
    return;
  }

  box.innerHTML = '';

  const pre = document.createElement('pre');
  pre.className = 'conv-text';
  pre.textContent = r.data.text || '(没有正文)';
  box.appendChild(pre);

  const cue = r.data.cue;
  if (cue && cue.cues) {
    const meta = document.createElement('div');
    meta.className = 'meta';
    const lines = cue.cues.map((c) =>
      c.cue + ' — ' + (c.meaning || '') +
      '（置信度 ' + Number(c.confidence).toFixed(2) + '）');
    if (cue.unable_to_judge) {
      /* The reason matters more than the flag: "画面不完整" tells the wearer
       * to move, "光线不足" tells them to turn on a light. */
      lines.push('未能判断' + (cue.reason ? '：' + cue.reason : ''));
    }
    if (cue.suggestion) { lines.push('建议：' + cue.suggestion); }
    meta.textContent = lines.join('\n');
    box.appendChild(meta);
  }
}

function convRow(item, tbody) {
  const tr = tbody.insertRow();
  tr.insertCell().textContent = item.seq;
  tr.insertCell().textContent = convDate(item.date);
  tr.insertCell().textContent = (item.duration_ms / 1000).toFixed(0) + 's';

  const cueCell = tr.insertCell();
  cueCell.textContent = convCueText(item);
  if (item.unable_to_judge) { cueCell.className = 'masked'; }

  tr.insertCell().textContent = item.summary || '';

  tr.className = 'conv-row';
  tr.onclick = async () => {
    const box = $('conv-detail');
    if (convOpen === item.seq) {
      convOpen = null;
      box.innerHTML = '';
      return;
    }
    convOpen = item.seq;
    box.textContent = '读取第 ' + item.seq + ' 条…';
    await convDetail(item.seq, box);
  };
}

async function convQuery(all) {
  const args = {};
  if (!all) {
    const from = parseInt($('conv-from').value.trim(), 10);
    const to = parseInt($('conv-to').value.trim(), 10);
    const keyword = $('conv-keyword').value.trim();
    const cue = $('conv-cue').value.trim();
    if (!isNaN(from)) { args.from = from; }
    if (!isNaN(to)) { args.to = to; }
    if (keyword) { args.keyword = keyword; }
    if (cue) { args.cue = cue; }
  }

  $('conv-meta').textContent = '查询中…';
  $('conv-detail').innerHTML = '';
  convOpen = null;

  const r = await cmd('conv.query', args);
  const list = $('conv-list');
  list.innerHTML = '';

  if (!r || !r.ok) {
    $('conv-meta').textContent = '查询失败：' + ((r && r.err) || '未知');
    return;
  }

  const d = r.data;
  const items = d.items || [];

  /* "0 matches" and "the board is not reachable" look the same on a page that
   * just shows an empty list, so say which one it is. */
  $('conv-meta').textContent = d.matched === 0
    ? '没有符合条件的记录（板子上共 ' + d.matched + ' 条命中）'
    : '命中 ' + d.matched + ' 条' +
      (d.returned < d.matched
        ? '，显示前 ' + d.returned + ' 条（上限 ' + d.limit + '）'
        : '') + '；点一行看完整对话';

  if (!items.length) { return; }

  const table = document.createElement('table');
  table.className = 'conv-table';
  const head = table.createTHead().insertRow();
  ['#', '日期', '时长', '表情线索', '摘要'].forEach((h) => {
    const th = document.createElement('th');
    th.textContent = h;
    head.appendChild(th);
  });
  const tbody = table.createTBody();
  items.forEach((item) => convRow(item, tbody));
  list.appendChild(table);
}

/* ---- kvdb / llm / wifi ----------------------------------------------- */

async function llmPull() {
  const r = await cmd('kvdb.list');
  if (!r || !r.ok) { return; }
  const items = {};
  (r.data.items || []).forEach((i) => { items[i.key] = i; });
  if (items['llm.host']) { $('llm-host').value = items['llm.host'].value; }
  if (items['llm.model']) { $('llm-model').value = items['llm.model'].value; }
  const meta = [];
  if (items['llm.key']) {
    meta.push('板子上的 llm.key：' + items['llm.key'].value);
  } else {
    meta.push('板子上没有 llm.key');
  }
  meta.push(r.data.persistent
    ? '写入会保留到复位之后'
    : '注意：flash 后端不可用，写入仅本次启动有效');
  $('key-meta').textContent = meta.join('\n');
  boardPersistent = r.data.persistent === true;
}

async function llmPush() {
  const pairs = [
    ['llm.host', $('llm-host').value.trim()],
    ['llm.model', $('llm-model').value.trim()],
    ['llm.key', $('llm-key').value],
  ];
  let wrote = 0;
  let persistent = null;
  for (const [key, value] of pairs) {
    if (!value) { continue; }
    const r = await cmd('kvdb.set', { key, value });
    if (r && r.ok) { wrote++; persistent = r.data.persistent; }
  }
  addLog('me', 'llm: 写入 ' + wrote + ' 项' +
         (persistent === false ? '（仅本次启动有效）' : ''));
  await llmPull();
}

/* ---- key store ------------------------------------------------------- */

function refreshKeyMeta() {
  const st = keystore.status();
  if (!st.stored) {
    $('key-meta').textContent = '本机未保存 API key';
    $('key-warn').textContent = '';
    return;
  }
  $('key-meta').textContent =
    '本机已加密保存：' + (st.hint || '') +
    '\n剩余尝试次数 ' + st.attemptsLeft + ' / ' + MAX_ATTEMPTS +
    '（连续错 ' + MAX_ATTEMPTS + ' 次自动清除）';
  $('key-warn').textContent = st.attemptsLeft <= 2
    ? '只剩 ' + st.attemptsLeft + ' 次，错完就没了' : '';
}

async function keySave() {
  const pin = $('pin').value;
  const secret = $('llm-key').value;
  if (!isValidPin(pin)) {
    $('key-warn').textContent = 'PIN 必须是至少 ' + MIN_PIN_DIGITS + ' 位数字';
    return;
  }
  if (!secret) {
    $('key-warn').textContent = '先把 API key 填进上面的框';
    return;
  }
  try {
    await keystore.save(pin, secret);
    $('pin').value = '';
    $('key-warn').textContent =
      '已加密保存。注意：这只防住「有人拿到浏览器后乱猜」；' +
      '整份记录被拷走后可以离线爆破，PIN 越长越好。';
  } catch (e) {
    $('key-warn').textContent = e.message;
  }
  refreshKeyMeta();
}

async function keyUnlock() {
  const pin = $('pin').value;
  if (!pin) { $('key-warn').textContent = '先输入 PIN'; return; }
  const r = await keystore.unlock(pin);
  $('pin').value = '';
  if (r.ok) {
    $('llm-key').value = r.secret;
    $('key-warn').textContent = '已填入，可以「写入板子」';
  } else if (r.reason === 'wiped') {
    $('key-warn').textContent =
      '连续错 ' + MAX_ATTEMPTS + ' 次，本机保存的 API key 已被清除';
  } else if (r.reason === 'empty') {
    $('key-warn').textContent = '本机没有保存过';
  } else {
    $('key-warn').textContent = 'PIN 不对，还剩 ' + r.attemptsLeft + ' 次';
  }
  refreshKeyMeta();
}

/* ---- wiring ---------------------------------------------------------- */

function wire() {
  $('btn-serial-hold').onclick = () =>
    send({ op: 'serial', action: 'acquire' }).then((r) => {
      if (!r.ok) { addLog('err', r.err); }
    });
  $('btn-serial-free').onclick = () => send({ op: 'serial', action: 'release' });

  $('btn-cam-start').onclick = async () => {
    const [w, h] = $('cam-size').value.split('x').map(Number);
    await cmd('camera.start', { width: w, height: h });
  };
  $('btn-cam-stop').onclick = async () => {
    const r = await cmd('camera.stop');
    if (r && r.ok) {
      addLog('me', 'camera: 发出 ' + r.data.frames_sent + ' 帧，丢弃 ' +
             r.data.frames_dropped + ' 帧' +
             (r.data.frames_dropped === 0
               ? '（一帧没丢：帧率低是驱动交付得慢，不是这里节流）' : ''));
    }
  };
  $('btn-grab').onclick = async () => {
    if (grabFinish !== null) {
      addConsole('err', '已有一次抓帧正在进行');
      return;
    }

    const cap = await send({ op: 'capture', action: 'start' });
    if (!cap || cap.ok === false) {
      addConsole('err', '无法创建抓帧目录：' + ((cap && cap.err) || '未知'));
      return;
    }

    let done = false;
    let timer = null;
    const finish = async (timedOut) => {
      if (done) { return; }
      done = true;
      if (timer !== null) { clearTimeout(timer); }
      await cmd('camera.stop');
      await send({ op: 'capture', action: 'stop' });
      if (timedOut) {
        addConsole('err', '抓帧超时：5 秒内没有收到可保存的帧');
      }
    };

    grabFinish = finish;
    const [w, h] = $('cam-size').value.split('x').map(Number);
    const started = await cmd('camera.start', { width: w, height: h });
    if (!started || started.ok === false) {
      grabFinish = null;
      await finish(false);
      return;
    }

    timer = setTimeout(() => {
      if (grabFinish === finish) { grabFinish = null; }
      void finish(true);
    }, 5000);
  };
  $('btn-rec-start').onclick = async () => {
    await send({ op: 'capture', action: 'start' });
    const [w, h] = $('cam-size').value.split('x').map(Number);
    await cmd('camera.start', { width: w, height: h });
  };
  $('btn-rec-stop').onclick = async () => {
    await cmd('camera.stop');
    await send({ op: 'capture', action: 'stop' });
  };

  /* Dragging only moves the label; nothing is sent until 应用.  A slider that
   * fired an ioctl per pixel would put a few hundred requests on the wire for
   * one gesture, and the board answers each one. */
  $('vol-slider').oninput = volRender;
  $('btn-vol-apply').onclick = () => volApply();
  $('btn-vol-mute').onclick = () => volApply(0);
  $('btn-vol-unity').onclick = () => volApply(volUnity === null ? 714 : volUnity);
  $('btn-vol-read').onclick = volRead;
  $('btn-vol-upload').onclick = volUpload;

  $('btn-conv-query').onclick = () => convQuery(false);
  $('btn-conv-all').onclick = () => {
    ['conv-from', 'conv-to', 'conv-keyword', 'conv-cue']
      .forEach((id) => { $(id).value = ''; });
    convQuery(true);
  };
  ['conv-from', 'conv-to', 'conv-keyword', 'conv-cue'].forEach((id) => {
    $(id).onkeydown = (e) => {
      if (e.key === 'Enter') { convQuery(false); }
    };
  });

  $('btn-llm-push').onclick = llmPush;
  $('btn-llm-pull').onclick = llmPull;
  $('btn-key-show').onclick = () => {
    const el = $('llm-key');
    el.type = el.type === 'password' ? 'text' : 'password';
  };
  $('btn-key-save').onclick = keySave;
  $('btn-key-unlock').onclick = keyUnlock;
  $('btn-key-forget').onclick = () => {
    keystore.forget();
    $('key-warn').textContent = '已从本机清除';
    refreshKeyMeta();
  };
  $('pin').onkeydown = (e) => {
    if (e.key === 'Enter') { keyUnlock(); }
  };

  $('btn-wifi-connect').onclick = async () => {
    const ssid = $('wifi-ssid').value.trim();
    const psk = $('wifi-psk').value;
    if (!ssid) { return; }
    /* Applying this drops the link it travelled on -- the board is talking to
     * us over the very Wi-Fi it is about to re-associate.  The board answers
     * first and applies afterwards, so the sequence is: ack, link drops, board
     * dials back in.  Say that here rather than let it look like a failure. */
    $('wifi-meta').textContent = '已提交…关联会断开当前连接，板子随后自己重连';
    const r = await cmd('wifi.connect', { ssid, psk });
    if (r && r.ok) {
      $('wifi-meta').textContent =
        '已保存并开始关联 ' + (r.data.ssid || ssid) +
        (r.data.persistent === false ? '（仅本次启动有效）' : '') +
        '\n连接会断开几秒，然后板子重新拨入；重连后按「查看状态」确认地址';
      addLog('me', 'wifi.connect: ' + (r.data.note || '已提交'));
    }
  };
  $('btn-wifi-status').onclick = async () => {
    const r = await cmd('wifi.status');
    if (r && r.ok) {
      $('wifi-meta').textContent =
        (r.data.running ? '已连接 ' : '未就绪 ') + r.data.ip +
        ' / ' + r.data.netmask + ' gw ' + r.data.gw +
        (r.data.ssid ? ' ssid ' + r.data.ssid : '');
    }
  };

  document.querySelectorAll('[data-cmd]').forEach((btn) => {
    btn.onclick = () => runConsole(btn.getAttribute('data-cmd'));
  });

  $('btn-status').onclick = async () => {
    const r = await cmd('sys.status');
    if (!r || !r.ok) { return; }
    const d = r.data;
    const heaps = (d.heaps || [])
      .map((h) => '  ' + h.name.padEnd(18) + String(h.free).padStart(9) +
                  ' free / ' + h.total)
      .join('\n');
    $('sys-meta').textContent =
      'uptime ' + d.uptime + 's, ' + d.tasks + ' 个任务\n' + heaps;
  };

  $('btn-reboot').onclick = async () => {
    /* With a non-persistent kvdb this is a one-way trip: the reboot wipes
     * web.host / web.fp / web.token along with the Wi-Fi credentials, so the
     * board comes back with no way to find us.  It then needs the four kvdb
     * commands re-pasted over the serial console.  Better to say so than to
     * let the page go dark and look broken. */
    const warn = boardPersistent === false
      ? '重启开发板？\n\n注意：这块板子的 kvdb 不持久（flash 后端未启用），'
        + '重启会连同 web.host / web.fp / web.token 和 Wi-Fi 凭据一起清掉，'
        + '板子将无法自己回来，必须再从串口粘一次配对命令。\n\n继续？'
      : '重启开发板？TLS 连接会断开，板子重启后会自己重连。';
    if (!confirm(warn)) { return; }
    await cmd('sys.reboot');
    addLog('me', 'sys.reboot: 已应答，板子随后重启'
           + (boardPersistent === false
             ? '；kvdb 不持久，需要从串口重新配对' : ''));
  };

  $('btn-pair').onclick = async () => {
    const ssid = $('pair-ssid').value.trim();
    const psk = $('pair-psk').value;
    if (!ssid) { addConsole('err', '先填 SSID'); return; }
    const btn = $('btn-pair');
    btn.disabled = true;
    btn.textContent = '配对中…（约 30s）';
    $('pair-state').textContent = '正在通过串口配网并启动 web_tool…';
    addConsole('me', '--- 一键配对：占用串口，配网，启动 web_tool ---');
    try {
      const r = await send({ op: 'pair', ssid, psk });
      const d = r.data || {};
      if (d.transcript) {
        d.transcript.split('\n').forEach((l) => addConsole('', l));
      }
      if (r.ok) {
        addConsole('exit', '[配对完成] 板子 ' + (d.ip || '') +
                   ' 将拨回 ' + (d.host || ''));
        $('pair-state').textContent = '配网成功 ' + (d.ip || '') +
          '，等待板子拨入…';
      } else {
        addConsole('err', '配对失败：' + (d.err || r.err || '未知'));
        $('pair-state').textContent = '配对失败：' + (d.err || r.err || '');
      }
    } catch (e) {
      addConsole('err', '配对失败：' + e.message);
    } finally {
      btn.disabled = false;
      btn.textContent = '一键配对（走串口）';
    }
  };

  $('btn-cap-start').onclick = () => send({ op: 'capture', action: 'start' });
  $('btn-cap-stop').onclick = () => send({ op: 'capture', action: 'stop' });

  const input = $('console-input');
  input.onkeydown = (e) => {
    if (e.key === 'Enter') {
      const line = input.value.trim();
      input.value = '';
      runConsole(line);
      return;
    }
    if (e.key === 'ArrowUp') {
      if (!history.length) { return; }
      historyPos = historyPos < 0 ? history.length - 1
        : Math.max(0, historyPos - 1);
      input.value = history[historyPos];
      e.preventDefault();
      return;
    }
    if (e.key === 'ArrowDown') {
      if (historyPos < 0) { return; }
      historyPos++;
      if (historyPos >= history.length) { historyPos = -1; input.value = ''; }
      else { input.value = history[historyPos]; }
      e.preventDefault();
    }
  };
  $('btn-console-kill').onclick = async () => {
    await cmd('shell.kill');
    addConsole('err', '已请求停止转发（命令可能仍在板上跑完）');
    consoleRunning = null;
    setConsoleState();
  };
  $('btn-console-clear').onclick = () => { $('consoleout').innerHTML = ''; };

  $('btn-log-sub').onclick = () => cmd('log.subscribe', { on: true })
    .then((r) => {
      if (r && r.ok) { addLog('me', 'log: 补发 ' + r.data.replayed + ' 行'); }
    });
  $('btn-log-unsub').onclick = () => cmd('log.subscribe', { on: false });
  $('btn-log-pause').onclick = (e) => {
    paused = !paused;
    e.target.textContent = paused ? '继续' : '暂停';
    if (!paused) { renderLog(); }
  };
  $('btn-log-clear').onclick = () => { logLines = []; renderLog(); };
  $('log-filter').oninput = (e) => { filterText = e.target.value; renderLog(); };

  /* Wrap each card's contents below its heading in one scrolling box.
   *
   * Done here rather than in the markup so the seven cards stay plain HTML that
   * anyone can edit without knowing about this: add a row to a card and it lands
   * inside the scroller automatically.  The page never scrolls, so a card that
   * outgrows its cell has to scroll itself or its bottom controls would be
   * unreachable -- which is the thing this layout exists to prevent. */
  document.querySelectorAll('#cards .card').forEach((card) => {
    const body = document.createElement('div');
    body.className = 'card-body';
    /* Everything except the heading, in order. */
    const rest = [];
    card.childNodes.forEach((n) => {
      if (!(n.nodeType === 1 && n.tagName === 'H2')) { rest.push(n); }
    });
    rest.forEach((n) => body.appendChild(n));
    card.appendChild(body);
  });

  /* Draggable split: reading a long self-test output wants most of the screen
   * for a moment, and a fixed one makes that impossible.  It resizes the bottom
   * row; the cards above take whatever is left, so nothing ever ends up off
   * screen. */
  const dragger = $('dragger');
  let dragging = false;
  dragger.onmousedown = (e) => {
    dragging = true;
    document.body.style.userSelect = 'none';
    e.preventDefault();
  };
  window.onmouseup = () => {
    dragging = false;
    document.body.style.userSelect = '';
  };
  window.onmousemove = (e) => {
    if (!dragging) { return; }
    /* Sets the cards' height; the panes below take the rest, so one number
     * decides the split.  Bounded at both ends: dragged to the ceiling the
     * panes would vanish, dragged to the floor the cards would. */
    const h = Math.min(window.innerHeight - 140,
                       Math.max(90, e.clientY));
    const cards = $('cards');
    cards.style.maxHeight = h + 'px';
    cards.style.height = h + 'px';
  };

  setConsoleState();
  refreshKeyMeta();
}

wire();
connect();
