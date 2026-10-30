const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { JSDOM } = require('../browser/node_modules/jsdom');
const ts = require('../browser/node_modules/typescript');
const dom = new JSDOM();
function load(name) {
  const source = fs.readFileSync(require.resolve('../browser/src/' + name + '.ts'), 'utf8');
  const context = { exports: {}, URL, DOMParser: dom.window.DOMParser, console,
    require: name => load(name.replace('./', '')) };
  vm.createContext(context);
  vm.runInContext(ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText, context);
  return context.exports;
}
const { HybridcastHost } = load('hybridcast');
const messages = [];
let available = true;
const host = new HybridcastHost({ available: () => available, post: msg => messages.push(msg), fetchXMLAIT: async () => null });
const app = (id, code, priority) => ({ organizationId: 1, applicationId: id, controlCode: code, priority,
  transports: [{ transportProtocolLabel: 0, urls: [{ urlBase: 'https://example.test/app/', urlExtensions: [] }] }],
  transportProtocolLabels: [], location: 'index.html', boundaries: [] });
const ait = apps => ({ type: 'ait', section: { applicationType: 0x10, applications: apps, commonTransports: [] } });
host.emit(ait([app(1, 1, 10)]));
assert.equal(messages.length, 0, 'disabled engine does not launch');
host.configure(true, true);
assert.equal(messages.at(-1).applicationId, 1, 'AIT received before configuration is usable');
host.emit(ait([app(1, 2, 10)]));
assert.equal(messages.filter(x => x.type === 'launchHtml5').length, 1, 'PRESENT retains running app');
host.emit(ait([app(1, 4, 10)]));
assert.equal(messages.find(x => x.type === 'killHtml5').reason, 'ait');
host.stopped();
host.emit(ait([app(2, 1, 1), app(3, 1, 20)]));
assert.equal(messages.filter(x => x.type === 'launchHtml5').at(-1).applicationId, 3, 'highest priority wins');
available = false;
host.emit({ type: 'currentTime', timeUnixMillis: 1 });
assert.equal(messages.at(-1).type, 'killHtml5', 'recording/network/B62 gate terminates HTML5');
assert.ok(host.ait.getBroadcastAIT(), 'network gate retains broadcast signalling for re-enable');
available = true;
host.configure(true, false);
const count = messages.filter(x => x.type === 'launchHtml5').length;
host.emit(ait([app(4, 1, 10)]));
assert.equal(messages.filter(x => x.type === 'launchHtml5').length, count, 'autostart can be disabled');
console.log('Hybridcast ownership, priority, AIT control and availability tests passed');

function runtime(url, boundaries) {
  const d = new JSDOM('<!doctype html><html><body></body></html>', { url, runScripts: 'outside-only', pretendToBeVisual: true });
  const w = d.window, sent = [], calls = [], listeners = [];
  w.TextDecoder = TextDecoder;
  w.chrome = { webview: { postMessage: m => sent.push(m), addEventListener: (_, cb) => listeners.push(cb) } };
  w.XMLHttpRequest = class {
    open() {} setRequestHeader() {}
    send(body) { const request = JSON.parse(body); calls.push(request); this.status = 200; this.responseText = JSON.stringify({ result: null }); }
  };
  const info = { organizationId: 1, applicationId: 2, controlCode: 2, applicationType: 0x10, priority: 1, url, boundaries };
  w.__aribHtml5Config = { profile: 'hybridcast', app: info, applications: [info], aitSource: null,
    originalNetworkId: 1, transportStreamId: 2, serviceId: 3, broadcastObjectTypes: ['video/broadcast'],
    keyCodes: { VK_RED: 403 }, keySetValues: { RED: 1, GREEN: 2, YELLOW: 4, BLUE: 8, NAVIGATION: 16, VCR: 32, NUMERIC: 256, DBUTTON: 2048 },
    deviceIdentifiers: {}, defaultResolution: { width: 1920, height: 1080 }, rpcUrl: 'https://api.invalid/html5rpc', systemInformation: {}, debug: false };
  w.eval(fs.readFileSync(require.resolve('../browser/dist/aribhtml5_runtime.js'), 'utf8'));
  return { w, sent, calls, receive: data => listeners.forEach(cb => cb({ data })), close: () => w.close() };
}
const r = runtime('https://example.test/app/index.html', [{ managedURLs: ['https://example.test/app/'], permissionBitmaps: [0x1fff] }]);
try {
  const owner = r.w.navigator.applicationManager.getOwnerApplication();
  assert.equal(owner.application_id, 2);
  owner.replaceApplication(1, 3, 'https://example.test/next.xml');
  assert.equal(r.sent.at(-1).type, 'html5Replace');
  assert.equal(r.sent.at(-1).applicationId, 3);
  owner.keySet.setValue(1);
  let key = null;
  r.w.document.addEventListener('keydown', e => { key = e.keyCode; e.preventDefault(); });
  r.receive({ type: 'key', name: 'VK_RED', action: 'press' });
  assert.equal(key, 403, 'native remote key reaches app');
  owner.destroyApplication();
  assert.equal(r.sent.at(-1).type, 'html5Destroy');
} finally { r.close(); }
const outside = runtime('https://other.test/', [{ managedURLs: ['https://example.test/app/'], permissionBitmaps: [0x1fff] }]);
try {
  assert.equal(outside.w.navigator.applicationManager, undefined, 'out-of-boundary document has no receiver API');
  assert.throws(() => outside.w.__aribHtml5.receiverDevice.getDeviceIdentifier(5, () => {}), e => String(e).includes('Not authorized'));
} finally { outside.close(); }
console.log('HTML5 runtime ownership, boundaries, replacement and remote key tests passed');
