import { HybridcastHost, type HybridcastIncoming, type HybridcastOutgoing } from "./hybridcast";
import { CommentData, CommentRenderer } from "./comment";
import { ChannelInfo, ChannelsClient } from "./channels";
import type { ComponentPMT, ResponseMessage } from "web-bml/protocol";
import { BMLBrowser, BMLBrowserFontFace, EPG, Indicator, IP, InputApplication, InputCancelReason, InputCharacterType, playRomSound } from "web-bml";
import { decodeTS } from "web-bml/ts";
import { CaptionPlayer, type NativeCaptionImage } from "./caption_player";
import { TtmlCaptionPlayer, type TtmlCaptionConfig, type TtmlCaptionMessage } from "./ttml_caption";
import { B62ApplicationFrame, type B62Caption, type B62EventMessage, type B62FollowingEvent, type B62Residence, type B62NptReference } from "./b62_application";

declare global {
    interface Window {
        chrome: {
            webview: {
                postMessage(msg: any): any;
                addEventListener(type: string, listener: EventListenerOrEventListenerObject): void;
            }
        };
        sendMessage?(data: ToWebViewMessage): FromWebViewMessage | null;
        bmlBrowser?: BMLBrowser;
    }
}
function postMessage(message: FromWebViewMessage): void {
    window.chrome.webview.postMessage(message);
}

export class StatusBarIndicator implements Indicator {
    private readonly receivingStatusElement: HTMLElement;
    private readonly networkingStatusElement: HTMLElement;
    constructor(receivingStatusElement: HTMLElement, networkingStatusElement: HTMLElement) {
        this.receivingStatusElement = receivingStatusElement;
        this.networkingStatusElement = networkingStatusElement;
    }
    url = "";
    receiving = false;
    // 4Kデータ放送のアプリケーションのファイルを受信中
    b62Receiving = false;
    eventName: string | null = "";
    loading = false;
    networkingPost = false;
    // 4Kデータ放送のアプリケーションが通信中
    b62Networking = false;
    private update() {
        postMessage({
            type: "status",
            loading: this.loading,
            url: this.url,
            receiving: this.receiving,
        });
        if (this.receiving || this.b62Receiving) {
            this.receivingStatusElement.style.display = "";
        } else {
            this.receivingStatusElement.style.display = "none";
        }
        if (this.networkingPost || this.b62Networking) {
            this.networkingStatusElement.style.display = "";
        } else {
            this.networkingStatusElement.style.display = "none";
        }
    }
    public setUrl(name: string, loading: boolean): void {
        if (loading) hybridcast.ait.invalidateDocument();
        this.url = name;
        this.loading = loading;
        this.update();
    }
    public setReceivingStatus(receiving: boolean): void {
        this.receiving = receiving;
        this.update();
    }
    public setB62NetworkingStatus(networking: boolean): void {
        this.b62Networking = networking;
        this.update();
    }
    public setB62ReceivingStatus(receiving: boolean): void {
        this.b62Receiving = receiving;
        this.update();
    }
    // 「データ取得中」を表示しているか
    public isReceiving(): boolean {
        return this.receiving || this.b62Receiving;
    }
    public setNetworkingGetStatus(get: boolean): void {

    }
    public setNetworkingPostStatus(post: boolean): void {
        this.networkingPost = post;
        this.update();
    }
    public setEventName(eventName: string | null): void {
        this.eventName = eventName;
        this.update();
    }
}

const params = new URLSearchParams(location.search);
const networkId = Number(params.get("networkId")) ?? undefined;
const serviceId = Number(params.get("serviceId")) ?? undefined;

// BML文書と動画と字幕が入る要素
const browserElement = document.getElementById("data-broadcasting-browser")!;
// 4Kデータ放送 (ARIB STD-B62) のアプリケーション
const b62Application = new B62ApplicationFrame(browserElement, () => onResized(), (id) => playRomSound(id, gainNode), () => postMessage({ type: "b62GetFollowingEvent" }), () => getB62Residence(), (keySet) => onB62KeySetChanged(keySet));
// 動画が入っている要素
const videoContainer = browserElement.querySelector(".arib-video-container") as HTMLElement;
// BML文書が入る要素
const contentElement = browserElement.querySelector(".data-broadcasting-browser-content") as HTMLElement;
// BML用フォント
const roundGothic: BMLBrowserFontFace = { source: "url('../dist/KosugiMaru-Regular.woff2'), local('MS Gothic')" };
const boldRoundGothic: BMLBrowserFontFace = { source: "url('../dist/KosugiMaru-Bold.woff2'), local('MS Gothic')" };
const squareGothic: BMLBrowserFontFace = { source: "url('../dist/Kosugi-Regular.woff2'), local('MS Gothic')" };
const ccContainer = browserElement.querySelector(".arib-video-cc-container") as HTMLElement;
const player = new CaptionPlayer(document.createElement("video"), ccContainer, postMessage);
// 4K/8K放送の字幕をTTMLのまま描く。描き始めたらB24に変換された同じ字幕は表示しない
const ttmlPlayer = new TtmlCaptionPlayer(ccContainer);
ttmlPlayer.onActivated = (track) => player.suppress(track);
ttmlPlayer.onDeactivated = (track) => player.unsuppress(track);
player.onSuppressedStatement = (track) => ttmlPlayer.noteB24Statement(track);
// リモコン
const remoteControl = new StatusBarIndicator(browserElement.querySelector(".remote-control-receiving-status")!, browserElement.querySelector(".remote-control-networking-status")!);

const remoteControlStatusContainer = document.querySelector(".remote-control-status-container") as HTMLElement;
remoteControlStatusContainer.style.visibility = "hidden";

const ccStatus = document.getElementById("cc-status")!;

const ccStatusAnimation = ccStatus.animate([{ visibility: "hidden", offset: 1 }], {
    duration: 2000,
    fill: "forwards",
});

const epg: EPG = {
    tune(originalNetworkId, transportStreamId, serviceId) {
        console.error("tune", originalNetworkId, transportStreamId, serviceId);
        postMessage({
            type: "tune",
            originalNetworkId,
            transportStreamId,
            serviceId,
        });
        return true;
    }
};

let networkEnabled = false;
let isRecord = false;
let broadcastTimeKnown = false;

const apiIP: IP = {
    getConnectionType() {
        return 403;
    },
    isIPConnected() {
        return (networkEnabled && !isRecord) ? 1 : 0;
    },
    async transmitTextDataOverIP(uri, body) {
        if (!(networkEnabled && !isRecord)) {
            return { resultCode: NaN, statusCode: "", response: new Uint8Array() };
        }
        try {
            const res = await window.fetch("https://tvtdatabroadcastingwv2-api.invalid/api/post/" + uri, {
                method: "POST",
                headers: {
                    "Content-Type": "application/x-www-form-urlencoded"
                },
                body,
            });
            return { resultCode: 1, statusCode: res.status.toString(), response: new Uint8Array(await res.arrayBuffer()) };
        } catch {
            return { resultCode: NaN, statusCode: "", response: new Uint8Array() };
        }
    },
    async get(uri) {
        if (!(networkEnabled && !isRecord)) {
            return {};
        }
        try {
            const res = await window.fetch("https://tvtdatabroadcastingwv2-api.invalid/api/get/" + uri, {
                method: "GET",
            });
            return { statusCode: res.status, headers: res.headers, response: new Uint8Array(await res.arrayBuffer()) };
        } catch {
            return {};
        }
    },
};

const audioContext = new AudioContext();
const gainNode = audioContext.createGain();
gainNode.connect(audioContext.destination);

player.setPRAAudioNode(gainNode);

let changeCallback: ((value: string) => void) | undefined;

const inputApplication: InputApplication = {
    launch(opts) {
        if (changeCallback != null) {
            this.cancel("other");
        }
        postMessage({
            type: "input",
            characterType: opts.characterType,
            maxLength: opts.maxLength,
            value: opts.value,
            allowedCharacters: opts.allowedCharacters,
            inputMode: opts.inputMode,
            multiline: opts.multiline,
        });
        changeCallback = opts.callback;
    },
    cancel(reason) {
        if (changeCallback != null) {
            changeCallback = undefined;
            postMessage({
                type: "cancelInput",
                reason,
            });
        }
    },
};

function X_DPA_startResidentApp(appName: string, showAV: number, returnURI: string, Ex_info: string[]): number {
    if (appName === "ComBrowser") {
        const uri = Ex_info[0];
        const mode = Ex_info[1];
        const fullscreen = Ex_info[2] === "1";
        if (!uri.startsWith("http://") && !uri.startsWith("https://")) {
            return NaN;
        }
        if (mode === "0") {
            // 0: 本規定に準じたコンテンツを表示可能なブラウザ
            // 非リンクコンテンツへと遷移 (TR-B14 第三分冊 図8-9)
            return NaN;
        }
        // 1: 通信事業者仕様ブラウザ
        // 2: HTMLブラウザ
        // fullscreenが1ならば原則としてデータ放送ブラウザの表示を終了する
        postMessage({
            type: "startBrowser",
            uri,
            fullscreen,
        });
        // データ放送ブラウザは，当該拡張関数を実行後，引き続きスクリプトの実行を継続することが望ましい
        // TR-B14 第三分冊 7.10.8
        return 1;
    } else if (appName === "BookmarkList") {
        return NaN;
    }
    return NaN;
}

let b62Active = false;
const hybridcast = new HybridcastHost({
    available: () => networkEnabled && broadcastTimeKnown && !isRecord && !b62Active && !cProfile,
    post: postMessage,
    async fetchXMLAIT(uri) {
        const response = await apiIP.get!(uri);
        return response.statusCode === 200 && response.response ? new TextDecoder().decode(response.response) : null;
    },
});

const bmlBrowser = new BMLBrowser({
    extension: hybridcast.extension,
    containerElement: contentElement,
    mediaElement: videoContainer,
    indicator: remoteControl,
    fonts: {
        roundGothic,
        boldRoundGothic,
        squareGothic
    },
    epg,
    videoPlaneModeEnabled: true,
    audioNodeProvider: {
        getAudioDestinationNode() {
            return gainNode;
        }
    },
    ip: apiIP,
    inputApplication,
    greg: {
        getReg(index) {
            return window.sessionStorage.getItem(`Greg[${index}]`) ?? "";
        },
        setReg(index, value) {
            window.sessionStorage.setItem(`Greg[${index}]`, value);
        },
    },
    setMainAudioStreamCallback(componentId, channelId) {
        const index = audioESList.findIndex(x => x.componentId === componentId);
        const es = audioESList[index];
        if (es == null) {
            return false;
        }
        postMessage({
            type: "changeMainAudioStream",
            componentId,
            index,
            pid: es.pid,
            channelId,
        });
        return true;
    },
    X_DPA_startResidentApp,
});

function isFullScreenVideo(): boolean {
    return videoContainer.clientWidth === browserElement.clientWidth && videoContainer.clientHeight === browserElement.clientHeight;
}

// trueであればデータ放送の上に動画を表示させる非表示状態
bmlBrowser.addEventListener("invisible", (evt) => {
    console.log("invisible", evt.detail);
    if (evt.detail) {
        contentElement.style.clipPath = "inset(0px)";
    } else {
        contentElement.style.clipPath = "";
    }
    postMessage({
        type: "invisible",
        // 4Kデータ放送のアプリケーションが映像の位置を決めている間はそちらを優先する
        invisible: b62Application.videoRectInViewport() != null ? false : evt.detail || isFullScreenVideo(),
    });
});

bmlBrowser.addEventListener("load", (evt) => {
    console.log("load", evt.detail);
    const width = evt.detail.resolution.width + "px";
    const aspectNum = evt.detail.displayAspectRatio.numerator;
    const aspectDen = evt.detail.displayAspectRatio.denominator;
    const scaleY = (evt.detail.resolution.width / evt.detail.resolution.height) / (aspectNum / aspectDen);
    const transform = `scaleY(${scaleY})`;
    const height = (evt.detail.resolution.height * scaleY) + "px";
    if (browserElement.style.width !== width || browserElement.style.height !== height || contentElement.style.transform !== transform) {
        browserElement.style.width = width;
        browserElement.style.height = height;
        contentElement.style.transform = transform;
        ccContainer.style.width = width;
        ccContainer.style.height = height;
        onResized();
    }
});

type FromWebViewMessage = HybridcastOutgoing | {
    type: "videoChanged",
    left: number,
    top: number,
    right: number,
    bottom: number,
    invisible: boolean,
} | {
    type: "status",
    url: string,
    receiving: boolean,
    loading: boolean,
} | {
    type: "invisible",
    invisible: boolean,
} | {
    // 次の番組の情報を求める (b62FollowingEventで返る)
    type: "b62GetFollowingEvent",
} | {
    type: "tune",
    originalNetworkId: number,
    transportStreamId: number,
    serviceId: number,
} | {
    type: "nvramRead",
    filename: string,
    structure: string,
    data: any[] | null,
} | {
    type: "usedKeyList",
    usedKeyList: { [key: string]: boolean },
} | {
    type: "cancelInput",
    reason: InputCancelReason,
} | {
    type: "input",
    characterType: InputCharacterType,
    allowedCharacters?: string,
    maxLength: number,
    value: string,
    inputMode: "text" | "password",
    multiline: boolean,
} | {
    type: "changeAudioStream",
    componentId: number,
    pid: number,
    // -1
    index: number,
    channelId?: number,
} | {
    type: "changeMainAudioStream",
    componentId: number,
    pid: number,
    index: number,
    channelId?: number,
} | {
    type: "serviceInfo",
    networkId: number,
    serviceId: number,
    cProfile: boolean,
} | {
    type: "startBrowser",
    uri: string,
    fullscreen: boolean,
} | {
    type: "channelsUpdate",
    channels: ChannelInfo[],
} | {
    type: "captionReset",
} | {
    type: "captionPes",
    streamId: number,
    data: string,
    pts?: number,
} | {
    type: "captionTime",
    time: number,
    width: number,
    height: number,
};

bmlBrowser.addEventListener("videochanged", (evt) => {
    const { left, top, right, bottom } = evt.detail.boundingRect;
    postMessage({
        type: "videoChanged",
        left: left * window.devicePixelRatio,
        top: top * window.devicePixelRatio,
        right: right * window.devicePixelRatio,
        bottom: bottom * window.devicePixelRatio,
        invisible: (bmlBrowser.content.invisible ?? true) || isFullScreenVideo(),
    });
});

// BML文書が使うキーと、4Kデータ放送のアプリケーションのkeySet (アプリケーションが無ければnull)
let bmlUsedKeyList: { [key: string]: boolean } | null = null;
let b62KeySet: number | null = null;

// ホストにキーを横取りさせる範囲を伝える。4Kデータ放送のアプリケーションがあればそのkeySetに従う
function postUsedKeyList() {
    if (b62KeySet == null) {
        if (bmlUsedKeyList != null) {
            postMessage({ type: "usedKeyList", usedKeyList: bmlUsedKeyList });
        }
        return;
    }
    // 方向・決定・戻る・色・dボタンは今までどおり渡し、数字キーはNUMERIC (0x100) を設定したときだけ渡す
    const usedKeyList: { [key: string]: boolean } = { "basic": true, "data-button": true };
    if (b62KeySet & 0x100) {
        usedKeyList["numeric-tuning"] = true;
    }
    postMessage({ type: "usedKeyList", usedKeyList });
}

function onB62KeySetChanged(keySet: number | null) {
    b62KeySet = keySet;
    postUsedKeyList();
}

bmlBrowser.addEventListener("usedkeylistchanged", (evt) => {
    const { usedKeyList } = evt.detail;
    bmlUsedKeyList = Object.fromEntries([...usedKeyList.values()].map(x => [x, true]));
    postUsedKeyList();
});

bmlBrowser.addEventListener("audiostreamchanged", (evt) => {
    const { componentId, channelId } = evt.detail;
    const index = audioESList.findIndex(x => x.componentId === componentId);
    postMessage({
        type: "changeAudioStream",
        componentId,
        index,
        pid: audioESList[index]?.pid,
        channelId,
    });
});

let pcr: number | undefined;

// LibISDB/Filters/AnalyzerFilter.cppを参照
const audioStreamType = new Set([
    0x03,
    0x04,
    0x0f,
    0x11,
    0x81,
    0x82,
    0x83,
    0x87,
]);

let audioESList: ComponentPMT[] = [];

let pmtRetrieved = false;
// ワンセグのデータ放送の場合dボタンを押すまで起動しない状態にしておく
let cProfile = false;
let oneSegLaunched = false;
let oneSegCaptionOverlay = false;

function onMessage(msg: ResponseMessage) {
    if (msg.type === "pes") {
        player.push(msg.streamId, Uint8Array.from(msg.data), msg.pts);
    } else if (msg.type === "pcr") {
        pcr = (msg.pcrBase + msg.pcrExtension / 300) / 90;
    } else if (msg.type === "pmt") {
        audioESList = msg.components.filter(x => audioStreamType.has(x.streamType)).sort((a, b) => a.componentId - b.componentId);
        // 字幕 (0x30-0x37) と文字スーパー (0x38-0x3F)
        b62Application.setCaptionComponents(msg.components.map(x => x.componentId).filter(id => id >= 0x30 && id <= 0x3f).sort((a, b) => a - b));
        if (!pmtRetrieved) {
            // 地上デジタル放送向けマルチメディア符号化方式(Cプロファイル) 0x000d
            cProfile = msg.components.some(x => x.dataComponentId === 0x000d);
            if (cProfile) {
                // loadが呼ばれる前はまだ960pxが設定されていてデータ取得中の表示が小さくなってしまうので変えておく
                browserElement.style.width = "240px";
                browserElement.style.height = "480px";
                onResized();
                if (!oneSegLaunched) {
                    browserElement.style.visibility = "hidden";
                    // BMLの子要素はvisibility:visibleを明示できるため、親の
                    // visibilityだけでは隠れない。字幕受信中も文書全体を透明にする。
                    browserElement.style.opacity = "0";
                }
                remoteControlStatusContainer.style.visibility = "visible";
            }
            postMessage({
                type: "serviceInfo",
                networkId,
                serviceId,
                cProfile,
            });
        }
        pmtRetrieved = true;
    } else if (msg.type === "currentTime") {
        broadcastTimeKnown = true;
        isRecord = new Date().getTime() - msg.timeUnixMillis >= 5 * 60 * 1000;
        b62Application.setBroadcastTime(msg.timeUnixMillis);
    } else if (msg.type === "programInfo") {
        b62Application.setProgramInfo(msg);
    }
    hybridcast.emit(msg);
    bmlBrowser.emitMessage(msg);
}

const tsStream = decodeTS({
    sendCallback: onMessage,
    serviceId,
    parsePES: true,
});

type ToWebViewMessage = HybridcastIncoming | {
    type: "oneSegCaptionConfig",
    overlay: boolean,
} | {
    type: "stream",
    data: number[],
    time?: number,
} | {
    type: "streamBase64",
    data: string,
    time?: number,
} | {
    type: "key",
    keyCode: number,
} | {
    type: "caption",
    enable: boolean,
    showIndicator: boolean,
} | {
    type: "captionImage",
    track: number,
    frameWidth?: number,
    frameHeight?: number,
    images: NativeCaptionImage[],
} | {
    type: "captionSound",
    sound: number,
} | {
    type: "nvramRead",
    filename: string,
    structure: string,
} | {
    type: "nvramWrite",
    filename: string,
    structure: string,
    data: any[],
} | {
    type: "volume",
    value: number,
} | {
    type: "nvramDelete",
} | {
    type: "enableNetwork",
    enable: boolean,
} | {
    type: "changeInput",
    value: string,
} | {
    type: "cancelInput",
} | {
    type: "mainAudioStreamChanged",
    pid?: number,
    index: number,
    componentId?: number,
    channelId: number,
} | {
    type: "launchOneSeg",
} | {
    type: "comments",
    comments: CommentData[],
} | {
    type: "clearComments",
} | {
    type: "commentConfig",
    opacity: number,
    duration_ms?: number,
    shadow_color?: string,
    shadow_enabled?: boolean,
    outline_enabled?: boolean,
    font_size_medium?: number,
} | {
    type: "channelsConfig",
    ws_uri: string,
} | ({
    type: "b62Caption",
} & B62Caption) | ({
    type: "ttmlCaption",
} & TtmlCaptionMessage) | {
    // 字幕のリソースのSVGフォント (外字)
    type: "ttmlCaptionFont",
    componentTag: number,
    data: string,
} | ({
    type: "ttmlCaptionConfig",
} & TtmlCaptionConfig) | ({
    type: "b62EventMessage",
} & B62EventMessage) | {
    // 4Kデータ放送 (ARIB STD-B62) のアプリケーション。urlがnullなら終了
    type: "b62Application",
    url: string | null,
    organizationId?: number,
    applicationId?: number,
    version?: number,
    // アプリケーションのファイルを受信中
    loading?: boolean,
} | {
    // 4Kデータ放送のアプリケーションが通信中か
    type: "b62Networking",
    active: boolean,
} | {
    // 4Kデータ放送の受信済みのファイルとその版
    type: "b62Files",
    files: [string, string][],
} | ({
    type: "b62NptReference",
} & B62NptReference) | ({
    type: "b62FollowingEvent",
} & B62FollowingEvent);

let b62NetworkingTimeout: number | null = null;

// 4Kデータ放送のアプリケーションに渡す視聴者の居住地域 (ARIB TR-B39 10.1.5)。
// BMLの受信機設定 (nvram://receiverinfo/) と同じものを使う
function getB62Residence(): B62Residence {
    const read = (filename: string, structure: string): unknown => {
        try {
            return bmlBrowser.nvram.readPersistentArray(filename, structure)?.[0];
        } catch {
            return undefined;
        }
    };
    const toHex = (bits: string) => bits.match(/.{4}/g)!.map(x => parseInt(x, 2).toString(16)).join("");
    // 県域指定ビットマップ (56ビット) の左からprefecture番目のビット
    const prefecture = read("nvram://receiverinfo/prefecture", "U:1B");
    const prefectureCode = typeof prefecture === "number" && prefecture >= 1 && prefecture <= 56
        ? toHex("0".repeat(prefecture - 1) + "1" + "0".repeat(56 - prefecture)) : "";
    // 緊急警報信号の地域符号 (12ビット)
    const region = read("nvram://receiverinfo/regioncode", "U:2B");
    const regionCode = typeof region === "number" && region > 0 && region < 0x1000 ? toHex(region.toString(2).padStart(12, "0")) : "";
    const zip = read("nvram://receiverinfo/zipcode", "S:7B");
    const zipCode = typeof zip === "string" ? zip.replace(/[^0-9]/g, "").slice(0, 7) : "";
    return { _prefecture: prefectureCode, _regioncode: regionCode, _zipcode: zipCode };
}

// 1分間無操作であればデータ取得中の表示を消す
let remoteControlStatusTimeoutMillis = 60 * 1000;
let remoteControlStatusTimeout = Number.MAX_VALUE;

function fromBase64(input: string): Uint8Array<ArrayBuffer> {
    if ("fromBase64" in globalThis.Uint8Array) {
        return Uint8Array.fromBase64(input);
    } else {
        return Uint8Array.from(window.atob(input), c => c.charCodeAt(0));
    }
}

function onWebViewMessage(data: ToWebViewMessage, reply: (data: FromWebViewMessage) => void) {
    if (!cProfile && data.type !== "key" && performance.now() >= remoteControlStatusTimeout) {
        remoteControlStatusContainer.style.visibility = "hidden";
        remoteControlStatusTimeout = Number.MAX_VALUE;
    }
    if (data.type === "html5Config") {
        hybridcast.configure(data.enabled, data.autoStartBroadcastAIT, data.residentAppVersion);
    } else if (data.type === "html5Stopped") {
        hybridcast.stopped();
    } else if (data.type === "bmlVisibility") {
        // Opacity cannot be overridden by BML descendants with visibility:visible.
        browserElement.style.opacity = data.visible ? (cProfile && !oneSegLaunched ? "0" : "") : "0";
    } else if (data.type === "html5Replace") {
        const result = hybridcast.ait.startAITControlledApp(data.organizationId.toString(16), data.applicationId.toString(16), data.aitUrl ?? undefined, null);
        if (Number.isNaN(result)) postMessage({ type: "killHtml5", reason: "replace" });
    } else if (data.type === "rpc") {
        let result: any = null;
        const p = data.params;
        switch (data.method) {
            case "readPersistentArray": result = bmlBrowser.nvram.readPersistentArray(p.filename, p.structure); break;
            case "writePersistentArray": result = bmlBrowser.nvram.writePersistentArray(p.filename, p.structure, p.data, undefined, true); break;
            case "getGreg": result = Array.from({ length: 64 }, (_, i) => sessionStorage.getItem(`Greg[${i}]`) ?? ""); break;
            case "setGreg": if (Number.isInteger(p.index) && p.index >= 0 && p.index < 64) { sessionStorage.setItem(`Greg[${p.index}]`, String(p.value)); result = true; } break;
            case "getCurrentEvent": result = hybridcast.lastEIT; break;
            case "getBroadcastAIT": result = hybridcast.ait.getBroadcastAIT(); break;
            case "isRecord": result = isRecord; break;
        }
        reply({ type: "rpcResult", result });
    } else if (data.type === "stream") {
        if (!oneSegLaunched && cProfile && !oneSegCaptionOverlay) {
            return;
        }
        const ts = data.data;
        const prevPCR = pcr;
        tsStream.push(Uint8Array.from(ts));
        const curPCR = pcr;
        if (prevPCR !== curPCR && curPCR != null) {
            // 表示遅延はホスト側の設定(DelayTime)で調整する
            player.updateTime(curPCR);
            ttmlPlayer.updateTime(curPCR);
            b62Application.updateTime(curPCR);
        }
    } else if (data.type === "streamBase64") {
        if (!oneSegLaunched && cProfile && !oneSegCaptionOverlay) {
            return;
        }
        const ts = data.data;
        const prevPCR = pcr;
        tsStream.push(fromBase64(ts));
        const curPCR = pcr;
        if (prevPCR !== curPCR && curPCR != null) {
            // 表示遅延はホスト側の設定(DelayTime)で調整する
            player.updateTime(curPCR);
            ttmlPlayer.updateTime(curPCR);
            b62Application.updateTime(curPCR);
        }
    } else if (data.type === "key") {
        remoteControlStatusContainer.style.visibility = "visible";
        // 「データ取得中」の間はdボタンを渡さない。表示が終わる前にアプリケーションや
        // BMLが受け取ると、表示しようとしているデータ放送を閉じてしまう。
        if (data.keyCode === 20 && remoteControl.isReceiving()) {
            remoteControlStatusTimeout = performance.now() + remoteControlStatusTimeoutMillis;
            return;
        }
        if (!b62Application.processKey(data.keyCode)) {
            bmlBrowser.content.processKeyDown(data.keyCode);
            bmlBrowser.content.processKeyUp(data.keyCode);
        }
        remoteControlStatusTimeout = performance.now() + remoteControlStatusTimeoutMillis;
    } else if (data.type === "captionImage") {
        player.draw(data.track, data.frameWidth, data.frameHeight, data.images);
    } else if (data.type === "captionSound") {
        player.playBuiltinSound(data.sound);
    } else if (data.type === "caption") {
        ttmlPlayer.setCaptionEnabled(data.enable);
        if (data.enable) {
            if (data.showIndicator) {
                ccStatus.style.display = "";
                ccStatus.textContent = "字幕オン";
                ccStatusAnimation.cancel();
                ccStatusAnimation.play();
            }
            player.showCC();
        } else {
            if (data.showIndicator) {
                ccStatus.style.display = "";
                ccStatus.textContent = "字幕オフ";
                ccStatusAnimation.cancel();
                ccStatusAnimation.play();
            }
            player.hideCC();
        }
    } else if (data.type === "nvramRead") {
        reply({
            type: "nvramRead",
            filename: data.filename,
            structure: data.structure,
            data: bmlBrowser.nvram.readPersistentArray(data.filename, data.structure),
        });
    } else if (data.type === "nvramWrite") {
        bmlBrowser.nvram.writePersistentArray(data.filename, data.structure, data.data, undefined, true);
    } else if (data.type === "volume") {
        gainNode.gain.value = data.value;
    } else if (data.type === "nvramDelete") {
        const keys: string[] = [];
        for (let i = 0; i < window.localStorage.length; i++) {
            const key = window.localStorage.key(i);
            if (key != null && key.startsWith("nvram_")) {
                keys.push(key);
            }
        }
        for (const key of keys) {
            window.localStorage.removeItem(key);
        }
    } else if (data.type === "enableNetwork") {
        networkEnabled = data.enable;
        if (!networkEnabled) hybridcast.suspend();
        else hybridcast.refreshAvailability();
    } else if (data.type === "changeInput") {
        changeCallback?.(data.value);
        changeCallback = undefined;
    } else if (data.type === "cancelInput") {
        changeCallback = undefined;
    } else if (data.type === "mainAudioStreamChanged") {
        if (data.componentId != null) {
            bmlBrowser.setMainAudioStream(data.componentId, data.channelId);
        } else {
            const index = data.pid != null ? audioESList.findIndex(x => x.pid === data.pid) : data.index;
            const es = audioESList[index];
            if (es != null) {
                bmlBrowser.setMainAudioStream(es.componentId, data.channelId);
            }
        }
    } else if (data.type === "oneSegCaptionConfig") {
        oneSegCaptionOverlay = data.overlay;
    } else if (data.type === "launchOneSeg") {
        oneSegLaunched = true;
        browserElement.style.visibility = "visible";
        browserElement.style.opacity = "";
    } else if (data.type === "comments") {
        commentRenderer.add(data.comments);
    } else if (data.type === "clearComments") {
        commentRenderer.clear();
    } else if (data.type === "commentConfig") {
        commentRenderer.setOpacity(data.opacity);
        if (data.duration_ms !== undefined) {
            commentRenderer.setDuration(data.duration_ms);
        }
        if (data.shadow_color !== undefined) {
            commentRenderer.setShadowColor(data.shadow_color);
        }
        if (data.shadow_enabled !== undefined) {
            commentRenderer.setShadowEnabled(data.shadow_enabled);
        }
        if (data.outline_enabled !== undefined) {
            commentRenderer.setOutlineEnabled(data.outline_enabled);
        }
        if (data.font_size_medium !== undefined) {
            commentRenderer.setFontSizeMedium(data.font_size_medium);
        }
    } else if (data.type === "channelsConfig") {
        if (channelsClient) {
            channelsClient.close();
        }
        channelsClient = new ChannelsClient(data.ws_uri, (channels) => {
            postMessage({ type: "channelsUpdate", channels });
        });
    } else if (data.type === "ttmlCaption") {
        ttmlPlayer.push(data);
    } else if (data.type === "ttmlCaptionFont") {
        ttmlPlayer.pushFont(data.data);
    } else if (data.type === "ttmlCaptionConfig") {
        ttmlPlayer.setConfig(data);
    } else if (data.type === "b62Caption") {
        b62Application.pushCaption(data);
    } else if (data.type === "b62EventMessage") {
        b62Application.pushEventMessage(data);
    } else if (data.type === "b62Application") {
        b62Active = data.url != null || !!data.loading;
        if (b62Active) hybridcast.suspend();
        b62Application.update(data.url, data.version);
        remoteControl.setB62ReceivingStatus(data.loading ?? false);
    } else if (data.type === "b62Networking") {
        // 短い通信で表示がちらつかないよう、終わってから少しの間は出したままにする
        if (b62NetworkingTimeout != null) {
            window.clearTimeout(b62NetworkingTimeout);
            b62NetworkingTimeout = null;
        }
        if (data.active) {
            remoteControl.setB62NetworkingStatus(true);
        } else {
            b62NetworkingTimeout = window.setTimeout(() => {
                b62NetworkingTimeout = null;
                remoteControl.setB62NetworkingStatus(false);
            }, 1000);
        }
    } else if (data.type === "b62NptReference") {
        b62Application.setNptReference(data);
    } else if (data.type === "b62Files") {
        b62Application.setFiles(data.files);
    } else if (data.type === "b62FollowingEvent") {
        b62Application.setFollowingEvent(data);
    }
}


let channelsClient: ChannelsClient | null = null;

const commentCanvas = document.getElementById("comment-canvas") as HTMLCanvasElement;
const commentParent = document.getElementById("data-broadcasting-browser")!;
commentCanvas.width = commentParent.clientWidth || 960;
commentCanvas.height = commentParent.clientHeight || 540;
const commentRenderer = new CommentRenderer(commentCanvas);
commentRenderer.start();

window.chrome.webview.addEventListener("message", (ev: any) => onWebViewMessage(ev.data as ToWebViewMessage, window.chrome.webview.postMessage));

window.sendMessage = (data: ToWebViewMessage): FromWebViewMessage | null => {
    let result: FromWebViewMessage | null = null;
    onWebViewMessage(data, (data) => result = data);
    return result;
};

function onResized() {
    const windowWidth = document.documentElement.clientWidth;
    const windowHeight = document.documentElement.clientHeight;
    const contentWidth = browserElement.clientWidth;
    const contentHeight = browserElement.clientHeight;
    const scaleX = windowWidth / contentWidth;
    const scaleY = windowHeight / contentHeight;
    const s = Math.min(scaleX, scaleY);
    document.body.style.transform = `translate(${Math.ceil((contentWidth * s + windowWidth) / 2 - contentWidth * s)}px, ${Math.ceil((contentHeight * s + windowHeight) / 2 - contentHeight * s)}px) scale(${s})`;
    document.body.style.transformOrigin = `0px 0px`;
    b62Application.resize();
    ttmlPlayer.render();
    // 4Kデータ放送のアプリケーションが映像を置いていればその位置に映像を出す
    const b62VideoRect = b62Application.videoRectInViewport();
    const { left, top, right, bottom } = b62VideoRect ?? videoContainer.getBoundingClientRect();
    postMessage({
        type: "videoChanged",
        left: left * window.devicePixelRatio,
        top: top * window.devicePixelRatio,
        right: right * window.devicePixelRatio,
        bottom: bottom * window.devicePixelRatio,
        invisible: b62VideoRect != null ? false : (bmlBrowser.content.invisible ?? true) || isFullScreenVideo(),
    });
}

window.addEventListener("resize", onResized);

onResized();

window.bmlBrowser = bmlBrowser;
