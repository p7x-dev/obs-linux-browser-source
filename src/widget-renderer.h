#pragma once

#include <QImage>
#include <QString>
#include <QVector>
#include <QMutex>
#include <QObject>
#include <memory>

class QQuickRenderControl;
class QQuickWindow;
class QQmlEngine;
class QQmlComponent;
class QQuickItem;
class QTimer;
class QRhi;
class QRhiTexture;
class QRhiRenderBuffer;
class QRhiTextureRenderTarget;
class QRhiRenderPassDescriptor;
class QNetworkAccessManager;

// One endpoint discovered in the widget page (fetch/XHR/beacon/websocket or
// declared by the page itself).
struct WidgetEndpoint
{
    QString method;
    QString url;
    QString type;
};

// Exposed to the page through Qt WebChannel as `widget`. The injected script
// calls reportEndpoint() for every request it observes.
class WidgetBridge : public QObject
{
    Q_OBJECT

public:
    explicit WidgetBridge(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    void reportEndpoint(const QString &method, const QString &url,
                        const QString &type)
    {
        emit endpointFound(method, url, type);
    }

signals:
    void endpointFound(const QString &method, const QString &url,
                       const QString &type);
};

// Off-screen Qt WebEngine renderer.
//
// Renders a WebEngineView (Chromium) into an FBO via QQuickRenderControl and
// exposes the latest frame as a premultiplied RGBA QImage. All Qt objects are
// created and used on the application's main thread; the OBS side marshals
// calls there (see widget-source.cpp).
class WidgetRenderer
{
public:
    explicit WidgetRenderer(QString url, int width, int height, int fps);
    ~WidgetRenderer();

    WidgetRenderer(const WidgetRenderer &) = delete;
    WidgetRenderer &operator=(const WidgetRenderer &) = delete;

    bool isValid() const { return m_valid; }

    // Latest captured frame (premultiplied RGBA8888), may be null.
    // Safe to call from any thread.
    QImage takeFrame();

    // Endpoints observed in the page. Thread-safe.
    QVector<WidgetEndpoint> endpoints() const;

    void setUrl(const QString &url);
    void setSize(int width, int height);
    void setFps(int fps);
    void reload();

    // Executes an action: either fetch() inside the page (keeps cookies and
    // session) or an HTTP request sent by the plugin itself.
    void executeInPage(const QString &url, const QString &method);
    void executeHttp(const QString &url, const QString &method);

    // Input forwarding for OBS' interaction window. Coordinates are source
    // pixels; OBS mouse button constants are MOUSE_LEFT/MIDDLE/RIGHT.
    void sendMouseClick(int x, int y, int button, bool up, uint32_t clickCount,
                        int modifiers);
    void sendMouseMove(int x, int y, bool leave, int modifiers);
    void sendMouseWheel(int x, int y, int xDelta, int yDelta, int modifiers);
    void sendKey(int obsKey, const QString &text, bool up, int modifiers,
                 uint32_t nativeScancode);
    void setInputFocus(bool focus);

private:
    void renderFrame();
    void restartTimer();
    void updateViewportSize();
    void recreateRenderTarget();
    void onEndpoint(const QString &method, const QString &url,
                    const QString &type);

    bool m_valid = false;
    int m_width = 800;
    int m_height = 600;
    int m_fps = 30;
    QString m_url;

    std::unique_ptr<WidgetBridge> m_bridge;
    std::unique_ptr<QNetworkAccessManager> m_network;

    std::unique_ptr<QQuickRenderControl> m_renderControl;
    std::unique_ptr<QQuickWindow> m_window;
    std::unique_ptr<QQmlEngine> m_qmlEngine;
    std::unique_ptr<QQmlComponent> m_qmlComponent;
    QQuickItem *m_rootItem = nullptr;

    QRhi *m_rhi = nullptr;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiRenderBuffer> m_depthStencil;
    std::unique_ptr<QRhiTextureRenderTarget> m_renderTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPassDesc;

    std::unique_ptr<QTimer> m_timer;

    mutable QMutex m_frameMutex;
    QImage m_frame;

    mutable QMutex m_endpointsMutex;
    QVector<WidgetEndpoint> m_endpoints;

    Qt::MouseButtons m_mouseButtons = Qt::NoButton;
};
