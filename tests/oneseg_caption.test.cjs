const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const ts = require('../browser/node_modules/typescript');

// 実際の受信ハンドラーを使い、dボタンを押す前のTS受信を確認する。
const source = fs.readFileSync(`${__dirname}/../browser/src/TVTDataBroadcastingWV2.ts`, 'utf8');
const ast = ts.createSourceFile('browser.ts', source, ts.ScriptTarget.Latest, true);
const handler = ast.statements.find(n => ts.isFunctionDeclaration(n) && n.name?.text === 'onWebViewMessage');
const compiled = ts.transpileModule(handler.getText(ast), { compilerOptions: { target: ts.ScriptTarget.ES2022 } }).outputText;
const streamHandler = ast.statements.find(n => ts.isFunctionDeclaration(n) && n.name?.text === 'onMessage');
const compiledStreamHandler = ts.transpileModule(streamHandler.getText(ast), { compilerOptions: { target: ts.ScriptTarget.ES2022 } }).outputText;
let pushes = 0;
let updates = 0;
const context = {
    cProfile: true, oneSegLaunched: false, oneSegCaptionOverlay: false,
    performance: { now: () => 0 },
    pcr: 0, broadcastTimeKnown: false, remoteControlStatusTimeout: Infinity,
    tsStream: { push() { pushes++; context.pcr++; } },
    player: { updateTime() { updates++; } },
    ttmlPlayer: { updateTime() {} }, b62Application: { updateTime() {} },
    fromBase64: () => new Uint8Array([0x47]),
    browserElement: { style: { visibility: 'hidden' } },
    pmtRetrieved: false, audioESList: [], audioStreamType: new Set(),
    networkId: 1, serviceId: 1,
    onResized() {}, postMessage() {},
    remoteControlStatusContainer: { style: {} },
    // BMLが親のvisibilityを上書きする文書でも非表示を維持する必要がある。
    bmlBrowser: { emitMessage() { context.bmlChildVisibility = 'visible'; } },
    hybridcast: { emit() {} },
};
context.b62Application.setCaptionComponents = () => {};
vm.createContext(context);
vm.runInContext(compiled, context);
vm.runInContext(compiledStreamHandler, context);
context.onMessage({ type: 'pmt', components: [{ dataComponentId: 0x000d }] });
assert.equal(context.bmlChildVisibility, 'visible');
assert.equal(context.browserElement.style.opacity, '0', 'BML descendants cannot override group opacity');
for (const type of ['stream', 'streamBase64']) {
    const send = () => context.onWebViewMessage({ type, data: type === 'stream' ? [0x47] : 'Rw==' }, () => {});
    context.oneSegLaunched = false;
    context.onWebViewMessage({ type: 'oneSegCaptionConfig', overlay: false });
    const before = pushes;
    send();
    assert.equal(pushes, before, `${type}: legacy mode waits for d button`);
    context.onWebViewMessage({ type: 'oneSegCaptionConfig', overlay: true });
    send();
    assert.equal(pushes, before + 1, `${type}: overlay receives captions before d button`);
    assert.equal(context.oneSegLaunched, false);
    assert.equal(context.browserElement.style.visibility, 'hidden');
    assert.equal(context.browserElement.style.opacity, '0', 'caption-only reception keeps BML transparent');
    context.onWebViewMessage({ type: 'oneSegCaptionConfig', overlay: false });
    send();
    assert.equal(pushes, before + 1, `${type}: disabling overlay restores gate`);
    context.onWebViewMessage({ type: 'launchOneSeg' });
    send();
    assert.equal(pushes, before + 2, `${type}: dedicated window continues receiving`);
    assert.equal(context.browserElement.style.visibility, 'visible');
    assert.equal(context.browserElement.style.opacity, '', 'd button restores BML in its dedicated window');
    context.browserElement.style.visibility = 'hidden';
    context.browserElement.style.opacity = '0';
}
assert.equal(updates, pushes);
context.cProfile = false;
context.oneSegLaunched = false;
context.onWebViewMessage({ type: 'streamBase64', data: 'Rw==' });
assert.equal(pushes, 5, 'ordinary services continue receiving without the one-seg window');
console.log('OneSeg caption stream routing tests passed');
