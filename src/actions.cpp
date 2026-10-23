#include "actions.h"

#include <QCryptographicHash>
#include <QSet>
#include <QStringList>
#include <QUrl>

QString actionHotkeyKey(const QString &method, const QString &url)
{
    const QByteArray source =
        (method.toUpper() + QLatin1Char('\n') + url).toUtf8();
    const QByteArray hash =
        QCryptographicHash::hash(source, QCryptographicHash::Sha1).toHex();
    return QStringLiteral("action_") +
           QString::fromLatin1(hash.left(16));
}

QString defaultActionName(const QString &method, const QString &url)
{
    const QUrl parsed(url);
    QString path = parsed.path();
    if (path.isEmpty() || path == QLatin1String("/"))
        path = parsed.host();
    if (path.isEmpty())
        path = url;
    return QStringLiteral("%1 %2").arg(method.toUpper(), path);
}

void normalizeAction(WidgetAction &action)
{
    action.method = action.method.trimmed().toUpper();
    if (action.method.isEmpty())
        action.method = QStringLiteral("GET");

    action.url = action.url.trimmed();
    action.name = action.name.trimmed();
    action.name.remove(QLatin1Char('|'));
    if (action.name.isEmpty())
        action.name = defaultActionName(action.method, action.url);

    action.hotkeyKey = actionHotkeyKey(action.method, action.url);
}

QVector<WidgetAction> parseActions(obs_data_t *settings)
{
    QVector<WidgetAction> actions;
    obs_data_array_t *array = obs_data_get_array(settings, kActionsSettingsKey);
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

        normalizeAction(action);
        if (seen.contains(action.hotkeyKey))
            continue;
        seen.insert(action.hotkeyKey);
        actions.append(action);
    }

    obs_data_array_release(array);
    return actions;
}

void storeActions(obs_data_t *settings, const QVector<WidgetAction> &actions)
{
    obs_data_array_t *array = obs_data_array_create();

    QSet<QString> seen;
    for (const WidgetAction &action : actions) {
        if (action.url.isEmpty())
            continue;

        WidgetAction normalized = action;
        normalizeAction(normalized);
        if (seen.contains(normalized.hotkeyKey))
            continue;
        seen.insert(normalized.hotkeyKey);

        const QString line =
            QStringLiteral("%1 | %2 | %3 | %4")
                .arg(normalized.name)
                .arg(normalized.method)
                .arg(normalized.url)
                .arg(normalized.inPage ? QStringLiteral("page")
                                       : QStringLiteral("plugin"));

        obs_data_t *item = obs_data_create();
        obs_data_set_string(item, "value", line.toUtf8().constData());
        obs_data_array_push_back(array, item);
        obs_data_release(item);
    }

    obs_data_set_array(settings, kActionsSettingsKey, array);
    obs_data_array_release(array);
}
