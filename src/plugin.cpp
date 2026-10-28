#include <obs-module.h>

#include <QString>
#include <QStringList>
#include <QByteArray>

#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include "gpu-selector.h"
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

static void configure_gpu_preference()
{
#ifdef Q_OS_LINUX
    // An explicit backend/GPU configuration wins over automatic selection.
    if (!qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS")) {
        blog(LOG_INFO, "[obs-widget-browser] GPU auto-selection skipped: "
                       "QTWEBENGINE_CHROMIUM_FLAGS is set");
        return;
    }
    if (gpu_environment_overridden()) {
        blog(LOG_INFO, "[obs-widget-browser] GPU auto-selection skipped: "
                       "the GPU environment is already configured");
        return;
    }

    const std::vector<GpuInfo> gpus = detect_gpus();
    for (const GpuInfo &gpu : gpus) {
        blog(LOG_DEBUG,
             "[obs-widget-browser] GPU %s: vendor=%s device=%s driver=%s "
             "boot_vga=%d score=%d",
             gpu.name.c_str(), gpu.vendorId.c_str(), gpu.deviceId.c_str(),
             gpu.driver.c_str(), gpu.bootVga ? 1 : 0, gpu.score);
    }

    const GpuInfo selected = pick_best_gpu(gpus);
    if (selected.score <= 0) {
        blog(LOG_INFO, "[obs-widget-browser] no usable GPU detected; "
                       "using Chromium defaults");
        return;
    }

    const std::vector<GpuEnvVar> env = gpu_environment(selected, gpus);
    for (const GpuEnvVar &var : env) {
        if (qEnvironmentVariableIsEmpty(var.name.c_str()))
            qputenv(var.name.c_str(), QByteArray::fromStdString(var.value));
        blog(LOG_DEBUG, "[obs-widget-browser] %s=%s", var.name.c_str(),
             var.value.c_str());
    }

    blog(LOG_INFO, "[obs-widget-browser] rendering GPU: %s (%s, PCI %s)%s",
         selected.name.c_str(), selected.driver.c_str(),
         selected.pciAddress.c_str(),
         env.empty() ? " [default]" : " [PRIME offload]");
#endif
}

static void prepare_chromium_flags()
{
#ifdef Q_OS_LINUX
    // Keep Chromium's GPU path enabled so WebGL/WebGPU and accelerated
    // compositing work. Some launchers export --disable-gpu globally; strip it
    // here so WebGL is still available in the widget browser.
    QStringList flags = QString::fromUtf8(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"))
                            .split(' ', Qt::SkipEmptyParts);
    flags.removeAll("--disable-gpu");
    flags.removeAll("--disable-gpu-compositing");
    flags.removeAll("--disable-gpu-rasterization");

    const QStringList required = {
        "--no-sandbox",
        "--disable-gpu",
        "--disable-gpu-compositing",
        "--use-gl=angle",
        "--use-angle=swiftshader",
        "--enable-unsafe-swiftshader",
        "--disable-background-timer-throttling",
        "--disable-renderer-backgrounding",
        "--disable-backgrounding-occluded-windows",
        "--disable-features=Vulkan,IntensiveWakeUpThrottling",
    };

    for (const QString &flag : required) {
        if (!flags.contains(flag))
            flags.append(flag);
    }

    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags.join(' ').toUtf8());
#endif
}

MODULE_EXPORT bool obs_module_load(void)
{
    configure_gpu_preference();
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