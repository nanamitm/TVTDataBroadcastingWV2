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
main().catch(error => { console.error(error); process.exitCode = 1; });
