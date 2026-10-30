const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { JSDOM } = require('../browser/node_modules/jsdom');
const ts = require('../browser/node_modules/typescript');
const { window } = new JSDOM();
const source = fs.readFileSync(require.resolve('../browser/src/ait_controlled_app.ts'), 'utf8');
const context = { exports: {}, URL, DOMParser: window.DOMParser, console };
vm.createContext(context);
vm.runInContext(ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText, context);
const { AITControlledAppHost, parseXMLAIT } = context.exports;
const xml = (code = 'PRESENT', location = 'index.html') => `<ServiceDiscovery xmlns="urn:test"><Application>
  <applicationIdentifier><orgId>42</orgId><appId>7</appId></applicationIdentifier>
  <applicationDescriptor><type><ISDB-HTML/></type><controlCode>${code}</controlCode><priority>0a</priority></applicationDescriptor>
  <applicationTransport><URLBase>https://example.test/app/</URLBase></applicationTransport>
  <applicationLocation>${location}</applicationLocation>
  <boundaryAndPermission><permissionBitmap>ff</permissionBitmap><managedURL>example.test/app/</managedURL></boundaryAndPermission>
</Application></ServiceDiscovery>`;
async function main() {
  assert.equal(parseXMLAIT('<broken>', 'https://example.test/ait'), null);
  const parsed = parseXMLAIT(xml(), 'https://example.test/ait');
  assert.equal(parsed.applications[0].url, 'https://example.test/app/index.html');
  assert.equal(parsed.applications[0].boundaries[0].permissionBitmaps[0], 255);
  assert.equal(parseXMLAIT(xml('PRESENT', 'javascript:alert(1)'), '').applications[0].url, null);
  const pending = [], launched = [];
  const host = new AITControlledAppHost({ supportedApplicationTypes: new Set([0x10]),
    fetchXMLAIT: uri => new Promise(resolve => pending.push({ uri, resolve })),
    launch: app => { launched.push(app); return true; } });
  const uri = 'https://example.test/ait';
  assert.equal(host.getReceivedAIT(uri, 'doc1'), -2);
  assert.equal(host.startReceivingAIT(uri, 'doc1'), 1);
  host.startReceivingAIT(uri, 'doc1');
  assert.equal(pending.length, 1, 'polling does not restart an in-flight request');
  assert.equal(host.getReceivedAIT(uri, 'doc1'), 0);
  pending.shift().resolve(xml());
  await new Promise(setImmediate);
  assert.equal(host.getReceivedAIT(uri, 'doc1')[1][0], '2a');
  assert.ok(Number.isNaN(host.startAITControlledApp('2az', '7', uri, 'doc1')));
  host.startAITControlledApp('2a', '7', uri, 'doc1');
  host.getReceivedAIT(uri, 'doc2');
  pending.shift().resolve(xml());
  await new Promise(setImmediate);
  assert.equal(launched.length, 0, 'old document cannot launch after navigation');
  host.startAITControlledApp('0', '7', uri, 'doc2');
  pending.shift().resolve(xml());
  await new Promise(setImmediate);
  assert.equal(launched.length, 1, 'wildcard organization launches PRESENT app');
  host.startAITControlledApp('2a', '7', uri, 'doc2');
  pending.shift().resolve(xml('KILL'));
  await new Promise(setImmediate);
  assert.equal(launched.length, 1, 'KILL app cannot launch');
  host.startAITControlledApp('2a', '7', uri, 'doc2');
  host.reset();
  pending.shift().resolve(xml());
  await new Promise(setImmediate);
  assert.equal(launched.length, 1, 'service reset invalidates pending launches');
  for (let i = 0; i < 10; i++) assert.equal(host.startReceivingAIT(uri + i, 'doc3'), 1);
  assert.ok(Number.isNaN(host.startReceivingAIT(uri + 'overflow', 'doc3')));
  console.log('AIT XML, lifecycle, control-code and request-limit tests passed');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
