import QtQuick
import QtWebEngine
import QtWebChannel

// Root of the off-screen widget scene. Properties are driven from C++
// (WidgetRenderer): url, and reload() is invoked to refresh the page.
WebEngineView {
    id: view
    objectName: "widgetView"
    anchors.fill: parent
    backgroundColor: "transparent"

    // Offscreen pages are treated as hidden and get frozen by the lifecycle
    // manager (JS timers stop). Keep the widget always active so it stays
    // in sync with its resource.
    lifecycleState: WebEngineView.Active

    // The discovery script (installed into the view's user scripts from C++)
    // talks to the C++ WidgetBridge through this channel and reports every
    // endpoint the page requests.
    webChannel: WebChannel {
        id: channel
        Component.onCompleted: registerObject("widget", obsBridge)
    }

    settings.javascriptEnabled: true
    settings.localStorageEnabled: true
    settings.pluginsEnabled: false
    settings.showScrollBars: false

    // Called from C++ to execute a user-configured action inside the page.
    function runJs(script) {
        runJavaScript(script)
    }

    // Called from C++ to inject the endpoint discovery script at document
    // creation, before the page's own scripts run.
    function installUserScript(script) {
        userScripts.insert(script)
    }

    onLoadingChanged: function(info) {
        if (info.status === WebEngineView.LoadFailedStatus)
            console.log("widget: load failed")
    }
}
