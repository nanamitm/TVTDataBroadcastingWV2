// Apply small compatibility fixes to the pinned web-bml build without changing
// the submodule checkout. Fail the build if an upstream update changes anchors.
module.exports = function patchContent(input) {
    let source = input.toString().replace(/\r\n/g, '\n');
    function replaceOnce(before, after) {
        if (source.split(before).length !== 2) throw new Error('web-bml navigation patch no longer matches; review upstream Content');
        source = source.replace(before, after);
    }
    replaceOnce('    async launchDocumentAsync(documentName, options) {', `    async handleDocumentFailure(message, code) {
        try {
            await this.exitDocument();
        } finally {
            // Even a failed onunload must not leave local events discarded.
            this.eventQueue.reset();
            this.keyProcessStatus = undefined;
            this.resources.activeDocument = null;
            this.loaded = false;
            this.indicator?.setUrl("", false);
            this.bmlEventTarget.dispatchEvent(new CustomEvent("invisible", { detail: true }));
        }
        this.showErrorMessage("データ放送の読み込みエラー", message, code);
    }
    async launchDocumentAsync(documentName, options) {
        try {
            return await this.launchDocumentUnchecked(documentName, options);
        } catch (error) {
            this.logger.error("[content] document navigation failed", error);
            try {
                await this.handleDocumentFailure("リンク先の読み込みまたは解析に失敗しました", "E401");
            } catch (recoveryError) {
                this.logger.error("[content] document cleanup failed", recoveryError);
            }
            return NaN;
        }
    }
    async launchDocumentUnchecked(documentName, options) {`);
    replaceOnce(`        if (res == null) {
            if (normalizedDocument.startsWith("http")) {
                this.fail("ネットワークエラー", "文書の取得に失敗しました", "E400");
            }
            this.logger.error(\`\${this.logger.prefix}NOT FOUND\`);
            await this.quitDocument();
            return NaN;
        }`, `        if (res == null) {
            await this.handleDocumentFailure("文書の取得に失敗しました。通信状態とConsoleのHTTPステータスを確認してください", "E400");
            return NaN;
        }`);
    return source;
};
