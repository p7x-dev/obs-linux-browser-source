#include "widget-renderer.h"

#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QQuickRenderTarget>
#include <QQuickItem>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QWebEngineScript>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QWheelEvent>
#include <QColor>
#include <QUrl>

#include <QOpenGLContext>

#if __has_include(<QtGui/rhi/qrhi_platform.h>)
#include <QtGui/rhi/qrhi_platform.h>
#endif

#if __has_include(<QtGui/rhi/qrhi.h>)
#include <QtGui/rhi/qrhi.h>
#elif __has_include(<rhi/qrhi.h>)
#include <rhi/qrhi.h>
#endif

#include <obs-module.h>
#include <obs-interaction.h>

namespace {

constexpr const char *kQmlSource = "qrc:/qml/WidgetView.qml";

QString buildDiscoveryScript()
{
    QString script;

    QFile qwebchannel(QStringLiteral(":/qml/qwebchannel.js"));
    if (qwebchannel.open(QIODevice::ReadOnly))
        script = QString::fromUtf8(qwebchannel.readAll());
    else
        blog(LOG_WARNING,
             "[widget-renderer] qwebchannel.js resource is missing");

    QFile discovery(QStringLiteral(":/qml/discovery.js"));
    if (discovery.open(QIODevice::ReadOnly))
        script += QString::fromUtf8(discovery.readAll());
    else
        blog(LOG_WARNING,
             "[widget-renderer] discovery.js resource is missing");

    return script;
}

void installDiscoveryScript(QQuickItem *rootItem)
{
    if (!rootItem)
        return;

    QWebEngineScript script;
    script.setName(QStringLiteral("obs-widget-discovery"));
    script.setSourceCode(buildDiscoveryScript());
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setWorldId(QWebEngineScript::MainWorld);
    script.setRunsOnSubFrames(false);

    // The WebEngineView QML type exposes userScripts as a read-only
    // collection; insert() is the only public way to add a script to it.
    if (!QMetaObject::invokeMethod(
            rootItem, "installUserScript",
            Q_ARG(QVariant, QVariant::fromValue(script))))
        blog(LOG_WARNING,
             "[widget-renderer] cannot install endpoint discovery script");
}

Qt::KeyboardModifiers toQtModifiers(int modifiers)
{
    Qt::KeyboardModifiers out;
    if (modifiers & INTERACT_SHIFT_KEY)
        out |= Qt::ShiftModifier;
    if (modifiers & INTERACT_CONTROL_KEY)
        out |= Qt::ControlModifier;
    if (modifiers & INTERACT_ALT_KEY)
        out |= Qt::AltModifier;
    if (modifiers & INTERACT_COMMAND_KEY)
        out |= Qt::MetaModifier;
    return out;
}

Qt::Key toQtKey(int key, const QString &text)
{
    if (key >= OBS_KEY_A && key <= OBS_KEY_Z)
        return (Qt::Key)(Qt::Key_A + (key - OBS_KEY_A));
    if (key >= OBS_KEY_0 && key <= OBS_KEY_9)
        return (Qt::Key)(Qt::Key_0 + (key - OBS_KEY_0));
    if (key >= OBS_KEY_F1 && key <= OBS_KEY_F12)
        return (Qt::Key)(Qt::Key_F1 + (key - OBS_KEY_F1));

    switch (key) {
    case OBS_KEY_RETURN:
        return Qt::Key_Return;
    case OBS_KEY_ENTER:
        return Qt::Key_Enter;
    case OBS_KEY_ESCAPE:
        return Qt::Key_Escape;
    case OBS_KEY_TAB:
        return Qt::Key_Tab;
    case OBS_KEY_BACKTAB:
        return Qt::Key_Backtab;
    case OBS_KEY_BACKSPACE:
        return Qt::Key_Backspace;
    case OBS_KEY_INSERT:
        return Qt::Key_Insert;
    case OBS_KEY_DELETE:
        return Qt::Key_Delete;
    case OBS_KEY_HOME:
        return Qt::Key_Home;
    case OBS_KEY_END:
        return Qt::Key_End;
    case OBS_KEY_LEFT:
        return Qt::Key_Left;
    case OBS_KEY_UP:
        return Qt::Key_Up;
    case OBS_KEY_RIGHT:
        return Qt::Key_Right;
    case OBS_KEY_DOWN:
        return Qt::Key_Down;
    case OBS_KEY_PAGEUP:
        return Qt::Key_PageUp;
    case OBS_KEY_PAGEDOWN:
        return Qt::Key_PageDown;
    case OBS_KEY_SPACE:
        return Qt::Key_Space;
    case OBS_KEY_SHIFT:
        return Qt::Key_Shift;
    case OBS_KEY_CONTROL:
        return Qt::Key_Control;
    case OBS_KEY_ALT:
        return Qt::Key_Alt;
    case OBS_KEY_META:
        return Qt::Key_Meta;
    case OBS_KEY_CAPSLOCK:
        return Qt::Key_CapsLock;
    case OBS_KEY_NUMLOCK:
        return Qt::Key_NumLock;
    case OBS_KEY_SCROLLLOCK:
        return Qt::Key_ScrollLock;
    case OBS_KEY_PAUSE:
        return Qt::Key_Pause;
    case OBS_KEY_PRINT:
        return Qt::Key_Print;
    case OBS_KEY_SEMICOLON:
        return Qt::Key_Semicolon;
    case OBS_KEY_QUOTE:
        return Qt::Key_Apostrophe;
    case OBS_KEY_COMMA:
        return Qt::Key_Comma;
    case OBS_KEY_PERIOD:
        return Qt::Key_Period;
    case OBS_KEY_SLASH:
        return Qt::Key_Slash;
    case OBS_KEY_BACKSLASH:
        return Qt::Key_Backslash;
    case OBS_KEY_BRACKETLEFT:
        return Qt::Key_BracketLeft;
    case OBS_KEY_BRACKETRIGHT:
        return Qt::Key_BracketRight;
    case OBS_KEY_MINUS:
        return Qt::Key_Minus;
    case OBS_KEY_EQUAL:
        return Qt::Key_Equal;
    default:
        break;
    }

    if (!text.isEmpty()) {
        const QChar ch = text.at(0);
        if (ch.unicode() >= 0x20)
            return (Qt::Key)ch.toUpper().unicode();
    }

    return Qt::Key_unknown;
}

} // namespace

WidgetRenderer::WidgetRenderer(QString url, int width, int height, int fps)
    : m_width(width), m_height(height), m_fps(fps), m_url(std::move(url))
{
    m_bridge = std::make_unique<WidgetBridge>();
    m_bridge->setObjectName(QStringLiteral("widget"));
    QObject::connect(
        m_bridge.get(), &WidgetBridge::endpointFound, m_bridge.get(),
        [this](const QString &method, const QString &url, const QString &type) {
            onEndpoint(method, url, type);
        });

    m_network = std::make_unique<QNetworkAccessManager>();

    m_renderControl = std::make_unique<QQuickRenderControl>();
    m_window = std::make_unique<QQuickWindow>(m_renderControl.get());
    // Transparent clear color: Chromium already delivers per-pixel alpha for
    // transparent pages; the window must not paint over it with white.
    m_window->setColor(QColor(Qt::transparent));

    m_qmlEngine = std::make_unique<QQmlEngine>();
    m_qmlEngine->rootContext()->setContextProperty("obsBridge", m_bridge.get());
    m_qmlComponent =
        std::make_unique<QQmlComponent>(m_qmlEngine.get(), QUrl(kQmlSource));
    if (m_qmlComponent->isError()) {
        for (const QQmlError &err : m_qmlComponent->errors())
            qWarning("WidgetRenderer: QML error: %s", qPrintable(err.toString()));
        return;
    }

    QObject *rootObject = m_qmlComponent->create();
    m_rootItem = qobject_cast<QQuickItem *>(rootObject);
    if (!m_rootItem) {
        qWarning("WidgetRenderer: QML root is not a QQuickItem");
        delete rootObject;
        return;
    }

    // Attach the scene to the offscreen window BEFORE any size is reported:
    // without a window, Chromium never receives visual properties and keeps
    // a 0x0 viewport (blank frames).
    m_rootItem->setParentItem(m_window->contentItem());

    installDiscoveryScript(m_rootItem);

    updateViewportSize();

    if (!m_renderControl->initialize()) {
        qWarning("WidgetRenderer: QQuickRenderControl::initialize() failed");
        return;
    }

    m_rhi = m_renderControl->rhi();
    if (!m_rhi) {
        qWarning("WidgetRenderer: no QRhi available");
        return;
    }

    recreateRenderTarget();
    if (!m_renderTarget)
        return;

    m_timer = std::make_unique<QTimer>();
    m_timer->setTimerType(Qt::PreciseTimer);
    QObject::connect(m_timer.get(), &QTimer::timeout, [this] { renderFrame(); });
    restartTimer();

    m_valid = true;
    blog(LOG_INFO, "[widget-renderer] initialized %dx%d @%d fps", m_width,
         m_height, m_fps);

    if (!m_url.isEmpty()) {
        QTimer::singleShot(0, [this] { setUrl(m_url); });
    }
}

WidgetRenderer::~WidgetRenderer()
{
    m_timer.reset();
    if (m_rootItem) {
        // Qt WebEngine asserts (Q_ASSERT(rvh)) when a FocusOut is forwarded
        // during teardown of a half-destroyed WebContents. Trigger the
        // visibility/focus transitions while the contents are still alive,
        // so no focus event fires during the actual deletion.
        m_rootItem->setVisible(false);
        m_rootItem->setProperty("focus", false);
        delete m_rootItem;
    }
    m_renderPassDesc.reset();
    m_renderTarget.reset();
    m_depthStencil.reset();
    m_texture.reset();
    m_qmlComponent.reset();
    m_qmlEngine.reset();
    m_window.reset();
    m_renderControl.reset();
}

void WidgetRenderer::setUrl(const QString &url)
{
    m_url = url;
    if (m_valid && m_rootItem) {
        // Set via the QML engine so the property write happens through the
        // binding machinery (direct setProperty before lazy init is ignored).
        m_rootItem->setProperty("url", QUrl::fromUserInput(url));
    }
}

void WidgetRenderer::setSize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    if (width == m_width && height == m_height)
        return;
    m_width = width;
    m_height = height;
    blog(LOG_INFO, "[widget-renderer] resize to %dx%d", width, height);
    if (!m_valid)
        return;
    updateViewportSize();
    recreateRenderTarget();
}

void WidgetRenderer::setFps(int fps)
{
    if (fps <= 0)
        return;
    m_fps = fps;
    if (m_timer)
        restartTimer();
}

void WidgetRenderer::reload()
{
    if (m_valid && m_rootItem)
        QMetaObject::invokeMethod(m_rootItem, "reload");
}

void WidgetRenderer::renderFrame()
{
    if (!m_valid)
        return;

    // OBS switches the GL context on the main thread directly via
    // eglMakeCurrent/glXMakeCurrent, bypassing QOpenGLContext, which leaves
    // Qt's "current context" cache stale. The QRhi GL backend then skips its
    // own makeCurrent and all GL calls go to OBS's context - crashing the
    // Mesa driver. Re-assert our context explicitly on every frame.
    if (m_rhi) {
        if (auto *handles = static_cast<const QRhiGles2NativeHandles *>(
                m_rhi->nativeHandles())) {
            if (handles->context && handles->context->surface())
                handles->context->makeCurrent(handles->context->surface());
        }
    }

    m_renderControl->polishItems();
    m_renderControl->beginFrame();
    m_renderControl->sync();
    m_renderControl->render();

    QRhiReadbackResult readResult;
    QRhiResourceUpdateBatch *batch = m_rhi->nextResourceUpdateBatch();
    batch->readBackTexture(m_texture.get(), &readResult);
    m_renderControl->commandBuffer()->resourceUpdate(batch);
    m_renderControl->endFrame();

    if (readResult.data.isEmpty())
        return;


    // Chromium 140's software compositor delivers premultiplied RGBA
    // (byte order R,G,B,A) - wrap the readback directly as such.
    QImage frame(reinterpret_cast<const uchar *>(readResult.data.constData()),
                 readResult.pixelSize.width(), readResult.pixelSize.height(),
                 QImage::Format_RGBA8888_Premultiplied);
    if (m_rhi->isYUpInFramebuffer())
        frame = frame.flipped(Qt::Vertical);

    QMutexLocker lock(&m_frameMutex);
    m_frame = std::move(frame);
}

QImage WidgetRenderer::takeFrame()
{
    QMutexLocker lock(&m_frameMutex);
    return m_frame;
}

QVector<WidgetEndpoint> WidgetRenderer::endpoints() const
{
    QMutexLocker lock(&m_endpointsMutex);
    return m_endpoints;
}

void WidgetRenderer::onEndpoint(const QString &method, const QString &url,
                                const QString &type)
{
    QMutexLocker lock(&m_endpointsMutex);
    for (const WidgetEndpoint &endpoint : m_endpoints) {
        if (endpoint.method == method && endpoint.url == url)
            return;
    }
    m_endpoints.append({method, url, type});
    blog(LOG_DEBUG, "[widget-renderer] endpoint %s %s (%s)",
         method.toUtf8().constData(), url.toUtf8().constData(),
         type.toUtf8().constData());
}

void WidgetRenderer::executeInPage(const QString &url, const QString &method)
{
    if (!m_valid || !m_rootItem)
        return;

    QJsonObject options;
    options.insert(QStringLiteral("url"), url);
    options.insert(QStringLiteral("method"), method.isEmpty() ? "GET" : method);
    const QByteArray json =
        QJsonDocument(options).toJson(QJsonDocument::Compact);
    const QString script =
        QStringLiteral("window.__obsRunAction && window.__obsRunAction(%1);")
            .arg(QString::fromUtf8(json));

    const bool invoked = QMetaObject::invokeMethod(
        m_rootItem, "runJs", Q_ARG(QVariant, script));
    if (!invoked)
        blog(LOG_WARNING, "[widget-renderer] failed to run page action");
}

void WidgetRenderer::executeHttp(const QString &url, const QString &method)
{
    if (!m_network || url.isEmpty())
        return;

    QUrl target(url);
    if (target.isRelative()) {
        const QUrl base = QUrl::fromUserInput(m_url);
        if (!base.isValid()) {
            blog(LOG_WARNING,
                 "[widget-renderer] cannot resolve action URL '%s'",
                 url.toUtf8().constData());
            return;
        }
        target = base.resolved(target);
    }
    if (!target.isValid()) {
        blog(LOG_WARNING, "[widget-renderer] invalid action URL '%s'",
             url.toUtf8().constData());
        return;
    }

    QNetworkRequest request(target);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("obs-widget-browser"));

    const QString verb = method.isEmpty() ? QStringLiteral("GET") : method;
    QNetworkReply *reply = nullptr;
    if (verb.compare("GET", Qt::CaseInsensitive) == 0)
        reply = m_network->get(request);
    else if (verb.compare("POST", Qt::CaseInsensitive) == 0)
        reply = m_network->post(request, QByteArray());
    else
        reply = m_network->sendCustomRequest(request, verb.toUtf8(),
                                             QByteArray());

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, target]() {
        if (reply->error() != QNetworkReply::NoError)
            blog(LOG_WARNING, "[widget-renderer] action %s failed: %s",
                 target.toString().toUtf8().constData(),
                 reply->errorString().toUtf8().constData());
        reply->deleteLater();
    });
}

void WidgetRenderer::sendMouseClick(int x, int y, int button, bool up,
                                    uint32_t clickCount, int modifiers)
{
    UNUSED_PARAMETER(clickCount);
    if (!m_valid || !m_window)
        return;

    Qt::MouseButton qtButton = Qt::NoButton;
    switch (button) {
    case MOUSE_LEFT:
        qtButton = Qt::LeftButton;
        break;
    case MOUSE_MIDDLE:
        qtButton = Qt::MiddleButton;
        break;
    case MOUSE_RIGHT:
        qtButton = Qt::RightButton;
        break;
    default:
        return;
    }

    if (up)
        m_mouseButtons &= ~Qt::MouseButtons(qtButton);
    else
        m_mouseButtons |= qtButton;

    const QPointF pos(x, y);
    QMouseEvent event(up ? QEvent::MouseButtonRelease : QEvent::MouseButtonPress,
                      pos, pos, pos, qtButton, m_mouseButtons,
                      toQtModifiers(modifiers));
    QCoreApplication::sendEvent(m_window.get(), &event);

    if (!up && m_rootItem)
        m_rootItem->forceActiveFocus(Qt::MouseFocusReason);
}

void WidgetRenderer::sendMouseMove(int x, int y, bool leave, int modifiers)
{
    if (!m_valid || !m_window)
        return;

    if (leave) {
        QEvent event(QEvent::Leave);
        QCoreApplication::sendEvent(m_window.get(), &event);
        return;
    }

    const QPointF pos(x, y);
    QMouseEvent event(QEvent::MouseMove, pos, pos, pos, Qt::NoButton,
                      m_mouseButtons, toQtModifiers(modifiers));
    QCoreApplication::sendEvent(m_window.get(), &event);
}

void WidgetRenderer::sendMouseWheel(int x, int y, int xDelta, int yDelta,
                                    int modifiers)
{
    if (!m_valid || !m_window)
        return;

    const QPointF pos(x, y);
    QWheelEvent event(pos, pos, QPoint(), QPoint(xDelta, yDelta),
                      m_mouseButtons, toQtModifiers(modifiers),
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(m_window.get(), &event);
}

void WidgetRenderer::sendKey(int obsKey, const QString &text, bool up,
                             int modifiers, uint32_t nativeScancode)
{
    if (!m_valid || !m_window)
        return;

    QKeyEvent event(up ? QEvent::KeyRelease : QEvent::KeyPress,
                    toQtKey(obsKey, text), toQtModifiers(modifiers), text,
                    false, nativeScancode);
    QCoreApplication::sendEvent(m_window.get(), &event);
}

void WidgetRenderer::setInputFocus(bool focus)
{
    if (!m_valid || !m_rootItem)
        return;

    if (focus)
        m_rootItem->forceActiveFocus(Qt::MouseFocusReason);
    else
        m_rootItem->setFocus(false);
}

void WidgetRenderer::restartTimer()
{
    m_timer->start(1000 / m_fps);
}

void WidgetRenderer::updateViewportSize()
{
    if (!m_rootItem)
        return;
    m_rootItem->setSize(QSizeF(m_width, m_height));
    m_window->contentItem()->setSize(QSizeF(m_width, m_height));
    m_window->setGeometry(0, 0, m_width, m_height);
}

void WidgetRenderer::recreateRenderTarget()
{
    m_renderPassDesc.reset();
    m_renderTarget.reset();
    m_depthStencil.reset();
    m_texture.reset();
    if (!m_rhi)
        return;

    const QSize pixelSize(m_width, m_height);
    m_texture.reset(m_rhi->newTexture(
        QRhiTexture::RGBA8, pixelSize, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!m_texture->create())
        return;
    m_depthStencil.reset(
        m_rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, 1));
    if (!m_depthStencil->create())
        return;
    QRhiTextureRenderTargetDescription rtDesc(QRhiColorAttachment(m_texture.get()));
    rtDesc.setDepthStencilBuffer(m_depthStencil.get());
    m_renderTarget.reset(m_rhi->newTextureRenderTarget(rtDesc));
    m_renderPassDesc.reset(m_renderTarget->newCompatibleRenderPassDescriptor());
    m_renderTarget->setRenderPassDescriptor(m_renderPassDesc.get());
    if (!m_renderTarget->create())
        return;
    m_window->setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(m_renderTarget.get()));
}