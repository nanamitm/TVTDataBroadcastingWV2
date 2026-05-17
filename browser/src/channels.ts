export interface ChannelInfo {
    id: number;
    name: string;
    video: string;
    bs: boolean;
    force: number;
    viewers: number;
    comments: number;
    programTitle: string | null;
}

export class ChannelsClient {
    private ws: WebSocket | null = null;
    private channels = new Map<number, ChannelInfo>();
    private retryTimer = 0;

    constructor(
        private readonly url: string,
        private readonly onUpdate: (channels: ChannelInfo[]) => void,
    ) {
        this.connect();
    }

    private connect() {
        const ws = new WebSocket(this.url);
        this.ws = ws;
        ws.onmessage = (e) => this.onMessage(JSON.parse(e.data));
        ws.onerror = () => {};
        ws.onclose = () => {
            if (this.ws === ws) {
                this.retryTimer = window.setTimeout(() => this.connect(), 5000);
            }
        };
    }

    private onMessage(msg: any) {
        if (msg.type === "snapshot") {
            this.channels.clear();
            for (const ch of msg.channels ?? []) {
                this.channels.set(ch.id, {
                    id: ch.id,
                    name: ch.name ?? "",
                    video: ch.video ?? "",
                    bs: ch.bs ?? false,
                    force: ch.force ?? 0,
                    viewers: ch.viewers ?? 0,
                    comments: ch.comments ?? 0,
                    programTitle: ch.program?.title ?? null,
                });
            }
        } else if (msg.type === "stats") {
            for (const ch of msg.channels ?? []) {
                const entry = this.channels.get(ch.id);
                if (!entry) continue;
                if (ch.force !== undefined)    entry.force    = ch.force;
                if (ch.viewers !== undefined)  entry.viewers  = ch.viewers;
                if (ch.comments !== undefined) entry.comments = ch.comments;
            }
        } else if (msg.type === "programs") {
            for (const ch of msg.channels ?? []) {
                const entry = this.channels.get(ch.id);
                if (!entry) continue;
                entry.programTitle = ch.program?.title ?? null;
            }
        }
        this.notifyUpdate();
    }

    private notifyUpdate() {
        const sorted = [...this.channels.values()].sort((a, b) => b.force - a.force);
        this.onUpdate(sorted);
    }

    close() {
        clearTimeout(this.retryTimer);
        const ws = this.ws;
        this.ws = null;
        ws?.close();
    }
}
