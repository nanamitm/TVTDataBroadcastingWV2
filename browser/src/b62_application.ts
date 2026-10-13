// 4Kデータ放送 (ARIB STD-B62) のアプリケーションを表示するiframe
// アプリケーションのファイルはネイティブ側が https://tvtdatabroadcastingwv2-b62.invalid/ から返す。
// このページとは別オリジンなので、アプリケーションからこのページには触れない。

const appOrigin = "https://tvtdatabroadcastingwv2-b62.invalid";
// ARIB STD-B62のアプリケーションは3840x2160で描画される
const appWidth = 3840;
const appHeight = 2160;

type FrameMessage = {
    source: "b62Application",
    type: "destroy" | "show" | "hide",
} | {
    source: "b62Application",
    type: "getEventInformation",
    id: number,
};

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

    constructor(container: HTMLElement) {
        this.container = container;
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

    private close() {
        this.frame?.remove();
        this.frame = null;
        this.currentKey = null;
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
        } else if (message.type === "destroy") {
            this.destroyedKey = this.currentKey;
            this.close();
        } else if (message.type === "hide") {
            this.frame.style.visibility = "hidden";
        } else if (message.type === "show") {
            this.frame.style.visibility = "visible";
        }
    }
}
