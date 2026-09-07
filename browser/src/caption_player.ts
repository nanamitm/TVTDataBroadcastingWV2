import { CanvasMainThreadRenderer, MPEGTSFeeder, type PartialCanvasRendererOption } from "aribb24.js";
import { playRomSound } from "web-bml";
import { VideoPlayer } from "./video_player";

const MAX_CAPTION_DURATION_SECONDS = 3 * 60;

type CaptionTrack = {
    feeder: MPEGTSFeeder;
    renderer: CanvasMainThreadRenderer;
    previousPresentation: number | undefined;
};

// web-bmlからPESを受け取り、外部PCRクロックに合わせて字幕を描画する
export class CaptionPlayer extends VideoPlayer {
    private readonly captionTrack: CaptionTrack;
    private readonly superimposeTrack: CaptionTrack;
    private pcr: number | undefined;
    private paintQueued = false;
    private audioNode?: AudioNode;

    public constructor(video: HTMLVideoElement, container: HTMLElement) {
        super(video, container);

        const rendererOption: PartialCanvasRendererOption = {
            font: {
                normal: "丸ゴシック",
                arib: "丸ゴシック",
            },
            color: {
                stroke: "black",
            },
            resize: {
                target: "container",
                objectFit: "none",
            },
        };

        this.captionTrack = {
            feeder: new MPEGTSFeeder({ recieve: { type: "Caption" }, tokenizer: {}, offset: {} }),
            renderer: new CanvasMainThreadRenderer(rendererOption),
            previousPresentation: undefined,
        };
        this.superimposeTrack = {
            feeder: new MPEGTSFeeder({ recieve: { type: "Superimpose" }, tokenizer: {}, offset: {} }),
            renderer: new CanvasMainThreadRenderer(rendererOption),
            previousPresentation: undefined,
        };

        // TVTestから渡される外部PCRを使うため、Controllerの動画時計の代わりに
        // フィーダーの読み出し開始位置を明示する。
        this.captionTrack.feeder.prepare(0);
        this.superimposeTrack.feeder.prepare(0);
        this.captionTrack.renderer.onAttach(container);
        this.superimposeTrack.renderer.onAttach(container);
    }

    public setSource(_source: string): void {
    }

    public updateTime(pcr: number): void {
        this.pcr = pcr / 1000;

        // content()で、この時刻までに届いたPESの非同期デコードを開始する。
        this.captionTrack.feeder.content(this.pcr);
        this.superimposeTrack.feeder.content(this.pcr);

        if (!this.paintQueued) {
            this.paintQueued = true;
            requestAnimationFrame(() => {
                this.paintQueued = false;
                if (this.pcr == null) {
                    return;
                }
                this.paint(this.captionTrack, this.pcr);
                this.paint(this.superimposeTrack, this.pcr);
            });
        }
    }

    public push(streamId: number, pes: Uint8Array, pts?: number): void {
        if (streamId === 0xbd && pts != null) {
            // web-bmlのPTSは90 kHz、aribb24.js v2の時刻単位は秒。
            this.captionTrack.feeder.feedB24(pes, pts / 90000);
        } else if (streamId === 0xbf && this.pcr != null) {
            // 文字スーパーにはPTSがないため、受信時点のPCRを表示時刻にする。
            this.superimposeTrack.feeder.feedB24(pes, this.pcr);
        }
    }

    private paint(track: CaptionTrack, currentTime: number): void {
        this.ensureRendererSize(track);
        const presentation = track.feeder.content(currentTime);
        if (presentation == null) {
            if (track.previousPresentation != null) {
                track.renderer.clear();
                track.previousPresentation = undefined;
            }
            return;
        }

        const duration = Math.min(presentation.duration, MAX_CAPTION_DURATION_SECONDS);
        if (currentTime >= presentation.pts + duration) {
            const endTime = presentation.pts + duration;
            if (track.previousPresentation !== endTime) {
                track.renderer.clear();
                track.previousPresentation = endTime;
            }
            return;
        }

        if (track.previousPresentation === presentation.pts) {
            return;
        }

        track.renderer.clear();
        track.renderer.render(presentation.state, structuredClone(presentation.data), presentation.info);
        track.previousPresentation = presentation.pts;

        if (this.audioNode != null && this.container.style.display !== "none") {
            for (const token of presentation.data) {
                if (token.tag === "BuiltinSoundReplay") {
                    playRomSound(token.sound, this.audioNode);
                }
            }
        }
    }

    public showCC(): void {
        this.container.style.display = "";
    }

    private ensureRendererSize(track: CaptionTrack): void {
        const layoutElement = this.container.parentElement ?? this.container;
        const pixelRatio = globalThis.devicePixelRatio || 1;
        const width = Math.max(1, Math.round(layoutElement.clientWidth * pixelRatio));
        const height = Math.max(1, Math.round(layoutElement.clientHeight * pixelRatio));
        const canvas = track.renderer.getPresentationCanvas();
        if (canvas.width !== width || canvas.height !== height) {
            track.renderer.resize(width, height);
            track.previousPresentation = undefined;
        }
    }

    public hideCC(): void {
        this.container.style.display = "none";
    }

    public override setPRAAudioNode(audioNode?: AudioNode): void {
        this.audioNode = audioNode;
    }
}
