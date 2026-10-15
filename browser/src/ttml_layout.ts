// 4K/8K放送の字幕 (ARIB-TTML、ARIB STD-B62 / TR-B39) の配置を計算する。
// TTMLの解析 (スタイルの継承、入れ子のspan、タイミングの合成) はプラグインが
// dantto4kのparser/resolverで済ませて送ってくる (TtmlCaption.cpp)。ここでは
// その文書のどの文字をどこに描くかを決める。描画 (Canvas) から切り離して
// あるので単体テストできる (tests/ttml_caption.test.cjs)。
// ts-live (src/lib/ttml.ts) と同じ計算をしている。

export type TtmlColor = [number, number, number, number];
export type TtmlPair = [number, number];

// 解決済みのスタイル。値はTTMLの座標系 (px) のまま
export type TtmlStyle = {
    backgroundColor?: TtmlColor,
    color?: TtmlColor,
    origin?: TtmlPair,
    extent?: TtmlPair,
    // [横, 縦]。横が縦より小さいのは半角 (中型)
    fontSize?: TtmlPair,
    fontFamily?: string,
    fontStyle?: "normal" | "italic",
    fontWeight?: "normal" | "bold",
    // nullはnormal
    lineHeight?: number | null,
    underline?: boolean,
    textOutline?: { color?: TtmlColor, width: number, blur: number },
    writingMode?: "lrtb" | "tbrl",
    letterSpacing?: number,
    ruby?: string,
    borderTop?: boolean,
    borderRight?: boolean,
    borderBottom?: boolean,
    borderLeft?: boolean,
    flashing?: boolean,
};

export type TtmlRegion = { id: string, style: TtmlStyle };

// 文書の時刻0からの秒。未指定は文書が表示されている間ずっと
export type TtmlTiming = { begin?: number, end?: number };

export type TtmlSpan = TtmlTiming & {
    style: TtmlStyle,
    region?: TtmlRegion,
    // 文字列と、<br/>を表すnull
    content: (string | null)[],
};

export type TtmlParagraph = TtmlTiming & {
    style: TtmlStyle,
    region?: TtmlRegion,
    spans: TtmlSpan[],
};

export type TtmlDocument = TtmlTiming & {
    paragraphs: TtmlParagraph[],
};

// ARIB STD-B60のTMD。1111は時刻制御なし
export const tmdNoTimeControl = 0b1111;

// 字幕プレーンの大きさ (ARIB STD-B60のresolution)。分からなければ4K
export function ttmlPlaneSize(resolution: number): TtmlPair {
    switch (resolution) {
        case 0b0000:
            return [1920, 1080];
        case 0b0010:
            return [7680, 4320];
        default:
            return [3840, 2160];
    }
}

export function ttmlColor(c: TtmlColor | undefined): string | null {
    return c ? `rgba(${c[0]},${c[1]},${c[2]},${c[3] / 255})` : null;
}

function sameColor(a: TtmlColor | undefined, b: TtmlColor | undefined): boolean {
    return a === b || (!!a && !!b && a.every((v, i) => v === b[i]));
}

// at (文書の時刻、秒) に表示されているか
export function isTtmlActive(t: TtmlTiming, at: number): boolean {
    return (t.begin ?? -Infinity) <= at && at < (t.end ?? Infinity);
}

// 表示が切り替わりうる時刻 (begin/end) を昇順で返す
export function ttmlBoundaries(doc: TtmlDocument): number[] {
    const times = new Set<number>();
    for (const p of doc.paragraphs) {
        for (const s of p.spans) {
            if (s.begin !== undefined) {
                times.add(s.begin);
            }
            if (s.end !== undefined) {
                times.add(s.end);
            }
        }
    }
    return Array.from(times).sort((a, b) => a - b);
}

// 文書で最初に何かが表示される時刻。beginが無ければundefined (時刻0から)
export function ttmlFirstBegin(doc: TtmlDocument): number | undefined {
    let first: number | undefined;
    for (const p of doc.paragraphs) {
        for (const s of p.spans) {
            const b = s.begin ?? -Infinity;
            if (first === undefined || b < first) {
                first = b;
            }
        }
    }
    if (first === undefined) {
        return doc.begin;
    }
    return Number.isFinite(first) ? first : undefined;
}

// 縦書きで字形の変わる約物を、縦書き用の字形 (Vertical Forms / CJK Compatibility Forms)
// にする。dantto4kのB24変換と同じ表
const verticalForms: Record<number, number> = {
    0xff0c: 0xfe10, 0x3001: 0xfe11, 0x3002: 0xfe12, 0xff1a: 0xfe13, 0xff1b: 0xfe14,
    0x3016: 0xfe17, 0x3017: 0xfe18, 0x2026: 0xfe19, 0x2025: 0xfe30, 0x30fc: 0xfe31,
    0x2014: 0xfe31, 0x2015: 0xfe31, 0x2013: 0xfe32, 0xff3f: 0xfe33, 0xff08: 0xfe35,
    0xff09: 0xfe36, 0xff5b: 0xfe37, 0xff5d: 0xfe38, 0x3014: 0xfe39, 0x3015: 0xfe3a,
    0x3010: 0xfe3b, 0x3011: 0xfe3c, 0x300a: 0xfe3d, 0x300b: 0xfe3e, 0x3008: 0xfe3f,
    0x3009: 0xfe40, 0x300c: 0xfe41, 0x300d: 0xfe42, 0x300e: 0xfe43, 0x300f: 0xfe44,
    0xff3b: 0xfe47, 0xff3d: 0xfe48, 0xff5e: 0x2240, 0x301c: 0x2240, 0xff0d: 0xff5c,
    0xff1d: 0x2016, 0x2010: 0xfe32, 0x2016: 0xff1d, 0xff5c: 0x2015, 0x25b6: 0x25bc,
    0x25c0: 0x25b2,
};

export function verticalForm(ch: string): string {
    const cp = ch.codePointAt(0);
    const v = cp === undefined ? undefined : verticalForms[cp];
    return v === undefined ? ch : String.fromCodePoint(v);
}

// 縦書きではセルの右上に寄せる小書きの仮名 (ARIB TR-B39 表8-4)
const smallKana = new Set([
    0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087,
    0x308e, 0x3095, 0x3096, 0x30a1, 0x30a3, 0x30a5, 0x30a7, 0x30a9, 0x30c3,
    0x30e3, 0x30e5, 0x30e7, 0x30ee, 0x30f5, 0x30f6,
]);

export function isSmallKana(ch: string): boolean {
    const cp = ch.codePointAt(0) ?? 0;
    return smallKana.has(cp) || (cp >= 0x31f0 && cp <= 0x31ff);
}

// 文字は1文字ずつ固定のセルに置く (ARIBの字幕は字詰めをしない)。セルの大きさは
// fontSizeの[横, 縦]で、文字の送りはそれにletter-spacingを足したもの。字形は
// セルの中にletter-spacingの半分だけ寄せて置く (ARIB TR-B39 8.3.4の文字表示領域)。

export type TtmlRect = { x: number, y: number, w: number, h: number, color: TtmlColor };

// 字形1つ。(x, y) からw×hの枠に、字形の全角の枠を合わせて描く
export type TtmlGlyph = { ch: string, x: number, y: number, w: number, h: number, style: TtmlStyle };

export type TtmlLayout = { backgrounds: TtmlRect[], glyphs: TtmlGlyph[] };

const defaultFontSize = 144;

type Run = { text: string, style: TtmlStyle };

// 表示中のspanを<br/>で行に分ける
function splitLines(spans: TtmlSpan[]): Run[][] {
    const lines: Run[][] = [[]];
    for (const s of spans) {
        for (const item of s.content) {
            if (item === null) {
                lines.push([]);
            } else if (item.length > 0) {
                lines[lines.length - 1].push({ text: item, style: s.style });
            }
        }
    }
    return lines;
}

function fontSizeOf(style: TtmlStyle): TtmlPair {
    const [w, h] = style.fontSize ?? [defaultFontSize, defaultFontSize];
    return [w > 0 ? w : h, h > 0 ? h : defaultFontSize];
}

function letterSpacingOf(style: TtmlStyle): number {
    return Math.max(0, style.letterSpacing ?? 0);
}

function visibleColor(c: TtmlColor | undefined): c is TtmlColor {
    return !!c && c[3] > 0;
}

export function layoutTtml(doc: TtmlDocument, at: number): TtmlLayout {
    const out: TtmlLayout = { backgrounds: [], glyphs: [] };
    for (const p of doc.paragraphs) {
        const region = p.region?.style;
        if (!region?.origin) {
            continue;
        }
        const spans = p.spans.filter(s => isTtmlActive(s, at));
        if (spans.length === 0) {
            continue;
        }
        const lines = splitLines(spans);
        if (lines.every(l => l.length === 0)) {
            continue;
        }
        const [ox, oy] = region.origin;
        const [ew, eh] = region.extent ?? [0, 0];
        const regionBackground = region.backgroundColor;
        if (visibleColor(regionBackground) && ew > 0 && eh > 0) {
            out.backgrounds.push({ x: ox, y: oy, w: ew, h: eh, color: regionBackground });
        }
        // spanの背景色。resolverは領域の背景色も継承させるので、領域と同じ色は
        // 二重に塗らない (半透明が濃くなる)
        const spanBackground = (style: TtmlStyle): TtmlColor | undefined =>
            visibleColor(style.backgroundColor) && !sameColor(style.backgroundColor, regionBackground) ? style.backgroundColor : undefined;
        if (region.writingMode === "tbrl" && region.extent) {
            layoutVertical(out, lines, ox + ew, oy, spanBackground);
        } else {
            layoutHorizontal(out, lines, ox, oy, spanBackground);
        }
    }
    return out;
}

function layoutHorizontal(out: TtmlLayout, lines: Run[][], ox: number, oy: number, spanBackground: (style: TtmlStyle) => TtmlColor | undefined) {
    let lineTop = oy;
    for (const line of lines) {
        // 行の高さは行内で最大のlineHeight (normalは文字の高さの1.2倍)
        let lineHeight = 0;
        for (const { style } of line) {
            const [, h] = fontSizeOf(style);
            lineHeight = Math.max(lineHeight, style.lineHeight ?? h * 1.2);
        }
        if (line.length === 0) {
            lineHeight = defaultFontSize * 1.2;
        }
        let x = ox;
        for (const { text, style } of line) {
            const [w, h] = fontSizeOf(style);
            const ls = letterSpacingOf(style);
            const advance = w + ls;
            const chars = Array.from(text);
            const background = spanBackground(style);
            if (background) {
                out.backgrounds.push({ x, y: lineTop, w: advance * chars.length, h: lineHeight, color: background });
            }
            // 文字は行の高さの中で上下中央に置く
            const top = lineTop + (lineHeight - h) / 2;
            for (const ch of chars) {
                out.glyphs.push({ ch, x: x + ls / 2, y: top, w, h, style });
                x += advance;
            }
        }
        lineTop += lineHeight;
    }
}

// 縦書き: 行は領域の右端から左へ並ぶ列、文字は列を上から下へ。半角の2文字だけの
// 並び (2桁の数字など) は1つのセルに横に並べ (縦中横)、小書きの仮名はセルの右上へ
// 字形の1/8ずらす
function layoutVertical(out: TtmlLayout, lines: Run[][], right: number, top: number, spanBackground: (style: TtmlStyle) => TtmlColor | undefined) {
    let columnRight = right;
    for (const line of lines) {
        let columnWidth = 0;
        for (const { style } of line) {
            const [w] = fontSizeOf(style);
            columnWidth = Math.max(columnWidth, style.lineHeight ?? w * 1.2);
        }
        if (line.length === 0) {
            columnWidth = defaultFontSize * 1.2;
        }
        const columnLeft = columnRight - columnWidth;
        let y = top;
        for (const { text, style } of line) {
            const [w, h] = fontSizeOf(style);
            const ls = letterSpacingOf(style);
            const step = h + ls;
            const x = columnLeft + (columnWidth - w) / 2;
            const chars = Array.from(text);
            const tateChuYoko = w < h && chars.length === 2;
            const cells = tateChuYoko ? 1 : chars.length;
            const background = spanBackground(style);
            if (background) {
                out.backgrounds.push({ x: columnLeft, y, w: columnWidth, h: step * cells, color: background });
            }
            if (tateChuYoko) {
                chars.forEach((ch, i) => {
                    out.glyphs.push({ ch, x: x + (i === 0 ? -0.5 : 0.5) * w, y: y + ls / 2, w, h, style });
                });
                y += step;
                continue;
            }
            for (const ch of chars) {
                const small = isSmallKana(ch);
                out.glyphs.push({
                    ch: verticalForm(ch),
                    x: x + (small ? w / 8 : 0),
                    y: y + ls / 2 - (small ? h / 8 : 0),
                    w,
                    h,
                    style,
                });
                y += step;
            }
        }
        columnRight = columnLeft;
    }
}

// SVGフォント (外字の字幕リソース) のunicode / unicode-range属性値からコードポイントを
// 1つ取り出す。文字そのもの、"U+E001"や範囲"U+E001-E0FF" (先頭を使う)、二重に
// エスケープされて残った文字参照 ("&#xE001;")を受け付ける。文字参照を文字列のまま
// 読むと'&'に登録されてしまうので、解釈できなければundefinedを返す
export function parseCodepointAttribute(value: string | null | undefined): number | undefined {
    if (!value) {
        return undefined;
    }
    const valid = (cp: number) => Number.isFinite(cp) && cp > 0 && cp <= 0x10ffff ? cp : undefined;
    const reference = value.match(/^&#(x?)([0-9a-fA-F]+);?$/i);
    if (reference) {
        return valid(parseInt(reference[2], reference[1] ? 16 : 10));
    }
    if (value.startsWith("&#")) {
        return undefined;
    }
    const range = value.match(/^[uU]\+([0-9a-fA-F?]+)/);
    if (range) {
        // "U+E0??"のようなワイルドカードの範囲は先頭 (?を0とみなす) を使う
        return valid(parseInt(range[1].replace(/\?/g, "0"), 16));
    }
    return value.codePointAt(0);
}

// 文書の本文にcpsのコードポイントがあるか
export function ttmlUsesAnyCodepoint(doc: TtmlDocument, cps: Iterable<number>): boolean {
    const wanted = new Set(cps);
    if (wanted.size === 0) {
        return false;
    }
    for (const p of doc.paragraphs) {
        for (const s of p.spans) {
            for (const text of s.content) {
                if (text === null) {
                    continue;
                }
                for (const ch of text) {
                    const cp = ch.codePointAt(0);
                    if (cp !== undefined && wanted.has(cp)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}
