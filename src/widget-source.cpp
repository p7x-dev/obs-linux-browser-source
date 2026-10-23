#include "widget-source.h"

#include "actions-dialog.h"
#include "actions.h"
#include "widget-renderer.h"

#include <QApplication>
#include <QCoreApplication>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QString>
#include <QThread>
#include <atomic>
#include <functional>

#include <obs.h>
#include <util/dstr.h>

namespace {

constexpr const char *S_URL = "url";
constexpr const char *S_WIDTH = "width";
constexpr const char *S_HEIGHT = "height";
constexpr const char *S_FPS = "fps";
constexpr const char *S_INTERACTIVE = "interactive";

constexpr int DEFAULT_WIDTH = 800;
constexpr int DEFAULT_HEIGHT = 600;
constexpr int DEFAULT_FPS = 30;
constexpr int MIN_DIMENSION = 64;
constexpr int MAX_DIMENSION = 4096;
constexpr int MIN_FPS = 5;
constexpr int MAX_FPS = 60;

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

bool readInteractive(obs_data_t *settings)
{
    // Sources saved before the setting existed must keep input enabled.
    if (!obs_data_has_user_value(settings, S_INTERACTIVE))
        return true;
    return obs_data_get_bool(settings, S_INTERACTIVE);
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

QHash<QString, QString> currentHotkeyBindings(WidgetSource *ws)
{
    QHash<obs_hotkey_id, QString> keyById;
    {
        QMutexLocker lock(&ws->frameMutex);
        for (const HotkeyReg &reg : ws->hotkeys)
            keyById.insert(reg.id, reg.action.hotkeyKey);
    }

    struct BindingContext {
        const QHash<obs_hotkey_id, QString> *keyById;
        QHash<QString, QString> *result;
    };

    QHash<QString, QString> result;
    BindingContext context{&keyById, &result};
    obs_enum_hotkey_bindings(
        [](void *data, size_t, obs_hotkey_binding_t *binding) -> bool {
            auto *ctx = static_cast<BindingContext *>(data);
            const auto it = ctx->keyById->find(
                obs_hotkey_binding_get_hotkey_id(binding));
            if (it == ctx->keyById->end())
                return true;

            struct dstr str = {};
            obs_key_combination_to_str(
                obs_hotkey_binding_get_key_combination(binding), &str);
            if (str.array && *str.array) {
                const QString combo = QString::fromUtf8(str.array);
                QString &target = (*ctx->result)[it.value()];
                if (!target.isEmpty())
                    target += QStringLiteral(", ");
                target += combo;
            }
            dstr_free(&str);
            return true;
        },
        &context);

    return result;
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
    obs_data_set_default_array(settings, kActionsSettingsKey, empty);
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

bool widget_open_actions_dialog(obs_properties_t *, obs_property_t *,
                                void *data)
{
    auto *ws = static_cast<WidgetSource *>(data);
    if (!ws || !ws->source)
        return false;

    QVector<WidgetEndpoint> endpoints;
    if (WidgetRenderer *renderer = currentRenderer(ws))
        endpoints = renderer->endpoints();

    obs_data_t *settings = obs_source_get_settings(ws->source);
    const QVector<WidgetAction> actions = parseActions(settings);
    obs_data_release(settings);

    const QHash<QString, QString> bindings = currentHotkeyBindings(ws);
    obs_weak_source_t *weak = obs_source_get_weak_source(ws->source);

    QWidget *parent = QApplication::activeWindow();

    ActionsDialog dialog(endpoints, actions, bindings, parent);
    if (dialog.exec() != QDialog::Accepted) {
        obs_weak_source_release(weak);
        return false;
    }

    obs_source_t *source = obs_weak_source_get_source(weak);
    obs_weak_source_release(weak);
    if (!source)
        return false;

    obs_data_t *newSettings = obs_source_get_settings(source);
    storeActions(newSettings, dialog.actions());
    obs_source_update(source, newSettings);
    obs_data_release(newSettings);
    obs_source_release(source);
    return true;
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

    int endpointCount = 0;
    int actionCount = 0;
    if (ws) {
        if (WidgetRenderer *renderer = currentRenderer(ws))
            endpointCount = renderer->endpoints().size();
        QMutexLocker lock(&ws->frameMutex);
        actionCount = ws->actions.size();
    }

    const QString summary =
        QString::fromUtf8(obs_module_text("WidgetBrowser.Endpoints.Summary"))
            .arg(endpointCount)
            .arg(actionCount);

    obs_properties_t *actionsAndEndpoints = obs_properties_create();
    obs_property_t *summaryProp = obs_properties_add_text(
        actionsAndEndpoints, "endpoints_summary",
        summary.toUtf8().constData(), OBS_TEXT_INFO);
    obs_property_text_set_info_type(summaryProp, OBS_TEXT_INFO_NORMAL);
    obs_property_text_set_info_word_wrap(summaryProp, true);
    obs_properties_add_button2(
        actionsAndEndpoints, "open_actions",
        obs_module_text("WidgetBrowser.ActionsDialog.Open"),
        widget_open_actions_dialog, data);
    obs_properties_add_group(
        props, "actions_endpoints_group",
        obs_module_text("WidgetBrowser.Group.ActionsAndEndpoints"),
        OBS_GROUP_NORMAL, actionsAndEndpoints);

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
