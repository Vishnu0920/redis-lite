/* Browser-local teaching model of event_loop.cpp + InputBuffer. No TCP socket is opened. */
(function (root) {
  'use strict';
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();
  const bytes = value => Array.from(encoder.encode(value));
  const text = value => decoder.decode(new Uint8Array(value));
  const clone = value => structuredClone(value);
  const LIMIT = 1024 * 1024;
  const IO_BUDGET = 64 * 1024;
  const bulk = value => `$${bytes(value).length}\r\n${value}\r\n`;
  const encodeCommand = args => bytes(`*${args.length}\r\n${args.map(bulk).join('')}`);
  const escapeBytes = data => data.map(b => b === 13 ? '\\r' : b === 10 ? '\\n' : b >= 32 && b < 127 ? String.fromCharCode(b) : `\\x${b.toString(16).padStart(2, '0')}`).join('');
  const compare = (a, b) => {
    const left = bytes(a), right = bytes(b);
    for (let i = 0; i < Math.min(left.length, right.length); i++) if (left[i] !== right[i]) return left[i] - right[i];
    return left.length - right.length;
  };
  function hashKey(key) {
    let hash = 14695981039346656037n;
    for (const byte of bytes(key)) hash = BigInt.asUintN(64, (hash ^ BigInt(byte)) * 1099511628211n);
    return { hex: '0x' + hash.toString(16).padStart(16, '0'), bucket: Number(hash & 3n) };
  }
  function tokenize(line) {
    const args = []; let i = 0;
    while (i < line.length) {
      while (i < line.length && /\s/.test(line[i])) i++;
      if (i === line.length) break;
      let arg = '';
      if (line[i] === '"') {
        i++; let closed = false;
        while (i < line.length) {
          if (line[i] === '"') { i++; closed = true; break; }
          if (line[i] === '\\' && i + 1 < line.length) i++;
          arg += line[i++];
        }
        if (!closed) throw new Error('Close the double quote in your command.');
      } else while (i < line.length && !/\s/.test(line[i])) arg += line[i++];
      args.push(arg);
    }
    return args;
  }
  function parseCommand(input) {
    let pos = 0; const tokens = [], args = [];
    const fail = error => { throw { status: 'Error', error }; };
    const need = count => {
      if (count > LIMIT - pos) fail('frame too large');
      if (count > input.length - pos) throw { status: 'Incomplete', error: '' };
    };
    const marker = expected => { need(1); if (input[pos] !== expected.charCodeAt(0)) fail('unexpected RESP type'); pos++; };
    const number = limit => {
      let value = 0, digits = 0;
      while (true) {
        need(1); const ch = input[pos];
        if (ch === 13) {
          if (!digits) fail('empty length');
          need(2); if (input[pos + 1] !== 10) fail('expected CRLF'); pos += 2; return value;
        }
        if (ch < 48 || ch > 57) fail('invalid length');
        if (++digits > 20) fail('length header too long');
        const digit = ch - 48;
        if (value > Math.floor(limit / 10) || (value === Math.floor(limit / 10) && digit > limit % 10)) fail('length exceeds limit');
        value = value * 10 + digit; pos++;
      }
    };
    try {
      marker('*'); const count = number(128);
      tokens.push({ start: 0, end: pos, call: "Reader::marker('*') + number(MaxArgs)", title: `Array header: ${count} arguments`, detail: 'The count includes the command name. RESP lengths count bytes.' });
      if (!count) fail('empty command array');
      for (let i = 0; i < count; i++) {
        const start = pos; marker('$'); const length = number(65536); const payloadStart = pos;
        need(length + 2);
        if (input[pos + length] !== 13 || input[pos + length + 1] !== 10) fail('missing bulk string terminator');
        const value = text(input.slice(pos, pos + length)); args.push(value); pos += length + 2;
        tokens.push({ start, end: pos, payloadStart, length, value, call: 'Reader::readBulk()', title: `Argument ${i}: ${JSON.stringify(value)}`, detail: `$${length} describes ${length} payload bytes, followed by CRLF. readBulk copies the payload into a string.` });
      }
      return { status: 'Complete', arguments: args, consumed: pos, error: '', cursor: pos, tokens };
    } catch (result) {
      return { status: result.status, arguments: [], consumed: 0, error: result.error, cursor: pos, tokens };
    }
  }
  function parseReply(input) {
    let pos = 0;
    const need = count => { if (pos + count > input.length) throw 'Incomplete'; };
    const line = () => {
      const start = pos;
      while (pos + 1 < input.length && !(input[pos] === 13 && input[pos + 1] === 10)) pos++;
      if (pos + 1 >= input.length) throw 'Incomplete';
      const value = text(input.slice(start, pos)); pos += 2; return value;
    };
    const read = () => {
      need(1); const type = String.fromCharCode(input[pos++]); const header = line();
      if (type === '+' || type === '-') return { type, value: header };
      if (type === ':') return { type, value: Number(header) };
      if (type === '$') {
        const length = Number(header); if (length === -1) return { type, value: null };
        need(length + 2); const value = text(input.slice(pos, pos + length)); pos += length + 2; return { type, value };
      }
      if (type === '*') {
        const count = Number(header), value = []; for (let i = 0; i < count; i++) value.push(read()); return { type, value };
      }
      throw 'Error';
    };
    try { const reply = read(); return { status: 'Complete', ...reply, consumed: pos }; }
    catch (status) { return { status, consumed: 0 }; }
  }
  const simple = value => '+' + value + '\r\n';
  const errorReply = value => '-' + value + '\r\n';
  const integer = value => ':' + value + '\r\n';
  const array = elements => '*' + elements.length + '\r\n' + elements.join('');
  const scoreString = value => {
    if (!Number.isFinite(value)) return value < 0 ? '-inf' : 'inf';
    if (Object.is(value, -0)) return '-0';
    let result = value.toPrecision(17);
    if (result.includes('e')) {
      let [mantissa, exponent] = result.split('e');
      mantissa = mantissa.replace(/(\.\d*?)0+$/, '$1').replace(/\.$/, '');
      return mantissa + 'e' + (Number(exponent) < 0 ? '-' : '+') + String(Math.abs(Number(exponent))).padStart(2, '0');
    }
    result = result.replace(/(\.\d*?)0+$/, '$1').replace(/\.$/, '');
    if (value !== 0 && Math.abs(value) < 0.0001) {
      const [mantissa, exponent] = value.toExponential(16).split('e');
      return mantissa.replace(/(\.\d*?)0+$/, '$1').replace(/\.$/, '') + 'e-' + String(Math.abs(Number(exponent))).padStart(2, '0');
    }
    return result;
  };
  function execute(args, database) {
    const name = args[0].replace(/[a-z]/g, c => c.toUpperCase());
    const badArity = () => ({ reply: errorReply('ERR wrong number of arguments for ' + name), operation: 'Argument validation', usesStorage: false });
    const wrongType = () => ({ reply: errorReply('WRONGTYPE Operation against a key holding the wrong kind of value'), operation: 'Storage returns WrongType', usesStorage: true });
    const get = key => database.find(entry => entry.key === key);
    const remove = key => { const index = database.findIndex(entry => entry.key === key); if (index < 0) return false; database.splice(index, 1); return true; };
    const result = (reply, operation, usesStorage = true) => ({ reply, operation, usesStorage });
    const score = value => /\s/.test(value) || value === '' || /nan/i.test(value) ? NaN : /^[-+]?inf(inity)?$/i.test(value) ? (value.startsWith('-') ? -Infinity : Infinity) : Number(value);
    const signed = value => /^-?\d+$/.test(value) && BigInt(value) >= -(1n << 63n) && BigInt(value) < (1n << 63n);
    if (name === 'PING') return args.length <= 2 ? result(args.length === 1 ? simple('PONG') : bulk(args[1]), 'PING encodes a reply directly', false) : badArity();
    if (name === 'ECHO') return args.length === 2 ? result(bulk(args[1]), 'ECHO encodes the argument as a bulk string', false) : badArity();
    if (name === 'GET') {
      if (args.length !== 2) return badArity(); const entry = get(args[1]);
      if (entry && entry.type !== 'string') return wrongType();
      return result(entry ? bulk(entry.value) : '$-1\r\n', `Storage::get(${JSON.stringify(args[1])}) → entries_.find()`);
    }
    if (name === 'SET') {
      if (args.length !== 3) return badArity(); const entry = get(args[1]);
      if (entry) { entry.type = 'string'; entry.value = args[2]; delete entry.members; }
      else database.push({ key: args[1], type: 'string', value: args[2] });
      return result(simple('OK'), `Storage::set(${JSON.stringify(args[1])}) → replace or insert Entry`);
    }
    if (name === 'DEL') {
      if (args.length < 2) return badArity(); let count = 0; for (const key of args.slice(1)) count += remove(key);
      return result(integer(count), 'Storage::erase() → detach and destroy matching entries');
    }
    if (name === 'KEYS') {
      if (args.length > 2) return badArity();
      if (args.length === 2 && args[1] !== '*') return result(errorReply('ERR KEYS supports only *'), 'Argument validation', false);
      return result(array(database.map(e => e.key).sort(compare).map(bulk)), 'Storage::keys() → collect and sort keys');
    }
    if (name === 'ZADD') {
      if (args.length < 4 || args.length % 2) return badArity(); const members = [];
      for (let i = 2; i < args.length; i += 2) {
        const number = score(args[i]); if (Number.isNaN(number) || (!Number.isFinite(number) && !/inf/i.test(args[i]))) return result(errorReply('ERR score is not a valid number'), 'Validate every score before mutation', false);
        members.push({ name: args[i + 1], score: number });
      }
      let entry = get(args[1]); if (entry && entry.type !== 'sorted set') return wrongType();
      if (!entry) { entry = { key: args[1], type: 'sorted set', members: [] }; database.push(entry); }
      let added = 0; for (const member of members) { const existing = entry.members.find(m => m.name === member.name); if (existing) existing.score = member.score; else { entry.members.push(member); added++; } }
      entry.members.sort((a, b) => (a.score === b.score ? compare(a.name, b.name) : a.score < b.score ? -1 : 1));
      return result(integer(added), 'Storage::zadd() → name hash + score-ordered AVL index');
    }
    if (['ZREM', 'ZSCORE', 'ZRANK', 'ZCARD', 'ZQUERY'].includes(name)) {
      if ((name === 'ZREM' && args.length < 3) || (['ZSCORE', 'ZRANK'].includes(name) && args.length !== 3) || (name === 'ZCARD' && args.length !== 2) || (name === 'ZQUERY' && args.length !== 6)) return badArity();
      let lower, offset, limit;
      if (name === 'ZQUERY') {
        lower = score(args[2]); if (Number.isNaN(lower)) return result(errorReply('ERR score is not a valid number'), 'Validate score', false);
        if (!signed(args[4]) || !signed(args[5])) return result(errorReply('ERR offset and limit must be signed 64-bit integers'), 'Validate signed offset and limit', false);
        offset = Number(args[4]); limit = Math.max(0, Number(args[5]));
      }
      const entry = get(args[1]); if (entry && entry.type !== 'sorted set') return wrongType();
      const members = entry ? entry.members : [];
      if (name === 'ZREM') {
        let removed = 0; for (const member of args.slice(2)) { const i = members.findIndex(m => m.name === member); if (i >= 0) { members.splice(i, 1); removed++; } }
        if (entry && !members.length) remove(args[1]); return result(integer(removed), 'Storage::zrem() → remove from both indexes');
      }
      if (name === 'ZCARD') return result(integer(members.length), 'Storage::zcard() → name-index size');
      if (name === 'ZSCORE' || name === 'ZRANK') {
        const index = members.findIndex(m => m.name === args[2]);
        return result(index < 0 ? '$-1\r\n' : name === 'ZRANK' ? integer(index) : bulk(scoreString(members[index].score)), name === 'ZRANK' ? 'Storage::zrank() → name lookup, then AVL subtree counts' : 'Storage::zscore() → lookup member by name');
      }
      const index = members.findIndex(m => m.score > lower || (m.score === lower && compare(m.name, args[3]) >= 0));
      const first = index < 0 ? -1 : index + offset;
      const selected = first < 0 || first >= members.length || limit === 0 ? [] : members.slice(first, first + limit);
      return result(array(selected.flatMap(m => [bulk(m.name), bulk(scoreString(m.score))])), 'Storage::zquery() → lowerBound, rank offset, next()');
    }
    return result(errorReply('ERR unknown command'), 'Command lookup: unknown command', false);
  }
  const presets = {
    normal: { title: 'A complete request', description: 'GET reads a seeded string and returns a RESP2 bulk reply.', script: 'GET greeting', fragment: 0, writeChunk: 0, blockOnce: false, capacity: 64 },
    fragmented: { title: 'Fragmented input', description: 'Small deliveries split an ECHO request. Incomplete bytes survive between poll cycles.', script: 'ECHO "hello 🌍"', fragment: 8, writeChunk: 0, blockOnce: false, capacity: 64 },
    pipeline: { title: 'Pipelined commands', description: 'A protocol client sends SET, GET, and DEL together. One command and one reply are processed at a time.', script: 'SET greeting "hello world"\nGET greeting\nDEL greeting', fragment: 0, writeChunk: 0, blockOnce: false, capacity: 128 },
    partial: { title: 'Partial writes + EAGAIN', description: 'The first optimistic send writes five bytes. The next send would block; POLLOUT resumes the unsent suffix.', script: 'ECHO "hello world"', fragment: 0, writeChunk: 5, blockOnce: true, capacity: 64 },
    compaction: { title: 'Watch buffer compaction', description: 'After ECHO consumes 45 of 60 stored bytes, the next append needs space. Only the 15 unread bytes move.', script: 'ECHO abcdefghijklmnopqrstuvwx\nGET greeting', fragment: 60, writeChunk: 0, blockOnce: false, capacity: 64 },
    sorted: { title: 'Sorted-set storage', description: 'Follow ZADD, ZRANK, and ZQUERY through the storage interface, then decode integer and array replies.', script: 'ZADD leaderboard 15 charlie\nZRANK leaderboard charlie\nZQUERY leaderboard -inf "" 0 3', fragment: 0, writeChunk: 0, blockOnce: false, capacity: 128 },
    protocol: { title: 'A protocol error', description: 'A malformed bulk terminator produces an error reply. The server drains it before destroying Conn.', script: '(malformed RESP2 bytes)', raw: '*1\r\n$4\r\nPINGxx\r\n', fragment: 0, writeChunk: 0, blockOnce: false, capacity: 64 }
  };
  function buildTrace(configuration = presets.normal) {
    const config = { ...presets.normal, ...configuration };
    const commands = config.raw !== undefined ? [] : config.script.split('\n').filter(line => line.trim()).map(tokenize);
    if (config.raw === undefined && !commands.length) throw new Error('Enter at least one command.');
    if (commands.some(args => args.length < 1 || args.length > 128 || args.some(arg => bytes(arg).length > 65536))) throw new Error('Commands need 1–128 arguments, with at most 64 KiB per argument.');
    const frames = [];
    const state = {
      connected: false, exists: false, cycle: 0, interest: '—', event: '—', connState: '—', storage: [], begin: 0, capacity: Number(config.capacity), inputCopied: 0,
      output: [], sent: 0, closeAfterWrite: false, received: 0, written: 0, mutex: false,
      clientRequest: [], networkRemaining: [], clientKernel: [], clientInput: [], clientReplies: [],
      parser: { status: 'Not called', arguments: [], consumed: 0, cursor: 0, tokens: [] },
      database: [{ key: 'greeting', type: 'string', value: 'hello' }, { key: 'leaderboard', type: 'sorted set', members: [{ name: 'alice', score: 10 }, { name: 'bob', score: 20 }] }],
      command: [], storageOperation: '', activeKey: '', replyValue: '', requestIndex: 0, highlight: null, note: ''
    };
    const record = (stage, title, detail, call, source, patch = {}) => {
      if (frames.length >= 1000) throw new Error('This trace exceeds 1,000 steps. Choose larger deliveries or send chunks, or use fewer commands. The server has no such step limit.');
      Object.assign(state, patch); frames.push({ stage, title, detail, call, source, snapshot: clone(state) });
    };
    const source = { loop: 'server/runners/event_loop.cpp', buffer: 'server/buffers/input_buffer.h', protocol: 'common/resp.cpp', commands: 'server/commands/commands.cpp', storage: 'server/storage/storage.cpp', client: 'client/main.cpp' };
    record('client', 'Start with command arguments', 'The demo starts with a seeded in-memory database: greeting = hello; leaderboard contains alice and bob. Time and network deliveries are illustrative.', 'query(fd, args, input)', source.client);
    record('poll', 'Create the connection', 'main() selects runEventLoop<InputBuffer>(). handleNewConnections() accepts a nonblocking fd and stores an owning Conn in fd2Conn. The new connection joins the next poll snapshot.', 'handleNewConnections() → Conn<InputBuffer>', source.loop, { connected: true, exists: true, connState: 'Request', interest: 'POLLIN' });
    let request = config.raw !== undefined ? bytes(config.raw) : commands.flatMap(encodeCommand);
    if (request.length > 16384) throw new Error('Use at most 16 KiB of request bytes for this teaching view. The server’s frame limit is 1 MiB.');
    record('encode', 'Arguments become RESP2 bytes', config.raw !== undefined ? 'This scenario supplies deliberately malformed bytes.' : 'encodeCommand() writes an array header and a byte-length-prefixed bulk string for every argument. CRLF terminates headers and values.', 'resp::encodeCommand(arguments)', source.protocol, { clientRequest: request });
    record('network', 'The client sends its request', commands.length > 1 ? 'This protocol-client demonstration pipelines several encoded commands. The supplied C++ CLI normally sends one command and waits for its reply.' : 'net::sendAll() writes the encoded bytes on the client’s blocking socket. TCP preserves byte order, but not command boundaries.', 'net::sendAll(fd, request)', 'common/net.h', { networkRemaining: request.slice() });
    let blockPending = Boolean(config.blockOnce), hasSent = false;
    function append(chunk) {
      const remaining = state.storage.length - state.begin;
      const spare = state.capacity - state.storage.length;
      if (chunk.length > spare && state.begin > 0 && state.begin >= remaining) {
        const oldBegin = state.begin;
        state.storage = state.storage.slice(state.begin); state.begin = 0; state.inputCopied += remaining;
        record('buffer', 'Compact only when append needs space', `Incoming ${chunk.length} B exceeds ${spare} B of tail room, and consumed ${oldBegin} B ≥ unread ${remaining} B. Erase the consumed prefix; move only ${remaining} unread bytes.`, 'InputBuffer::append() → storage_.erase(0, begin_)', source.buffer);
      }
      if (state.storage.length + chunk.length > state.capacity) {
        state.capacity = Math.max(state.storage.length + chunk.length, state.capacity * 2);
        record('buffer', 'Grow backing storage', 'The unread prefix is less than half the stored data, or no bytes have been consumed. std::string can grow instead. The displayed capacity/growth policy is illustrative.', 'storage_.append(data, length)', source.buffer);
      }
      state.storage.push(...chunk);
      record('buffer', 'Append the bytes; keep the read offset', 'Appending does not normally shift the unread bytes. input.view() will expose storage_ starting at begin_.', 'InputBuffer::append(buffer, n)', source.buffer);
    }
    function prepare(context) {
      const input = state.storage.slice(state.begin);
      record('parser', 'Parse before receiving more', context === 'pipeline' ? 'handleWrite() has drained the previous reply. prepareResponse() inspects retained pipeline bytes once; this call performs no recv().' : 'handleRead() calls prepareResponse() first, even before its first recv(). The parser sees only unread bytes through a string_view.', 'prepareResponse() → resp::parseCommand(input.view())', source.loop, { parser: { status: 'Parsing', arguments: [], consumed: 0, cursor: 0, tokens: [] }, highlight: null });
      const result = parseCommand(input);
      for (const token of result.tokens) {
        state.parser.cursor = token.end; state.parser.tokens.push(token);
        record('parser', token.title, token.detail, token.call, source.protocol, { highlight: { start: state.begin + token.start, end: state.begin + token.end } });
      }
      record('parser', result.status === 'Complete' ? 'One command is complete' : result.status === 'Incomplete' ? 'More bytes are needed' : 'The request is malformed', result.status === 'Complete' ? `The result contains ${result.arguments.length} argument strings and consumed = ${result.consumed}. Following commands remain unparsed.` : result.status === 'Incomplete' ? 'The result clears partial arguments and consumes zero bytes. Retain the buffer; no command runs.' : result.error + '. The runner will send a protocol error and close after writing it.', 'return CommandResult', source.protocol, { parser: result, highlight: null });
      if (result.status === 'Incomplete') return false;
      if (result.status === 'Error') {
        state.output = bytes(errorReply('ERR Protocol error: ' + result.error)); state.storage = []; state.begin = 0; state.closeAfterWrite = true;
        record('reply', 'Queue a protocol-error reply', 'Clear input and set closeAfterWrite. Sending remains nonblocking; the error reply must drain before the normal close path.', 'conn.output = resp::error(...); conn.input.clear()', source.loop);
      } else {
        const args = result.arguments, name = args[0].replace(/[a-z]/g, ch => ch.toUpperCase());
        state.command = args; state.requestIndex++;
        record('command', 'Dispatch the command', `${name} receives a vector of argument strings. Command execution has no socket and is shared by both server modes.`, 'CommandProcessor::execute(arguments)', source.commands, { mutex: !['PING', 'ECHO'].includes(name), activeKey: args[1] || '' });
        const outcome = execute(args, state.database);
        state.storageOperation = outcome.operation;
        if (outcome.usesStorage) record('storage', 'Call the storage interface', outcome.operation + '. The custom C++ backend owns Entries in a hash table; sorted sets also use a name index and an AVL index.', outcome.operation.split(' → ')[0], source.storage);
        else record('command', 'Construct a direct reply', outcome.operation + '. PING/ECHO bypass the mutex; command errors are ordinary replies and keep the connection open.', 'CommandProcessor::execute()', source.commands);
        state.replyValue = outcome.reply;
        record('reply', 'Encode the result as RESP2', 'Simple strings use +, bulk strings use $ and byte lengths, integers use :, arrays use *, and errors use -. The reply is fully constructed before the command mutex is released.', 'resp::simple / bulk / integer / array / error', source.protocol);
        state.output = bytes(outcome.reply); state.mutex = false;
        record('reply', 'Store one pending reply in Conn', 'execute() returns and releases the storage mutex before socket I/O. Conn holds one output string, so a slow reader cannot grow a reply queue.', 'conn.output = conn.commands.execute(...)', source.loop);
        state.begin += result.consumed;
        record('buffer', 'Consume by advancing begin_', `begin_ advances by ${result.consumed} B. The unread suffix stays in place; consuming this request does not erase or copy it.`, 'InputBuffer::consume(result.consumed)', source.buffer);
        if (state.begin === state.storage.length) {
          state.storage = []; state.begin = 0;
          record('buffer', 'The input is empty: reset for reuse', 'consume() calls clear() when every stored byte is consumed. begin_ becomes zero, but the std::string allocation can be reused.', 'InputBuffer::clear()', source.buffer);
        }
      }
      record('reply', 'Switch Conn into Response state', 'sent = 0. A read handler returns now; the dispatcher immediately checks its separate write condition.', 'conn.sent = 0; conn.state = State::Response', source.loop, { sent: 0, connState: 'Response' });
      return true;
    }
    function clientReceive() {
      record('decode', 'The client checks its retained reply bytes', 'query() tries parseReply() before receiving. A server send means local socket acceptance; client reads occur independently.', 'resp::parseReply(client.input)', source.client);
      if (state.clientKernel.length) {
        const n = state.clientKernel.length; state.clientInput.push(...state.clientKernel); state.clientKernel = [];
        record('decode', 'Receive reply bytes at the client', `receiveMore() appends ${n} B. A partial reply remains in the client’s own input string.`, 'net::receiveMore(fd, input)', source.client);
      }
      while (true) {
        const parsed = parseReply(state.clientInput);
        if (parsed.status !== 'Complete') {
          if (state.clientInput.length) record('decode', 'The reply is still incomplete', 'parseReply() needs more bytes. Preserve client input; no reply is printed yet.', 'resp::parseReply() → Incomplete', source.protocol);
          break;
        }
        const consumed = state.clientInput.splice(0, parsed.consumed);
        state.clientReplies.push({ type: parsed.type, value: parsed.value, raw: consumed });
        record('decode', 'Decode and print the reply', 'The client prints the decoded value, erases only reply.consumed bytes, and preserves any following reply bytes.', 'parseReply() → printReply() → input.erase()', source.client);
      }
    }
    for (let turn = 0; turn < 10000; turn++) {
      state.cycle++; state.interest = state.connState === 'Request' ? 'POLLIN' : 'POLLOUT'; state.event = '—';
      record('poll', 'Rebuild the poll snapshot', 'Request watches POLLIN; Response watches POLLOUT. The listener also watches POLLIN. poll(..., -1) waits without a periodic timer.', 'runEventLoop() → poll(pollfds, -1)', source.loop);
      state.event = state.interest;
      record('poll', state.event + ' resumes this connection', 'The dispatcher visits this ready fd. The snapshot remains the same throughout this client turn.', 'pfd.revents → dispatch Conn', source.loop);
      if (state.connState === 'Request') {
        state.received = 0;
        record('read', 'Enter the read handler', 'received starts at zero for this invocation. At most 64 KiB are read per handler turn, in recv() calls of at most 4096 bytes.', 'handleRead(conn)', source.loop);
        let available = Math.min(config.fragment || state.networkRemaining.length, state.networkRemaining.length);
        while (state.connState === 'Request') {
          if (prepare('read')) break;
          if (state.received >= IO_BUDGET) { record('read', 'Read budget exhausted', 'Return with Request state and unread input preserved. Parsing was attempted before the budget check.', 'if (received >= IoBudget) return', source.loop); break; }
          if (!available) { record('read', 'recv() would block: EAGAIN', 'No more bytes are currently available. Return with Request state; the write condition fails, and a later POLLIN resumes reading.', 'recv() → EAGAIN → return', source.loop); break; }
          const n = Math.min(4096, available, IO_BUDGET - state.received), chunk = state.networkRemaining.splice(0, n); available -= n;
          record('read', `recv() returns ${n} bytes`, 'Bytes first land in the 4096-byte local receive array. They are then appended to the persistent input buffer.', 'recv(conn.socket.get(), buffer, count, 0)', source.loop, { note: escapeBytes(chunk) });
          append(chunk);
          state.received += n;
          record('read', 'Count the bytes read this turn', `received is now ${state.received} B. The loop parses the retained input again before checking the budget or receiving more.`, 'received += n; continue', source.loop);
        }
      }
      if (state.connState === 'Response') {
        state.written = 0;
        record('write', state.event === 'POLLIN' ? 'Try an optimistic write now' : 'Resume the pending reply', state.event === 'POLLIN' ? 'The read created a Response. The second if accepts POLLIN, so handleWrite() runs in the same dispatch without waiting for POLLOUT.' : 'Conn.output and sent survived the previous return. POLLOUT allows the handler to try the unsent suffix.', 'handleWrite(conn)', source.loop);
        let blocked = false;
        while (state.sent < state.output.length && state.written < IO_BUDGET) {
          if (blockPending && hasSent) {
            blockPending = false; blocked = true;
            record('write', 'send() would block: EAGAIN', 'Keep Response state, output, and sent. Return to other clients; the next poll snapshot watches POLLOUT.', 'send() → EAGAIN → return', source.loop); break;
          }
          const n = Math.min(config.writeChunk || state.output.length, state.output.length - state.sent, IO_BUDGET - state.written);
          const chunk = state.output.slice(state.sent, state.sent + n); state.sent += n; state.written += n; state.clientKernel.push(...chunk); hasSent = true;
          record('write', `send() accepts ${n} bytes`, `Advance sent to ${state.sent}. The next call starts at output.data() + sent. These bytes have reached socket buffers; client receipt is a separate event.`, 'send(fd, output.data() + sent, count, MSG_NOSIGNAL)', source.loop);
          clientReceive();
        }
        if (blocked || state.sent < state.output.length) {
          if (!blocked) record('write', 'Write budget exhausted', 'Keep the unsent output suffix and Response state for a later POLLOUT turn.', 'if (conn.sent != conn.output.size()) return', source.loop);
        } else {
          state.output = []; state.sent = 0; state.connState = state.closeAfterWrite ? 'End' : 'Request';
          record('write', 'The entire reply has drained', 'Clear output and reset sent. A normal reply returns Conn to Request; a protocol-error reply changes it to End.', 'conn.output.clear(); conn.sent = 0', source.loop);
          if (state.connState === 'Request') {
            prepare('pipeline');
            record('poll', state.connState === 'Response' ? 'Next pipeline reply is queued' : 'No complete buffered command remains', state.connState === 'Response' ? 'handleWrite() returns after preparing this reply. It is not sent immediately. The next poll must watch POLLOUT even if no new input arrives.' : 'handleWrite() returns. Retain any incomplete bytes, and watch POLLIN on the next poll.', 'return from handleWrite()', source.loop);
          }
        }
      }
      if (state.connState === 'End') {
        state.exists = false; state.connected = false; state.interest = '—'; state.storage = []; state.begin = 0;
        record('done', 'Destroy Conn after the error reply', 'fd2Conn[fd].reset() destroys Conn, closes its socket, and frees connection buffers. The shared database remains.', 'fd2Conn[fd].reset()', source.loop); break;
      }
      if (state.connState === 'Request' && !state.networkRemaining.length) {
        record('done', 'The round trip is complete', 'The connection remains open in Request state. A future poll waits for POLLIN; stored values survive until deleted or the server stops.', 'next poll: POLLIN', source.loop, { interest: 'POLLIN', event: '—' }); break;
      }
    }
    if (frames.at(-1).stage !== 'done') throw new Error('The trace could not complete. Choose a smaller scenario.');
    return { frames, config, commands, request };
  }
  const api = { bytes, text, escapeBytes, tokenize, encodeCommand, parseCommand, parseReply, execute, hashKey, presets, buildTrace, IO_BUDGET };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.RedisFlow = api;
})(typeof window !== 'undefined' ? window : globalThis);
