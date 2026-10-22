#include <obs-module.h>

#include <QString>
#include <QByteArray>

#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include "widget-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-widget-browser", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
    return "obs-widget-browser";
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Browser widget source powered by Qt WebEngine (Chromium)";
}

static void prepare_chromium_flags()
{
#ifdef Q_OS_LINUX
    // On Linux, Chromium's hardware compositing crashes the iris GL driver on
    // some Intel iGPU setups (Mesa). Software compositing is safe everywhere.
    // Users may override via the standard QTWEBENGINE_CHROMIUM_FLAGS env var.
    if (qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS")) {
        // Offscreen pages are "hidden" to Chromium: without these flags the
        // renderer throttles/freezes JS timers and the widget goes stale.
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
                QByteArray("--disable-gpu --no-sandbox "
                           "--disable-background-timer-throttling "
                           "--disable-renderer-backgrounding "
                           "--disable-backgrounding-occluded-windows "
                           "--disable-features=IntensiveWakeUpThrottling"));
    }
#endif
}

MODULE_EXPORT bool obs_module_load(void)
{
    prepare_chromium_flags();

    // Must happen before any WebEngine object is created.
    QtWebEngineQuick::initialize();

    register_widget_browser_source();

    blog(LOG_INFO, "[obs-widget-browser] loaded (Qt WebEngine / Chromium)");
    return true;
}

MODULE_EXPORT void obs_module_unload(void)
{
    blog(LOG_INFO, "[obs-widget-browser] unloaded");
}

MODULE_EXPORT void obs_module_post_load(void) {}

MODULE_EXPORT void obs_module_free(void) {}