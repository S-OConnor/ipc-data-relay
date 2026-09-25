// ipc-relay-ctl frontend. Talks JSON over a WebSocket to the ipc-relay-ctl
// backend (same host, path /ws):
//   backend -> browser: {"type":"hello",...}, {"type":"stats",...} every
//                       update interval, {"type":"command_result",...}
//   browser -> backend: {"type":"record","enabled":true|false,"id":N}
'use strict';

(() => {
  const $ = (id) => document.getElementById(id);

  // ---- Formatting ---------------------------------------------------------
  const intFmt = new Intl.NumberFormat();
  const num = (v) => (typeof v === 'number' ? intFmt.format(v) : '-');
  const bytes = (v) => {
    if (typeof v !== 'number') return '-';
    const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
    let i = 0;
    let x = v;
    while (x >= 1024 && i < units.length - 1) { x /= 1024; i += 1; }
    return i === 0 ? `${intFmt.format(v)} B` : `${x.toFixed(x < 10 ? 2 : 1)} ${units[i]}`;
  };
  const duration = (s) => {
    if (typeof s !== 'number') return '-';
    const d = Math.floor(s / 86400);
    const h = Math.floor((s % 86400) / 3600);
    const m = Math.floor((s % 3600) / 60);
    const sec = Math.floor(s % 60);
    const hms = `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}`;
    return d > 0 ? `${d}d ${hms}` : hms;
  };
  const get = (obj, path) => path.split('.').reduce((o, k) => (o == null ? undefined : o[k]), obj);

  // ---- What is listed -----------------------------------------------------
  // [stats path, label, formatter, highlight when non-zero]
  const COUNTERS = [
    ['packets_received', 'Packets received', num],
    ['packet_bytes', 'Packet bytes', bytes],
    ['messages_received', 'Messages received', num],
    ['payload_bytes', 'Payload bytes', bytes],
    ['recording.records_written', 'Records written', num],
    ['recording.bytes_written', 'Bytes written', bytes],
    ['recording.records_skipped', 'Messages not recorded', num],
    ['reassembly_pending', 'Reassembly pending', num],
    ['malformed_packets', 'Malformed packets', num, true],
    ['incomplete_fragments', 'Incomplete messages', num, true],
    ['duplicate_fragments', 'Duplicate fragments', num, true],
    ['oversize_packets', 'Oversize packets', num, true],
    ['receive_errors', 'Receive errors', num, true],
    ['kernel_drops', 'Kernel drops', num, true],
    ['recording.write_errors', 'Write errors', num, true],
    ['commands_received', 'Commands received', num],
  ];
  const SOURCE_FIELDS = [
    ['messages', 'Messages', num],
    ['packets', 'Packets', num],
    ['payload_bytes', 'Payload', bytes],
    ['records_written', 'Recorded', num],
    ['last_sequence', 'Last sequence', num],
    ['sequence_gaps', 'Sequence gaps', num, true],
    ['dropped_messages', 'Dropped', num, true],
    ['out_of_order', 'Out of order', num, true],
    ['incomplete_fragments', 'Incomplete', num, true],
    ['duplicate_fragments', 'Duplicates', num, true],
    ['malformed_fragments', 'Malformed', num, true],
  ];

  // ---- DOM construction (values are only ever set via textContent) --------
  const counterCells = COUNTERS.map(([, label]) => {
    const li = document.createElement('li');
    const k = document.createElement('span');
    k.textContent = label;
    const v = document.createElement('span');
    v.className = 'value';
    v.textContent = '-';
    li.append(k, v);
    $('counters').append(li);
    return { li, v };
  });

  const sourceItems = new Map();
  function sourceItem(id) {
    let item = sourceItems.get(id);
    if (item) return item;
    const li = document.createElement('li');
    li.className = 'source';
    const h = document.createElement('h3');
    h.textContent = `Source ${id}`;
    const dl = document.createElement('dl');
    dl.className = 'source-stats';
    const cells = SOURCE_FIELDS.map(([, label]) => {
      const row = document.createElement('div');
      const dt = document.createElement('dt');
      dt.textContent = label;
      const dd = document.createElement('dd');
      dd.className = 'value';
      row.append(dt, dd);
      dl.append(row);
      return { row, dd };
    });
    li.append(h, dl);
    item = { li, cells };
    sourceItems.set(id, item);
    // Keep sources in numeric order.
    const ids = [...sourceItems.keys()].sort((a, b) => Number(a) - Number(b));
    const next = sourceItems.get(ids[ids.indexOf(id) + 1]);
    $('sources').insertBefore(li, next ? next.li : null);
    return item;
  }

  // ---- State --------------------------------------------------------------
  const state = {
    ws: null,
    open: false,
    reconnectDelay: 500,
    hello: null,
    envelope: null,
    pending: null, // {enabled, deadline}
    nextId: 1,
    commandError: '',
  };

  function setPill(el, text, s) {
    el.textContent = text;
    el.dataset.state = s;
  }

  function render() {
    const env = state.envelope;
    const stats = env && env.stats;
    const online = !!(state.open && env && env.receiver_online);
    const recording = stats ? !!get(stats, 'recording.enabled') : null;

    setPill($('link-pill'), state.open ? 'Backend: connected' : 'Backend: disconnected', state.open ? 'ok' : 'bad');
    if (!state.open) setPill($('receiver-pill'), 'Receiver: unknown', 'idle');
    else if (!stats) setPill($('receiver-pill'), 'Receiver: waiting for statistics', 'idle');
    else setPill($('receiver-pill'), online ? 'Receiver: online' : 'Receiver: offline', online ? 'ok' : 'bad');
    if (recording === null) setPill($('recording-pill'), 'Recording: unknown', 'idle');
    else setPill($('recording-pill'), recording ? 'Recording' : 'Not recording', recording ? 'rec' : 'idle');

    // Pending start/stop: done once the receiver reports the new state.
    if (state.pending && recording === state.pending.enabled) state.pending = null;
    if (state.pending && Date.now() > state.pending.deadline) {
      state.commandError = `Receiver did not confirm ${state.pending.enabled ? 'start' : 'stop'} of recording.`;
      state.pending = null;
    }

    const btn = $('record-button');
    const wantStop = state.pending ? !state.pending.enabled : recording === true;
    btn.dataset.action = wantStop ? 'stop' : 'start';
    $('record-label').textContent = state.pending
      ? (state.pending.enabled ? 'Starting…' : 'Stopping…')
      : (recording ? 'Stop recording' : 'Start recording');
    btn.disabled = !online || recording === null || !!state.pending;

    const problems = [];
    if (state.commandError) problems.push(state.commandError);
    if (stats && get(stats, 'recording.failed')) {
      problems.push(`Capture file failed: ${get(stats, 'recording.error') || 'unknown error'}`);
    }
    $('alert').hidden = problems.length === 0;
    $('alert').textContent = problems.join(' ');

    $('capture-file').textContent = (stats && get(stats, 'recording.file')) || '-';
    $('uptime').textContent = stats ? duration(stats.uptime_s) : '-';
    $('stats-age').textContent = env && typeof env.stats_age_ms === 'number'
      ? `${(env.stats_age_ms / 1000).toFixed(1)} s ago` : 'never';
    $('update-rate').textContent = state.hello ? `every ${num(state.hello.update_interval_ms)} ms` : '-';

    COUNTERS.forEach(([path, , fmt, warn], i) => {
      const value = stats ? get(stats, path) : undefined;
      counterCells[i].v.textContent = fmt(value);
      counterCells[i].li.classList.toggle('warn-on', !!warn && typeof value === 'number' && value > 0);
    });

    const sources = (stats && stats.sources) || {};
    const ids = Object.keys(sources);
    ids.forEach((id) => {
      const src = sources[id];
      const item = sourceItem(id);
      SOURCE_FIELDS.forEach(([key, , fmt, warn], i) => {
        item.cells[i].dd.textContent = fmt(src[key]);
        item.cells[i].row.classList.toggle('warn-on', !!warn && src[key] > 0);
      });
    });
    // A restarted receiver forgets sources; drop the ones it no longer reports.
    for (const [id, item] of sourceItems) {
      if (stats && !(id in sources)) { item.li.remove(); sourceItems.delete(id); }
    }
    $('no-sources').hidden = sourceItems.size > 0;
    $('source-count').textContent = sourceItems.size ? `(${sourceItems.size})` : '';
  }

  // ---- WebSocket ----------------------------------------------------------
  function connect() {
    const url = `${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/ws`;
    const ws = new WebSocket(url);
    state.ws = ws;
    ws.onopen = () => {
      state.open = true;
      state.reconnectDelay = 500;
      render();
    };
    ws.onmessage = (ev) => {
      let msg;
      try { msg = JSON.parse(ev.data); } catch (e) { return; }
      if (msg.type === 'hello') state.hello = msg;
      else if (msg.type === 'stats') state.envelope = msg;
      else if (msg.type === 'command_result' && !msg.ok) {
        state.pending = null;
        state.commandError = `Command failed: ${msg.error || 'unknown error'}`;
      }
      render();
    };
    ws.onclose = () => {
      state.open = false;
      state.ws = null;
      state.pending = null;
      render();
      setTimeout(connect, state.reconnectDelay);
      state.reconnectDelay = Math.min(state.reconnectDelay * 2, 5000);
    };
  }

  $('record-button').addEventListener('click', () => {
    const env = state.envelope;
    if (!state.open || !env || !env.stats) return;
    const enabled = !get(env.stats, 'recording.enabled');
    const interval = (state.hello && state.hello.update_interval_ms) || 1000;
    state.commandError = '';
    state.pending = { enabled, deadline: Date.now() + Math.max(5000, 3 * interval) };
    state.ws.send(JSON.stringify({ type: 'record', enabled, id: state.nextId++ }));
    render();
  });

  // Re-render between pushes so "last statistics" and timeouts stay current.
  setInterval(render, 1000);
  render();
  connect();
})();
