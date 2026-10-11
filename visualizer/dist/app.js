(function () {
  'use strict';
  const model = window.RedisFlow;
  const $ = id => document.getElementById(id);
  const nodes = Array.from(document.querySelectorAll('.flow-node'));
  const edges = Array.from(document.querySelectorAll('.flow-edge'));
  const labels = { client: 'CLIENT', encode: 'CLIENT · RESP2 ENCODING', network: 'TCP · REQUEST BYTES', poll: 'EVENT LOOP · SCHEDULING', read: 'NONBLOCKING READ', buffer: 'OFFSET INPUT BUFFER', parser: 'RESP2 COMMAND PARSER', command: 'COMMAND PROCESSOR', storage: 'CUSTOM STORAGE', reply: 'RESP2 REPLY', write: 'NONBLOCKING WRITE', decode: 'CLIENT · REPLY DECODING', done: 'ROUND TRIP FINISHED' };
  const points = { client: [108, 73], encode: [108, 73], network: [232, 111], poll: [355, 73], read: [560, 73], buffer: [765, 73], parser: [965, 73], command: [965, 233], storage: [765, 233], reply: [560, 233], write: [355, 233], decode: [108, 233], done: [108, 233] };
  const snippets = {
    client: 'const std::string request =\n    resp::encodeCommand(args);\nnet::sendAll(fd, request);',
    encode: 'std::string result = "*" +\n    std::to_string(arguments.size())\n    + "\\r\\n";\nfor (const auto& arg : arguments)\n    result += bulk(arg);',
    network: 'while (offset < data.size()) {\n    n = send(fd, data.data() + offset,\n        data.size() - offset,\n        MSG_NOSIGNAL);\n    // Retry EINTR; advance offset.\n}',
    poll: 'const short events =\n    conn->state == State::Request\n    ? POLLIN : POLLOUT;\npollfds.push_back({fd, events, 0});\n::poll(pollfds.data(),\n    pollfds.size(), -1);',
    read: 'while (conn.state == State::Request) {\n    if (prepareResponse(conn)) return;\n    if (received >= IoBudget) return;\n    // recv(), append; retry or return.\n}',
    buffer: 'begin_ += length;\nif (empty()) clear();\n\n// view exposes only unread bytes:\nreturn std::string_view(storage_)\n    .substr(begin_);',
    append: 'const auto remaining =\n    storage_.size() - begin_;\nif (length > capacity - size &&\n    begin_ > 0 && begin_ >= remaining) {\n    storage_.erase(0, begin_);\n    begin_ = 0;\n}\nstorage_.append(data, length);',
    parser: 'Reader reader{input, 0,\n    Status::Complete, {}};\n// Read array count, then bulk strings.\nresult.status = Status::Complete;\nresult.consumed = reader.pos;\nreturn result;',
    command: '// PING/ECHO return directly.\nstd::lock_guard<std::mutex> lock(mutex_);\nif (name == "GET") {\n    auto result = store_->get(args[1]);\n    // Encode null, wrong-type or value.\n}',
    storage: 'Entry* entry = entries_.find(key);\nif (!entry)\n    return {Status::NotFound, {}};\nif (entry->sortedSet)\n    return {Status::WrongType, {}};\nreturn {Status::Ok, entry->value};',
    reply: 'conn.output = conn.commands.execute(\n    result.arguments);\nconn.input.consume(result.consumed);\nconn.sent = 0;\nconn.state = State::Response;',
    write: '// Two independent conditions:\nif (conn.state == State::Request &&\n    (pfd.revents & (POLLIN | POLLHUP)))\n    handleRead(conn);\nif (conn.state == State::Response &&\n    (pfd.revents &\n     (POLLIN | POLLOUT | POLLHUP)))\n    handleWrite(conn);',
    send: 'const auto count = std::min(\n    conn.output.size() - conn.sent,\n    IoBudget - written);\n// send(output.data() + sent, count)\nconn.sent += n;\nwritten += n;',
    decode: 'auto reply = resp::parseReply(input);\nif (reply.status == Status::Complete) {\n    printReply(reply);\n    input.erase(0, reply.consumed);\n    return true;\n}',
    pipeline: 'conn.output.clear();\nconn.sent = 0;\nconn.state = closeAfterWrite\n    ? State::End : State::Request;\nif (conn.state == State::Request)\n    prepareResponse(conn);',
    done: 'if (conn.state == State::End)\n    fd2Conn[pfd.fd].reset();\n// Otherwise retain Conn for next poll.'
  };
  let trace, index = 0, timer = null, hex = false, config = { ...model.presets.normal }, customConfig = null;
  const short = (value, limit = 240) => value.length > limit ? value.slice(0, limit) + ' …' : value;
  const set = (id, value) => { $(id).textContent = value; };
  const element = (tag, className, content) => { const e = document.createElement(tag); if (className) e.className = className; if (content !== undefined) e.textContent = content; return e; };
  const displayReply = reply => {
    if (reply.value === null) return '(nil)';
    if (reply.type === ':') return '(integer) ' + reply.value;
    if (reply.type === '*') return reply.value.length ? reply.value.map((item, i) => `${i + 1}) ${displayReply(item)}`).join('\n') : '(empty array)';
    return (reply.type === '-' ? '(error) ' : '') + reply.value;
  };
  function renderBytes(state) {
    const strip = $('byte-strip'); strip.replaceChildren();
    if (!state.storage.length) {
      strip.append(element('div', 'empty-buffer', '∅  No stored bytes · begin_ = 0')); return;
    }
    const focus = state.highlight ? state.highlight.start : state.begin;
    const start = state.storage.length > 256 ? Math.max(0, Math.min(focus - 32, state.storage.length - 256)) : 0;
    const end = Math.min(start + 256, state.storage.length);
    if (start) strip.append(element('span', 'empty-buffer', `… ${start} earlier bytes`));
    const fragment = document.createDocumentFragment();
    for (let i = start; i < end; i++) {
      const b = state.storage[i];
      const cell = element('div', 'byte-cell' + (i < state.begin ? ' consumed' : '') + (i === state.begin ? ' cursor' : '') + (state.highlight && i >= state.highlight.start && i < state.highlight.end ? ' highlighted' : ''));
      const char = hex ? b.toString(16).padStart(2, '0').toUpperCase() : b === 13 ? '\\r' : b === 10 ? '\\n' : b >= 32 && b < 127 ? String.fromCharCode(b) : b.toString(16).toUpperCase();
      cell.title = `Byte ${i}: ${model.escapeBytes([b])} (0x${b.toString(16).padStart(2, '0')}) · ${i < state.begin ? 'consumed' : 'unread'}`;
      cell.append(element('span', 'byte-value', char), element('span', 'byte-index', String(i))); fragment.append(cell);
    }
    strip.append(fragment);
    if (state.begin === state.storage.length) strip.append(element('span', 'empty-buffer', '◀ begin_ at end; clear() follows'));
    if (end < state.storage.length) strip.append(element('span', 'empty-buffer', `… ${state.storage.length - end} later bytes`));
    const cursor = strip.querySelector('.cursor,.highlighted');
    if (cursor) strip.scrollLeft = Math.max(0, cursor.offsetLeft - strip.offsetLeft - strip.clientWidth / 2);
  }
  function render() {
    const frame = trace.frames[index], s = frame.snapshot;
    const activeStage = frame.stage === 'client' ? 'encode' : frame.stage === 'done' ? 'decode' : frame.stage;
    const visited = new Set(trace.frames.slice(0, index + 1).map(frame => frame.stage));
    nodes.forEach(node => { node.classList.toggle('active', node.dataset.stage === activeStage); node.classList.toggle('visited', visited.has(node.dataset.stage)); });
    edges.forEach(edge => edge.classList.toggle('active', edge.id === 'edge-' + activeStage));
    const point = points[frame.stage]; $('packet').style.transform = `translate(${point[0]}px, ${point[1]}px)`;
    set('cycle', s.cycle); set('state-token', s.exists ? 'Conn.' + s.connState : s.connState === 'End' ? 'Conn destroyed' : 'Conn not created');
    $('state-token').className = 'state-token ' + s.connState.toLowerCase();
    set('event-label', s.event === '—' ? 'next interest: ' + s.interest : 'revents: ' + s.event);
    set('request-count', s.clientRequest.length ? s.clientRequest.length + ' B' : '—');
    set('reply-count', s.output.length ? s.output.length + ' B' : '—');
    set('journey-context', short(frame.call, 120));
    set('encoded-request', s.clientRequest.length ? model.escapeBytes(s.clientRequest) : 'Arguments have not been encoded yet.');
    set('network-remaining', s.networkRemaining.length + ' B awaiting server recv');
    set('buffer-begin', s.begin + ' B'); set('buffer-size', s.storage.length + ' B'); set('buffer-unread', s.storage.length - s.begin + ' B');
    set('buffer-spare', s.capacity - s.storage.length + ' B'); set('buffer-copied', s.inputCopied + ' B'); set('capacity-note', 'capacity: ' + s.capacity + ' B (illustrative)');
    renderBytes(s);
    set('input-view', s.storage.length > s.begin ? short(model.escapeBytes(s.storage.slice(s.begin)), 550) : 'empty');
    set('parser-status', s.parser.status.toLowerCase()); $('parser-status').className = 'mini-badge ' + s.parser.status.toLowerCase();
    const args = $('argument-list'); args.replaceChildren();
    if (!s.parser.arguments.length) args.append(element('span', 'empty-label', s.parser.status === 'Incomplete' ? 'No complete command. Retain input; consume 0 bytes.' : s.parser.status === 'Error' ? 'Protocol error: ' + s.parser.error : 'Arguments appear when one command is complete.'));
    s.parser.arguments.forEach((arg, i) => { const chip = element('span', 'argument-chip'); chip.append(element('span', '', String(i)), element('span', '', short(JSON.stringify(arg), 100))); args.append(chip); });
    set('parser-consumed', s.parser.consumed); set('parser-position', 'cursor: ' + s.parser.cursor + ' B');
    set('sent-label', `sent ${s.sent} / ${s.output.length} B`); set('pending-remaining', s.output.length - s.sent + ' B remaining');
    $('pending-reply').replaceChildren();
    if (!s.output.length) set('pending-reply', 'No output queued');
    else $('pending-reply').append(element('span', 'sent-bytes', short(model.escapeBytes(s.output.slice(0, s.sent)))), element('span', 'unsent-bytes', short(model.escapeBytes(s.output.slice(s.sent)))));
    $('write-progress-fill').style.width = (s.output.length ? s.sent / s.output.length * 100 : 0) + '%';
    const database = $('database'); database.replaceChildren();
    s.database.forEach(entry => {
      const row = element('div', 'database-entry' + (entry.key === s.activeKey ? ' active' : ''));
      row.append(element('code', 'key', entry.key), element('span', 'data-type', entry.type));
      if (entry.type === 'string') row.append(element('code', 'value', short(JSON.stringify(entry.value), 100)));
      else { const members = element('div', 'members'); entry.members.forEach(m => members.append(element('span', 'member-tag', `${m.name}: ${m.score}`))); row.append(members); }
      row.title = `Entry key: ${entry.key}; FNV-1a: ${model.hashKey(entry.key).hex}. The C++ backend stores the key in its custom hash table.`;
      database.append(row);
    });
    if (!s.database.length) database.append(element('div', 'database-entry empty-label', 'The database is empty.'));
    set('storage-operation', s.storageOperation || 'Storage operations appear here.');
    set('client-byte-count', s.clientInput.length + ' B buffered'); set('client-input', s.clientInput.length ? short(model.escapeBytes(s.clientInput), 450) : s.clientReplies.length ? 'Complete replies have been consumed.' : 'Waiting for reply bytes');
    const replies = $('client-replies'); replies.replaceChildren();
    if (!s.clientReplies.length) replies.append(element('span', 'empty-label', 'Decoded replies appear here.'));
    s.clientReplies.forEach((reply, i) => { const row = element('div', 'client-reply' + (reply.type === '-' ? ' error' : '')); row.append(element('span', 'reply-prefix', String(i + 1).padStart(2, '0')), element('pre', '', short(displayReply(reply), 600))); replies.append(row); });
    set('step-label', `${String(index + 1).padStart(2, '0')} / ${trace.frames.length}`); set('stage-label', labels[frame.stage]);
    set('step-title', short(frame.title, 140)); set('step-detail', frame.detail); set('function-call', frame.call);
    set('conn-exists', s.exists ? 'fd2Conn[fd]' : s.connState === 'End' ? 'destroyed' : 'not created');
    set('conn-state', s.connState); $('conn-state').className = s.connState.toLowerCase();
    set('conn-interest', s.exists ? (s.connState === 'Request' ? 'POLLIN' : 'POLLOUT') : '—');
    set('conn-sent', s.sent + ' B'); set('conn-close', String(s.closeAfterWrite)); set('conn-received', s.received + ' / 65536 B'); set('conn-written', s.written + ' / 65536 B'); set('conn-mutex', s.mutex ? 'locked' : 'unlocked');
    set('source-file', frame.source + ' · abridged excerpt');
    let snippet = snippets[frame.stage];
    if (frame.stage === 'buffer' && /append|Compact|Grow/.test(frame.title + frame.call)) snippet = snippets.append;
    if (frame.stage === 'write' && frame.call.startsWith('send(')) snippet = snippets.send;
    if (frame.call.includes('return from handleWrite') || frame.detail.includes('handleWrite() has drained')) snippet = snippets.pipeline;
    if (frame.call === 'Reader::readBulk()') snippet = "std::size_t length = 0;\nif (!marker('$') ||\n    !number(MaxBulk, length)) return false;\nif (!need(length + 2)) return false;\n// Check the CRLF terminator.\nvalue.assign(input.substr(pos, length));\npos += length + 2;";
    if (frame.call.startsWith("Reader::marker('*')")) snippet = "std::size_t count = 0;\nif (!reader.marker('*') ||\n    !reader.number(MaxArgs, count))\n    return unfinished();\n// Then read count bulk arguments.";
    if (frame.title === 'Encode the result as RESP2') {
      const prefix = s.replyValue[0];
      snippet = prefix === '$' ? s.replyValue === '$-1\r\n' ? 'return "$-1\\r\\n"; // null bulk reply' : 'return "$" +\n    std::to_string(value.size())\n    + "\\r\\n" + std::string(value)\n    + "\\r\\n";' : prefix === '+' ? 'return "+" + std::string(value)\n    + "\\r\\n";' : prefix === '-' ? 'return "-" + std::string(value)\n    + "\\r\\n";' : prefix === ':' ? 'return ":" + std::to_string(value)\n    + "\\r\\n";' : '// Each element is already encoded.\nstd::string result = "*" +\n    std::to_string(elements.size())\n    + "\\r\\n";\nfor (const auto& item : elements)\n    result += item;\nreturn result;';
    }
    if (frame.stage === 'storage' && s.command[0].toUpperCase() === 'SET') snippet = 'std::string replacement(value);\nif (Entry* entry = entries_.find(key)) {\n    entry->value.swap(replacement);\n    entry->sortedSet.reset();\n} else {\n    // Create an owning Entry.\n    entries_.insert(std::move(created));\n}';
    if (frame.stage === 'storage' && s.command[0].toUpperCase() === 'DEL') snippet = 'bool erase(std::string_view key) {\n    return static_cast<bool>(\n        entries_.erase(key));\n}';
    if (frame.stage === 'storage' && s.command[0].toUpperCase().startsWith('Z')) snippet = s.command[0].toUpperCase() === 'ZQUERY' ? 'Member* member = byScore_.offset(\n    byScore_.lowerBound(lower), offset);\nwhile (member && result.size() < limit) {\n    result.push_back({member->key,\n        member->score});\n    member = decltype(byScore_)::next(member);\n}' : s.command[0].toUpperCase() === 'ZRANK' ? 'if (Member* member = byName_.find(name))\n    return byScore_.rank(member);\nreturn std::nullopt;' : '// SortedSet adapter owns / indexes members.\nbyName_.insert(std::move(member));\nbyScore_.insert(pointer);\n// Score updates detach, change, reinsert.';
    if (frame.stage === 'storage' && s.command[0].toUpperCase() === 'ZREM') snippet = 'Member* member = byName_.find(name);\nif (!member) return false;\nbyScore_.erase(member);\nbyName_.erase(name);\nreturn true;';
    if (frame.stage === 'storage' && s.command[0].toUpperCase() === 'ZSCORE') snippet = 'if (Member* member = byName_.find(name))\n    return member->score;\nreturn std::nullopt;';
    if (frame.stage === 'storage' && s.command[0].toUpperCase() === 'ZCARD') snippet = 'std::size_t size() const override {\n    return byName_.size();\n}';
    if (frame.stage === 'storage' && s.command[0].toUpperCase() === 'KEYS') snippet = 'entries_.forEach([&](const Entry& entry) {\n    result.push_back(entry.key);\n});\nstd::sort(result.begin(), result.end());\nreturn result;';
    set('source-code', snippet);
    const log = $('event-log'); log.replaceChildren();
    for (let i = Math.max(0, index - 4); i <= index; i++) { const item = element('li'); const button = element('button', i === index ? 'current' : ''); button.type = 'button'; button.append(element('span', 'log-index', String(i + 1).padStart(2, '0')), element('span', 'log-title', short(trace.frames[i].title, 90))); button.addEventListener('click', () => { pause(); setIndex(i); }); item.append(button); log.append(item); }
    $('timeline').max = trace.frames.length - 1; $('timeline').value = index;
    $('timeline').setAttribute('aria-valuetext', `Step ${index + 1}: ${frame.title}`);
    set('timeline-position', 'Step ' + (index + 1)); set('timeline-total', trace.frames.length + ' source-level steps');
    $('previous').disabled = index === 0; $('next').disabled = index === trace.frames.length - 1;
  }
  function setIndex(value) { index = Math.max(0, Math.min(trace.frames.length - 1, Number(value))); render(); }
  function pause() { if (timer !== null) clearTimeout(timer); timer = null; set('play-icon', '▶'); set('play-text', 'Play trace'); $('play').setAttribute('aria-label', 'Play trace'); }
  function play() {
    pause(); if (index === trace.frames.length - 1) setIndex(0);
    set('play-icon', 'Ⅱ'); set('play-text', 'Pause'); $('play').setAttribute('aria-label', 'Pause trace');
    const advance = () => { if (index >= trace.frames.length - 1) { pause(); return; } setIndex(index + 1); timer = setTimeout(advance, Number($('speed').value)); };
    timer = setTimeout(advance, Number($('speed').value));
  }
  function load(configuration) {
    const nextTrace = model.buildTrace(configuration); pause(); config = { ...configuration }; trace = nextTrace; index = 0;
    set('command-preview', config.raw !== undefined ? model.escapeBytes(model.bytes(config.raw)) : config.script.split('\n').join('  →  ')); set('scenario-description', config.description);
    $('commands').value = config.raw !== undefined ? 'PING' : config.script;
    $('fragment').value = String(config.fragment); $('write-chunk').value = String(config.writeChunk); $('capacity').value = String(config.capacity); $('block-once').checked = config.blockOnce;
    set('input-error', ''); render();
  }
  function setScenario(key) { $('scenario').value = key; load(key === 'custom' ? customConfig : model.presets[key]); }
  $('scenario').addEventListener('change', () => setScenario($('scenario').value));
  $('customize').addEventListener('click', () => { const opening = $('custom-panel').hidden; $('custom-panel').hidden = !opening; $('customize').setAttribute('aria-expanded', String(opening)); if (opening) { pause(); $('commands').focus(); } });
  $('custom-panel').addEventListener('submit', event => {
    event.preventDefault();
    const candidate = { title: 'Your custom trace', description: 'Your commands, using the same event loop and offset buffer.', script: $('commands').value, fragment: Number($('fragment').value), writeChunk: Number($('write-chunk').value), capacity: Number($('capacity').value), blockOnce: $('block-once').checked };
    try { load(candidate); customConfig = candidate; if (!$('scenario').querySelector('[value="custom"]')) $('scenario').append(new Option('08 / Your custom trace', 'custom')); $('scenario').value = 'custom'; }
    catch (error) { set('input-error', error.message); }
  });
  $('byte-format').addEventListener('click', () => { hex = !hex; $('byte-format').setAttribute('aria-pressed', String(hex)); set('byte-format', hex ? 'Show characters' : 'Show hex'); render(); });
  $('play').addEventListener('click', () => timer === null ? play() : pause());
  $('previous').addEventListener('click', () => { pause(); setIndex(index - 1); });
  $('next').addEventListener('click', () => { pause(); setIndex(index + 1); });
  $('restart').addEventListener('click', () => { pause(); setIndex(0); });
  $('timeline').addEventListener('input', () => { pause(); setIndex($('timeline').value); });
  $('speed').addEventListener('change', () => { if (timer !== null) play(); });
  for (const node of nodes) {
    const jump = () => { pause(); const key = node.dataset.stage; let next = trace.frames.findIndex((frame, i) => i > index && frame.stage === key); if (next < 0) next = trace.frames.findIndex(frame => frame.stage === key); if (next >= 0) setIndex(next); };
    node.addEventListener('click', jump); node.addEventListener('keydown', event => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); jump(); } });
  }
  document.addEventListener('keydown', event => {
    if (event.target.closest('input,textarea,select,button,[role="button"]')) return;
    if (event.key === ' ') { event.preventDefault(); timer === null ? play() : pause(); }
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') { event.preventDefault(); pause(); setIndex(index + (event.key === 'ArrowRight' ? 1 : -1)); }
  });
  document.addEventListener('visibilitychange', () => { if (document.hidden) pause(); });
  window.FlowApp = { getTrace: () => trace, getIndex: () => index, setIndex, setScenario, play, pause, load };
  load(config);
})();
