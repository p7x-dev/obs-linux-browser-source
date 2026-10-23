#pragma once

#include <QString>
#include <QVector>

#include <obs-data.h>

constexpr const char *kActionsSettingsKey = "actions";

// One user-defined action bound to a discovered (or manually entered)
// endpoint. Persisted as "Name | METHOD | URL | page/plugin".
struct WidgetAction
{
    QString name;
    QString method;
    QString url;
    bool inPage = true;
    QString hotkeyKey;
};

QString actionHotkeyKey(const QString &method, const QString &url);
QString defaultActionName(const QString &method, const QString &url);
void normalizeAction(WidgetAction &action);

QVector<WidgetAction> parseActions(obs_data_t *settings);
void storeActions(obs_data_t *settings, const QVector<WidgetAction> &actions);
