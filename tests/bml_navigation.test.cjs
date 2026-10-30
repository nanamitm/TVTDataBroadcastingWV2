const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const ts = require('../browser/node_modules/typescript');
const patch = require('../browser/loaders/bml-content-recovery.cjs');
const compiled = fs.readFileSync(path.join(__dirname, '../web-bml/dist/client/content.js'), 'utf8');
const { EventQueue } = require('../web-bml/dist/client/event_queue');
const log = { prefix: '', log() {}, error() {}, debug() {} };
const context = { exports: {}, URL, console, CustomEvent: class { constructor(type, init) { this.type = type; Object.assign(this, init); } },
    require(name) { return name === './resource' ? { Profile: { TrProfileC: 2 } } : {}; } };
vm.runInNewContext(patch(compiled), context);
const { Content, AribKeyCode } = context.exports;
const network = { exports: {}, URL, Uint8Array, console: { error(...args) { network.messages.push(args.join(' ')); } }, messages: [] };
vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(__dirname, '../browser/src/bml_resource.ts'), 'utf8'),
    { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS } }).outputText, network);

function fixture(result) {
    const c = Object.create(Content.prototype);
    const q = new EventQueue({}, log);
    let hidden = false, loads = 0, unloaded = 0, events = 0;
    Object.assign(c, {
        logger: log, eventQueue: q, loaded: true, keyProcessStatus: { keyCode: 1 },
        indicator: { setUrl(url, loading) { c.loading = loading; } },
        resources: { activeDocument: '/61/0100/optout.bml', profile: 1, isInternetContent: false,
            parseURL() { return {}; }, setBaseURIDirectory() {}, invalidateRemoteCache() {},
            async fetchResourceAsync() { return typeof result === 'function' ? result() : result; }, unlockModules() {} },
        eventDispatcher: { dispatchDataButtonPressedEvent() { q.queueAsyncEvent(async () => { events++; return false; }); q.processEventQueue(); } },
        bmlEventTarget: { dispatchEvent(e) { if (e.type === 'invisible') hidden = e.detail; } },
        documentElement: { childNodes: [] }, unloadAllDRCS() {},
        async unloadDocument() { unloaded++; },
        async loadDocument() { loads++; q.reset(); c.keyProcessStatus = undefined; },
        showErrorMessage(title, message, code) { c.error = code; },
    });
    return { c, q, state: () => ({ hidden, loads, unloaded, events }) };
}

(async () => {
    for (const status of [403, 404, 500, 502]) {
        let read = false;
        const result = await network.exports.getBMLResource('http://example.invalid/link?token=secret', async () => ({
            ok: false, status, async arrayBuffer() { read = true; throw Error('error body consumed'); } }));
        assert.equal(result.response, undefined);
        assert.equal(read, false);
    }
    assert(!network.messages.join('\n').includes('secret'), 'diagnostics omit query keys');
    const bytes = new Uint8Array([1, 2, 3]);
    const headers = new Map();
    const good = await network.exports.getBMLResource('http://example.invalid/link', async () => ({
        ok: true, status: 200, headers, async arrayBuffer() { return bytes.buffer; } }));
    assert.deepEqual([...good.response], [...bytes]);
    assert.equal(good.headers, headers);
    for (const mode of ['missing', 'fetch-rejection', 'malformed']) {
        const f = fixture(mode === 'fetch-rejection' ? () => { throw Error('offline'); } : mode === 'missing' ? null : {});
        if (mode === 'malformed') f.c.loadDocument = async () => { throw new TypeError("Cannot read properties of undefined (reading 'bml')"); };
        await f.c.launchDocumentAsync('http://example.invalid/link?tsid=0X7C14');
        assert.equal(f.c.error, mode === 'missing' ? 'E400' : 'E401');
        assert.equal(f.c.loaded, false);
        assert.equal(f.c.loading, false, 'navigation indicator is cleared');
        assert.equal(f.c.resources.activeDocument, null);
        assert.equal(f.c.keyProcessStatus, undefined);
        assert.equal(f.state().hidden, true);
        if (mode === 'missing') assert.equal(f.state().loads, 0, 'failed response never parsed');
        f.c.processKeyDown(AribKeyCode.DataButton);
        await f.q.processEventQueue();
        assert.equal(f.state().events, 1, 'd-button event accepted after failure');
    }
    const success = fixture({});
    await success.c.launchDocumentAsync('http://example.invalid/link');
    assert.equal(success.state().loads, 1);
    assert.equal(success.c.error, undefined);
    assert.equal(success.state().hidden, false);
    const badCleanup = fixture(null);
    badCleanup.c.unloadDocument = async () => { throw Error('onunload failed'); };
    await badCleanup.c.launchDocumentAsync('http://example.invalid/link');
    badCleanup.c.processKeyDown(AribKeyCode.DataButton);
    await badCleanup.q.processEventQueue();
    assert.equal(badCleanup.state().events, 1, 'cleanup exceptions cannot strand the queue');
    assert.throws(() => patch('changed upstream'), /no longer matches/);
    console.log('BML HTTP failure, navigation recovery and d-button regressions passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
