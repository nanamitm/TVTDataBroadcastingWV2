// Exercise the same decoder and webpack compatibility adapter as the product,
// using complete CRC-protected PAT, PMT and AIT packets.
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const webpack = require('../browser/node_modules/webpack');
const root = path.resolve(__dirname, '..');
const browser = path.join(root, 'browser');
const config = require('../browser/webpack.config');
const dir = path.join(root, 'work/ait-transport-test');
function crc(bytes) {
  let value = 0xffffffff;
  for (const b of bytes) {
    value ^= b << 24;
    for (let i = 0; i < 8; i++) value = ((value << 1) ^ ((value & 0x80000000) ? 0x04c11db7 : 0)) >>> 0;
  }
  return value;
}
function section(id, extension, body, version = 0) {
  const length = 9 + body.length;
  const data = Buffer.from([id, 0xb0 | (length >> 8), length & 255, extension >> 8, extension & 255, 0xc1 | (version << 1), 0, 0, ...body]);
  const tail = Buffer.alloc(4); tail.writeUInt32BE(crc(data));
  return Buffer.concat([data, tail]);
}
const counter = new Map();
function packet(pid, data) {
  assert.ok(data.length < 184);
  const p = Buffer.alloc(188, 0xff), cc = counter.get(pid) ?? 0;
  counter.set(pid, (cc + 1) & 15);
  p.set([0x47, 0x40 | (pid >> 8), pid & 255, 0x10 | cc, 0]); p.set(data, 5);
  return p;
}
async function main() {
  fs.mkdirSync(dir, { recursive: true });
  const compiler = webpack({ ...config, mode: 'development', devtool: false, context: browser,
    entry: path.join(root, 'web-bml/dist/server/decode_ts.js'),
    output: { path: dir, filename: 'decoder.cjs', library: { type: 'commonjs2' } },
    resolveLoader: { modules: [path.join(browser, 'node_modules')] },
    module: { rules: [{ test: /\.ts$/, use: { loader: 'ts-loader', options: { transpileOnly: true } } }] } });
  await new Promise((resolve, reject) => compiler.run((error, stats) => compiler.close(() => {
    if (error || stats.hasErrors()) reject(error ?? new Error(stats.toString({ all: false, errors: true })));
    else resolve();
  })));
  const { decodeTS } = require(path.join(dir, 'decoder.cjs'));
  checkPMTUpdates(decodeTS);
  const messages = [], decoder = decodeTS({ serviceId: 1, parsePES: true, sendCallback: m => messages.push(m) });
  const pat = section(0, 1, [0, 1, 0xe1, 0]);
  const pmt = section(2, 1, [0xe1, 1, 0xf0, 0, 5, 0xe1, 0x20, 0xf0, 7, 0xfd, 2, 0, 9, 0x52, 1, 0x40]);
  const descriptors = [0x15, 10, ...Buffer.from('index.html')];
  const app = [0x89, 0xab, 0xcd, 0xef, 0x12, 0x34, 1, 0xf0, descriptors.length, ...descriptors];
  const ait = section(0x74, 0x10, [0xf0, 0, 0xf0, app.length, ...app]);
  const stream = [];
  for (let i = 0; i < 4; i++) stream.push(packet(0, pat), packet(0x100, pmt), packet(0x120, ait));
  decoder.push(Buffer.concat(stream));
  const result = messages.find(m => m.type === 'ait');
  assert.ok(result, 'PMT-discovered AIT reaches the application host');
  assert.equal(result.section.applications[0].organizationId, 0x89abcdef);
  assert.equal(result.section.applications[0].applicationId, 0x1234);
  assert.equal(result.section.applications[0].location, 'index.html');
  const broken = Buffer.from(section(0x74, 0x10, [0xf0, 0, 0xf0, app.length, ...app], 1));
  broken[broken.length - 1] ^= 1;
  const before = messages.filter(m => m.type === 'ait').length;
  decoder.push(Buffer.concat([packet(0x120, broken), packet(0x120, broken), packet(0x120, broken)]));
  assert.equal(messages.filter(m => m.type === 'ait').length, before, 'bad CRC is rejected');
  console.log('PAT/PMT/TS AIT routing, unsigned identifiers and CRC tests passed');
}
function checkPMTUpdates(decodeTS) {
  const updates = [];
  const decoder = decodeTS({ serviceId: 1, sendCallback: message => {
    if (message.type === 'pmt') updates.push(message);
  } });
  const pat = section(0, 1, [0, 1, 0xe1, 0]);
  function component(tag, pid, type, dataId, info = [0x33, 0x70, 0xf8, 0x80]) {
    const descriptors = [0x52, 1, tag];
    if (dataId != null) descriptors.push(0xfd, 2 + info.length, dataId >> 8, dataId & 255, ...info);
    return [type, 0xe0 | (pid >> 8), pid & 255, 0xf0, descriptors.length, ...descriptors];
  }
  let version = 0;
  function feed(components) {
    const pmt = section(2, 1, [0xe1, 1, 0xf0, 0, ...components.flat()], version++);
    const stream = [];
    for (let i = 0; i < 4; i++) stream.push(packet(0, pat), packet(0x100, pmt));
    decoder.push(Buffer.concat(stream));
  }
  const audio = component(0x10, 0x110, 0x0f);
  feed([audio, component(0x40, 0x120, 0x0d, 0x0c)]);
  assert.equal(updates.length, 1);
  feed([component(0x40, 0x120, 0x0d, 0x0c), audio]);
  assert.equal(updates.length, 1, 'PMT version/order alone must not notify unchanged components');
  feed([audio, component(0x40, 0x121, 0x0d, 0x0c)]);
  assert.equal(updates.length, 2, 'same-tag PID change must notify BML');
  assert.equal(updates.at(-1).components.find(c => c.componentId === 0x40).pid, 0x121);
  feed([audio, component(0x40, 0x121, 0x05, 0x0c)]);
  assert.equal(updates.length, 3, 'same-tag stream type change must notify BML');
  feed([audio, component(0x40, 0x121, 0x05, 0x0d)]);
  assert.equal(updates.length, 4, 'same-tag data component ID change must notify BML');
  feed([audio, component(0x40, 0x121, 0x05, 0x0d, [0x23, 0x70, 0xf8, 0x80])]);
  assert.equal(updates.length, 5, 'nested BXML entry point change must notify BML');
  assert.equal(updates.at(-1).components.find(c => c.componentId === 0x40).bxmlInfo.entryPointInfo.autoStartFlag, false);
  feed([audio, component(0x40, 0x121, 0x05, 0x0d, [0x23, 0x70, 0xf8, 0xa0])]);
  assert.equal(updates.length, 6, 'nested carousel priority change must notify BML');
  feed([audio, component(0x40, 0x121, 0x05)]);
  assert.equal(updates.length, 7, 'descriptor removal must notify BML');
  feed([audio]);
  assert.equal(updates.length, 8, 'component removal must notify BML');
  feed([audio]);
  assert.equal(updates.length, 8, 'repeated identical PMT must not notify twice');
  console.log('PMT PID, stream type, descriptors, removal and unchanged-version tests passed');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
