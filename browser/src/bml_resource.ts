/** HTTP error bodies are not broadcast documents, tables or images. */
export async function getBMLResource(uri: string, fetcher: typeof fetch) {
    const res = await fetcher("https://tvtdatabroadcastingwv2-api.invalid/api/get/" + uri);
    if (!res.ok) {
        // Do not put linking keys or other query parameters in diagnostics.
        const url = new URL(uri);
        console.error("[network] BML resource GET failed", res.status, url.origin + url.pathname);
        return {};
    }
    return { statusCode: res.status, headers: res.headers, response: new Uint8Array(await res.arrayBuffer()) };
}
