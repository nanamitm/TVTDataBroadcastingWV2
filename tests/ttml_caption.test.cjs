// TTMLのまま表示する字幕 (browser/src/ttml_layout.ts、ttml_caption.ts) のテスト
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('../browser/node_modules/typescript');

// vmのコンテキストで作られた配列は、そのままでは比べられない
const same = (actual, expected, message) => assert.deepEqual(JSON.parse(JSON.stringify(actual)), expected, message);

let checks = 0;
function check(fn, name) {
    try {
        fn();
    } catch (e) {
        console.error(`FAIL: ${name}`);
        throw e;
    }
    checks++;
}

// キャンバスの代わり。描いた文字を覚える
const drawn = [];
const strokes = [];
const canvasContext = {
    save() {}, restore() {}, scale() {}, translate() {}, clearRect() { drawn.length = 0; strokes.length = 0; },
    fillRect() {}, fill() {},
    strokeText(text) { strokes.push({ text, color: this.strokeStyle, width: this.lineWidth }); },
    fillText(text) { drawn.push(text); },
    measureText() { return { width: 100 }; },
};
const modules = {};
function load(name, globals) {
    const source = fs.readFileSync(path.join(__dirname, `../browser/src/${name}.ts`), 'utf8');
    const compiled = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS } });
    const context = { exports: {}, require: (m) => modules[m.replace('./', '')], console, ...globals };
    vm.createContext(context);
    vm.runInContext(compiled.outputText, context);
    modules[name] = context.exports;
    return context.exports;
}
const layout = load('ttml_layout', {});
const caption = load('ttml_caption', {
    document: { createElement() { return { style: {}, width: 0, height: 0, getContext() { return canvasContext; } }; } },
    globalThis: { devicePixelRatio: 1 },
});

const white = [255, 255, 255, 255];
const region = { id: 'r1', style: { origin: [1000, 1800], extent: [1600, 240], backgroundColor: [0, 0, 0, 128] } };
function doc(spans) {
    return { paragraphs: [{ style: {}, region, spans }] };
}
function span(text, begin, end, style = {}) {
    return { begin, end, style: { fontSize: [144, 144], color: white, ...style }, content: [text] };
}

// 横書き: 文字は固定のセルに並び、letter-spacingの半分だけ寄せる
check(() => {
    const result = layout.layoutTtml(doc([span('あい', 0, 2, { letterSpacing: 16, lineHeight: 240 })]), 1);
    assert.equal(result.glyphs.length, 2);
    same(result.glyphs.map(g => [g.ch, g.x, g.y]), [['あ', 1008, 1848], ['い', 1168, 1848]]);
    same(result.backgrounds[0], { x: 1000, y: 1800, w: 1600, h: 240, color: [0, 0, 0, 128] });
}, 'horizontal layout');
check(() => {
    assert.equal(layout.layoutTtml(doc([span('あ', 0, 2)]), 2).glyphs.length, 0);
    assert.equal(layout.layoutTtml(doc([span('あ', 0, 2)]), -0.1).glyphs.length, 0);
}, 'end is exclusive and begin inclusive');
check(() => {
    const vertical = { id: 'r2', style: { origin: [3000, 200], extent: [240, 1600], writingMode: 'tbrl' } };
    const result = layout.layoutTtml({ paragraphs: [{ style: {}, region: vertical, spans: [span('ー12', 0, 1, { fontSize: [144, 144] })] }] }, 0);
    assert.equal(result.glyphs[0].ch, '︱', 'the long vowel mark turns');
    assert.ok(result.glyphs.every(g => g.x >= 3000 && g.x < 3240), 'the column is at the right edge of the region');
    const tcy = layout.layoutTtml({ paragraphs: [{ style: {}, region: vertical, spans: [span('12', 0, 1, { fontSize: [72, 144] })] }] }, 0);
    assert.equal(tcy.glyphs[0].y, tcy.glyphs[1].y, 'two half width digits share one cell');
}, 'vertical layout');
check(() => {
    same(layout.ttmlPlaneSize(0), [1920, 1080]);
    same(layout.ttmlPlaneSize(1), [3840, 2160]);
    same(layout.ttmlPlaneSize(2), [7680, 4320]);
    assert.equal(layout.parseCodepointAttribute('&#xE001;'), 0xe001);
    assert.equal(layout.parseCodepointAttribute('U+E0??'), 0xe000);
    assert.equal(layout.parseCodepointAttribute('&#bad'), undefined);
}, 'plane size and code points');

// 時刻合わせ。ptsは文書の時刻0のPTS (90kHz)、時刻はPCRのミリ秒
const message = (document, pts, extra = {}) => ({ componentTag: 0x30, tmd: 0b0010, pts, delay: 0, resolution: 1, document, ...extra });
check(() => {
    const track = new caption.TtmlCaptionTrack();
    track.update(10000);
    track.push(message(doc([span('一', 1, 3)]), 10000 * 90), 10000);
    assert.equal(track.update(10500), false, 'nothing before the first begin');
    assert.equal(track.state(), null);
    assert.equal(track.update(11000), true, 'shown at begin');
    assert.equal(track.state().at, 1);
    assert.equal(track.update(12000), false, 'unchanged while shown');
    assert.equal(track.update(13000), true, 'cleared at end');
    assert.equal(layout.layoutTtml(track.state().document, track.state().at).glyphs.length, 0);
}, 'a document follows begin and end');
check(() => {
    const track = new caption.TtmlCaptionTrack();
    track.update(10000);
    track.push(message(doc([span('一', 0, 10)]), 10000 * 90), 10000);
    track.update(10000);
    track.push(message(doc([span('二', 5, 10)]), 10000 * 90), 10100);
    track.update(12000);
    assert.equal(track.state().document.paragraphs[0].spans[0].content[0], '一', 'the old document stays until the new one begins');
    track.update(15000);
    assert.equal(track.state().document.paragraphs[0].spans[0].content[0], '二', 'replaced when the new one begins');
}, 'documents replace each other');
check(() => {
    const track = new caption.TtmlCaptionTrack();
    track.update(50000);
    track.push(message(doc([span('遅', 1, 4)]), 10000 * 90), 50000);
    assert.equal(track.update(50000), true, 'a late document is shown now');
    assert.equal(track.state().at, 1);
    track.update(52900);
    assert.equal(layout.layoutTtml(track.state().document, track.state().at).glyphs.length, 1, 'and keeps its duration');
}, 'late documents are shifted');
check(() => {
    const track = new caption.TtmlCaptionTrack();
    track.update(10000);
    track.push(message(doc([span('遅延', 0, 2)]), 10000 * 90, { delay: 450 }), 10000);
    assert.equal(track.update(10400), false);
    assert.equal(track.update(10450), true, 'the plugin delay is added');
}, 'display delay');
check(() => {
    const track = new caption.TtmlCaptionTrack();
    track.update(10000);
    track.push(message(doc([span('即', undefined, undefined)]), null, { tmd: 0b1111, componentTag: 0x38 }), 10000);
    assert.equal(track.update(10000), true, 'no time control shows on arrival');
    assert.equal(track.state().at, 0);
}, 'no time control');
check(() => {
    const track = new caption.TtmlCaptionTrack();
    const period = 2 ** 33 / 90;
    track.update(period - 1000);
    track.push(message(doc([span('一周', 1, 3)]), 0), period - 1000);
    track.update(period - 10);
    assert.equal(track.state(), null);
    track.update(1000);
    assert.equal(track.state().document.paragraphs[0].spans[0].content[0], '一周', 'shown across the clock wrap');
}, 'clock wrap');
check(() => {
    const track = new caption.TtmlCaptionTrack();
    track.update(10000);
    track.push(message(doc([span('一', 0, 100)]), 10000 * 90), 10000);
    track.update(11000);
    assert.notEqual(track.state(), null);
    track.update(500000);
    assert.equal(track.state(), null, 'a jump of the clock clears the caption');
}, 'seek');

// 表示の切り替え: TTMLを描き始めたトラックはB24の字幕を止める
check(() => {
    const container = { append() {}, getBoundingClientRect() { return { width: 960, height: 540 }; } };
    const player = new caption.TtmlCaptionPlayer(container);
    player.setCaptionEnabled(true);
    const activated = [];
    player.onActivated = (track) => activated.push(track);
    player.push(message(doc([span('無効', 0, 1)]), 0));
    assert.equal(player.isActive(0), false, 'nothing while disabled');
    player.setConfig({ enable: true, faces: ['MS Gothic'], strokeWidth: 30, noBackground: false, showCaption: true, showSuperimpose: true });
    player.updateTime(1000);
    player.push(message(doc([span('字幕', 0, 10)]), 1000 * 90, { componentTag: 0x31 }));
    same(activated, [0]);
    assert.equal(player.isActive(0), true);
    assert.equal(player.isActive(1), false, 'the superimposition keeps B24 until TTML arrives');
    player.updateTime(1100);
    same(drawn, ['字', '幕']);
    player.push(message(doc([span('英語', 0, 10)]), 1100 * 90, { componentTag: 0x32 }));
    player.updateTime(1200);
    same(drawn, ['字', '幕'], 'a second language is not mixed in');
    const deactivated = [];
    player.onDeactivated = (track) => deactivated.push(track);
    player.setConfig({ enable: false, faces: [], strokeWidth: 30, noBackground: false, showCaption: true, showSuperimpose: true });
    assert.equal(player.isActive(0), false);
    same(drawn, [], 'disabling clears the canvas');
    same(deactivated, [0], 'disabling hands the caption back to B24');
    player.setConfig({ enable: true, faces: [], strokeWidth: 30, noBackground: false, showCaption: true, showSuperimpose: true });
    player.push(message(doc([span('再開', 0, 10)]), 1200 * 90));
    same(activated, [0, 0], 'enabling again takes the caption over when TTML arrives');
}, 'player');

// 通常字幕のボタンと両トラックの設定の全組み合わせ。表示中の設定変更も即座に反映する。
check(() => {
    const container = { append() {}, getBoundingClientRect() { return { width: 960, height: 540 }; } };
    const player = new caption.TtmlCaptionPlayer(container);
    const config = { enable: true, faces: [], strokeWidth: 30, noBackground: false, showCaption: true, showSuperimpose: true };
    player.setConfig(config);
    player.updateTime(1000);
    player.push(message(doc([span('字', 0, 10)]), 1000 * 90));
    player.push(message(doc([span('ス', 0, 10)]), 1000 * 90, { componentTag: 0x38 }));
    same(drawn, ['ス'], 'superimposition is visible with the caption button initially off');
    for (const enabled of [true, false, true]) {
        for (const showCaption of [true, false]) {
            for (const showSuperimpose of [true, false]) {
                player.setConfig({ ...config, showCaption, showSuperimpose });
                player.setCaptionEnabled(enabled);
                same(drawn, [...(enabled && showCaption ? ['字'] : []), ...(showSuperimpose ? ['ス'] : [])]);
            }
        }
    }
}, 'caption button and settings independently control TTML tracks');

check(() => {
    const container = { append() {}, getBoundingClientRect() { return { width: 960, height: 540 }; } };
    const player = new caption.TtmlCaptionPlayer(container);
    const config = { enable: true, faces: [], strokeWidth: 30, noBackground: false, showCaption: true, showSuperimpose: true };
    player.setConfig(config);
    player.updateTime(1000);
    const push = (style) => player.push(message(doc([span('震度3', 0, 10, style)]), 1000 * 90, { componentTag: 0x38 }));
    const blackOnGreen = { color: [0, 0, 0, 255], backgroundColor: [0, 255, 0, 255] };
    push(blackOnGreen);
    same(drawn, ['震', '度', '3']);
    same(strokes, ['震', '度', '3'].map(text => ({ text, color: 'rgba(0,255,0,1)', width: 24 })), 'forced outline uses green instead of thickening the black letters');
    push({ ...blackOnGreen, backgroundColor: [0, 0, 0, 128] });
    assert.ok(strokes.every(s => s.color === 'rgba(0,0,0,0.5019607843137255)'), 'background alpha is preserved');
    push({ ...blackOnGreen, backgroundColor: [0, 0, 0, 0] });
    assert.ok(strokes.every(s => s.color === 'rgba(0,0,0,0)'), 'a transparent background does not add an opaque outline');
    push({ ...blackOnGreen, textOutline: { color: white, width: 2, blur: 0 } });
    assert.ok(strokes.every(s => s.color === 'rgba(255,255,255,1)' && s.width === 4), 'broadcast outline color and width take priority');
    push({ ...blackOnGreen, textOutline: { width: 2, blur: 0 } });
    assert.ok(strokes.every(s => s.color === 'rgba(0,0,0,1)' && s.width === 4), 'a broadcast outline without a color uses the text color');
    push({ ...blackOnGreen, textOutline: { width: 0, blur: 0 } });
    same(strokes, [], 'an explicit zero width is respected');
    player.setConfig({ ...config, strokeWidth: 0 });
    push(blackOnGreen);
    same(strokes, [], 'forced outline can be disabled');
    push({ ...blackOnGreen, textOutline: { color: white, width: 2, blur: 0 } });
    assert.equal(strokes.length, 3, 'broadcast outlines remain when forced outline is disabled');
}, 'TTML forced outlines match background colors and preserve broadcast outlines');

check(() => {
    const canvases = [];
    load('video_player', {});
    modules['web-bml'] = { playRomSound() {} };
    const { CaptionPlayer } = load('caption_player', {
        document: { createElement() {
            const canvas = { style: {}, width: 0, height: 0, getContext() { return canvasContext; } };
            canvases.push(canvas);
            return canvas;
        } },
    });
    const container = { style: {}, append() {} };
    const player = new CaptionPlayer({}, container, () => {});
    const visible = () => canvases.map(c => c.style.display !== 'none');
    same(visible(), [false, true], 'superimposition is initially visible');
    player.showCC();
    same(visible(), [true, true]);
    player.hideCC();
    same(visible(), [false, true], 'turning captions off leaves superimposition visible');
    assert.notEqual(container.style.display, 'none', 'the shared TTML container stays visible');
    player.suppress(0);
    player.unsuppress(0);
    same(visible(), [false, true], 'fallback from TTML respects the caption button');
    player.suppress(0);
    player.showCC();
    same(visible(), [false, true], 'turning captions on respects TTML suppression');
    player.unsuppress(0);
    same(visible(), [true, true]);
    player.suppress(1);
    player.hideCC();
    player.unsuppress(1);
    same(visible(), [false, true], 'superimposition fallback works with captions off');
}, 'B24 caption button and TTML fallback preserve independent track visibility');

// B24の字幕のPES: data_identifier、private_stream_id、ヘッダー長、データグループ
const b24Pes = (dataGroupId, headerLength = 0) => new Uint8Array([0x80, 0xff, 0xf0 | headerLength, ...new Array(headerLength).fill(0), dataGroupId << 2, 0, 0]);
check(() => {
    assert.equal(caption.isB24CaptionStatement(b24Pes(0x00)), false, 'caption management (group A)');
    assert.equal(caption.isB24CaptionStatement(b24Pes(0x20)), false, 'caption management (group B)');
    assert.equal(caption.isB24CaptionStatement(b24Pes(0x01)), true, 'statement (group A)');
    assert.equal(caption.isB24CaptionStatement(b24Pes(0x21, 3)), true, 'statement after the PES data header');
    assert.equal(caption.isB24CaptionStatement(new Uint8Array([0x80, 0xff])), false, 'too short');
}, 'B24 statements');

// TTMLが届かなくなったらB24の字幕に戻す (古いdantto4kで変換した録画を続けて開いたときなど)
check(() => {
    const container = { append() {}, getBoundingClientRect() { return { width: 960, height: 540 }; } };
    const player = new caption.TtmlCaptionPlayer(container);
    const events = [];
    player.onActivated = (track) => events.push(['on', track]);
    player.onDeactivated = (track) => events.push(['off', track]);
    player.setConfig({ enable: true, faces: [], strokeWidth: 30, noBackground: false, showCaption: true, showSuperimpose: true });
    player.updateTime(1000);
    player.push(message(doc([span('一', 0, 1)]), 1000 * 90));
    // 同じ字幕のB24は同時に届く
    player.noteB24Statement(0);
    player.updateTime(10000);
    same(events, [['on', 0]], 'B24 with its TTML keeps TTML');
    // 字幕の無い間に届くのは字幕管理データだけ。TTMLより先にB24が届くこともある
    player.updateTime(60000);
    player.noteB24Statement(0);
    player.updateTime(60500);
    player.push(message(doc([span('二', 0, 1)]), 60500 * 90));
    player.updateTime(70000);
    same(events, [['on', 0]], 'TTML arriving shortly after B24 keeps TTML');
    // B24だけが届き続ける
    player.updateTime(100000);
    player.noteB24Statement(0);
    player.updateTime(102000);
    same(events, [['on', 0]], 'waits for TTML');
    player.updateTime(103500);
    same(events, [['on', 0], ['off', 0]], 'falls back to B24');
    assert.equal(player.isActive(0), false);
    player.push(message(doc([span('三', 0, 1)]), 104000 * 90));
    same(events, [['on', 0], ['off', 0], ['on', 0]], 'TTML takes over again when it comes back');
}, 'fallback to B24');

console.log(`TTML caption tests passed: ${checks} checks`);
