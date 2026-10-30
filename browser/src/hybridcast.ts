import type { BrowserExtension } from "web-bml";
import type { ResponseMessage } from "web-bml/protocol";
import { AITControlledAppHost, type Html5Application, type Html5AIT } from "./ait_controlled_app";

export type HybridcastOutgoing = { type: "launchHtml5", url: string } & Record<string, unknown>
    | { type: "killHtml5", reason: string }
    | { type: "html5Data", data: ResponseMessage }
    | { type: "rpcResult", result: any };
export type HybridcastIncoming = { type: "html5Config", enabled: boolean, autoStartBroadcastAIT: boolean, residentAppVersion?: Record<string, any[]> }
    | { type: "html5Stopped" }
    | { type: "bmlVisibility", visible: boolean }
    | { type: "html5Replace", organizationId: number, applicationId: number, aitUrl: string | null }
    | { type: "rpc", method: string, params: any };

// Keeps Hybridcast application ownership separate from the B62 receiver/frame.
export class HybridcastHost {
    readonly ait: AITControlledAppHost;
    enabled = false;
    private autoStart = true;
    private running: Html5Application | null = null;
    private versions: Record<string, any[]> = {};
    private ids: Record<string, number> = {};
    lastEIT: Extract<ResponseMessage, { type: "eitPresentFollowing" }> | null = null;
    private timeCounter = 0;
    private wasAvailable = false;
    constructor(private options: {
        available(): boolean;
        post(message: HybridcastOutgoing): void;
        fetchXMLAIT(uri: string): Promise<string | null>;
    }) {
        this.ait = new AITControlledAppHost({
            supportedApplicationTypes: new Set([0x10]),
            fetchXMLAIT: uri => this.canLaunch() ? options.fetchXMLAIT(uri) : Promise.resolve(null),
            launch: (app, ait) => this.launch(app, ait),
        });
    }
    private canLaunch() { return this.enabled && this.options.available(); }
    private launch(app: Html5Application, ait: Html5AIT): boolean {
        if (!this.canLaunch() || app.url == null) return false;
        this.running = app;
        this.runningFromBroadcast = ait.source == null;
        this.options.post({ type: "launchHtml5", profile: "hybridcast", ...this.ids,
            ...app, url: app.url, aitSource: ait.source, applications: ait.applications });
        return true;
    }
    configure(enabled: boolean, autoStart: boolean, versions: Record<string, any[]> = {}) {
        this.enabled = enabled;
        this.autoStart = autoStart;
        this.versions = versions;
        if (!enabled) this.suspend();
        else this.processAIT();
    }
    suspend() {
        this.ait.invalidateDocument();
        if (this.running) this.options.post({ type: "killHtml5", reason: "unavailable" });
        this.running = null;
        this.runningFromBroadcast = false;
    }
    refreshAvailability() {
        const available = this.options.available();
        if (!available && this.wasAvailable) this.suspend();
        this.wasAvailable = available;
        if (available) this.processAIT();
    }
    stopped() { this.running = null; }
    private processAIT() {
        const ait = this.ait.getBroadcastAIT();
        if (!ait || !this.canLaunch()) return;
        if (this.running) {
            const app = ait.applications.find(a => a.organizationId === this.running!.organizationId && a.applicationId === this.running!.applicationId);
            // XML-AIT applications have their own application signalling.
            if (this.running && this.runningFromBroadcast && (!app || [3, 4, 7].includes(app.controlCode))) {
                this.options.post({ type: "killHtml5", reason: "ait" });
                this.running = null;
                this.runningFromBroadcast = false;
            }
            return;
        }
        if (this.autoStart) {
            const app = ait.applications.filter(a => a.applicationType === 0x10 && a.controlCode === 1 && a.url != null)
                .sort((a, b) => b.priority - a.priority)[0];
            if (app) this.launch(app, ait);
        }
    }
    private runningFromBroadcast = false;
    emit(message: ResponseMessage) {
        const available = this.options.available();
        if (!available && (this.wasAvailable || this.running)) this.suspend();
        this.wasAvailable = available;
        if (message.type === "eitPresentFollowing") this.lastEIT = message;
        if (message.type === "programInfo") {
            const { originalNetworkId, transportStreamId, serviceId } = message;
            if (originalNetworkId != null && transportStreamId != null && serviceId != null) this.ids = { originalNetworkId, transportStreamId, serviceId };
        }
        if (message.type === "ait") {
            this.ait.setBroadcastAIT(message.section);
            this.processAIT();
        }
        if (this.enabled && this.running && (["eitPresentFollowing", "programInfo", "esEventUpdated", "ait", "pmt"].includes(message.type)
            || (message.type === "currentTime" && ++this.timeCounter % 10 === 0))) this.options.post({ type: "html5Data", data: message });
    }
    get extension(): BrowserExtension {
        return {
            aitControlledApp: this.ait,
            getResidentAppVersion: name => this.canLaunch() ? this.versions[name] ?? null : null,
            getBrowserSupport: (provider, name, args) => {
                if (provider !== "ARIB" || !this.canLaunch()) return undefined;
                if (name === "APIGroup" && args[0] === "AITControlledApp.Start") return 1;
                if (name === "AITControlledAppEngineFunction") return args[0] === "IPTV-F" && args[1] === "HTML5_ph0" ? 1 : 0;
                if (["AITTransferMethod", "AITTransportMethod"].includes(name)) return args[0] === "XML" && args[1] === "HTTP" ? 1 : 0;
                return undefined;
            },
        };
    }
}
