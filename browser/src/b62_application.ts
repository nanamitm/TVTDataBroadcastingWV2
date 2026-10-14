// 4Kデータ放送 (ARIB STD-B62) のアプリケーションを表示するiframe
// アプリケーションのファイルはネイティブ側が https://tvtdatabroadcastingwv2-b62.invalid/ から返す。
// このページとは別オリジンなので、アプリケーションからこのページには触れない。

const appOrigin = "https://tvtdatabroadcastingwv2-b62.invalid";
// ARIB STD-B62のアプリケーションは3840x2160で描画される
const appWidth = 3840;
const appHeight = 2160;

// web-bmlのAribKeyCodeからアプリケーションに渡すkeyCodeへ (B62Shim.hのVK_*と合わせる)
const keyCodeMap = new Map<number, number>([
    [1, 38], // Up
    [2, 40], // Down
    [3, 37], // Left
    [4, 39], // Right
    [5, 48], [6, 49], [7, 50], [8, 51], [9, 52], [10, 53], [11, 54], [12, 55], [13, 56], [14, 57], // 0-9
    [18, 13], // Enter
    [19, 461], // Back
    [20, 0x1F5], // dボタン
    [21, 406], // 青
    [22, 403], // 赤
    [23, 404], // 緑
    [24, 405], // 黄
]);

type FrameMessage = {
    source: "b62Application",
    type: "destroy" | "show" | "hide",
} | {
    source: "b62Application",
    type: "getEventInformation",
    id: number,
} | {
    source: "b62Application",
    type: "videoRect",
    rect: VideoRect | null,
};

// アプリケーションの文書 (3840x2160) 上の映像オブジェクトの位置
type VideoRect = { left: number, top: number, width: number, height: number };

// web-bmlのprogramInfoのうち使うもの
export type B62ProgramInfo = {
    serviceId: number | null,
    originalNetworkId: number | null,
    transportStreamId: number | null,
    eventId: number | null,
    eventName: string | null,
    startTimeUnixMillis: number | null,
    durationSeconds: number | null,
};

export class B62ApplicationFrame {
    private readonly container: HTMLElement;
    private frame: HTMLIFrameElement | null = null;
    private currentKey: string | null = null;
    // destroyApplication()で終了したアプリケーション。AITが変わるまで起動し直さない
    private destroyedKey: string | null = null;
    private programInfo: B62ProgramInfo | null = null;
    private videoRect: VideoRect | null = null;
    private readonly onVideoChanged: () => void;

    // onVideoChangedは映像を出す位置が変わったときに呼ばれる
    constructor(container: HTMLElement, onVideoChanged: () => void) {
        this.container = container;
        this.onVideoChanged = onVideoChanged;
        window.addEventListener("message", (ev) => this.onFrameMessage(ev));
    }

    update(url: string | null, version?: number) {
        if (url == null || !url.startsWith(appOrigin + "/")) {
            this.destroyedKey = null;
            this.close();
            return;
        }
        const key = `${url}#${version ?? ""}`;
        if (key === this.currentKey || key === this.destroyedKey) {
            return;
        }
        this.destroyedKey = null;
        this.close();
        const frame = document.createElement("iframe");
        frame.src = url;
        frame.setAttribute("allowtransparency", "true");
        frame.style.position = "absolute";
        frame.style.left = "0px";
        frame.style.top = "0px";
        frame.style.width = `${appWidth}px`;
        frame.style.height = `${appHeight}px`;
        frame.style.border = "none";
        frame.style.background = "transparent";
        frame.style.colorScheme = "normal";
        frame.style.transformOrigin = "0px 0px";
        // データ放送の表示 (BML) より上、字幕やコメントより下に置く
        const content = this.container.querySelector(".data-broadcasting-browser-content");
        this.container.insertBefore(frame, content?.nextSibling ?? null);
        this.frame = frame;
        this.currentKey = key;
        this.resize();
    }

    // アプリケーションが動いていればキーを渡してtrueを返す
    processKey(aribKeyCode: number): boolean {
        const frame = this.frame;
        if (frame?.contentWindow == null) {
            return false;
        }
        const keyCode = keyCodeMap.get(aribKeyCode);
        if (keyCode != null) {
            frame.contentWindow.postMessage({ source: "b62Host", type: "key", keyCode }, appOrigin);
        }
        // 対応するキーがなくてもBMLには渡さない (4KではBMLは動かない)
        return true;
    }

    setProgramInfo(programInfo: B62ProgramInfo) {
        this.programInfo = programInfo;
    }

    resize() {
        if (this.frame == null) {
            return;
        }
        const width = this.container.clientWidth || 960;
        const height = this.container.clientHeight || 540;
        this.frame.style.transform = `scale(${width / appWidth}, ${height / appHeight})`;
    }

    // 映像を出す位置 (このページのビューポート座標)。アプリケーションが映像を置いていなければnull
    videoRectInViewport(): DOMRect | null {
        if (this.frame == null || this.videoRect == null || this.frame.style.visibility === "hidden") {
            return null;
        }
        const frameRect = this.frame.getBoundingClientRect();
        const scaleX = frameRect.width / appWidth;
        const scaleY = frameRect.height / appHeight;
        return new DOMRect(
            frameRect.left + this.videoRect.left * scaleX,
            frameRect.top + this.videoRect.top * scaleY,
            this.videoRect.width * scaleX,
            this.videoRect.height * scaleY);
    }

    // 映像オブジェクトの部分をくり抜き、下にある映像ウィンドウを見せる
    private updateVideoHole() {
        if (this.frame == null) {
            return;
        }
        const r = this.videoRect;
        if (r == null) {
            this.frame.style.clipPath = "";
            return;
        }
        const left = Math.max(0, r.left), top = Math.max(0, r.top);
        const right = Math.min(appWidth, r.left + r.width), bottom = Math.min(appHeight, r.top + r.height);
        this.frame.style.clipPath = `polygon(evenodd, 0 0, ${appWidth}px 0, ${appWidth}px ${appHeight}px, 0 ${appHeight}px, 0 0, ` +
            `${left}px ${top}px, ${right}px ${top}px, ${right}px ${bottom}px, ${left}px ${bottom}px, ${left}px ${top}px)`;
    }

    private close() {
        const hadVideo = this.videoRect != null;
        this.frame?.remove();
        this.frame = null;
        this.currentKey = null;
        this.videoRect = null;
        if (hadVideo) {
            this.onVideoChanged();
        }
    }

    private onFrameMessage(ev: MessageEvent) {
        if (ev.origin !== appOrigin || this.frame == null || ev.source !== this.frame.contentWindow) {
            return;
        }
        const message = ev.data as FrameMessage;
        if (message?.source !== "b62Application") {
            return;
        }
        if (message.type === "getEventInformation") {
            // ARIB STD-B62のgetCurrentEventInformationの形にする (分かる項目のみ)
            const info = this.programInfo;
            const data = {
                service_id: info?.serviceId ?? null,
                original_network_id: info?.originalNetworkId ?? null,
                transport_stream_id: info?.transportStreamId ?? null,
                event_id: info?.eventId ?? null,
                event_name: info?.eventName ?? null,
                start_time: info?.startTimeUnixMillis != null ? new Date(info.startTimeUnixMillis).toISOString() : null,
                duration: info?.durationSeconds ?? null,
            };
            (ev.source as Window).postMessage({ source: "b62Host", id: message.id, data }, appOrigin);
        } else if (message.type === "videoRect") {
            this.videoRect = message.rect;
            this.updateVideoHole();
            this.onVideoChanged();
        } else if (message.type === "destroy") {
            this.destroyedKey = this.currentKey;
            this.close();
        } else if (message.type === "hide") {
            this.frame.style.visibility = "hidden";
            this.onVideoChanged();
        } else if (message.type === "show") {
            this.frame.style.visibility = "visible";
            this.onVideoChanged();
        }
    }
}
