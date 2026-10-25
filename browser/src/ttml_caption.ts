import { layoutTtml, parseCodepointAttribute, tmdNoTimeControl, ttmlBoundaries, ttmlColor, ttmlFirstBegin, ttmlPlaneSize, ttmlUsesAnyCodepoint, type TtmlDocument, type TtmlGlyph, type TtmlPair } from "./ttml_layout";

// 4K/8K放送の字幕をB24に変換したものではなく、TTMLのまま描く。
// dantto4kがTSに載せた字幕 (table_id 0xC1) をプラグインが解析して送ってくる
// (TVTDataBroadcastingWV2.cppのSendTtmlCaption)。

// ネイティブから届く字幕の文書
export type TtmlCaptionMessage = {
    componentTag: number,
    tmd: number,
    // 文書の時刻0を提示するPTS (90kHz)。nullなら届いた時点で表示する
    pts: number | null,
    // 字幕プラグインの表示遅延 (ミリ秒)
    delay: number,
    resolution: number,
    document: TtmlDocument,
};

// 字幕プラグインの設定のうち、TTMLの字幕に当てはまるもの
export type TtmlCaptionConfig = {
    enable: boolean,
    // 使うフォント。前にあるものほど優先する
    faces: string[],
    // 縁取りの幅の10倍 (960x540の字幕プレーンのドット)。0より大きいとき、TTMLが縁取りを
    // 指定していない文字も縁取る
    strokeWidth: number,
    noBackground: boolean,
    showCaption: boolean,
    showSuperimpose: boolean,
};

// PTSとPCRは33ビットで一周する (ミリ秒)
const clockPeriodMillis = 2 ** 33 / 90;
// これより先の字幕は時刻がおかしいとみなして今に合わせる
const maxLeadMillis = 60 * 1000;
// 時刻がこれ以上飛んだらシークなどとみなして字幕を捨てる
const maxJumpMillis = 10 * 1000;
// B24の字幕文が届いてからこれだけ経ってもTTMLが届かなければ、TTMLの無いストリーム
// (古いdantto4kで変換した録画など) に変わったとみなしてB24の字幕に戻す。
// dantto4kは同じ字幕のTTMLとB24を同時に送るので、普段は数ミリ秒しか離れない
const fallbackMillis = 3 * 1000;

function wrapMillis(diff: number): number {
    diff %= clockPeriodMillis;
    if (diff > clockPeriodMillis / 2) {
        diff -= clockPeriodMillis;
    } else if (diff <= -clockPeriodMillis / 2) {
        diff += clockPeriodMillis;
    }
    return diff;
}

// B24の字幕のPES (PES_data_byte) が字幕文か。字幕管理データ (data_group_idが0x00/0x20)
// は字幕が無くても送られてくるので数えない (ARIB STD-B24 第三編 表9-1、表9-3)
export function isB24CaptionStatement(pes: Uint8Array): boolean {
    if (pes.length < 3) {
        return false;
    }
    // data_identifier、private_stream_id、PES_data_packet_header_lengthの後にデータグループ
    const index = 3 + (pes[2] & 0x0f);
    return index < pes.length && ((pes[index] >> 2) & 0x0f) !== 0;
}

type Entry = {
    id: number,
    document: TtmlDocument,
    plane: TtmlPair,
    // 文書の時刻0に当たる時刻 (PCR、ミリ秒)
    base: number,
    // 表示を始める時刻 (PCR、ミリ秒)。nullなら次に時刻が分かった時点
    start: number | null,
    // 時刻制御なし: 届いた文書全体をそのまま表示する
    immediate: boolean,
};

export type TtmlCaptionState = {
    document: TtmlDocument,
    plane: TtmlPair,
    // 文書の時刻 (秒)
    at: number,
};

// 字幕か文字スーパーの1つ分。新しい文書は、最初の字幕を表示する時刻に前の文書と
// 入れ替わる。文書の中ではbegin/endの時刻ごとに表示が変わる
export class TtmlCaptionTrack {
    private pending: Entry[] = [];
    private current: Entry | null = null;
    private nextId = 0;
    private shownKey = "";
    private lastNow: number | null = null;

    push(caption: TtmlCaptionMessage, now: number | null) {
        const document = caption.document;
        const plane = ttmlPlaneSize(caption.resolution);
        const id = this.nextId++;
        if (caption.tmd === tmdNoTimeControl || caption.pts == null || now == null) {
            // 時刻制御なし (文字スーパーなど)。届いた時点 (に遅延を足した時刻) で表示する
            const start = now == null ? null : now + caption.delay;
            this.pending.push({ id, document, plane, base: start ?? 0, start, immediate: true });
            return;
        }
        const first = (ttmlFirstBegin(document) ?? 0) * 1000;
        let base = caption.pts / 90 + caption.delay;
        const lead = wrapMillis(base + first - now);
        if (lead < 0 || lead > maxLeadMillis) {
            // 遅れて届いた文書は、最初の字幕が今になるよう全体をずらす (表示時間を保つ)。
            // dantto4kのB24変換と同じ扱い
            base = now - first;
        }
        this.pending.push({ id, document, plane, base, start: base + first, immediate: false });
    }

    reset() {
        this.pending = [];
        this.current = null;
        this.lastNow = null;
    }

    // 時刻を進める。表示が変わったらtrue
    update(now: number): boolean {
        if (this.lastNow != null && Math.abs(wrapMillis(now - this.lastNow)) > maxJumpMillis) {
            this.reset();
        }
        this.lastNow = now;
        for (let i = this.pending.length - 1; i >= 0; i--) {
            const entry = this.pending[i];
            if (entry.start == null || wrapMillis(entry.start - now) <= 0) {
                if (entry.start == null) {
                    entry.start = now;
                    entry.base = now;
                }
                // それより前に届いた文書は、表示されないまま入れ替わる
                this.current = entry;
                this.pending.splice(0, i + 1);
                break;
            }
        }
        const key = this.stateKey(now);
        if (key === this.shownKey) {
            return false;
        }
        this.shownKey = key;
        return true;
    }

    private stateKey(now: number): string {
        const entry = this.current;
        if (entry == null) {
            return "";
        }
        if (entry.immediate) {
            return String(entry.id);
        }
        const at = this.documentTime(entry, now);
        const index = ttmlBoundaries(entry.document).filter(t => t <= at).length;
        return `${entry.id}:${index}`;
    }

    private documentTime(entry: Entry, now: number): number {
        return entry.immediate ? 0 : wrapMillis(now - entry.base) / 1000;
    }

    // 今表示するもの
    state(): TtmlCaptionState | null {
        const entry = this.current;
        if (entry == null || this.lastNow == null) {
            return null;
        }
        return { document: entry.document, plane: entry.plane, at: this.documentTime(entry, this.lastNow) };
    }
}

// 外字 (DRCS) のグリフ。字幕リソースのSVGフォント由来で、unitsPerEmの座標系
// (y軸が上向き、ベースラインが0) のパスを持つ
type DrcsGlyph = { path: Path2D, unitsPerEm: number };

// SVGフォント (<font><glyph unicode d>...) のグリフを登録する。TTMLの本文にある
// 私用領域の文字をこれで描く。初めて登録したコードポイントを返す
function registerDrcsGlyphs(svg: string, glyphs: Map<number, DrcsGlyph>): number[] {
    const doc = new DOMParser().parseFromString(svg, "image/svg+xml");
    if (doc.getElementsByTagName("parsererror").length > 0) {
        return [];
    }
    const added: number[] = [];
    // <font>ごとにfont-face (units-per-em、unicode-range) が付く。<font>が無ければ
    // 文書全体を1つのフォントとして扱う
    const fonts = Array.from(doc.getElementsByTagName("font"));
    const scopes: Element[] = fonts.length > 0 ? fonts : [doc.documentElement];
    for (const font of scopes) {
        const face = font.getElementsByTagName("font-face")[0];
        const unitsPerEm = parseFloat(face?.getAttribute("units-per-em") || "") || 1000;
        // 放送のリソースは1フォント1グリフで、コードポイントをfont-faceのunicode-rangeに
        // しか書かないことが多い。glyphに無ければこれを使う
        const faceCodepoint = parseCodepointAttribute(face?.getAttribute("unicode-range"));
        for (const glyph of Array.from(font.getElementsByTagName("glyph"))) {
            const d = glyph.getAttribute("d");
            if (!d) {
                continue;
            }
            const cp = parseCodepointAttribute(glyph.getAttribute("unicode")) ?? parseCodepointAttribute(glyph.getAttribute("unicode-range")) ?? faceCodepoint;
            if (cp === undefined) {
                continue;
            }
            try {
                if (!glyphs.has(cp)) {
                    added.push(cp);
                }
                glyphs.set(cp, { path: new Path2D(d), unitsPerEm });
            } catch {
                // 無効なパスは使わない
            }
        }
    }
    return added;
}

const genericFamilies = new Set(["serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui"]);

function quoteFamily(family: string): string {
    return genericFamilies.has(family) ? family : `"${family.replace(/["\\]/g, "")}"`;
}

export class TtmlCaptionPlayer {
    private readonly canvas = document.createElement("canvas");
    private readonly container: HTMLElement;
    private readonly tracks = [new TtmlCaptionTrack(), new TtmlCaptionTrack()];
    // 表示しているcomponent_tag。複数の言語があれば番号の小さいほうを使う
    private readonly componentTags: (number | null)[] = [null, null];
    private readonly glyphs = new Map<number, DrcsGlyph>();
    private config: TtmlCaptionConfig = { enable: false, faces: [], strokeWidth: 30, noBackground: false, showCaption: true, showSuperimpose: true };
    private now: number | null = null;
    private captionEnabled = false;
    // TTMLの文書が最後に届いた時刻 (PCR、ミリ秒)
    private readonly lastArrivals: (number | null)[] = [null, null];
    // TTMLを表示している間にB24の字幕文だけが届いた時刻
    private readonly b24OnlySince: (number | null)[] = [null, null];
    // 字幕 (0) か文字スーパー (1) をTTMLで表示し始めた
    public onActivated?: (track: number) => void;
    // TTMLで表示するのをやめた (設定で無効にした、TTMLが届かなくなった)。B24の字幕に戻す
    public onDeactivated?: (track: number) => void;

    constructor(container: HTMLElement) {
        this.container = container;
        const style = this.canvas.style;
        style.position = "absolute";
        style.left = "0";
        style.top = "0";
        style.width = "100%";
        style.height = "100%";
        style.pointerEvents = "none";
        container.append(this.canvas);
    }

    setConfig(config: TtmlCaptionConfig) {
        this.config = config;
        if (!config.enable) {
            this.deactivate(0);
            this.deactivate(1);
        }
        this.render();
    }

    // 通常字幕の表示ボタン。文字スーパーの表示設定には影響しない。
    setCaptionEnabled(enabled: boolean) {
        this.captionEnabled = enabled;
        this.render();
    }

    private deactivate(index: number) {
        this.tracks[index].reset();
        this.lastArrivals[index] = null;
        this.b24OnlySince[index] = null;
        if (this.componentTags[index] != null) {
            this.componentTags[index] = null;
            this.onDeactivated?.(index);
        }
    }

    // TTMLを表示しているトラックにB24の字幕文 (isB24CaptionStatement) が届いた。
    // 少し待ってもTTMLが届かなければB24の字幕に戻す
    noteB24Statement(index: number) {
        if (!this.isActive(index) || this.now == null || this.b24OnlySince[index] != null) {
            return;
        }
        const last = this.lastArrivals[index];
        if (last != null && Math.abs(wrapMillis(this.now - last)) <= fallbackMillis) {
            return;
        }
        this.b24OnlySince[index] = this.now;
    }

    // B24の字幕の代わりにTTMLを表示しているか
    isActive(track: number): boolean {
        return this.config.enable && this.componentTags[track] != null;
    }

    push(caption: TtmlCaptionMessage) {
        if (!this.config.enable) {
            return;
        }
        const index = caption.componentTag >= 0x38 ? 1 : 0;
        const selected = this.componentTags[index];
        if (selected != null && caption.componentTag > selected) {
            return;
        }
        if (selected !== caption.componentTag) {
            this.tracks[index].reset();
            this.componentTags[index] = caption.componentTag;
            if (selected == null) {
                this.onActivated?.(index);
            }
        }
        this.lastArrivals[index] = this.now;
        this.b24OnlySince[index] = null;
        this.tracks[index].push(caption, this.now);
        if (this.now != null && this.tracks[index].update(this.now)) {
            this.render();
        }
    }

    pushFont(svg: string) {
        const added = registerDrcsGlyphs(svg, this.glyphs);
        // 本文がリソースより先に届くと外字はグリフ無しで描かれている。表示中の字幕が
        // いま届いたグリフを使っていれば描き直す (同じ文書は送り直されない)
        if (this.tracks.some(track => {
            const state = track.state();
            return state != null && ttmlUsesAnyCodepoint(state.document, added);
        })) {
            this.render();
        }
    }

    // 映像の時刻 (PCR、ミリ秒)
    updateTime(pcrMillis: number) {
        this.now = pcrMillis;
        let changed = false;
        this.tracks.forEach((track, index) => {
            const since = this.b24OnlySince[index];
            if (since != null && Math.abs(wrapMillis(pcrMillis - since)) > fallbackMillis) {
                this.deactivate(index);
                changed = true;
            }
            changed = track.update(pcrMillis) || changed;
        });
        if (changed) {
            this.render();
        }
    }

    // 大きさが変わったときや表示し直すとき
    render() {
        const rect = this.container.getBoundingClientRect();
        const pixelRatio = globalThis.devicePixelRatio || 1;
        const width = Math.max(1, Math.round(rect.width * pixelRatio));
        const height = Math.max(1, Math.round(rect.height * pixelRatio));
        if (rect.width <= 0 || rect.height <= 0) {
            // 字幕を非表示にしている間は描かない (表示したときに描き直す)
            return;
        }
        if (this.canvas.width !== width || this.canvas.height !== height) {
            this.canvas.width = width;
            this.canvas.height = height;
        }
        const context = this.canvas.getContext("2d");
        if (context == null) {
            return;
        }
        context.clearRect(0, 0, width, height);
        if (!this.config.enable) {
            return;
        }
        const visible = [this.captionEnabled && this.config.showCaption, this.config.showSuperimpose];
        this.tracks.forEach((track, index) => {
            const state = track.state();
            if (visible[index] && state != null) {
                this.draw(context, state);
            }
        });
    }

    private draw(context: CanvasRenderingContext2D, state: TtmlCaptionState) {
        const layout = layoutTtml(state.document, state.at);
        if (layout.glyphs.length === 0 && layout.backgrounds.length === 0) {
            return;
        }
        context.save();
        // TTMLの座標系 (4Kなら3840x2160) のまま描く
        context.scale(this.canvas.width / state.plane[0], this.canvas.height / state.plane[1]);
        if (!this.config.noBackground) {
            for (const r of layout.backgrounds) {
                context.fillStyle = ttmlColor(r.color)!;
                context.fillRect(r.x, r.y, r.w, r.h);
            }
        }
        // 縁取りを先に全部描き、本体が隣の文字の縁取りに隠れないようにする
        const forcedStroke = this.config.strokeWidth / 10 * state.plane[0] / 960;
        for (const glyph of layout.glyphs) {
            this.drawGlyph(context, glyph, forcedStroke, true);
        }
        for (const glyph of layout.glyphs) {
            this.drawGlyph(context, glyph, forcedStroke, false);
            drawDecorations(context, glyph);
        }
        context.restore();
    }

    private font(glyph: TtmlGlyph): string {
        const families = this.config.faces.map(quoteFamily);
        if (glyph.style.fontFamily) {
            families.push(...glyph.style.fontFamily.split(",").map(f => f.trim()).filter(f => f.length > 0).map(quoteFamily));
        }
        families.push("sans-serif");
        const italic = glyph.style.fontStyle === "italic" ? "italic " : "";
        const bold = glyph.style.fontWeight === "bold" ? "bold " : "";
        return `${italic}${bold}${glyph.h}px ${families.join(", ")}`;
    }

    // 字形を (x, y) のw×hの枠に描く。strokeなら縁取りだけを描く
    private drawGlyph(context: CanvasRenderingContext2D, glyph: TtmlGlyph, forcedStroke: number, stroke: boolean) {
        const drcs = this.glyphs.get(glyph.ch.codePointAt(0)!);
        const sx = glyph.w / glyph.h;
        if (drcs) {
            if (stroke) {
                return;
            }
            // SVGフォントはy軸が上向きでベースラインが0。枠の上端に合わせて反転する
            const s = glyph.h / drcs.unitsPerEm;
            context.save();
            context.translate(glyph.x, glyph.y);
            context.scale(s * sx, -s);
            context.translate(0, -drcs.unitsPerEm);
            context.fillStyle = ttmlColor(glyph.style.color) ?? "white";
            context.fill(drcs.path);
            context.restore();
            return;
        }
        context.save();
        context.font = this.font(glyph);
        context.textBaseline = "top";
        // 全角の字形は枠いっぱい、それより狭い字形 (英数字など) は枠の中央に置く
        const width = context.measureText(glyph.ch).width * sx;
        context.translate(glyph.x + Math.max(0, (glyph.w - width) / 2), glyph.y);
        context.scale(sx, 1);
        if (stroke) {
            const outline = glyph.style.textOutline;
            // textOutlineの太さは字形の外側の幅。strokeは線の半分が外に出る
            const outlineWidth = outline != null ? outline.width : forcedStroke;
            if (outlineWidth > 0) {
                context.lineWidth = outlineWidth * 2;
                // 強制縁取りはB24表示と同じく文字の背景色を使う。
                // 黒文字に黒い縁取りを足すと、震度表示などの字形が潰れてしまう。
                context.strokeStyle = outline == null
                    ? ttmlColor(glyph.style.backgroundColor) ?? "black"
                    : ttmlColor(outline.color) ?? ttmlColor(glyph.style.color) ?? "white";
                context.lineJoin = "round";
                context.strokeText(glyph.ch, 0, 0);
            }
        } else {
            context.fillStyle = ttmlColor(glyph.style.color) ?? "white";
            context.fillText(glyph.ch, 0, 0);
        }
        context.restore();
    }
}

// 下線と囲み (arib-tt:border)。文字の色で描く
function drawDecorations(context: CanvasRenderingContext2D, glyph: TtmlGlyph) {
    const style = glyph.style;
    if (!style.underline && !style.borderTop && !style.borderRight && !style.borderBottom && !style.borderLeft) {
        return;
    }
    const lineWidth = Math.max(1, glyph.h / 24);
    context.fillStyle = ttmlColor(style.color) ?? "white";
    if (style.underline || style.borderBottom) {
        context.fillRect(glyph.x, glyph.y + glyph.h - lineWidth, glyph.w, lineWidth);
    }
    if (style.borderTop) {
        context.fillRect(glyph.x, glyph.y, glyph.w, lineWidth);
    }
    if (style.borderLeft) {
        context.fillRect(glyph.x, glyph.y, lineWidth, glyph.h);
    }
    if (style.borderRight) {
        context.fillRect(glyph.x + glyph.w - lineWidth, glyph.y, lineWidth, glyph.h);
    }
}
