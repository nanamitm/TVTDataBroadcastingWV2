export interface CommentData {
    text: string;
    color?: string;
    size?: "small" | "medium" | "big";
    position?: "naka" | "ue" | "shita";
    // NicoJKのローカル拡張属性 ([CustomReplace]でchatタグに付与する)
    align?: "left" | "right" | "";  // ue/shitaコメの寄せ。既定は中央
    insertLast?: boolean;           // 直前のコメントの隣のレーンに積む
    yourpost?: boolean;             // 背景を敷いて強調表示する
}

interface ActiveComment {
    text: string;
    color: string;
    fontSize: number;
    startX: number;
    y: number;
    textWidth: number;
    position: "naka" | "ue" | "shita";
    laneKey: string;
    lane: number;
    yourpost: boolean;
    createdAt: number;
}

const DEFAULT_DURATION_MS = 4000;
const MIN_DURATION_MS = 1000;
const MAX_DURATION_MS = 5000;
const DEFAULT_OPACITY = 1.0;
const DEFAULT_SHADOW_COLOR = "rgba(0,0,0,0.7)";
const DEFAULT_FONT_SIZE: Record<string, number> = { small: 18, medium: 24, big: 36 };
// yourpostコメの背景の濃さ (NicoJKのbackOpacity=160相当)
const YOURPOST_BACK_ALPHA = 160 / 255;

// ニコニコ実況の色名 → CSS色
const COLOR_MAP: Record<string, string> = {
    white: "white", red: "red", blue: "#4169e1", yellow: "yellow",
    green: "#00b300", cyan: "cyan", purple: "#cc00cc", black: "black",
    niconicowhite: "#cccc99", cadetblue: "cadetblue", maroon: "maroon",
    fuchsia: "fuchsia", lime: "lime", olive: "olive", navy: "navy",
    teal: "teal", silver: "silver", gray: "gray", orange: "orange",
    midori: "#00b300",
};

export class CommentRenderer {
    private readonly canvas: HTMLCanvasElement;
    private readonly ctx: CanvasRenderingContext2D;
    private comments: ActiveComment[] = [];
    private rafId = 0;
    private opacity = DEFAULT_OPACITY;
    private durationMs = DEFAULT_DURATION_MS;
    private fontSize: Record<string, number> = { ...DEFAULT_FONT_SIZE };
    private shadowColor = DEFAULT_SHADOW_COLOR;
    private shadowEnabled = true;
    private outlineEnabled = true;
    // レーンごとの「次にコメントを追加できる時刻」(画面の高さに応じて伸ばす)
    // キーは "{position}:{alignFactor}"。NicoJKと同じく寄せの異なるコメントは
    // 重ならないので、レーンは寄せごとに独立して管理する
    private lanes = new Map<string, number[]>();

    constructor(canvas: HTMLCanvasElement) {
        this.canvas = canvas;
        this.ctx = canvas.getContext("2d")!;
    }

    add(comments: CommentData[]) {
        for (const c of comments) {
            this.addOne(c);
        }
    }

    private addOne(data: CommentData) {
        const fontSize = this.fontSize[data.size ?? "medium"] ?? this.fontSize.medium;
        const pos = data.position ?? "naka";
        // 0=左寄せ 1=中央 2=右寄せ。流れるコメントは寄せの指定を持たない
        const alignFactor = pos === "naka" ? 0
            : data.align === "left" ? 0 : data.align === "right" ? 2 : 1;
        const color = COLOR_MAP[data.color ?? "white"] ?? data.color ?? "white";
        const now = performance.now();

        this.ctx.font = `bold ${fontSize}px sans-serif`;
        const textWidth = this.ctx.measureText(data.text).width;
        const laneH = fontSize + 2;
        const maxLanes = Math.max(1, Math.floor(this.canvas.height / laneH));

        const laneKey = `${pos}:${alignFactor}`;
        let lanes = this.lanes.get(laneKey);
        if (lanes === undefined) {
            lanes = [];
            this.lanes.set(laneKey, lanes);
        }

        let lane = -1;
        if (pos !== "naka" && data.insertLast) {
            // 同じ位置・寄せで最後に追加されたコメントの1つ隣(画面端から遠い側)に積む
            const last = this.lastLane(laneKey);
            if (last >= 0 && last + 1 < maxLanes) {
                lane = last + 1;
                // そのレーンに残っているコメントは表示期限を切り上げて重なりを避ける
                for (const c of this.comments) {
                    if (c.laneKey === laneKey && c.lane === lane) {
                        c.createdAt = now - this.durationMs;
                    }
                }
            }
        }
        if (lane < 0) {
            lane = this.freeLane(lanes, maxLanes, now);
        }
        // 流れるコメントは先頭が画面左端に到達するまでの時間だけレーンをブロック
        lanes[lane] = now + (pos === "naka"
            ? (textWidth / (this.canvas.width + textWidth)) * this.durationMs
            : this.durationMs);

        const y = pos === "shita"
            ? this.canvas.height - laneH * lane - Math.ceil(fontSize * 0.25) - laneH / 2
            : laneH * (lane + 1);

        this.comments.push({
            text: data.text,
            color,
            fontSize,
            startX: pos === "naka"
                ? this.canvas.width
                : (this.canvas.width - textWidth) * alignFactor / 2,
            y,
            textWidth,
            position: pos,
            laneKey,
            lane,
            yourpost: data.yourpost === true,
            createdAt: now,
        });
    }

    // laneKeyのグループで最後に追加された(まだ表示中の)コメントのレーン
    private lastLane(laneKey: string): number {
        for (let i = this.comments.length - 1; i >= 0; i--) {
            if (this.comments[i].laneKey === laneKey) {
                return this.comments[i].lane;
            }
        }
        return -1;
    }

    // 任意のCSS色をrgba()に正規化する(yourpostの背景を半透明にするため)
    private toRgba(color: string, alpha: number): string {
        const prev = this.ctx.fillStyle;
        this.ctx.fillStyle = "#000000";
        this.ctx.fillStyle = color;
        const normalized = String(this.ctx.fillStyle);
        this.ctx.fillStyle = prev;
        if (normalized.length === 7 && normalized.charAt(0) === "#") {
            const r = parseInt(normalized.substring(1, 3), 16);
            const g = parseInt(normalized.substring(3, 5), 16);
            const b = parseInt(normalized.substring(5, 7), 16);
            return `rgba(${r},${g},${b},${alpha})`;
        }
        const m = /^rgba?\(([^)]*)\)/.exec(normalized);
        if (m !== null) {
            const p = m[1].split(",").map(x => parseFloat(x));
            const a = p.length > 3 && isFinite(p[3]) ? p[3] : 1;
            return `rgba(${p[0] | 0},${p[1] | 0},${p[2] | 0},${alpha * a})`;
        }
        return `rgba(0,0,0,${alpha})`;
    }

    private freeLane(lanes: number[], max: number, now: number): number {
        while (lanes.length < max) {
            lanes.push(0);
        }
        let best = 0;
        for (let i = 0; i < max; i++) {
            if (lanes[i] <= now) return i;
            if (lanes[i] < lanes[best]) best = i;
        }
        return best;
    }

    setOpacity(opacity: number) {
        this.opacity = Math.max(0, Math.min(1, opacity));
    }

    setDuration(ms: number) {
        this.durationMs = Math.max(MIN_DURATION_MS, Math.min(MAX_DURATION_MS, ms));
    }

    setShadowColor(color: string) {
        this.shadowColor = color;
    }

    setShadowEnabled(enabled: boolean) {
        this.shadowEnabled = enabled;
    }

    setOutlineEnabled(enabled: boolean) {
        this.outlineEnabled = enabled;
    }

    setFontSizeMedium(medium: number) {
        this.fontSize = { small: Math.round(medium * 0.75), medium, big: Math.round(medium * 1.5) };
    }

    private draw() {
        const { canvas, ctx } = this;
        const now = performance.now();
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        ctx.globalAlpha = this.opacity;

        this.comments = this.comments.filter(c => now - c.createdAt < this.durationMs);

        for (const c of this.comments) {
            const progress = (now - c.createdAt) / this.durationMs;
            let x: number;
            if (c.position === "naka") {
                x = c.startX - progress * (canvas.width + c.textWidth);
            } else {
                x = c.startX;
            }

            ctx.font = `bold ${c.fontSize}px sans-serif`;
            if (c.yourpost) {
                // 自分の投稿は背景を敷いて目立たせる(NicoJKのbackOpacity相当)
                const offset = Math.max(1, c.fontSize / 15);
                const top = c.y - c.fontSize * 0.9 - 1;
                const h = c.fontSize * 1.2 + offset + 3;
                const grad = ctx.createLinearGradient(0, top, 0, top + h);
                grad.addColorStop(0, this.toRgba(c.color, YOURPOST_BACK_ALPHA));
                grad.addColorStop(1, this.toRgba(this.shadowColor, YOURPOST_BACK_ALPHA));
                ctx.fillStyle = grad;
                ctx.fillRect(x - 1, top, c.textWidth + offset + 3, h);
            }
            if (this.shadowEnabled) {
                const shadowOffset = Math.max(1, c.fontSize / 15);
                ctx.fillStyle = this.shadowColor;
                ctx.fillText(c.text, x + shadowOffset, c.y + shadowOffset);
            }
            if (this.outlineEnabled) {
                ctx.lineWidth = Math.max(2, c.fontSize / 8);
                ctx.strokeStyle = "rgba(0,0,0,0.85)";
                ctx.lineJoin = "round";
                ctx.strokeText(c.text, x, c.y);
            }
            ctx.fillStyle = c.color;
            ctx.fillText(c.text, x, c.y);
        }

        this.rafId = requestAnimationFrame(() => this.draw());
    }

    start() {
        if (this.rafId === 0) {
            this.rafId = requestAnimationFrame(() => this.draw());
        }
    }

    stop() {
        if (this.rafId !== 0) {
            cancelAnimationFrame(this.rafId);
            this.rafId = 0;
            this.ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
        }
    }

    clear() {
        this.comments = [];
        this.lanes.clear();
    }
}
