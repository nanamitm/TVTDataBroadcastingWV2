const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('../browser/node_modules/typescript');
const source = fs.readFileSync(path.join(__dirname, '../browser/src/b62_application.ts'), 'utf8');
const compiled = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS } });
const sent = [];
const context = {
    exports: {}, console, Date,
    window: { addEventListener() {} },
    document: { createElement() { return {
        style: {}, setAttribute() {}, remove() {},
        contentWindow: { postMessage(message) { sent.push(message); } },
    }; } },
};
vm.createContext(context);
vm.runInContext(compiled.outputText, context);
const container = { clientWidth: 960, clientHeight: 540, querySelector() { return null; }, insertBefore() {} };
const app = new context.exports.B62ApplicationFrame(container, () => {}, () => {}, () => {}, () => ({}), () => {});
const origin = 'https://tvtdatabroadcastingwv2-b62.invalid';
const message = {
    emtTag: 1, messageGroupId: 1, timeMode: 2, timeSeconds: 100,
    timeFraction: 0, messageType: 0, messageId: 258, privateData: 'old-app-event',
};
app.update(origin + '/old.html', 1);
app.pushEventMessage(message);
assert.equal(app.nptPending.length, 1, 'event waits for timing references');
app.update(origin + '/new.html', 2);
assert.equal(app.nptPending.length, 0, 'URL change discards old pending events');
app.setBroadcastTime(1000000);
app.setNptReference({ emtTag: 1, utcSeconds: 2208989800, utcFraction: 0, nptSeconds: 100, nptFraction: 0, leapIndicator: 0, scale: 3 });
assert.equal(sent.filter(m => m.type === 'eventMessage').length, 0, 'old event never reaches new frame');
app.pushEventMessage({ ...message, privateData: 'new-app-event' });
assert.equal(sent.filter(m => m.type === 'eventMessage').length, 1, 'new app still receives its event');
assert.equal(sent.find(m => m.type === 'eventMessage').message.private_data_byte, 'new-app-event');
app.setNptReference({ emtTag: 1, utcSeconds: 2208989800, utcFraction: 0, nptSeconds: 100, nptFraction: 0, leapIndicator: 0, scale: 1 });
app.pushEventMessage({ ...message, timeSeconds: 200 });
assert.equal(app.nptPending.length, 1, 'frozen NPT keeps future event pending');
app.update(origin + '/new.html', 3);
assert.equal(app.nptPending.length, 0, 'version change clears pending event');
assert.equal(app.nptReferences.size, 1, 'same-service reference survives app replacement');
app.pushEventMessage({ ...message, timeSeconds: 200 });
app.onFrameMessage({ origin, source: app.frame.contentWindow, data: { source: 'b62Application', type: 'destroy' } });
assert.equal(app.nptPending.length, 0, 'application destroy clears pending event');
assert.equal(app.frame, null, 'application frame destroyed');
console.log('B62 application lifecycle tests passed: 10 checks');
