import type { ResponseMessage } from "web-bml/protocol";

export type BroadcastCertificatesMessage = {
    type: "broadcastCertificates",
    moduleVersion: number,
    dataEventId: number,
    certificates: { location: string, dataBase64: string }[],
};

// Standard data-broadcast certificate carousel. It is received independently
// of network enablement; native code is the trust boundary and verifies pins.
export class BroadcastCertificateCollector {
    private last = "";
    constructor(private post: (message: BroadcastCertificatesMessage) => void) {}
    accept(message: ResponseMessage) {
        if (message.type !== "moduleDownloaded" || message.componentId !== 0x40 || message.moduleId !== 0xffff) return;
        const certificates = message.files.filter(file => file.contentType.type.toLowerCase() === "application"
            && file.contentType.subtype.toLowerCase() === "x-arib-rootcertificate"
            && file.contentLocation != null && /^\d+\.\d+\.\d+$/.test(file.contentLocation)
            && file.contentLocation.length <= 64 && file.dataBase64.length > 0 && file.dataBase64.length <= 21848)
            .map(file => ({ location: file.contentLocation!, dataBase64: file.dataBase64 }));
        if (certificates.length === 0 || certificates.length > 8) return;
        const result: BroadcastCertificatesMessage = { type: "broadcastCertificates", moduleVersion: message.version,
            dataEventId: message.dataEventId, certificates };
        const signature = JSON.stringify(result);
        if (signature === this.last) return;
        this.last = signature;
        this.post(result);
    }
}
