
(function () {
    if (window.__obsWidgetDiscovery) return;
    window.__obsWidgetDiscovery = true;

    var pending = [];
    var seen = {};
    var bridge = null;

    function absolute(url) {
        try {
            return new URL(String(url), window.location.href).href;
        } catch (e) {
            return String(url || "");
        }
    }

    function report(method, url, type) {
        url = absolute(url);
        if (!url) return;
        method = String(method || "GET").toUpperCase();
        var key = method + " " + url;
        if (seen[key]) return;
        seen[key] = true;
        if (bridge) {
            try { bridge.reportEndpoint(method, url, type || ""); } catch (e) {}
        } else {
            pending.push([method, url, type || ""]);
        }
    }

    function flush() {
        if (!bridge) return;
        var queue = pending;
        pending = [];
        for (var i = 0; i < queue.length; ++i) {
            try {
                bridge.reportEndpoint(queue[i][0], queue[i][1], queue[i][2]);
            } catch (e) {}
        }
    }

    var originalFetch = window.fetch;
    if (originalFetch) {
        window.fetch = function (input, init) {
            try {
                var method = (init && init.method) ||
                             (input && input.method) || "GET";
                var url = (typeof input === "string") ? input
                                                      : (input && input.url);
                if (url) report(method, url, "fetch");
            } catch (e) {}
            return originalFetch.apply(this, arguments);
        };
    }

    var originalOpen = XMLHttpRequest.prototype.open;
    XMLHttpRequest.prototype.open = function (method, url) {
        try { if (url) report(method || "GET", url, "xhr"); } catch (e) {}
        return originalOpen.apply(this, arguments);
    };

    if (navigator.sendBeacon) {
        var originalBeacon = navigator.sendBeacon.bind(navigator);
        navigator.sendBeacon = function (url, data) {
            try { if (url) report("POST", url, "beacon"); } catch (e) {}
            return originalBeacon(url, data);
        };
    }

    if (window.EventSource) {
        var OriginalEventSource = window.EventSource;
        var ObsEventSource = function (url, config) {
            try { if (url) report("GET", url, "eventsource"); } catch (e) {}
            return config === undefined ? new OriginalEventSource(url)
                                        : new OriginalEventSource(url, config);
        };
        ObsEventSource.prototype = OriginalEventSource.prototype;
        ObsEventSource.CONNECTING = OriginalEventSource.CONNECTING;
        ObsEventSource.OPEN = OriginalEventSource.OPEN;
        ObsEventSource.CLOSED = OriginalEventSource.CLOSED;
        window.EventSource = ObsEventSource;
    }

    if (window.WebSocket) {
        var OriginalWebSocket = window.WebSocket;
        var ObsWebSocket = function (url, protocols) {
            try { if (url) report("WS", url, "websocket"); } catch (e) {}
            return protocols === undefined ? new OriginalWebSocket(url)
                                           : new OriginalWebSocket(url, protocols);
        };
        ObsWebSocket.prototype = OriginalWebSocket.prototype;
        ObsWebSocket.CONNECTING = OriginalWebSocket.CONNECTING;
        ObsWebSocket.OPEN = OriginalWebSocket.OPEN;
        ObsWebSocket.CLOSING = OriginalWebSocket.CLOSING;
        ObsWebSocket.CLOSED = OriginalWebSocket.CLOSED;
        window.WebSocket = ObsWebSocket;
    }

    function pageDeclared() {
        var list = window.__OBS_WIDGET_ENDPOINTS__ ||
                   (window.obsWidget && window.obsWidget.endpoints);
        if (!Array.isArray(list)) return;
        for (var i = 0; i < list.length; ++i) {
            var item = list[i];
            if (typeof item === "string")
                report("GET", item, "declared");
            else if (item && item.url)
                report(item.method || "GET", item.url, "declared");
        }
    }

    var connectAttempts = 0;
    function connect() {
        if (!window.qt || !window.qt.webChannelTransport) {
            if (++connectAttempts > 50) return;
            setTimeout(connect, 100);
            return;
        }
        try {
            new QWebChannel(window.qt.webChannelTransport, function (channel) {
                bridge = channel.objects.widget;
                flush();
                pageDeclared();
            });
        } catch (e) {
            setTimeout(connect, 250);
        }
    }

    window.__obsRunAction = function (options) {
        try {
            if (!options || !options.url) return;
            var method = String(options.method || "GET").toUpperCase();
            fetch(absolute(options.url),
                  { method: method, credentials: "include" });
        } catch (e) {}
    };

    window.addEventListener("load", function () {
        setTimeout(pageDeclared, 0);
    });
    setInterval(pageDeclared, 2000);
    connect();
})();
