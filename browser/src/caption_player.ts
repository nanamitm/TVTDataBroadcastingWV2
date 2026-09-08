import { playRomSound } from "web-bml";
import { VideoPlayer } from "./video_player";

type SendMessage = (message: any) => void;

export type NativeCaptionImage = {
    x: number;
    y: number;
    width: number;
    height: number;
    stride: number;
    data: string;
};

// PESの分離はweb-bmlに任せ、字幕のデコードと描画はホスト側のlibaribcaptionで行う。
export class CaptionPlayer extends VideoPlayer {
    private readonly canvases = [document.createElement("canvas"), document.createElement("canvas")];
    private readonly sendMessage: SendMessage;
    private audioNode?: AudioNode;

    public constructor(video: HTMLVideoElement, container: HTMLElement, sendMessage: SendMessage) {
        super(video, container);
        this.sendMessage = sendMessage;
        for (const canvas of this.canvases) {
            canvas.style.position = "absolute";
            canvas.style.left = "0";
            canvas.style.top = "0";
            canvas.style.width = "100%";
            canvas.style.height = "100%";
            canvas.style.pointerEvents = "none";
            this.container.append(canvas);
        }
        this.sendMessage({ type: "captionReset" });
    }

    public setSource(_source: string): void {
    }

    public updateTime(pcr: number): void {
        const layoutElement = this.container.parentElement ?? this.container;
        const pixelRatio = globalThis.devicePixelRatio || 1;
        this.sendMessage({
            type: "captionTime",
            time: pcr,
            width: Math.max(1, Math.round(layoutElement.clientWidth * pixelRatio)),
            height: Math.max(1, Math.round(layoutElement.clientHeight * pixelRatio)),
        });
    }

    public push(streamId: number, pes: Uint8Array, pts?: number): void {
        if (streamId !== 0xbd && streamId !== 0xbf) {
            return;
        }
        this.sendMessage({
            type: "captionPes",
            streamId,
            data: Array.from(pes),
            ...(pts == null ? {} : { pts }),
        });
    }

    public draw(track: number, frameWidth: number | undefined, frameHeight: number | undefined,
                images: NativeCaptionImage[]): void {
        const canvas = this.canvases[track];
        if (canvas == null) {
            return;
        }
        if (frameWidth != null && frameHeight != null &&
            (canvas.width !== frameWidth || canvas.height !== frameHeight)) {
            canvas.width = frameWidth;
            canvas.height = frameHeight;
        }
        const context = canvas.getContext("2d");
        if (context == null) {
            return;
        }
        context.clearRect(0, 0, canvas.width, canvas.height);
        for (const image of images) {
            if (image.width <= 0 || image.height <= 0 || image.stride < image.width * 4) {
                continue;
            }
            const binary = atob(image.data);
            const source = Uint8Array.from(binary, c => c.charCodeAt(0));
            if (source.length < image.stride * image.height) {
                continue;
            }
            const pixels = new Uint8ClampedArray(image.width * image.height * 4);
            for (let y = 0; y < image.height; ++y) {
                pixels.set(source.subarray(y * image.stride, y * image.stride + image.width * 4),
                           y * image.width * 4);
            }
            // libaribcaptionのDirectWrite出力はpremultiplied RGBAだが、
            // ImageDataの入力はstraight alphaなので色成分を戻してから渡す。
            for (let i = 0; i < pixels.length; i += 4) {
                const alpha = pixels[i + 3];
                if (alpha > 0 && alpha < 255) {
                    pixels[i] = Math.min(255, Math.round(pixels[i] * 255 / alpha));
                    pixels[i + 1] = Math.min(255, Math.round(pixels[i + 1] * 255 / alpha));
                    pixels[i + 2] = Math.min(255, Math.round(pixels[i + 2] * 255 / alpha));
                }
            }
            context.putImageData(new ImageData(pixels, image.width, image.height), image.x, image.y);
        }
    }

    public playBuiltinSound(sound: number): void {
        if (this.audioNode != null && this.container.style.display !== "none") {
            playRomSound(sound, this.audioNode);
        }
    }

    public showCC(): void {
        this.container.style.display = "";
    }

    public hideCC(): void {
        this.container.style.display = "none";
    }

    public override setPRAAudioNode(audioNode?: AudioNode): void {
        this.audioNode = audioNode;
    }
}
