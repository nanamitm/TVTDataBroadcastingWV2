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
    // Receivers for advanced BS put "A-PAB/1.0" in the user agent, and some
    // applications (BS TV Tokyo 4K) run as on a PC, with their own emulators
    // in place of the receiver APIs, when it is missing.
    try {
        const userAgent = navigator.userAgent + " A-PAB/1.0";
        Object.defineProperty(Navigator.prototype, "userAgent", { get: function () { return userAgent; }, configurable: true });
    } catch (e) {
    }
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
        // The plugin page lets the keys through to the application (the
        // number keys select channels otherwise).
        setValue: function (value) {
            this.value = Number(value) >>> 0;
            notifyParent({ type: "keySet", value: this.value });
            return this.value;
        },
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
    // General event messages (ARIB STD-B62 Volume 2 3.3.12.1). The plugin page
    // sends each one when its time comes; a listener gets those that match
    // every field its param names.
    const eventMessageListeners = [];
    function eventParamKey(param) {
        const source = param.source || {};
        return JSON.stringify([source.event_message_tag, param.message_group_id, param.message_id, param.message_version]);
    }
    function eventMatches(param, message) {
        const source = param.source || {};
        return (source.event_message_tag == null || Number(source.event_message_tag) === message.source.event_message_tag) &&
            (param.message_group_id == null || Number(param.message_group_id) === message.message_group_id) &&
            (param.message_id == null || Number(param.message_id) === message.message_id) &&
            (param.message_version == null || Number(param.message_version) === message.message_version);
    }
    window.addEventListener("message", function (ev) {
        const data = ev.data;
        if (ev.source !== window.top || !data || data.source !== "b62Host" || data.type !== "eventMessage" || !data.message) {
            return;
        }
        eventMessageListeners.slice().forEach(function (entry) {
            if (!eventMatches(entry.param, data.message)) {
                return;
            }
            try {
                entry.listener(Object.assign({}, data.message, { source: Object.assign({}, data.message.source) }));
            } catch (e) {
                console.error(e);
            }
        });
    });

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
    // Cache events (ARIB STD-B62 Volume 2 3.3.13, ARIB TR-B39 10.7.14). The
    // plugin keeps every broadcast file it has received, so a file is "stored"
    // as soon as it is there. The plugin page sends the list of received files
    // with their versions (MPU and item) whenever it changes; the events come
    // from comparing it with the previous list.
    const cacheListeners = [];
    const storedPaths = new Set();
    // path -> version; null until the first list arrives
    let cacheFiles = null;
    // "a/b/" for a directory, "a/b/c.html" for a file, relative to the
    // application root. null for a path outside this application.
    function cachePath(path) {
        try {
            const u = new URL(String(path), location.href);
            if (u.origin !== location.origin) {
                return null;
            }
            return decodeURIComponent(u.pathname.replace(/^\/+/, ""));
        } catch (e) {
            return null;
        }
    }
    // The version of what path names: a file's own, or every file under a
    // directory. undefined when nothing of it has been received.
    function cacheState(files, path) {
        if (!files) {
            return undefined;
        }
        if (!path.endsWith("/") && files.has(path)) {
            return files.get(path);
        }
        const prefix = path === "" || path.endsWith("/") ? path : path + "/";
        const versions = [];
        files.forEach(function (version, file) {
            if (file.startsWith(prefix)) {
                versions.push(file + "=" + version);
            }
        });
        return versions.length > 0 ? versions.sort().join("\n") : undefined;
    }
    function isDirectoryPath(files, path) {
        return path.endsWith("/") || !(files && files.has(path));
    }
    function fireCacheEvent(entry, event) {
        setTimeout(function () {
            if (cacheListeners.indexOf(entry) < 0) {
                return;
            }
            try {
                entry.listener(entry.originalPath, event);
            } catch (e) {
                console.error(e);
            }
        }, 0);
    }
    function addCacheListener(path, listener) {
        const resolved = cachePath(path);
        if (resolved == null || typeof listener !== "function") {
            const error = new Error("INVALID_PARAM_ERR");
            error.name = "INVALID_PARAM_ERR";
            throw error;
        }
        const entry = { path: resolved, originalPath: String(path), listener: listener };
        cacheListeners.push(entry);
        return entry;
    }
    window.addEventListener("message", function (ev) {
        const data = ev.data;
        if (ev.source !== window.top || !data || data.source !== "b62Host" || data.type !== "cacheFiles" || !Array.isArray(data.files)) {
            return;
        }
        const previous = cacheFiles;
        const next = new Map();
        data.files.forEach(function (file) {
            if (Array.isArray(file) && typeof file[0] === "string") {
                next.set(file[0], String(file[1]));
            }
        });
        cacheFiles = next;
        cacheListeners.slice().forEach(function (entry) {
            const stored = storedPaths.has(entry.path);
            const before = cacheState(previous, entry.path);
            const after = cacheState(next, entry.path);
            if (before === after) {
                return;
            }
            if (after === undefined) {
                // A file that disappears is not notified, only a directory.
                if (previous && isDirectoryPath(previous, entry.path)) {
                    fireCacheEvent(entry, "deleted");
                }
            } else if (stored) {
                // Stored as soon as it arrives, so the update is notified as
                // finished storing (ARIB TR-B39 10.7.14).
                fireCacheEvent(entry, "store_finished");
            } else if (previous) {
                fireCacheEvent(entry, "updated");
            }
        });
    });
    const cacheEvent = {
        addCacheEventListener: function (path, listener) {
            const entry = addCacheListener(path, listener);
            if (storedPaths.has(entry.path) && cacheState(cacheFiles, entry.path) !== undefined) {
                fireCacheEvent(entry, "store_finished");
            }
        },
        removeCacheEventListener: function (path, listener) {
            const resolved = cachePath(path);
            for (let i = cacheListeners.length - 1; i >= 0; i--) {
                const entry = cacheListeners[i];
                if (entry.path === resolved && (listener == null || entry.listener === listener)) {
                    cacheListeners.splice(i, 1);
                }
            }
        },
        storeDataResource: function (path, listener) {
            const resolved = cachePath(path);
            if (resolved == null) {
                const error = new Error("INVALID_PARAM_ERR");
                error.name = "INVALID_PARAM_ERR";
                throw error;
            }
            storedPaths.add(resolved);
            if (typeof listener === "function") {
                addCacheListener(path, listener);
            }
            // Already received: finished for everyone watching the path.
            if (cacheState(cacheFiles, resolved) !== undefined) {
                cacheListeners.forEach(function (entry) {
                    if (entry.path === resolved) {
                        fireCacheEvent(entry, "store_finished");
                    }
                });
            }
        },
        releaseDataResource: function (path) {
            if (path == null) {
                storedPaths.clear();
                return;
            }
            const resolved = cachePath(path);
            if (resolved != null) {
                storedPaths.delete(resolved);
            }
        },
    };
    // UTC-NPT (ARIB STD-B62 Volume 2 3.3.12.3, ARIB TR-B39 11.2.4.2). The
    // plugin page sends the latest UTC-NPT reference descriptor of each EMT
    // and the broadcast clock; NPT values are seconds as a Number.
    let utcNptState = { references: [], latestTag: null, clockOffsetMillis: 0, service: {} };
    const utcNptListeners = [];
    const utcNptAlarms = new Map();
    let nextAlarmHandle = 0;
    function invalidParam() {
        const error = new Error("INVALID_PARAM_ERR");
        error.name = "INVALID_PARAM_ERR";
        return error;
    }
    function utcNptTag(param) {
        const source = param && param.source;
        return source && source.event_message_tag != null ? Number(source.event_message_tag) : null;
    }
    // The reference of the EMT the source names, or the latest one.
    function utcNptReference(tag) {
        const wanted = tag != null ? tag : utcNptState.latestTag;
        return utcNptState.references.find(function (reference) { return reference.tag === wanted; }) || null;
    }
    function utcNptSource(reference) {
        return Object.assign({}, utcNptState.service, { event_message_tag: reference.tag });
    }
    const dayMillis = 24 * 60 * 60 * 1000;
    function leapBoundaryMillis(reference) {
        return Math.floor(reference.utcMillis / dayMillis) * dayMillis + dayMillis;
    }
    // ARIB TR-B39 Fig. 11-3
    function utcToNpt(reference, utcMillis) {
        if (reference.scale !== 3) {
            return reference.npt;
        }
        const boundary = leapBoundaryMillis(reference);
        if (reference.leap === 1 && utcMillis >= boundary - 1000) {
            utcMillis += 1000;
        } else if (reference.leap === 2 && utcMillis >= boundary) {
            utcMillis -= 1000;
        }
        return reference.npt + (utcMillis - reference.utcMillis) / 1000;
    }
    // ARIB TR-B39 Fig. 11-4; null when a stopped NPT never gets there
    function nptToUtc(reference, npt) {
        if (reference.scale !== 3) {
            return npt <= reference.npt ? reference.utcMillis : null;
        }
        let utc = reference.utcMillis + (npt - reference.npt) * 1000;
        const boundary = leapBoundaryMillis(reference);
        if (reference.leap === 1 && utc >= boundary) {
            utc -= 1000;
        } else if (reference.leap === 2 && utc >= boundary - 1000) {
            utc += 1000;
        }
        return utc;
    }
    // A listener runs once, when UTC-NPT becomes usable for it.
    function notifyUtcNptListeners() {
        for (let i = utcNptListeners.length - 1; i >= 0; i--) {
            const entry = utcNptListeners[i];
            const reference = utcNptReference(entry.tag);
            if (!reference) {
                continue;
            }
            utcNptListeners.splice(i, 1);
            setTimeout(function () {
                try {
                    entry.listener({ source: utcNptSource(reference) });
                } catch (e) {
                    console.error(e);
                }
            }, 0);
        }
    }
    function scheduleUtcNptAlarms() {
        utcNptAlarms.forEach(function (alarm, handle) {
            clearTimeout(alarm.timer);
            alarm.timer = null;
            const reference = utcNptReference(alarm.tag);
            const due = reference ? nptToUtc(reference, alarm.value) : null;
            if (due == null) {
                return;
            }
            const delay = Math.max(0, due - (Date.now() + utcNptState.clockOffsetMillis));
            alarm.timer = setTimeout(function () {
                utcNptAlarms.delete(handle);
                try {
                    alarm.handler({ source: utcNptSource(reference), utcnpt_value: alarm.value });
                } catch (e) {
                    console.error(e);
                }
            }, Math.min(delay, 2147483647));
        });
    }
    window.addEventListener("message", function (ev) {
        const data = ev.data;
        if (ev.source !== window.top || !data || data.source !== "b62Host" || data.type !== "utcnpt" || !Array.isArray(data.references)) {
            return;
        }
        utcNptState = {
            references: data.references,
            latestTag: data.latestTag,
            clockOffsetMillis: Number(data.clockOffsetMillis) || 0,
            service: data.service || {},
        };
        notifyUtcNptListeners();
        scheduleUtcNptAlarms();
    });
    const receiverDevice = {
        getSystemInformation: function (callback) {
            const info = Object.assign({}, systemInformation);
            later(callback, info);
            return info;
        },
        getCurrentEventInformation: function (callback) { askParent("getEventInformation", callback); },
        // The identifier asked for is the CAS module ID (ARIB TR-B39 Table
        // 10-59), which is not available here; null means it could not be
        // obtained.
        getDeviceIdentifier: function (type, callback) { later(callback, null); },
        streamEvent: {
            addGeneralEventMessageListener: function (param, listener) {
                if (typeof listener === "function") {
                    eventMessageListeners.push({ param: param || {}, listener: listener });
                }
            },
            removeGeneralEventMessageListener: function (param, listener) {
                const key = eventParamKey(param || {});
                for (let i = eventMessageListeners.length - 1; i >= 0; i--) {
                    const entry = eventMessageListeners[i];
                    if (eventParamKey(entry.param) === key && (listener == null || entry.listener === listener)) {
                        eventMessageListeners.splice(i, 1);
                    }
                }
            },
            addUTCNPTReferenceMessageListener: function (param, listener) {
                if (typeof listener !== "function") {
                    throw invalidParam();
                }
                utcNptListeners.push({ tag: utcNptTag(param), listener: listener });
                notifyUtcNptListeners();
            },
            removeUTCNPTReferenceMessageListener: function (param, listener) {
                const tag = utcNptTag(param);
                for (let i = utcNptListeners.length - 1; i >= 0; i--) {
                    if (utcNptListeners[i].tag === tag && (listener == null || utcNptListeners[i].listener === listener)) {
                        utcNptListeners.splice(i, 1);
                    }
                }
            },
            setAlarmByUTCNPT: function (param, handler) {
                const value = Number(param && param.utcnpt_value);
                if (typeof handler !== "function" || !isFinite(value)) {
                    throw invalidParam();
                }
                const handle = (nextAlarmHandle = (nextAlarmHandle % 4294967295) + 1);
                utcNptAlarms.set(handle, { tag: utcNptTag(param), value: value, handler: handler, timer: null });
                scheduleUtcNptAlarms();
                return handle;
            },
            unsetAlarmByUTCNPT: function (handle) {
                const alarm = utcNptAlarms.get(handle);
                if (alarm) {
                    clearTimeout(alarm.timer);
                    utcNptAlarms.delete(handle);
                }
            },
            getUTCNPT: function (param) {
                const reference = utcNptReference(utcNptTag(param));
                if (!reference) {
                    const error = new Error("MISC_ERR");
                    error.name = "MISC_ERR";
                    throw error;
                }
                return utcToNpt(reference, Date.now() + utcNptState.clockOffsetMillis);
            },
            addEventIDUpdateListener: function () { return true; },
            removeEventIDUpdateListener: function () { return true; },
        },
        cacheEvent: cacheEvent,
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

    // Viewer's residence information area (ARIB TR-B39 10.1.1, 10.1.5): the
    // receiver answers localStorage "_prefecture", "_regioncode" and "_zipcode"
    // with its settings, "" when not set. The plugin page sends them; the last
    // ones are kept so they are there when the next application starts.
    const residenceKeys = ["_prefecture", "_regioncode", "_zipcode"];
    const storageGetItem = Storage.prototype.getItem;
    const storageSetItem = Storage.prototype.setItem;
    const storageRemoveItem = Storage.prototype.removeItem;
    let residence = {};
    try {
        residence = JSON.parse(storageGetItem.call(localStorage, storagePrefix + "residence")) || {};
    } catch (e) {
    }
    function isResidenceKey(storage, key) {
        try {
            return storage === window.localStorage && residenceKeys.indexOf(String(key)) >= 0;
        } catch (e) {
            return false;
        }
    }
    Storage.prototype.getItem = function (key) {
        if (isResidenceKey(this, key)) {
            const value = residence[String(key)];
            return typeof value === "string" ? value : "";
        }
        return storageGetItem.apply(this, arguments);
    };
    // Applications cannot change the receiver's settings.
    Storage.prototype.setItem = function (key) {
        if (isResidenceKey(this, key)) {
            return;
        }
        return storageSetItem.apply(this, arguments);
    };
    Storage.prototype.removeItem = function (key) {
        if (isResidenceKey(this, key)) {
            return;
        }
        return storageRemoveItem.apply(this, arguments);
    };
    window.addEventListener("message", function (ev) {
        const data = ev.data;
        if (ev.source !== window.top || !data || data.source !== "b62Host" || data.type !== "residence" || !data.residence) {
            return;
        }
        const next = {};
        residenceKeys.forEach(function (key) {
            const value = data.residence[key];
            next[key] = typeof value === "string" ? value : "";
        });
        residence = next;
        try {
            storageSetItem.call(localStorage, storagePrefix + "residence", JSON.stringify(residence));
        } catch (e) {
        }
    });
    // ARIB STD-B62 Volume 2 3.3.10. The queries are defined by the operational
    // rules; none of the optional functions they ask about (BS/terrestrial
    // shared NVRAM, sourceIs, ...) is provided.
    define("capabilities", { hasCapability: function () { return false; } });

    // Requests to http URLs. The application's origin is https, so the browser
    // blocks them as mixed content, while on a receiver an application
    // delivered over http may make them. They are sent to
    // "/__http/<host>/<path>" of the application's origin instead, which the
    // plugin forwards to the http URL when networking is enabled.
    const httpProxyPrefix = location.origin + "/__http/";
    function toHttpProxyUrl(url) {
        try {
            const u = new URL(String(url), location.href);
            if (u.protocol === "http:" && u.origin !== location.origin) {
                return httpProxyPrefix + u.host + u.pathname + u.search;
            }
        } catch (e) {
        }
        return url;
    }
    const originalOpen = XMLHttpRequest.prototype.open;
    XMLHttpRequest.prototype.open = function (method, url) {
        const args = Array.prototype.slice.call(arguments);
        args[1] = toHttpProxyUrl(url);
        return originalOpen.apply(this, args);
    };
    const originalFetch = window.fetch;
    window.fetch = function (input, init) {
        if (input instanceof Request) {
            const proxied = toHttpProxyUrl(input.url);
            if (proxied !== input.url) {
                input = new Request(proxied, input);
            }
        } else {
            input = toHttpProxyUrl(input);
        }
        return originalFetch.call(this, input, init);
    };
    if (typeof navigator.sendBeacon === "function") {
        const originalSendBeacon = navigator.sendBeacon;
        navigator.sendBeacon = function (url, data) {
            return originalSendBeacon.call(navigator, toHttpProxyUrl(url), data);
        };
    }

    // Built-in receiver sounds: <audio src="romsound://7"> (or a <source> child)
    // cannot load, so play() asks the plugin page to play the sound instead.
    const originalPlay = HTMLMediaElement.prototype.play;
    HTMLMediaElement.prototype.play = function () {
        let src = this.getAttribute("src");
        if (!src) {
            const source = this.querySelector("source[src]");
            src = source ? source.getAttribute("src") : "";
        }
        const match = /^romsound:\/\/(\d+)$/i.exec(String(src || "").trim());
        if (match) {
            notifyParent({ type: "romsound", id: parseInt(match[1], 10) });
            return Promise.resolve();
        }
        return originalPlay.apply(this, arguments);
    };

    // Caption methods of the broadcast video object (<object type="video/x-arib2-broadcast">).
    // The plugin page sends which caption components the service has and the
    // caption documents themselves, at the time they are presented.
    let captionComponents = [];
    const captionListeners = [];
    // "arib2://-1.-1.-1/0030" -> 0x30; null when the URL names no component.
    function captionTag(url) {
        const match = /\/([0-9a-fA-F]{4})$/.exec(String(url || ""));
        return match ? parseInt(match[1], 16) : null;
    }
    function defaultCaptionTag(object) {
        if (object.b62CaptionSrc != null) {
            return captionTag(object.b62CaptionSrc);
        }
        const param = object.querySelector('param[name="caption_src" i]');
        const tag = param ? captionTag(param.getAttribute("value")) : null;
        return tag != null ? tag : 0x30;
    }
    Object.assign(HTMLObjectElement.prototype, {
        isCaptionExistent: function (url) {
            const tag = url != null ? captionTag(url) : defaultCaptionTag(this);
            return tag != null && captionComponents.indexOf(tag) >= 0;
        },
        addCaptionListener: function (listener, url) {
            if (typeof listener !== "function") {
                return;
            }
            const tag = url != null ? captionTag(url) : defaultCaptionTag(this);
            captionListeners.push({ object: this, listener: listener, tag: tag });
        },
        removeCaptionListener: function (listener) {
            for (let i = captionListeners.length - 1; i >= 0; i--) {
                const entry = captionListeners[i];
                if (entry.object === this && (listener == null || entry.listener === listener)) {
                    captionListeners.splice(i, 1);
                }
            }
        },
        setCaptionSrc: function (url) { this.b62CaptionSrc = String(url); return true; },
        getCaptionComponentURL: function () {
            const tag = defaultCaptionTag(this);
            return tag != null && captionComponents.indexOf(tag) >= 0
                ? "arib2://-1.-1.-1/" + tag.toString(16).padStart(4, "0") : null;
        },
        // The receiver's own caption display is TVTest's; only remembered here.
        setCaptionVisibility: function (flag) { this.b62CaptionVisible = !!flag; return true; },
        isCaptionVisible: function () { return this.b62CaptionVisible !== false; },
    });
    window.addEventListener("message", function (ev) {
        const data = ev.data;
        if (ev.source !== window.top || !data || data.source !== "b62Host") {
            return;
        }
        if (data.type === "captionComponents" && Array.isArray(data.tags)) {
            captionComponents = data.tags.filter(function (tag) { return typeof tag === "number"; });
        } else if (data.type === "caption" && typeof data.payload === "string") {
            captionListeners.slice().forEach(function (entry) {
                if (entry.tag != null && entry.tag !== data.componentTag) {
                    return;
                }
                try {
                    entry.listener(data.payload);
                } catch (e) {
                    console.error(e);
                }
            });
        }
    });
    notifyParent({ type: "hello" });

    // <object type="video/x-arib2-broadcast"> marks where the broadcast video
    // goes. The plugin draws the video behind the page, so the object must be
    // see-through instead of Chromium's white "plugin not supported" box.
    // A constructed sheet needs no <head>, which does not exist yet here.
    const sheet = new CSSStyleSheet();
    sheet.replaceSync('object[type="video/x-arib2-broadcast" i], object[type="audio/x-arib2-broadcast" i] { visibility: hidden !important; }');
    document.adoptedStyleSheets = [...document.adoptedStyleSheets, sheet];

    // The document's resolution from its viewport meta element: 1920x1080,
    // 3840x2160 or 7680x4320, 3840x2160 when not given (ARIB TR-B39 10.8.1.2).
    // The plugin page sizes the frame to it and scales it to the screen.
    function documentViewport() {
        const sizes = { 1920: 1080, 3840: 2160, 7680: 4320 };
        const meta = document.querySelector('meta[name="viewport" i]');
        const content = meta ? String(meta.getAttribute("content") || "") : "";
        const width = /(?:^|[,;\s])width\s*=\s*(\d+)/i.exec(content);
        const height = /(?:^|[,;\s])height\s*=\s*(\d+)/i.exec(content);
        let w = width ? parseInt(width[1], 10) : 0;
        if (!sizes[w] && height) {
            const h = parseInt(height[1], 10);
            w = Number(Object.keys(sizes).find(function (key) { return sizes[key] === h; })) || 0;
        }
        return sizes[w] ? { width: w, height: sizes[w] } : { width: 3840, height: 2160 };
    }

    // Tell the plugin page where the video object is (in this document's
    // coordinates), so it can cut a hole there and move the video
    // window under it. Only the application's own document reports.
    // Elements stacked above the video (a popup over it, say) are reported too,
    // so the hole leaves them visible.
    function createsStackingContext(style) {
        return (style.position !== "static" && style.zIndex !== "auto") ||
            style.position === "fixed" || style.position === "sticky" ||
            Number(style.opacity) < 1 || style.transform !== "none" || style.filter !== "none" ||
            style.isolation === "isolate" || style.mixBlendMode !== "normal";
    }
    // The outermost stacking context the element belongs to, and its z-index:
    // the document's own stacking context orders by these.
    function rootStackingOf(element) {
        let root = null;
        for (let e = element; e && e !== document.documentElement; e = e.parentElement) {
            if (createsStackingContext(getComputedStyle(e))) {
                root = e;
            }
        }
        if (!root) {
            return { element: null, z: 0 };
        }
        const z = parseInt(getComputedStyle(root).zIndex, 10);
        return { element: root, z: isNaN(z) ? 0 : z };
    }
    function rectsAboveVideo(video, videoRect) {
        const videoRoot = rootStackingOf(video);
        const seen = new Set();
        const rects = [];
        const elements = document.body ? document.body.querySelectorAll("*") : [];
        for (const element of elements) {
            const root = rootStackingOf(element);
            if (!root.element || root.element === videoRoot.element || seen.has(root.element) || root.element.contains(video)) {
                continue;
            }
            seen.add(root.element);
            const style = getComputedStyle(root.element);
            if (root.z <= videoRoot.z || style.visibility === "hidden" || Number(style.opacity) === 0 || root.element.getClientRects().length === 0) {
                continue;
            }
            for (const painted of paintedElements(root.element)) {
                const r = painted.getBoundingClientRect();
                const left = Math.max(r.left, videoRect.left);
                const top = Math.max(r.top, videoRect.top);
                const right = Math.min(r.right, videoRect.left + videoRect.width);
                const bottom = Math.min(r.bottom, videoRect.top + videoRect.height);
                // Something covering nearly the whole video is a full-screen layer,
                // not an overlay; keeping it would hide the video entirely.
                if (right <= left || bottom <= top || (right - left) * (bottom - top) >= videoRect.width * videoRect.height * 0.9) {
                    continue;
                }
                rects.push({ left: left, top: top, width: right - left, height: bottom - top });
            }
        }
        return rects;
    }
    // The element and descendants that draw something: an empty positioned box
    // over the video must not keep the page background below it visible.
    function paintedElements(root) {
        const result = [];
        const walk = function (element) {
            const style = getComputedStyle(element);
            if (style.display === "none" || style.visibility === "hidden" || Number(style.opacity) === 0) {
                return;
            }
            const background = style.backgroundColor;
            const hasBackground = style.backgroundImage !== "none" ||
                !(background === "transparent" || /rgba\(.*,\s*0\)$/.test(background));
            const hasBorder = parseFloat(style.borderTopWidth) > 0 || parseFloat(style.borderLeftWidth) > 0;
            const isMedia = /^(IMG|SVG|CANVAS|VIDEO|IFRAME|OBJECT)$/i.test(element.tagName);
            const hasText = Array.prototype.some.call(element.childNodes, function (node) {
                return node.nodeType === Node.TEXT_NODE && node.textContent.trim() !== "";
            });
            if (hasBackground || hasBorder || isMedia || hasText) {
                result.push(element);
                if (hasBackground || isMedia) {
                    return; // its box already covers the children
                }
            }
            for (const child of element.children) {
                walk(child);
            }
        };
        walk(root);
        return result;
    }
    if (window.parent === window.top) {
        let lastVideoRect = "";
        setInterval(function () {
            let rect = null;
            let above = [];
            const video = document.querySelector('object[type="video/x-arib2-broadcast" i]');
            if (video && video.getClientRects().length > 0) {
                const r = video.getBoundingClientRect();
                if (r.width > 0 && r.height > 0) {
                    rect = { left: r.left, top: r.top, width: r.width, height: r.height };
                    above = rectsAboveVideo(video, rect);
                }
            }
            const viewport = documentViewport();
            const key = JSON.stringify([rect, above, viewport]);
            if (key !== lastVideoRect) {
                lastVideoRect = key;
                notifyParent({ type: "videoRect", rect: rect, above: above, viewport: viewport });
            }
        }, 100);
    }
})();
)JS";

}
