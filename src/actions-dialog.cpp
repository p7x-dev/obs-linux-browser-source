#include "actions-dialog.h"

#include <obs-module.h>

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

QString text(const char *key)
{
    return QString::fromUtf8(obs_module_text(key));
}

void setupTable(QTableWidget *table)
{
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(30);
    table->horizontalHeader()->setHighlightSections(false);
}

bool isHttpUrl(const QString &url)
{
    const QString scheme = QUrl(url).scheme().toLower();
    return scheme == QLatin1String("http") ||
           scheme == QLatin1String("https");
}

} // namespace

ActionsDialog::ActionsDialog(const QVector<WidgetEndpoint> &endpoints,
                             const QVector<WidgetAction> &actions,
                             const QHash<QString, QString> &hotkeys,
                             QWidget *parent)
    : QDialog(parent), m_endpoints(endpoints), m_hotkeys(hotkeys)
{
    setWindowTitle(text("WidgetBrowser.ActionsDialog.Title"));
    setModal(true);
    setMinimumSize(880, 620);
    resize(1040, 780);

    buildUi();
    populateEndpoints();
    populateActions(actions);

    if (m_actionsTable->rowCount() > 0)
        m_actionsTable->setFocus();
    else if (m_endpointsTable->rowCount() > 0)
        m_endpointsTable->setFocus();
    else
        m_filter->setFocus();
}

QVector<WidgetAction> ActionsDialog::actions() const
{
    QVector<WidgetAction> result;
    QSet<QString> seen;

    for (const WidgetAction &raw : readRows()) {
        if (raw.url.isEmpty())
            continue;

        WidgetAction action = raw;
        normalizeAction(action);
        if (seen.contains(action.hotkeyKey))
            continue;
        seen.insert(action.hotkeyKey);
        result.append(action);
    }

    return result;
}

void ActionsDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto *intro = new QLabel(text("WidgetBrowser.ActionsDialog.Intro"), this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *endpointGroup =
        new QGroupBox(text("WidgetBrowser.ActionsDialog.Endpoints"), this);
    auto *endpointLayout = new QVBoxLayout(endpointGroup);
    endpointLayout->setSpacing(8);

    m_filter = new QLineEdit(endpointGroup);
    m_filter->setPlaceholderText(
        text("WidgetBrowser.ActionsDialog.Filter"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this,
            &ActionsDialog::filterEndpoints);
    endpointLayout->addWidget(m_filter);

    m_noEndpoints = new QLabel(
        text("WidgetBrowser.ActionsDialog.NoEndpoints"), endpointGroup);
    m_noEndpoints->setWordWrap(true);
    endpointLayout->addWidget(m_noEndpoints);

    m_endpointsTable =
        new QTableWidget(0, EndpointColumnCount, endpointGroup);
    m_endpointsTable->setHorizontalHeaderLabels(
        {text("WidgetBrowser.ActionsDialog.Method"),
         text("WidgetBrowser.ActionsDialog.Url"),
         text("WidgetBrowser.ActionsDialog.Type")});
    setupTable(m_endpointsTable);
    m_endpointsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_endpointsTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto *endpointHeader = m_endpointsTable->horizontalHeader();
    endpointHeader->setSectionResizeMode(EndpointMethod,
                                         QHeaderView::ResizeToContents);
    endpointHeader->setSectionResizeMode(EndpointUrl, QHeaderView::Stretch);
    endpointHeader->setSectionResizeMode(EndpointType,
                                         QHeaderView::ResizeToContents);
    m_endpointsTable->setMinimumHeight(190);
    endpointLayout->addWidget(m_endpointsTable);

    auto *endpointButtons = new QHBoxLayout;
    m_addSelected =
        new QPushButton(text("WidgetBrowser.ActionsDialog.AddSelected"),
                        endpointGroup);
    m_addSelected->setEnabled(false);
    connect(m_addSelected, &QPushButton::clicked, this,
            &ActionsDialog::addSelectedEndpoints);
    endpointButtons->addWidget(m_addSelected);
    endpointButtons->addStretch();
    endpointLayout->addLayout(endpointButtons);

    connect(m_endpointsTable, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { addSelectedEndpoints(); });

    auto *actionGroup =
        new QGroupBox(text("WidgetBrowser.ActionsDialog.Actions"), this);
    auto *actionLayout = new QVBoxLayout(actionGroup);
    actionLayout->setSpacing(8);

    m_actionsTable = new QTableWidget(0, ActionColumnCount, actionGroup);
    m_actionsTable->setHorizontalHeaderLabels(
        {text("WidgetBrowser.ActionsDialog.Name"),
         text("WidgetBrowser.ActionsDialog.Method"),
         text("WidgetBrowser.ActionsDialog.Url"),
         text("WidgetBrowser.ActionsDialog.Target"),
         text("WidgetBrowser.ActionsDialog.Hotkey")});
    setupTable(m_actionsTable);
    m_actionsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    auto *actionHeader = m_actionsTable->horizontalHeader();
    actionHeader->setSectionResizeMode(ActionName, QHeaderView::Interactive);
    m_actionsTable->setColumnWidth(ActionName, 220);
    actionHeader->setSectionResizeMode(ActionMethod,
                                       QHeaderView::ResizeToContents);
    actionHeader->setSectionResizeMode(ActionUrl, QHeaderView::Stretch);
    actionHeader->setSectionResizeMode(ActionTarget,
                                       QHeaderView::ResizeToContents);
    actionHeader->setSectionResizeMode(ActionHotkey,
                                       QHeaderView::ResizeToContents);
    actionLayout->addWidget(m_actionsTable);

    auto *actionButtons = new QHBoxLayout;
    auto *addButton =
        new QPushButton(text("WidgetBrowser.ActionsDialog.Add"), actionGroup);
    auto *removeButton =
        new QPushButton(text("WidgetBrowser.ActionsDialog.Remove"),
                        actionGroup);
    auto *upButton =
        new QPushButton(text("WidgetBrowser.ActionsDialog.Up"), actionGroup);
    auto *downButton =
        new QPushButton(text("WidgetBrowser.ActionsDialog.Down"), actionGroup);
    connect(addButton, &QPushButton::clicked, this,
            [this]() { addActionRow(WidgetAction{}); });
    connect(removeButton, &QPushButton::clicked, this,
            &ActionsDialog::removeSelectedActions);
    connect(upButton, &QPushButton::clicked, this,
            [this]() { moveSelectedAction(-1); });
    connect(downButton, &QPushButton::clicked, this,
            [this]() { moveSelectedAction(1); });
    actionButtons->addWidget(addButton);
    actionButtons->addWidget(removeButton);
    actionButtons->addStretch();
    actionButtons->addWidget(upButton);
    actionButtons->addWidget(downButton);
    actionLayout->addLayout(actionButtons);

    auto *hotkeyHint = new QLabel(
        text("WidgetBrowser.ActionsDialog.HotkeyHint"), actionGroup);
    hotkeyHint->setWordWrap(true);
    actionLayout->addWidget(hotkeyHint);

    root->addWidget(endpointGroup, 1);
    root->addWidget(actionGroup, 2);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void ActionsDialog::populateEndpoints()
{
    m_endpointsTable->setRowCount(0);

    const bool empty = m_endpoints.isEmpty();
    m_filter->setVisible(!empty);
    m_noEndpoints->setVisible(empty);
    m_endpointsTable->setVisible(!empty);
    m_addSelected->setEnabled(!empty);

    for (int i = 0; i < m_endpoints.size(); ++i) {
        const WidgetEndpoint &endpoint = m_endpoints.at(i);
        const int row = m_endpointsTable->rowCount();
        m_endpointsTable->insertRow(row);

        auto *method = new QTableWidgetItem(endpoint.method);
        method->setData(Qt::UserRole, i);
        method->setToolTip(endpoint.url);
        m_endpointsTable->setItem(row, EndpointMethod, method);

        auto *url = new QTableWidgetItem(endpoint.url);
        url->setToolTip(endpoint.url);
        m_endpointsTable->setItem(row, EndpointUrl, url);

        auto *type = new QTableWidgetItem(endpoint.type);
        type->setToolTip(endpoint.type);
        m_endpointsTable->setItem(row, EndpointType, type);
    }
}

void ActionsDialog::populateActions(const QVector<WidgetAction> &actions)
{
    m_actionsTable->setRowCount(0);
    for (const WidgetAction &action : actions)
        addActionRow(action);

    if (m_actionsTable->rowCount() > 0)
        m_actionsTable->selectRow(0);
}

void ActionsDialog::addActionRow(const WidgetAction &action)
{
    const int row = m_actionsTable->rowCount();
    m_actionsTable->insertRow(row);

    auto *name = new QTableWidgetItem(action.name);
    name->setFlags(name->flags() | Qt::ItemIsEditable);
    m_actionsTable->setItem(row, ActionName, name);

    auto *method = new QComboBox(m_actionsTable);
    method->setEditable(true);
    method->setInsertPolicy(QComboBox::NoInsert);
    method->addItems({QStringLiteral("GET"), QStringLiteral("POST"),
                      QStringLiteral("PUT"), QStringLiteral("PATCH"),
                      QStringLiteral("DELETE")});
    method->setCurrentText(action.method.isEmpty() ? QStringLiteral("GET")
                                                   : action.method);
    m_actionsTable->setCellWidget(row, ActionMethod, method);

    auto *url = new QComboBox(m_actionsTable);
    url->setEditable(true);
    url->setInsertPolicy(QComboBox::NoInsert);
    QSet<QString> seen;
    for (const WidgetEndpoint &endpoint : m_endpoints) {
        if (!isHttpUrl(endpoint.url))
            continue;
        if (seen.contains(endpoint.url))
            continue;
        seen.insert(endpoint.url);
        url->addItem(endpoint.url);
    }
    url->setCurrentText(action.url);
    m_actionsTable->setCellWidget(row, ActionUrl, url);

    auto *target = new QComboBox(m_actionsTable);
    target->addItem(text("WidgetBrowser.ActionsDialog.InPage"), true);
    target->addItem(text("WidgetBrowser.ActionsDialog.InPlugin"), false);
    target->setCurrentIndex(action.inPage ? 0 : 1);
    m_actionsTable->setCellWidget(row, ActionTarget, target);

    auto *hotkey = new QTableWidgetItem;
    hotkey->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    hotkey->setToolTip(text("WidgetBrowser.ActionsDialog.HotkeyHint"));
    m_actionsTable->setItem(row, ActionHotkey, hotkey);
    updateHotkeyCell(row);

    auto connectCombo = [this](QComboBox *combo) {
        connect(combo, &QComboBox::currentTextChanged, this,
                [this, combo](const QString &) {
                    for (int i = 0; i < m_actionsTable->rowCount(); ++i) {
                        if (m_actionsTable->cellWidget(i, ActionMethod) ==
                                combo ||
                            m_actionsTable->cellWidget(i, ActionUrl) == combo) {
                            updateHotkeyCell(i);
                            break;
                        }
                    }
                });
    };
    connectCombo(method);
    connectCombo(url);
}

void ActionsDialog::addSelectedEndpoints()
{
    const QModelIndexList selected =
        m_endpointsTable->selectionModel()->selectedRows();
    if (selected.isEmpty())
        return;

    QSet<QString> existing;
    for (const WidgetAction &action : readRows()) {
        if (!action.url.isEmpty())
            existing.insert(actionHotkeyKey(action.method, action.url));
    }

    int lastAdded = -1;
    for (const QModelIndex &index : selected) {
        auto *methodItem =
            m_endpointsTable->item(index.row(), EndpointMethod);
        if (!methodItem)
            continue;

        const int endpointIndex = methodItem->data(Qt::UserRole).toInt();
        if (endpointIndex < 0 || endpointIndex >= m_endpoints.size())
            continue;

        const WidgetEndpoint &endpoint = m_endpoints.at(endpointIndex);
        WidgetAction action;
        action.name = defaultActionName(endpoint.method, endpoint.url);
        action.method = endpoint.method;
        action.url = endpoint.url;
        action.inPage = true;

        const QString key = actionHotkeyKey(action.method, action.url);
        if (existing.contains(key))
            continue;
        existing.insert(key);

        addActionRow(action);
        lastAdded = m_actionsTable->rowCount() - 1;
    }

    if (lastAdded >= 0) {
        m_actionsTable->selectRow(lastAdded);
        m_actionsTable->setCurrentCell(lastAdded, ActionName);
        m_actionsTable->scrollToItem(
            m_actionsTable->item(lastAdded, ActionName));
    }
}

void ActionsDialog::removeSelectedActions()
{
    QList<int> rows;
    for (const QModelIndex &index :
         m_actionsTable->selectionModel()->selectedRows())
        rows.append(index.row());

    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows)
        m_actionsTable->removeRow(row);
}

void ActionsDialog::moveSelectedAction(int delta)
{
    const int row = m_actionsTable->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= m_actionsTable->rowCount())
        return;

    QVector<WidgetAction> rows = readRows();
    if (row >= rows.size() || target >= rows.size())
        return;

    std::swap(rows[row], rows[target]);
    populateActions(rows);
    m_actionsTable->selectRow(target);
    m_actionsTable->setCurrentCell(target, ActionName);
}

void ActionsDialog::updateHotkeyCell(int row)
{
    auto *item = m_actionsTable->item(row, ActionHotkey);
    if (!item)
        return;

    const WidgetAction action = actionAt(row);
    if (action.url.isEmpty()) {
        item->setText(text("WidgetBrowser.ActionsDialog.NotAssigned"));
        return;
    }

    const QString key = actionHotkeyKey(action.method, action.url);
    const QString binding = m_hotkeys.value(key);
    item->setText(binding.isEmpty()
                      ? text("WidgetBrowser.ActionsDialog.NotAssigned")
                      : binding);
}

void ActionsDialog::filterEndpoints(const QString &filter)
{
    const QString needle = filter.trimmed();
    for (int row = 0; row < m_endpointsTable->rowCount(); ++row) {
        bool matches = needle.isEmpty();
        if (!matches) {
            for (int col = 0; col < EndpointColumnCount; ++col) {
                auto *item = m_endpointsTable->item(row, col);
                if (item &&
                    item->text().contains(needle, Qt::CaseInsensitive)) {
                    matches = true;
                    break;
                }
            }
        }
        m_endpointsTable->setRowHidden(row, !matches);
    }
}

WidgetAction ActionsDialog::actionAt(int row) const
{
    WidgetAction action;

    if (auto *name = m_actionsTable->item(row, ActionName))
        action.name = name->text();

    if (auto *method = qobject_cast<QComboBox *>(
            m_actionsTable->cellWidget(row, ActionMethod)))
        action.method = method->currentText().trimmed().toUpper();

    if (auto *url = qobject_cast<QComboBox *>(
            m_actionsTable->cellWidget(row, ActionUrl)))
        action.url = url->currentText().trimmed();

    if (auto *target = qobject_cast<QComboBox *>(
            m_actionsTable->cellWidget(row, ActionTarget)))
        action.inPage = target->currentData().toBool();

    if (action.method.isEmpty())
        action.method = QStringLiteral("GET");
    action.hotkeyKey = actionHotkeyKey(action.method, action.url);
    return action;
}

QVector<WidgetAction> ActionsDialog::readRows() const
{
    QVector<WidgetAction> rows;
    rows.reserve(m_actionsTable->rowCount());
    for (int row = 0; row < m_actionsTable->rowCount(); ++row)
        rows.append(actionAt(row));
    return rows;
}
