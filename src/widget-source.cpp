#include "widget-source.h"

#include "widget-renderer.h"
#include "url-utils.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QMutex>
#include <QThread>
#include <QUrl>
#include <atomic>
#include <functional>

#include <obs.h>

namespace {

constexpr const char *S_URL = "url";
constexpr const char *S_WIDTH = "width";
constexpr const char *S_HEIGHT = "height";
constexpr const char *S_FPS = "fps";
constexpr const char *S_INTERACTIVE = "interactive";
constexpr const char *S_ACTIONS = "actions";

constexpr int DEFAULT_WIDTH = 800;
constexpr int DEFAULT_HEIGHT = 600;
constexpr int DEFAULT_FPS = 30;
constexpr int MIN_DIMENSION = 64;
constexpr int MAX_DIMENSION = 4096;
constexpr int MIN_FPS = 5;
constexpr int MAX_FPS = 60;

// One user-defined action bound to a discovered (or manually entered)
// endpoint. Format in the settings list: Name | METHOD | URL | page/plugin.
struct WidgetAction
{
    QString name;
    QString method;
    QString url;
    bool inPage = true;
    QString hotkeyKey;
};

struct HotkeyReg
{
    obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
    WidgetAction action;
};

// OBS may create/update/destroy sources from non-main threads (e.g. via
// obs-websocket). All Qt/WebEngine objects must stay on the main thread, so
// every operation on the renderer is marshalled there via queued calls.
struct WidgetSource
{
    obs_source_t *source = nullptr;
    std::atomic<WidgetRenderer *> renderer{nullptr};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> interactive{true};
    gs_texture_t *texture = nullptr;
    uint32_t texture_width = 0;
    uint32_t texture_height = 0;
    QMutex frameMutex;
    QImage frame;
    uint32_t width = DEFAULT_WIDTH;
    uint32_t height = DEFAULT_HEIGHT;
    int fps = DEFAULT_FPS;
    QString url;
    QVector<WidgetAction> actions;
    QVector<HotkeyReg> hotkeys;
};

bool on_main_thread()
{
    QCoreApplication *app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

void runOnMain(std::function<void()> fn)
{
    if (on_main_thread()) {
        fn();
    } else {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [fn = std::move(fn)]() { fn(); }, Qt::QueuedConnection);
    }
}

WidgetRenderer *currentRenderer(WidgetSource *ws)
{
    QMutexLocker lock(&ws->frameMutex);
    return ws->renderer.load();
}

QString actionHotkeyKey(const QString &method, const QString &url)
{
    const QByteArray source =
        (method.toUpper() + QLatin1Char('\n') + url).toUtf8();
    const QByteArray hash =
        QCryptographicHash::hash(source, QCryptographicHash::Sha1).toHex();
    return QStringLiteral("action_") +
           QString::fromLatin1(hash.left(16));
}

QString endpointActionName(const WidgetEndpoint &endpoint)
{
    const QUrl url(endpoint.url);
    QString path = url.path();
    if (path.isEmpty() || path == QLatin1String("/"))
        path = url.host();
    return QStringLiteral("%1 %2").arg(endpoint.method, path);
}

bool readInteractive(obs_data_t *settings)
{
    // Sources saved before the setting existed must keep input enabled.
    if (!obs_data_has_user_value(settings, S_INTERACTIVE))
        return true;
    return obs_data_get_bool(settings, S_INTERACTIVE);
}

QVector<WidgetAction> parseActions(obs_data_t *settings)
{
    QVector<WidgetAction> actions;
    obs_data_array_t *array = obs_data_get_array(settings, S_ACTIONS);
    if (!array)
        return actions;

    QSet<QString> seen;
    const size_t count = obs_data_array_count(array);
    for (size_t i = 0; i < count; ++i) {
        obs_data_t *item = obs_data_array_item(array, i);
        const char *raw = obs_data_get_string(item, "value");
        const QString line = QString::fromUtf8(raw ? raw : "");
        obs_data_release(item);

        QStringList parts = line.split(QLatin1Char('|'));
        for (QString &part : parts)
            part = part.trimmed();

        WidgetAction action;
        if (parts.size() >= 4) {
            action.name = parts[0];
            action.method = parts[1];
            action.url = parts[2];
            action.inPage =
                parts[3].compare(QLatin1String("plugin"), Qt::CaseInsensitive) !=
                0;
        } else if (parts.size() == 3) {
            action.name = parts[0];
            action.method = parts[1];
            action.url = parts[2];
        } else if (parts.size() == 2) {
            action.name = parts[0];
            action.method = QStringLiteral("GET");
            action.url = parts[1];
        } else {
            continue;
        }

        if (action.url.isEmpty())
            continue;
        action.method =
            action.method.isEmpty() ? QStringLiteral("GET")
                                    : action.method.toUpper();
        if (action.name.isEmpty())
            action.name = action.method + QLatin1Char(' ') + action.url;
        action.hotkeyKey = actionHotkeyKey(action.method, action.url);

        if (seen.contains(action.hotkeyKey))
            continue;
        seen.insert(action.hotkeyKey);
        actions.append(action);
    }

    obs_data_array_release(array);
    return actions;
}

void executeAction(WidgetSource *ws, const WidgetAction &action)
{
    WidgetRenderer *renderer = currentRenderer(ws);
    if (!renderer) {
        blog(LOG_WARNING,
             "[widget-browser] action '%s' ignored: renderer is not ready",
             action.name.toUtf8().constData());
        return;
    }

    if (action.inPage)
        renderer->executeInPage(action.url, action.method);
    else
        renderer->executeHttp(action.url, action.method);
}

void widget_action_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
                          bool pressed)
{
    UNUSED_PARAMETER(hotkey);
    if (!pressed)
        return;

    auto *ws = static_cast<WidgetSource *>(data);
    WidgetAction action;
    bool found = false;
    {
        QMutexLocker lock(&ws->frameMutex);
        for (const HotkeyReg &reg : ws->hotkeys) {
            if (reg.id == id) {
                action = reg.action;
                found = true;
                break;
            }
        }
    }
    if (!found)
        return;

    runOnMain([ws, action]() { executeAction(ws, action); });
}

bool sameActions(const QVector<WidgetAction> &a,
                 const QVector<WidgetAction> &b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || a[i].method != b[i].method ||
            a[i].url != b[i].url || a[i].inPage != b[i].inPage)
            return false;
    }
    return true;
}

void syncHotkeys(WidgetSource *ws, const QVector<WidgetAction> &actions)
{
    QVector<HotkeyReg> old;
    {
        QMutexLocker lock(&ws->frameMutex);
        old = ws->hotkeys;
        ws->hotkeys.clear();
        ws->actions = actions;
    }

    // Keep bindings assigned in Settings -> Hotkeys across settings updates.
    QHash<QString, obs_data_array_t *> saved;
    for (const HotkeyReg &reg : old) {
        if (obs_data_array_t *bindings = obs_hotkey_save(reg.id))
            saved.insert(reg.action.hotkeyKey, bindings);
        obs_hotkey_unregister(reg.id);
    }

    QVector<HotkeyReg> registered;
    if (ws->source) {
        for (const WidgetAction &action : actions) {
            const QByteArray name = action.hotkeyKey.toUtf8();
            const QString description =
                QStringLiteral("%1 (%2 %3)")
                    .arg(action.name, action.method, action.url);
            const QByteArray desc = description.toUtf8();

            const obs_hotkey_id id = obs_hotkey_register_source(
                ws->source, name.constData(), desc.constData(),
                widget_action_hotkey, ws);
            if (id == OBS_INVALID_HOTKEY_ID)
                continue;

            if (auto it = saved.find(action.hotkeyKey); it != saved.end())
                obs_hotkey_load(id, it.value());

            registered.append(HotkeyReg{id, action});
        }
    }

    for (obs_data_array_t *bindings : saved)
        obs_data_array_release(bindings);

    QMutexLocker lock(&ws->frameMutex);
    ws->hotkeys = registered;
}

void updateActions(WidgetSource *ws, const QVector<WidgetAction> &actions)
{
    QVector<WidgetAction> old;
    {
        QMutexLocker lock(&ws->frameMutex);
        old = ws->actions;
    }
    if (sameActions(old, actions))
        return;
    syncHotkeys(ws, actions);
}

void createRendererOnMain(WidgetSource *ws)
{
    QMutexLocker lock(&ws->frameMutex);
    if (ws->destroyed.load())
        return;
    auto *renderer =
        new WidgetRenderer(ws->url, (int)ws->width, (int)ws->height, ws->fps);
    ws->renderer.store(renderer);
}

const char *widget_get_name(void *)
{
    return obs_module_text("WidgetBrowser.SourceName");
}

uint32_t widget_get_width(void *data)
{
    auto *ws = static_cast<WidgetSource *>(data);
    QMutexLocker lock(&ws->frameMutex);
    return ws->width;
}

uint32_t widget_get_height(void *data)
{
    auto *ws = static_cast<WidgetSource *>(data);
    QMutexLocker lock(&ws->frameMutex);
    return ws->height;
}

void *widget_create(obs_data_t *settings, obs_source_t *source)
{
    auto *ws = new WidgetSource;
    ws->source = source;

    const bool interactive = readInteractive(settings);
    const QVector<WidgetAction> actions = parseActions(settings);

    {
        QMutexLocker lock(&ws->frameMutex);
        ws->width = (uint32_t)obs_data_get_int(settings, S_WIDTH);
        ws->height = (uint32_t)obs_data_get_int(settings, S_HEIGHT);
        ws->fps = (int)obs_data_get_int(settings, S_FPS);
        ws->url = QString::fromUtf8(obs_data_get_string(settings, S_URL));
        ws->interactive.store(interactive);
        ws->actions = actions;
    }

    syncHotkeys(ws, actions);
    runOnMain([ws]() { createRendererOnMain(ws); });
    return ws;
}

void destroyOnMain(WidgetSource *ws)
{
    QVector<HotkeyReg> hotkeys;
    {
        QMutexLocker lock(&ws->frameMutex);
        hotkeys = ws->hotkeys;
        ws->hotkeys.clear();
    }
    for (const HotkeyReg &reg : hotkeys)
        obs_hotkey_unregister(reg.id);

    WidgetRenderer *renderer;
    {
        QMutexLocker lock(&ws->frameMutex);
        renderer = ws->renderer.exchange(nullptr);
    }
    delete renderer;
    obs_enter_graphics();
    if (ws->texture)
        gs_texture_destroy(ws->texture);
    obs_leave_graphics();
    delete ws;
}

void widget_destroy(void *data)
{
    auto *ws = static_cast<WidgetSource *>(data);
    ws->destroyed.store(true);
    runOnMain([ws]() { destroyOnMain(ws); });
}

void applyRendererSettings(WidgetSource *ws, int width, int height, int fps,
                           const QString &url, const QString &oldUrl)
{
    WidgetRenderer *renderer = currentRenderer(ws);
    if (!renderer)
        return;

    if (url != oldUrl) {
        // Post-creation URL writes are unreliable in offscreen Qt WebEngine,
        // so a URL change rebuilds the renderer (guaranteed load path).
        {
            QMutexLocker lock(&ws->frameMutex);
            ws->renderer.store(nullptr);
            ws->url = url;
        }
        delete renderer;
        createRendererOnMain(ws);
        return;
    }

    renderer->setSize(width, height);
    renderer->setFps(fps);
}

void widget_update(void *data, obs_data_t *settings)
{
    auto *ws = static_cast<WidgetSource *>(data);

    const int width = (int)obs_data_get_int(settings, S_WIDTH);
    const int height = (int)obs_data_get_int(settings, S_HEIGHT);
    const int fps = (int)obs_data_get_int(settings, S_FPS);
    const QString url = QString::fromUtf8(obs_data_get_string(settings, S_URL));
    const bool interactive = readInteractive(settings);
    const QVector<WidgetAction> actions = parseActions(settings);

    QString oldUrl;
    {
        QMutexLocker lock(&ws->frameMutex);
        oldUrl = ws->url;
        ws->width = width;
        ws->height = height;
        ws->fps = fps;
        ws->interactive.store(interactive);
    }

    updateActions(ws, actions);

    runOnMain([ws, width, height, fps, url, oldUrl]() {
        applyRendererSettings(ws, width, height, fps, url, oldUrl);
    });
}

void widget_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, S_WIDTH, DEFAULT_WIDTH);
    obs_data_set_default_int(settings, S_HEIGHT, DEFAULT_HEIGHT);
    obs_data_set_default_int(settings, S_FPS, DEFAULT_FPS);
    obs_data_set_default_string(settings, S_URL, "");
    obs_data_set_default_bool(settings, S_INTERACTIVE, true);

    obs_data_array_t *empty = obs_data_array_create();
    obs_data_set_default_array(settings, S_ACTIONS, empty);
    obs_data_array_release(empty);
}

bool widget_reload_button(obs_properties_t *props, obs_property_t *,
                          void *data)
{
    UNUSED_PARAMETER(props);
    auto *ws = static_cast<WidgetSource *>(data);
    if (!ws)
        return false;
    runOnMain([ws]() {
        if (WidgetRenderer *renderer = currentRenderer(ws))
            renderer->reload();
    });
    return true;
}

bool widget_refresh_endpoints(obs_properties_t *, obs_property_t *, void *)
{
    // Endpoint discovery runs continuously in the page; rebuilding the
    // properties list is enough to show the latest data.
    return true;
}

bool widget_add_discovered(obs_properties_t *, obs_property_t *, void *data)
{
    auto *ws = static_cast<WidgetSource *>(data);
    if (!ws || !ws->source)
        return false;

    WidgetRenderer *renderer = currentRenderer(ws);
    if (!renderer)
        return false;

    const QVector<WidgetEndpoint> endpoints = renderer->endpoints();
    if (endpoints.isEmpty())
        return false;

    obs_data_t *settings = obs_source_get_settings(ws->source);
    const QVector<WidgetAction> existing = parseActions(settings);
    QSet<QString> keys;
    for (const WidgetAction &action : existing)
        keys.insert(action.hotkeyKey);

    obs_data_array_t *array = obs_data_get_array(settings, S_ACTIONS);
    if (!array)
        array = obs_data_array_create();

    bool changed = false;
    for (const WidgetEndpoint &endpoint : endpoints) {
        const QString scheme = QUrl(endpoint.url).scheme().toLower();
        if (scheme != QLatin1String("http") &&
            scheme != QLatin1String("https"))
            continue;

        const QString key = actionHotkeyKey(endpoint.method, endpoint.url);
        if (keys.contains(key))
            continue;

        const QString line =
            QStringLiteral("%1 | %2 | %3 | page")
                .arg(endpointActionName(endpoint), endpoint.method,
                     endpoint.url);
        obs_data_t *item = obs_data_create();
        obs_data_set_string(item, "value", line.toUtf8().constData());
        obs_data_array_push_back(array, item);
        obs_data_release(item);

        keys.insert(key);
        changed = true;
    }

    if (changed) {
        obs_data_set_array(settings, S_ACTIONS, array);
        obs_source_update(ws->source, settings);
    }

    obs_data_array_release(array);
    obs_data_release(settings);
    return changed;
}

QString buildEndpointsText(WidgetSource *ws)
{
    QString text = obs_module_text("WidgetBrowser.Endpoints.Hint");
    text += QLatin1String("\n\n");

    QString list;
    if (ws) {
        if (WidgetRenderer *renderer = currentRenderer(ws)) {
            const QVector<WidgetEndpoint> endpoints = renderer->endpoints();
            for (const WidgetEndpoint &endpoint : endpoints) {
                list += QStringLiteral("%1  %2  (%3)\n")
                            .arg(endpoint.method, endpoint.url,
                                 endpoint.type);
            }
        }
    }

    if (list.isEmpty())
        list = obs_module_text("WidgetBrowser.Endpoints.Empty");
    text += list.trimmed();
    return text;
}

obs_properties_t *widget_properties(void *data)
{
    auto *ws = static_cast<WidgetSource *>(data);
    obs_properties_t *props = obs_properties_create();

    obs_properties_t *source = obs_properties_create();
    obs_property_t *url = obs_properties_add_text(
        source, S_URL, obs_module_text("WidgetBrowser.Url"), OBS_TEXT_DEFAULT);
    obs_property_set_long_description(
        url, obs_module_text("WidgetBrowser.Url.Description"));
    obs_properties_add_group(props, "source_group",
                             obs_module_text("WidgetBrowser.Group.Source"),
                             OBS_GROUP_NORMAL, source);

    obs_properties_t *size = obs_properties_create();
    obs_property_t *width = obs_properties_add_int(
        size, S_WIDTH, obs_module_text("WidgetBrowser.Width"), MIN_DIMENSION,
        MAX_DIMENSION, 16);
    obs_property_int_set_suffix(width, " px");
    obs_property_set_long_description(
        width, obs_module_text("WidgetBrowser.Width.Description"));

    obs_property_t *height = obs_properties_add_int(
        size, S_HEIGHT, obs_module_text("WidgetBrowser.Height"), MIN_DIMENSION,
        MAX_DIMENSION, 16);
    obs_property_int_set_suffix(height, " px");
    obs_property_set_long_description(
        height, obs_module_text("WidgetBrowser.Height.Description"));

    obs_property_t *fps = obs_properties_add_int(
        size, S_FPS, obs_module_text("WidgetBrowser.Fps"), MIN_FPS, MAX_FPS, 1);
    obs_property_int_set_suffix(fps, " fps");
    obs_property_set_long_description(
        fps, obs_module_text("WidgetBrowser.Fps.Description"));

    obs_properties_add_group(props, "size_group",
                             obs_module_text("WidgetBrowser.Group.Size"),
                             OBS_GROUP_NORMAL, size);

    obs_properties_t *interaction = obs_properties_create();
    obs_property_t *interactive = obs_properties_add_bool(
        interaction, S_INTERACTIVE,
        obs_module_text("WidgetBrowser.Interactive"));
    obs_property_set_long_description(
        interactive, obs_module_text("WidgetBrowser.Interactive.Description"));
    obs_property_t *interactHint = obs_properties_add_text(
        interaction, "interact_hint",
        obs_module_text("WidgetBrowser.Interactive.Hint"), OBS_TEXT_INFO);
    obs_property_text_set_info_type(interactHint, OBS_TEXT_INFO_NORMAL);
    obs_property_text_set_info_word_wrap(interactHint, true);
    obs_properties_add_group(
        props, "interaction_group",
        obs_module_text("WidgetBrowser.Group.Interaction"), OBS_GROUP_NORMAL,
        interaction);

    obs_properties_t *endpoints = obs_properties_create();
    const QString endpointsText = buildEndpointsText(ws);
    obs_property_t *endpointsHint = obs_properties_add_text(
        endpoints, "endpoints_info", endpointsText.toUtf8().constData(),
        OBS_TEXT_INFO);
    obs_property_text_set_info_type(endpointsHint, OBS_TEXT_INFO_NORMAL);
    obs_property_text_set_info_word_wrap(endpointsHint, true);
    obs_properties_add_button2(
        endpoints, "refresh_endpoints",
        obs_module_text("WidgetBrowser.Endpoints.Refresh"),
        widget_refresh_endpoints, data);
    obs_properties_add_button2(
        endpoints, "add_endpoints",
        obs_module_text("WidgetBrowser.Endpoints.AddToActions"),
        widget_add_discovered, data);
    obs_properties_add_group(props, "endpoints_group",
                             obs_module_text("WidgetBrowser.Group.Endpoints"),
                             OBS_GROUP_NORMAL, endpoints);

    obs_properties_t *actions = obs_properties_create();
    obs_property_t *actionsHint = obs_properties_add_text(
        actions, "actions_hint", obs_module_text("WidgetBrowser.Actions.Hint"),
        OBS_TEXT_INFO);
    obs_property_text_set_info_type(actionsHint, OBS_TEXT_INFO_NORMAL);
    obs_property_text_set_info_word_wrap(actionsHint, true);
    obs_properties_add_editable_list(
        actions, S_ACTIONS, obs_module_text("WidgetBrowser.Actions"),
        OBS_EDITABLE_LIST_TYPE_STRINGS, nullptr, nullptr);
    obs_properties_add_group(props, "actions_group",
                             obs_module_text("WidgetBrowser.Group.ActionList"),
                             OBS_GROUP_NORMAL, actions);

    obs_properties_add_button2(
        props, "reload", obs_module_text("WidgetBrowser.Reload"),
        widget_reload_button, data);

    return props;
}

void widget_mouse_click(void *data, const obs_mouse_event *event, int32_t type,
                        bool mouse_up, uint32_t click_count)
{
    auto *ws = static_cast<WidgetSource *>(data);
    if (!ws || !ws->interactive.load())
        return;

    const int x = event->x;
    const int y = event->y;
    const int modifiers = (int)event->modifiers;
    runOnMain([ws, x, y, type, mouse_up, click_count, modifiers]() {
        if (WidgetRenderer *renderer = currentRenderer(ws))
            renderer->sendMouseClick(x, y, type, mouse_up, click_count,
                                     modifiers);
    });
}

void widget_mouse_move(void *data, const obs_mouse_event *event,
                       bool mouse_leave)
{
    auto *ws = static_cast<WidgetSource *>(data);
    if (!ws || !ws->interactive.load())
        return;

    const int x = event->x;
    const int y = event->y;
    const int modifiers = (int)event->modifiers;
    runOnMain([ws, x, y, mouse_leave, modifiers]() {
        if (WidgetRenderer *renderer = currentRenderer(ws))
            renderer->sendMouseMove(x, y, mouse_leave, modifiers);
    });
}

void widget_mouse_wheel(void *data, const obs_mouse_event *event, int x_delta,
                        int y_delta)
{
    auto *ws = static_cast<WidgetSource *>(data);
    if (!ws || !ws->interactive.load())
        return;

    const int x = event->x;
    const int y = event->y;
    const int modifiers = (int)event->modifiers;
    runOnMain([ws, x, y, x_delta, y_delta, modifiers]() {
        if (WidgetRenderer *renderer = currentRenderer(ws))
            renderer->sendMouseWheel(x, y, x_delta, y_delta, modifiers);
    });
}

void widget_focus(void *data, bool focus)
{
    auto *ws = static_cast<WidgetSource *>(data);
    if (!ws || !ws->interactive.load())
        return;

    runOnMain([ws, focus]() {
        if (WidgetRenderer *renderer = currentRenderer(ws))
            renderer->setInputFocus(focus);
    });
}

void widget_key_click(void *data, const obs_key_event *event, bool key_up)
{
    auto *ws = static_cast<WidgetSource *>(data);
    if (!ws || !ws->interactive.load())
        return;

    const QString text =
        QString::fromUtf8(event->text ? event->text : "");
    const int obsKey =
        (int)obs_key_from_virtual_key((int)event->native_vkey);
    const int modifiers = (int)event->modifiers;
    const uint32_t scancode = event->native_scancode;
    runOnMain([ws, text, obsKey, key_up, modifiers, scancode]() {
        if (WidgetRenderer *renderer = currentRenderer(ws))
            renderer->sendKey(obsKey, text, key_up, modifiers, scancode);
    });
}

void widget_video_tick(void *data, float)
{
    auto *ws = static_cast<WidgetSource *>(data);

    // The renderer may be destroyed concurrently (on the main thread);
    // holding frameMutex across takeFrame() guarantees the pointer stays
    // valid for the duration of the capture.
    QMutexLocker lock(&ws->frameMutex);
    if (WidgetRenderer *renderer = ws->renderer.load()) {
        QImage frame = renderer->takeFrame();
        if (!frame.isNull())
            ws->frame = std::move(frame);
    }
}

void widget_video_render(void *data, gs_effect_t *)
{
    auto *ws = static_cast<WidgetSource *>(data);
    WidgetRenderer *renderer;
    {
        QMutexLocker lock(&ws->frameMutex);
        renderer = ws->renderer.load();
    }
    if (!renderer)
        return;

    QImage frame;
    {
        QMutexLocker lock(&ws->frameMutex);
        frame = ws->frame;
    }
    if (frame.isNull() || frame.format() != QImage::Format_RGBA8888_Premultiplied)
        return;

    const int width = frame.width();
    const int height = frame.height();

    if (!ws->texture || ws->texture_width != (uint32_t)width ||
        ws->texture_height != (uint32_t)height) {
        if (ws->texture)
            gs_texture_destroy(ws->texture);
        ws->texture = gs_texture_create(width, height, GS_RGBA, 1, nullptr,
                                        GS_DYNAMIC);
        ws->texture_width = width;
        ws->texture_height = height;
        if (!ws->texture)
            return;
    }

    // Premultiplied alpha, like obs-browser: blend with ONE/INVSRCALPHA.
    gs_texture_set_image(ws->texture, frame.constBits(), width * 4, false);

    const bool previous = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture_srgb(image, ws->texture);

    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(ws->texture, 0, 0, 0);

    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(previous);
}

} // namespace

void register_widget_browser_source()
{
    static obs_source_info info = {};
    info.id = "widget_browser_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB |
                        OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_INTERACTION |
                        OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = widget_get_name;
    info.get_width = widget_get_width;
    info.get_height = widget_get_height;
    info.create = widget_create;
    info.destroy = widget_destroy;
    info.update = widget_update;
    info.get_defaults = widget_defaults;
    info.get_properties = widget_properties;
    info.mouse_click = widget_mouse_click;
    info.mouse_move = widget_mouse_move;
    info.mouse_wheel = widget_mouse_wheel;
    info.focus = widget_focus;
    info.key_click = widget_key_click;
    info.video_tick = widget_video_tick;
    info.video_render = widget_video_render;
    info.icon_type = OBS_ICON_TYPE_BROWSER;
    obs_register_source(&info);
}
