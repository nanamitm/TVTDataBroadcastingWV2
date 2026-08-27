export interface CommentData {
    text: string;
    color?: string;
    size?: "small" | "medium" | "big";
    position?: "naka" | "ue" | "shita";
}

interface ActiveComment {
    text: string;
    color: string;
    fontSize: number;
    startX: number;
    y: number;
    textWidth: number;
    position: "naka" | "ue" | "shita";
    createdAt: number;
}

const DEFAULT_DURATION_MS = 4000;
const MIN_DURATION_MS = 1000;
const MAX_DURATION_MS = 5000;
const DEFAULT_OPACITY = 1.0;
const DEFAULT_SHADOW_COLOR = "rgba(0,0,0,0.7)";
const DEFAULT_FONT_SIZE: Record<string, number> = { small: 18, medium: 24, big: 36 };

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
    private nakaLane: number[] = [];
    private topLane: number[] = [];
    private botLane: number[] = [];

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
        const color = COLOR_MAP[data.color ?? "white"] ?? data.color ?? "white";
        const now = performance.now();

        this.ctx.font = `bold ${fontSize}px sans-serif`;
        const textWidth = this.ctx.measureText(data.text).width;
        const laneH = fontSize + 2;
        const maxLanes = Math.max(1, Math.floor(this.canvas.height / laneH));

        let y: number;
        if (pos === "naka") {
            const lane = this.freeLane(this.nakaLane, maxLanes, now);
            // 先頭が画面左端に到達するまでの時間だけレーンをブロック
            const blockMs = (textWidth / (this.canvas.width + textWidth)) * this.durationMs;
            this.nakaLane[lane] = now + blockMs;
            y = laneH * (lane + 1);
        } else if (pos === "ue") {
            const lane = this.freeLane(this.topLane, maxLanes, now);
            this.topLane[lane] = now + this.durationMs;
            y = laneH * (lane + 1);
        } else {
            const lane = this.freeLane(this.botLane, maxLanes, now);
            this.botLane[lane] = now + this.durationMs;
            y = this.canvas.height - laneH * lane - Math.ceil(fontSize * 0.25) - laneH / 2;
        }

        this.comments.push({
            text: data.text,
            color,
            fontSize,
            startX: pos === "naka" ? this.canvas.width : (this.canvas.width - textWidth) / 2,
            y,
            textWidth,
            position: pos,
            createdAt: now,
        });
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
        this.nakaLane.fill(0);
        this.topLane.fill(0);
        this.botLane.fill(0);
    }
}
