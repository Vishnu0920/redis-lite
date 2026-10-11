// Run with Node 18+ (or a newer version supporting structuredClone).
'use strict';
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const flow = require('../dist/model.js');
const traces = Object.fromEntries(Object.entries(flow.presets).map(([key, config]) => [key, flow.buildTrace(config)]));
const end = trace => trace.frames.at(-1).snapshot;

const encoded = flow.encodeCommand(['ECHO', 'hello 🌍']);
assert.equal(flow.parseCommand(encoded).arguments[1], 'hello 🌍');
assert.equal(flow.text(encoded), '*2\r\n$4\r\nECHO\r\n$10\r\nhello 🌍\r\n');
for (let i = 0; i < encoded.length; i++) assert.equal(flow.parseCommand(encoded.slice(0, i)).status, 'Incomplete');
const pipeline = encoded.concat(flow.encodeCommand(['PING']));
assert.equal(flow.parseCommand(pipeline).consumed, encoded.length);
assert.deepEqual(flow.parseCommand(pipeline).arguments, ['ECHO', 'hello 🌍']);
assert.equal(flow.parseCommand(flow.bytes('*129\r\n')).error, 'length exceeds limit');
assert.equal(flow.parseCommand(flow.bytes('*0\r\n')).error, 'empty command array');
assert.equal(flow.parseCommand(flow.bytes('*1\r\n$4\r\nPINGxx\r\n')).error, 'missing bulk string terminator');
assert.throws(() => flow.tokenize('SET key "open quote'), /double quote/);
assert.throws(() => flow.buildTrace({ script: 'ECHO "' + 'a'.repeat(1024) + '"', fragment: 8 }), /1,000 steps/, 'Long traces must report a limit instead of silently ending mid-request');
assert.deepEqual(flow.tokenize('ECHO ""'), ['ECHO', '']);
assert.equal(end(traces.normal).clientReplies[0].value, 'hello');
assert.equal(end(traces.normal).connState, 'Request');
assert.equal(end(traces.normal).exists, true);
assert.ok(end(traces.fragmented).cycle > 1);
assert.equal(end(traces.fragmented).clientReplies[0].value, 'hello 🌍');
assert.ok(traces.fragmented.frames.some(f => f.title === 'recv() would block: EAGAIN'));
assert.deepEqual(end(traces.pipeline).clientReplies.map(r => r.value), ['OK', 'hello world', 1]);
assert.ok(!end(traces.pipeline).database.some(e => e.key === 'greeting'));
assert.ok(traces.pipeline.frames.some(f => f.snapshot.begin > 0 && f.snapshot.begin < f.snapshot.storage.length));
assert.equal(end(traces.pipeline).inputCopied, 0);
for (let i = 0; i < traces.pipeline.frames.length; i++) {
  const frame = traces.pipeline.frames[i];
  if (frame.title === 'Next pipeline reply is queued') {
    const nextSend = traces.pipeline.frames.slice(i + 1).find(f => f.call.startsWith('send('));
    assert.ok(nextSend.snapshot.cycle > frame.snapshot.cycle, 'Queued pipeline reply must wait for another poll dispatch');
  }
}
const blocked = traces.partial.frames.find(f => f.title === 'send() would block: EAGAIN');
assert.equal(blocked.snapshot.sent, 5); assert.equal(blocked.snapshot.connState, 'Response');
assert.equal(blocked.snapshot.event, 'POLLIN', 'The first write uses the existing POLLIN event');
assert.ok(traces.partial.frames.some(f => f.title === 'POLLOUT resumes this connection'));
assert.equal(end(traces.partial).clientReplies[0].value, 'hello world');
const compact = traces.compaction.frames.find(f => f.title === 'Compact only when append needs space');
assert.equal(compact.snapshot.inputCopied, 15); assert.equal(compact.snapshot.begin, 0); assert.equal(compact.snapshot.storage.length, 15);
assert.equal(end(traces.compaction).clientReplies.length, 2);
assert.equal(end(traces.protocol).connState, 'End'); assert.equal(end(traces.protocol).exists, false);
assert.equal(end(traces.protocol).clientReplies[0].value, 'ERR Protocol error: missing bulk string terminator');
assert.equal(end(traces.protocol).database.length, 2);
assert.deepEqual(end(traces.sorted).clientReplies.slice(0, 2).map(r => r.value), [1, 1]);
assert.deepEqual(end(traces.sorted).clientReplies[2].value.map(r => r.value), ['alice', '10', 'charlie', '15', 'bob', '20']);
const wrong = flow.buildTrace({ script: 'GET leaderboard' });
assert.equal(end(wrong).clientReplies[0].type, '-'); assert.equal(end(wrong).connState, 'Request');
const infinity = flow.buildTrace({ script: 'ZADD scores inf max\nZSCORE scores max' });
assert.equal(end(infinity).clientReplies[1].value, 'inf');
assert.equal(end(infinity).database.find(e => e.key === 'scores').members[0].score, Infinity);
const hex = flow.buildTrace({ script: 'SET "<script>" "<img src=x onerror=alert(1)>"\nGET "<script>"' });
assert.equal(end(hex).clientReplies[1].value, '<img src=x onerror=alert(1)>');
for (const trace of Object.values(traces)) {
  for (const frame of trace.frames) {
    const s = frame.snapshot;
    assert.ok(s.begin >= 0 && s.begin <= s.storage.length);
    assert.ok(s.sent >= 0 && s.sent <= s.output.length);
    assert.ok(s.received <= flow.IO_BUDGET && s.written <= flow.IO_BUDGET);
    assert.ok(s.storage.length <= s.capacity);
  }
}

// Optional C++ oracle: exact encoded replies from the project's real parser,
// CommandProcessor and custom storage, with the same seeded keyspace.
const oracle = process.argv[2];
if (oracle) {
  const cases = [...Object.values(flow.presets),
    { script: 'PING\nECHO ""\nGET missing\nGET leaderboard\nSET key "🌍"\nGET key\nDEL key key missing\nKEYS *' },
    { script: 'ZADD scores 0.1 a inf b -inf c\nZSCORE scores a\nZSCORE scores b\nZRANK scores c\nZQUERY scores -inf "" 0 3\nZREM scores a b c\nZCARD scores' },
    { script: 'ECHO too many\nunknown\nZADD scores bad a\nSET key value\nZADD key 1 a\nZQUERY leaderboard -inf "" -1 2' }
  ];
  for (const config of cases) {
    const trace = flow.buildTrace(config);
    const native = spawnSync(oracle, [], { input: Buffer.from(trace.request), encoding: 'utf8' });
    assert.equal(native.status, 0, native.stderr);
    const expected = native.stdout.trim().split('\n').filter(Boolean);
    const actual = end(trace).clientReplies.map(reply => Buffer.from(reply.raw).toString('hex'));
    assert.deepEqual(actual, expected, `Native C++ reply mismatch for ${config.script}`);
  }
  console.log('Native C++ oracle: all preset and edge-case replies match byte for byte.');
}
console.log('Model checks passed: UTF-8 framing, fragmentation, pipeline scheduling, partial writes, compaction, storage, protocol close, and snapshot invariants.');
