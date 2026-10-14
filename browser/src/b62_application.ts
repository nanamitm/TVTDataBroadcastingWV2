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
    // 映像より手前に重なっている要素 (映像の範囲内に切り詰めたもの)
    above?: VideoRect[],
} | {
    source: "b62Application",
    type: "hello",
} | {
    source: "b62Application",
    type: "romsound",
    id: number,
};

// ネイティブから届く字幕 (TVTDataBroadcastingWV2.cppのSendB62Captions)
export type B62Caption = {
    componentTag: number,
    dataType: number,
    tmd: number,
    // 文書の時刻0を提示するPTS (90kHz)。nullならすぐに渡す
    pts: number | null,
    // 字幕プラグインの表示遅延 (ミリ秒)
    delay: number,
    data: string,
};

// ネイティブから届くイベントメッセージ (TVTDataBroadcastingWV2.cppのSendB62Captions)
export type B62EventMessage = {
    emtTag: number,
    messageGroupId: number,
    // 0: すぐ、1/5: UTC、2: NPT、3: 番組開始からの相対時刻 (ARIB STD-B60 表11-4)
    timeMode: number,
    // event_msg_UTC / NPT / relativeTimeのNTP形式 (秒と小数部)
    timeSeconds: number,
    timeFraction: number,
    messageType: number,
    messageId: number,
    privateData: string,
};

// NTPの秒は1900年起点。最上位ビットが0なら2036年からの周回 (ARIB STD-B60 11.2.2.2)
function ntpToUnixMillis(seconds: number, fraction: number): number {
    const era = seconds < 0x80000000 ? 2 ** 32 : 0;
    return (seconds + era - 2208988800) * 1000 + fraction / 2 ** 32 * 1000;
}

// これより先のイベントメッセージは時刻がおかしいとみなしてすぐに発火する
const maxEventLeadMillis = 24 * 60 * 60 * 1000;

// PTSとPCRは33ビットで一周する (ミリ秒)
const clockPeriodMillis = 2 ** 33 / 90;
// これより先の字幕は時刻がおかしいとみなしてすぐに渡す
const maxCaptionLeadMillis = 60 * 1000;

function wrapMillis(diff: number): number {
    diff %= clockPeriodMillis;
    if (diff > clockPeriodMillis / 2) {
        diff -= clockPeriodMillis;
    } else if (diff <= -clockPeriodMillis / 2) {
        diff += clockPeriodMillis;
    }
    return diff;
}

// TTMLの時刻 ("00:00:01.500"、"1.5s"、"1500ms" など) をミリ秒に
function parseTtmlTime(value: string): number | null {
    const clock = /^(\d+):(\d{2}):(\d{2}(?:\.\d+)?)$/.exec(value);
    if (clock) {
        return (Number(clock[1]) * 3600 + Number(clock[2]) * 60 + Number(clock[3])) * 1000;
    }
    const offset = /^(\d+(?:\.\d+)?)(h|m|s|ms)$/.exec(value);
    if (offset) {
        const unit = { h: 3600000, m: 60000, s: 1000, ms: 1 }[offset[2] as "h" | "m" | "s" | "ms"];
        return Number(offset[1]) * unit;
    }
    return null;
}

// TTML文書で最初に表示される時刻 (ミリ秒)
function firstBeginMillis(ttml: string): number {
    let first: number | null = null;
    try {
        const doc = new DOMParser().parseFromString(ttml, "application/xml");
        for (const element of Array.from(doc.querySelectorAll("[begin]"))) {
            const begin = parseTtmlTime(element.getAttribute("begin") ?? "");
            if (begin != null && (first == null || begin < first)) {
                first = begin;
            }
        }
    } catch (e) {
    }
    return first ?? 0;
}

// 重なり合う矩形を、同じ範囲を覆う重ならない矩形に分ける (座標圧縮して行ごとにまとめる)
function disjointRects(rects: VideoRect[]): { left: number, top: number, right: number, bottom: number }[] {
    const boxes = rects.map(r => ({ left: r.left, top: r.top, right: r.left + r.width, bottom: r.top + r.height }))
        .filter(b => b.right > b.left && b.bottom > b.top);
    if (boxes.length === 0) {
        return [];
    }
    const xs = [...new Set(boxes.flatMap(b => [b.left, b.right]))].sort((a, b) => a - b);
    const ys = [...new Set(boxes.flatMap(b => [b.top, b.bottom]))].sort((a, b) => a - b);
    const result: { left: number, top: number, right: number, bottom: number }[] = [];
    for (let j = 0; j + 1 < ys.length; j++) {
        let start: number | null = null;
        for (let i = 0; i < xs.length; i++) {
            const covered = i + 1 < xs.length && boxes.some(b => b.left <= xs[i] && xs[i + 1] <= b.right && b.top <= ys[j] && ys[j + 1] <= b.bottom);
            if (covered && start == null) {
                start = xs[i];
            } else if (!covered && start != null) {
                result.push({ left: start, top: ys[j], right: xs[i], bottom: ys[j + 1] });
                start = null;
            }
        }
    }
    return result;
}

function toBinary4(value: number): string {
    return (value & 0xf).toString(2).padStart(4, "0");
}

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
    private videoOverlays: VideoRect[] = [];
    private captionComponents: number[] = [];
    private currentTimeMillis: number | null = null;
    private captionQueue: { due: number, caption: B62Caption }[] = [];
    // 放送の時刻 (TOT) と、それを受け取ったときのPCR
    private broadcastTime: { unixMillis: number, pcrMillis: number | null } | null = null;
    private eventQueue: { due: number, message: B62EventMessage }[] = [];
    private readonly onVideoChanged: () => void;
    private readonly playRomSound: (id: number) => void;

    // onVideoChangedは映像を出す位置が変わったときに呼ばれる。
    // playRomSoundは受信機内蔵音 (romsound://) を鳴らす
    constructor(container: HTMLElement, onVideoChanged: () => void, playRomSound: (id: number) => void) {
        this.container = container;
        this.onVideoChanged = onVideoChanged;
        this.playRomSound = playRomSound;
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

    // サービスの字幕・文字スーパーのcomponent_tag (PMTから)
    setCaptionComponents(tags: number[]) {
        if (tags.length === this.captionComponents.length && tags.every((tag, i) => tag === this.captionComponents[i])) {
            return;
        }
        this.captionComponents = tags;
        this.postCaptionComponents();
    }

    private postCaptionComponents() {
        this.frame?.contentWindow?.postMessage({ source: "b62Host", type: "captionComponents", tags: this.captionComponents }, appOrigin);
    }

    // 字幕を提示時刻まで待たせてからアプリケーションに渡す
    pushCaption(caption: B62Caption) {
        if (this.frame == null) {
            return;
        }
        if (caption.pts == null || this.currentTimeMillis == null) {
            this.deliverCaption(caption);
            return;
        }
        const begin = caption.dataType === 0 ? firstBeginMillis(caption.data) : 0;
        const due = caption.pts / 90 + begin + caption.delay;
        this.captionQueue.push({ due, caption });
        this.flushCaptions();
    }

    // 映像の時刻 (PCR、ミリ秒)
    updateTime(pcrMillis: number) {
        this.currentTimeMillis = pcrMillis;
        this.flushCaptions();
        this.flushEventMessages();
    }

    // TOTの時刻
    setBroadcastTime(unixMillis: number) {
        this.broadcastTime = { unixMillis, pcrMillis: this.currentTimeMillis };
        this.flushEventMessages();
    }

    // 今の放送の時刻 (TOTにその後進んだPCRを足したもの)
    private broadcastNow(): number | null {
        const base = this.broadcastTime;
        if (base == null) {
            return null;
        }
        if (base.pcrMillis == null || this.currentTimeMillis == null) {
            return base.unixMillis;
        }
        return base.unixMillis + wrapMillis(this.currentTimeMillis - base.pcrMillis);
    }

    // イベントメッセージを発火時刻まで待たせてからアプリケーションに渡す
    pushEventMessage(message: B62EventMessage) {
        if (this.frame == null) {
            return;
        }
        let due: number | null = null;
        if (message.timeMode === 1 || message.timeMode === 5) {
            due = ntpToUnixMillis(message.timeSeconds, message.timeFraction);
        } else if (message.timeMode === 3) {
            const start = this.programInfo?.startTimeUnixMillis;
            if (start != null) {
                due = start + (message.timeSeconds + message.timeFraction / 2 ** 32) * 1000;
            }
        } else if (message.timeMode !== 0) {
            // NPTはUTC-NPT参照記述子が必要なので未対応
            console.warn("[TVTDataBroadcastingWV2] unsupported event message time_mode", message.timeMode);
            return;
        }
        if (due == null || this.broadcastNow() == null) {
            this.deliverEventMessage(message);
            return;
        }
        this.eventQueue.push({ due, message });
        this.flushEventMessages();
    }

    private flushEventMessages() {
        const now = this.broadcastNow();
        if (now == null || this.eventQueue.length === 0) {
            return;
        }
        const ready = this.eventQueue.filter(entry => entry.due <= now || entry.due - now > maxEventLeadMillis);
        if (ready.length === 0) {
            return;
        }
        this.eventQueue = this.eventQueue.filter(entry => !ready.includes(entry));
        ready.sort((a, b) => a.due - b.due);
        for (const entry of ready) {
            this.deliverEventMessage(entry.message);
        }
    }

    // ARIB STD-B62のGeneralEventMessageの形にする。message_idはevent_msg_idの上位8ビット、
    // message_versionは下位8ビット
    private deliverEventMessage(message: B62EventMessage) {
        this.frame?.contentWindow?.postMessage({
            source: "b62Host",
            type: "eventMessage",
            message: {
                source: { event_message_tag: message.emtTag },
                message_group_id: message.messageGroupId,
                message_id: message.messageId >> 8,
                message_version: message.messageId & 0xff,
                private_data_byte: message.privateData,
            },
        }, appOrigin);
    }

    private flushCaptions() {
        const now = this.currentTimeMillis;
        if (now == null || this.captionQueue.length === 0) {
            return;
        }
        const ready = this.captionQueue.filter(entry => {
            const lead = wrapMillis(entry.due - now);
            return lead <= 0 || lead > maxCaptionLeadMillis;
        });
        if (ready.length === 0) {
            return;
        }
        this.captionQueue = this.captionQueue.filter(entry => !ready.includes(entry));
        ready.sort((a, b) => wrapMillis(a.due - b.due));
        for (const entry of ready) {
            this.deliverCaption(entry.caption);
        }
    }

    // ARIB STD-B62のCaptionListenerに渡す形 (運用規定。NHK BSP4Kの字幕ページが読む形に合わせる)
    private deliverCaption(caption: B62Caption) {
        const payload = JSON.stringify({
            data_type: toBinary4(caption.dataType),
            TMD: toBinary4(caption.tmd),
            data: caption.data,
        });
        this.frame?.contentWindow?.postMessage({ source: "b62Host", type: "caption", componentTag: caption.componentTag, payload }, appOrigin);
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
        // evenodd: 全体、映像の穴、穴の中で手前に重なる要素 (重ならないように分割したもの) の順に重ねる。
        // 各矩形は原点から行って戻るので、つなぎの線は面積を持たない
        const rects = [{ left: 0, top: 0, right: appWidth, bottom: appHeight }, { left, top, right, bottom }, ...disjointRects(this.videoOverlays)];
        const points = rects.flatMap(q => [
            `${q.left}px ${q.top}px`, `${q.right}px ${q.top}px`, `${q.right}px ${q.bottom}px`, `${q.left}px ${q.bottom}px`, `${q.left}px ${q.top}px`, "0 0",
        ]);
        this.frame.style.clipPath = `polygon(evenodd, ${points.join(", ")})`;
    }

    private close() {
        this.captionQueue = [];
        this.eventQueue = [];
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
            // ARIB STD-B62のgetCurrentEventInformationの形にする (分かる項目のみ)。
            // アプリケーションはstart_timeなどをDateとして扱う (postMessageはDateをそのまま渡せる)。
            // 次の番組 (f_*) は分からないので、開始時刻だけ今の番組の終了時刻で埋める
            const info = this.programInfo;
            const start = info?.startTimeUnixMillis != null ? new Date(info.startTimeUnixMillis) : null;
            const end = start != null && info?.durationSeconds != null ? new Date(start.getTime() + info.durationSeconds * 1000) : null;
            const data = {
                service_id: info?.serviceId ?? null,
                original_network_id: info?.originalNetworkId ?? null,
                transport_stream_id: info?.transportStreamId ?? null,
                event_id: info?.eventId ?? null,
                event_name: info?.eventName ?? null,
                start_time: start,
                duration: info?.durationSeconds ?? null,
                f_event_id: null,
                f_start_time: end ?? start,
            };
            (ev.source as Window).postMessage({ source: "b62Host", id: message.id, data }, appOrigin);
        } else if (message.type === "romsound") {
            if (Number.isInteger(message.id)) {
                this.playRomSound(message.id);
            }
        } else if (message.type === "hello") {
            this.postCaptionComponents();
        } else if (message.type === "videoRect") {
            this.videoRect = message.rect;
            this.videoOverlays = message.above ?? [];
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
