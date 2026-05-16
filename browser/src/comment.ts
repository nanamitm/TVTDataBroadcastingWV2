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

const DURATION_MS = 8000;
const DEFAULT_OPACITY = 1.0;
const FONT_SIZE: Record<string, number> = { small: 18, medium: 24, big: 36 };
const MAX_LANES = 20;

// ニコニコ実況の色名 → CSS色
const COLOR_MAP: Record<string, string> = {
    white: "white", red: "red", blue: "#4169e1", yellow: "yellow",
    green: "#00b300", cyan: "cyan", purple: "#cc00cc", black: "black",
    niconicowhite: "white", cadetblue: "cadetblue", maroon: "maroon",
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
    // レーンごとの「次にコメントを追加できる時刻」
    private nakaLane: number[] = new Array(MAX_LANES).fill(0);
    private topLane: number[] = new Array(MAX_LANES).fill(0);
    private botLane: number[] = new Array(MAX_LANES).fill(0);

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
        const fontSize = FONT_SIZE[data.size ?? "medium"] ?? FONT_SIZE.medium;
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
            const blockMs = (textWidth / (this.canvas.width + textWidth)) * DURATION_MS;
            this.nakaLane[lane] = now + blockMs;
            y = laneH * (lane + 1);
        } else if (pos === "ue") {
            const lane = this.freeLane(this.topLane, maxLanes, now);
            this.topLane[lane] = now + DURATION_MS;
            y = laneH * (lane + 1);
        } else {
            const lane = this.freeLane(this.botLane, maxLanes, now);
            this.botLane[lane] = now + DURATION_MS;
            y = this.canvas.height - laneH * lane;
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
        let best = 0;
        for (let i = 0; i < max && i < lanes.length; i++) {
            if (lanes[i] <= now) return i;
            if (lanes[i] < lanes[best]) best = i;
        }
        return best;
    }

    setOpacity(opacity: number) {
        this.opacity = Math.max(0, Math.min(1, opacity));
    }

    private draw() {
        const { canvas, ctx } = this;
        const now = performance.now();
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        ctx.globalAlpha = this.opacity;

        this.comments = this.comments.filter(c => now - c.createdAt < DURATION_MS);

        for (const c of this.comments) {
            const progress = (now - c.createdAt) / DURATION_MS;
            let x: number;
            if (c.position === "naka") {
                x = c.startX - progress * (canvas.width + c.textWidth);
            } else {
                x = c.startX;
            }

            ctx.font = `bold ${c.fontSize}px sans-serif`;
            ctx.lineWidth = Math.max(2, c.fontSize / 8);
            ctx.strokeStyle = "rgba(0,0,0,0.85)";
            ctx.lineJoin = "round";
            ctx.strokeText(c.text, x, c.y);
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
