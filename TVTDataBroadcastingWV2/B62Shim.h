#pragma once

namespace B62
{

// Origin that serves 4K data broadcasting (ARIB STD-B62) applications out of
// AppReceiver. Kept apart from the plugin page so an application cannot reach it.
constexpr wchar_t kAppOrigin[] = L"https://tvtdatabroadcastingwv2-b62.invalid";

// Injected into every document; only acts inside the application origin.
// A minimal stand-in for the receiver APIs of ARIB STD-B62 Volume 2 that
// applications seen on air call at startup, so they can load. Values are
// placeholders; event messages, caching and key sets do nothing yet.
constexpr wchar_t kShimScript[] = LR"JS(
(function () {
    if (location.origin !== "https://tvtdatabroadcastingwv2-b62.invalid") {
        return;
    }
    const storagePrefix = "b62:";
    function notifyParent(message) {
        try {
            window.top.postMessage(Object.assign({ source: "b62Application" }, message), "*");
        } catch (e) {
        }
    }
    const systemInformation = {
        baseurl: location.origin + "/",
        browsername: "TVTDataBroadcastingWV2",
        browserversion: "1.0",
        makerid: "ff",
        modelid: "ff",
        softwareversion: "1.0",
    };
    // OIPF-style key set. DBUTTON is ARIB's; its value here is a placeholder.
    const keySet = {
        RED: 0x1, GREEN: 0x2, YELLOW: 0x4, BLUE: 0x8, NAVIGATION: 0x10, VCR: 0x20,
        SCROLL: 0x40, INFO: 0x80, NUMERIC: 0x100, ALPHA: 0x200, OTHER: 0x400, DBUTTON: 0x800,
        value: 0,
        setValue: function (value) { this.value = value; return value; },
    };
    const ownerApplication = {
        keySet: keySet,
        destroyApplication: function () { notifyParent({ type: "destroy" }); },
        show: function () { notifyParent({ type: "show" }); return true; },
        hide: function () { notifyParent({ type: "hide" }); return true; },
        activate: function () { return true; },
        deactivate: function () { return true; },
    };
    function later(callback, value) {
        if (typeof callback === "function") {
            setTimeout(function () { callback(value); }, 0);
        }
    }
    // The plugin page answers with what it knows of the current programme.
    // Without an answer the callback still gets an object, as apps read
    // fields like service_id from it unconditionally.
    let requestId = 0;
    const pendingRequests = new Map();
    window.addEventListener("message", function (ev) {
        const data = ev.data;
        if (ev.source !== window.top || !data || data.source !== "b62Host") {
            return;
        }
        const callback = pendingRequests.get(data.id);
        if (callback) {
            pendingRequests.delete(data.id);
            callback(data.data);
        }
    });
    function askParent(type, callback) {
        const id = ++requestId;
        let done = false;
        const finish = function (value) {
            if (done) {
                return;
            }
            done = true;
            pendingRequests.delete(id);
            if (typeof callback === "function") {
                callback(value);
            }
        };
        pendingRequests.set(id, finish);
        setTimeout(function () { finish({}); }, 1000);
        notifyParent({ type: type, id: id });
    }
    const receiverDevice = {
        getSystemInformation: function (callback) {
            const info = Object.assign({}, systemInformation);
            later(callback, info);
            return info;
        },
        getCurrentEventInformation: function (callback) { askParent("getEventInformation", callback); },
        getDeviceIdentifier: function (type, callback) { later(callback, null); return null; },
        streamEvent: {
            addGeneralEventMessageListener: function () { return true; },
            removeGeneralEventMessageListener: function () { return true; },
            addEventIDUpdateListener: function () { return true; },
            removeEventIDUpdateListener: function () { return true; },
        },
        cacheEvent: {
            storeDataResource: function () { return true; },
            releaseDataResource: function () { return true; },
            addCacheEventListener: function () { return true; },
            removeCacheEventListener: function () { return true; },
        },
    };
    const browserPseudo = {
        readPersistentArray: function (filename, structure) {
            try {
                const value = localStorage.getItem(storagePrefix + filename);
                return value == null ? null : JSON.parse(value);
            } catch (e) {
                return null;
            }
        },
        writePersistentArray: function (filename, structure, data) {
            try {
                localStorage.setItem(storagePrefix + filename, JSON.stringify(Array.from(data)));
                return 1;
            } catch (e) {
                return NaN;
            }
        },
    };
    // Key codes. The apps compare KeyboardEvent.keyCode with these globals, so
    // the values only have to match what the plugin page sends. They follow
    // OIPF where it defines the key; VK_DBUTTON is a placeholder.
    const keyCodes = {
        VK_LEFT: 37, VK_UP: 38, VK_RIGHT: 39, VK_DOWN: 40, VK_ENTER: 13, VK_BACK: 461,
        VK_RED: 403, VK_GREEN: 404, VK_YELLOW: 405, VK_BLUE: 406, VK_DBUTTON: 0x1F5,
    };
    for (let i = 0; i <= 9; i++) {
        keyCodes["VK_" + i] = 48 + i;
    }
    for (const name in keyCodes) {
        window[name] = keyCodes[name];
    }
    function dispatchKey(type, keyCode) {
        const event = new KeyboardEvent(type, { bubbles: true, cancelable: true });
        // KeyboardEventInit cannot set keyCode in Chromium.
        Object.defineProperty(event, "keyCode", { get: function () { return keyCode; } });
        Object.defineProperty(event, "which", { get: function () { return keyCode; } });
        (document.activeElement || document.body || document.documentElement || document).dispatchEvent(event);
    }
    window.addEventListener("message", function (ev) {
        const data = ev.data;
        if (ev.source !== window.top || !data || data.source !== "b62Host" || data.type !== "key") {
            return;
        }
        if (typeof data.keyCode !== "number") {
            return;
        }
        const keyName = Object.keys(keyCodes).find(function (name) { return keyCodes[name] === data.keyCode; });
        console.debug("[TVTDataBroadcastingWV2] key " + (keyName || data.keyCode) + " -> " + location.pathname);
        dispatchKey("keydown", data.keyCode);
        dispatchKey("keyup", data.keyCode);
    });

    function define(name, value) {
        Object.defineProperty(navigator, name, { value: value, configurable: true });
    }
    define("receiverDevice", receiverDevice);
    define("applicationManager", { getOwnerApplication: function () { return ownerApplication; } });
    define("bmlCompat", { browserPseudo: browserPseudo });

    // <object type="video/x-arib2-broadcast"> marks where the broadcast video
    // goes. The plugin draws the video behind the page, so the object must be
    // see-through instead of Chromium's white "plugin not supported" box.
    // A constructed sheet needs no <head>, which does not exist yet here.
    const sheet = new CSSStyleSheet();
    sheet.replaceSync('object[type="video/x-arib2-broadcast" i], object[type="audio/x-arib2-broadcast" i] { visibility: hidden !important; }');
    document.adoptedStyleSheets = [...document.adoptedStyleSheets, sheet];

    // Tell the plugin page where the video object is (in this document's
    // 3840x2160 coordinates), so it can cut a hole there and move the video
    // window under it. Only the application's own document reports.
    if (window.parent === window.top) {
        let lastVideoRect = "";
        setInterval(function () {
            let rect = null;
            const video = document.querySelector('object[type="video/x-arib2-broadcast" i]');
            if (video && video.getClientRects().length > 0) {
                const r = video.getBoundingClientRect();
                if (r.width > 0 && r.height > 0) {
                    rect = { left: r.left, top: r.top, width: r.width, height: r.height };
                }
            }
            const key = JSON.stringify(rect);
            if (key !== lastVideoRect) {
                lastVideoRect = key;
                notifyParent({ type: "videoRect", rect: rect });
            }
        }, 100);
    }
})();
)JS";

}
