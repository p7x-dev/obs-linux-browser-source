#pragma once

#include <QDialog>
#include <QHash>
#include <QVector>

#include "actions.h"
#include "widget-renderer.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

// Editor for discovered widget endpoints and the actions bound to them.
// Actions are still stored in the source settings in the compact
// "Name | METHOD | URL | page/plugin" format; hotkeys are assigned by OBS.
class ActionsDialog : public QDialog
{
    Q_OBJECT

public:
    ActionsDialog(const QVector<WidgetEndpoint> &endpoints,
                  const QVector<WidgetAction> &actions,
                  const QHash<QString, QString> &hotkeys,
                  QWidget *parent = nullptr);

    QVector<WidgetAction> actions() const;

private:
    enum EndpointColumn {
        EndpointMethod,
        EndpointUrl,
        EndpointType,
        EndpointColumnCount,
    };

    enum ActionColumn {
        ActionName,
        ActionMethod,
        ActionUrl,
        ActionTarget,
        ActionHotkey,
        ActionColumnCount,
    };

    void buildUi();
    void populateEndpoints();
    void populateActions(const QVector<WidgetAction> &actions);
    void addActionRow(const WidgetAction &action);
    void addSelectedEndpoints();
    void removeSelectedActions();
    void moveSelectedAction(int delta);
    void updateHotkeyCell(int row);
    void filterEndpoints(const QString &text);
    WidgetAction actionAt(int row) const;
    QVector<WidgetAction> readRows() const;

    QVector<WidgetEndpoint> m_endpoints;
    QHash<QString, QString> m_hotkeys;

    QTableWidget *m_endpointsTable = nullptr;
    QTableWidget *m_actionsTable = nullptr;
    QLineEdit *m_filter = nullptr;
    QPushButton *m_addSelected = nullptr;
    QLabel *m_noEndpoints = nullptr;
};
