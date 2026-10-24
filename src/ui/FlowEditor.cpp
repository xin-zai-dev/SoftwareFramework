#include "ui/FlowEditor.h"
#include "domain/Project.h"
#include <QAbstractItemView>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QUuid>
#include <QVBoxLayout>
#include <functional>

namespace {
constexpr auto typeMime = "application/x-ivs-flow-type";
constexpr auto nodeMime = "application/x-ivs-flow-id";

class FlowList final : public QListWidget {
  public:
    explicit FlowList(bool palette, QWidget *parent = nullptr) : QListWidget(parent), m_palette(palette) {
        setSelectionMode(QAbstractItemView::SingleSelection);
        setDragEnabled(true);
        setAcceptDrops(!palette);
        setDropIndicatorShown(!palette);
        setDragDropMode(palette ? QAbstractItemView::DragOnly : QAbstractItemView::DragDrop);
    }
    std::function<void(const QString &, const QString &, int)> dropped;

  protected:
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override {
        auto *mime = new QMimeData;
        if (!items.isEmpty())
            mime->setData(m_palette ? typeMime : nodeMime,
                          items.first()->data(Qt::UserRole).toString().toUtf8());
        return mime;
    }
    Qt::DropActions supportedDropActions() const override {
        return Qt::CopyAction | Qt::MoveAction;
    }
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (!m_palette && (event->mimeData()->hasFormat(typeMime) || event->mimeData()->hasFormat(nodeMime)))
            event->acceptProposedAction();
        else
            event->ignore();
    }
    void dragMoveEvent(QDragMoveEvent *event) override {
        if (!m_palette && (event->mimeData()->hasFormat(typeMime) || event->mimeData()->hasFormat(nodeMime)))
            event->acceptProposedAction();
        else
            event->ignore();
    }
    void dropEvent(QDropEvent *event) override {
        const QString mime = event->mimeData()->hasFormat(typeMime) ? QString::fromLatin1(typeMime)
                                                                    : QString::fromLatin1(nodeMime);
        if (m_palette || !event->mimeData()->hasFormat(mime) || !dropped) {
            event->ignore();
            return;
        }
        int row = indexAt(event->position().toPoint()).row();
        if (row < 0)
            row = count();
        else if (event->position().y() > visualItemRect(item(row)).center().y())
            ++row;
        dropped(mime, QString::fromUtf8(event->mimeData()->data(mime)), row);
        event->setDropAction(Qt::CopyAction); // The owning model performs the move exactly once.
        event->accept();
    }

  private:
    bool m_palette;
};

QJsonObject defaultParameters(const QString &type) {
    if (type == QLatin1String("trigger"))
        return {{QStringLiteral("timeoutMs"), 60000}};
    if (type == QLatin1String("delay"))
        return {{QStringLiteral("ms"), 50}};
    if (type == QLatin1String("move"))
        return {{QStringLiteral("axis"), 0}, {QStringLiteral("position"), 0}, {QStringLiteral("speed"), 25}};
    if (type == QLatin1String("light"))
        return {{QStringLiteral("channel"), 0},
                {QStringLiteral("brightness"), 80},
                {QStringLiteral("enabled"), true}};
    if (type == QLatin1String("robot_move"))
        return {{QStringLiteral("x"), 220},
                {QStringLiteral("y"), 40},
                {QStringLiteral("z"), 100},
                {QStringLiteral("r"), 0},
                {QStringLiteral("elbowPositive"), true},
                {QStringLiteral("speedPercent"), 60},
                {QStringLiteral("timeoutMs"), 30000}};
    if (type == QLatin1String("robot_grip"))
        return {{QStringLiteral("closed"), true}};
    if (type == QLatin1String("robot_sort"))
        return {{QStringLiteral("speedPercent"), 60},
                {QStringLiteral("timeoutMs"), 60000},
                {QStringLiteral("restock"), false}};
    if (type == QLatin1String("output"))
        return {{QStringLiteral("function"), QStringLiteral("holding")}, {QStringLiteral("address"), 0}};
    return {};
}
} // namespace

namespace ivs {
FlowEditor::FlowEditor(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *heading = new QLabel(tr("顺序执行流程"), this);
    heading->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;"));
    layout->addWidget(heading);
    auto *description =
        new QLabel(tr("从节点库拖入步骤，按从上到下的顺序执行。拖动或使用箭头调整顺序。"), this);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *splitter = new QSplitter(this);
    layout->addWidget(splitter, 1);

    auto *palettePane = new QWidget(splitter);
    auto *paletteLayout = new QVBoxLayout(palettePane);
    paletteLayout->setContentsMargins(0, 0, 8, 0);
    paletteLayout->addWidget(new QLabel(tr("节点库"), palettePane));
    m_palette = new FlowList(true, palettePane);
    m_palette->setObjectName(QStringLiteral("flowPalette"));
    for (const QString &type : supportedNodeTypes()) {
        auto *item = new QListWidgetItem(nodeTitle(type), m_palette);
        item->setData(Qt::UserRole, type);
        item->setToolTip(type);
        item->setSizeHint(QSize(130, 38));
    }
    m_palette->setCurrentRow(0);
    paletteLayout->addWidget(m_palette, 1);
    auto *add = new QPushButton(tr("添加步骤 →"), palettePane);
    add->setObjectName(QStringLiteral("addFlowNode"));
    paletteLayout->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] {
        if (auto *item = m_palette->currentItem())
            addNode(item->data(Qt::UserRole).toString());
    });
    connect(m_palette, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) { addNode(item->data(Qt::UserRole).toString()); });

    auto *sequencePane = new QWidget(splitter);
    auto *sequenceLayout = new QVBoxLayout(sequencePane);
    sequenceLayout->setContentsMargins(0, 0, 8, 0);
    sequenceLayout->addWidget(new QLabel(tr("执行顺序 · 勾选启用"), sequencePane));
    auto *sequence = new FlowList(false, sequencePane);
    m_sequence = sequence;
    m_sequence->setObjectName(QStringLiteral("flowSequence"));
    sequenceLayout->addWidget(m_sequence, 1);
    sequence->dropped = [this](const QString &mime, const QString &value, int row) {
        if (!m_editingEnabled)
            return;
        if (mime == QLatin1String(typeMime)) {
            addNode(value, row);
            return;
        }
        int from = -1;
        for (int i = 0; i < m_nodes.size(); ++i)
            if (m_nodes.at(i).id == value) {
                from = i;
                break;
            }
        if (from < 0)
            return;
        if (row > from)
            --row;
        row = qBound(0, row, int(m_nodes.size()) - 1);
        if (row == from)
            return;
        m_nodes.move(from, row);
        rebuild(row);
        emit nodesChanged();
    };
    auto *actions = new QHBoxLayout;
    const QList<QPair<QString, QString>> buttons{{QStringLiteral("moveFlowUp"), tr("上移")},
                                                 {QStringLiteral("moveFlowDown"), tr("下移")},
                                                 {QStringLiteral("duplicateFlowNode"), tr("复制")},
                                                 {QStringLiteral("removeFlowNode"), tr("删除")}};
    for (const auto &button : buttons) {
        auto *widget = new QPushButton(button.second, sequencePane);
        widget->setObjectName(button.first);
        actions->addWidget(widget);
        if (button.first == QLatin1String("moveFlowUp"))
            connect(widget, &QPushButton::clicked, this, [this] { moveSelected(-1); });
        else if (button.first == QLatin1String("moveFlowDown"))
            connect(widget, &QPushButton::clicked, this, [this] { moveSelected(1); });
        else if (button.first == QLatin1String("duplicateFlowNode"))
            connect(widget, &QPushButton::clicked, this, [this] {
                const int row = m_sequence->currentRow();
                if (!m_editingEnabled || row < 0)
                    return;
                FlowNode copy = m_nodes.at(row);
                copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                copy.title += tr(" 副本");
                m_nodes.insert(row + 1, copy);
                rebuild(row + 1);
                emit nodesChanged();
            });
        else
            connect(widget, &QPushButton::clicked, this, [this] {
                const int row = m_sequence->currentRow();
                if (!m_editingEnabled || row < 0)
                    return;
                m_nodes.removeAt(row);
                rebuild(qMin(row, int(m_nodes.size()) - 1));
                emit nodesChanged();
            });
    }
    sequenceLayout->addLayout(actions);

    auto *detailPane = new QWidget(splitter);
    auto *detailLayout = new QVBoxLayout(detailPane);
    detailLayout->setContentsMargins(4, 0, 0, 0);
    detailLayout->addWidget(new QLabel(tr("步骤属性"), detailPane));
    auto *form = new QFormLayout;
    m_title = new QLineEdit(detailPane);
    m_title->setObjectName(QStringLiteral("flowTitle"));
    form->addRow(tr("名称"), m_title);
    detailLayout->addLayout(form);
    detailLayout->addWidget(new QLabel(tr("参数 JSON"), detailPane));
    m_parameters = new QPlainTextEdit(detailPane);
    m_parameters->setObjectName(QStringLiteral("flowParameters"));
    m_parameters->setPlaceholderText(QStringLiteral("{\n  \"deviceId\": \"axis\"\n}"));
    m_parameters->setTabStopDistance(28);
    detailLayout->addWidget(m_parameters, 1);
    auto *hint = new QLabel(tr("move: axis / position / speed · light: channel / brightness / enabled\n"
                               "trigger: requireValue / function / address · output: function / address\n"
                               "robot_move: x / y / z / r / elbowPositive / speedPercent\n"
                               "robot_grip: closed · robot_sort: restock / speedPercent（保存后执行）\n"
                               "delay: ms。可用 deviceId "
                               "指定已配置的设备。参数在点击应用后生效。"),
                            detailPane);
    hint->setWordWrap(true);
    detailLayout->addWidget(hint);
    m_validation = new QLabel(detailPane);
    m_validation->setObjectName(QStringLiteral("flowValidation"));
    m_validation->setWordWrap(true);
    detailLayout->addWidget(m_validation);
    m_apply = new QPushButton(tr("应用参数"), detailPane);
    m_apply->setObjectName(QStringLiteral("applyFlowParameters"));
    detailLayout->addWidget(m_apply);
    connect(m_apply, &QPushButton::clicked, this, &FlowEditor::applyParameters);
    connect(m_title, &QLineEdit::editingFinished, this, [this] {
        const int row = m_sequence->currentRow();
        if (!m_editingEnabled || m_loading || row < 0)
            return;
        const QString title = m_title->text().trimmed();
        if (title.isEmpty()) {
            m_title->setText(m_nodes.at(row).title);
            return;
        }
        if (m_nodes.at(row).title == title)
            return;
        m_nodes[row].title = title;
        rebuild(row);
        emit nodesChanged();
    });
    connect(m_sequence, &QListWidget::currentRowChanged, this, &FlowEditor::showSelection);
    connect(m_sequence, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const int row = m_sequence->row(item);
        if (m_loading || !m_editingEnabled || row < 0 || row >= m_nodes.size())
            return;
        const bool enabled = item->checkState() == Qt::Checked;
        if (m_nodes.at(row).enabled == enabled)
            return;
        m_nodes[row].enabled = enabled;
        emit nodesChanged();
    });
    splitter->setSizes({160, 300, 320});
    rebuild();
}

void FlowEditor::setNodes(const QList<FlowNode> &nodes) {
    m_nodes = nodes;
    rebuild(nodes.isEmpty() ? -1 : 0);
}
QList<FlowNode> FlowEditor::nodes() const {
    return m_nodes;
}

void FlowEditor::setEditingEnabled(bool enabled) {
    m_editingEnabled = enabled;
    m_palette->setEnabled(enabled);
    m_sequence->setDragEnabled(enabled);
    m_sequence->setAcceptDrops(enabled);
    m_title->setReadOnly(!enabled);
    m_parameters->setReadOnly(!enabled);
    for (auto *button : findChildren<QPushButton *>())
        button->setEnabled(enabled);
    rebuild(m_sequence->currentRow());
}

void FlowEditor::addNode(const QString &type, int row) {
    if (!m_editingEnabled || !supportedNodeTypes().contains(type))
        return;
    FlowNode node;
    node.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    node.type = type;
    node.title = nodeTitle(type);
    node.parameters = defaultParameters(type);
    if (row < 0)
        row = m_nodes.size();
    row = qBound(0, row, int(m_nodes.size()));
    m_nodes.insert(row, node);
    rebuild(row);
    emit nodesChanged();
}

void FlowEditor::rebuild(int selectedRow) {
    m_loading = true;
    m_sequence->clear();
    for (int i = 0; i < m_nodes.size(); ++i) {
        const auto &node = m_nodes.at(i);
        auto *item = new QListWidgetItem(
            QStringLiteral("%1  %2\n     %3")
                .arg(i + 1, 2, 10, QLatin1Char('0'))
                .arg(node.title.isEmpty() ? nodeTitle(node.type) : node.title, nodeTitle(node.type)),
            m_sequence);
        item->setData(Qt::UserRole, node.id);
        item->setFlags(
            Qt::ItemIsSelectable | Qt::ItemIsEnabled |
            (m_editingEnabled ? Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled : Qt::NoItemFlags));
        item->setCheckState(node.enabled ? Qt::Checked : Qt::Unchecked);
        item->setSizeHint(QSize(230, 58));
    }
    m_sequence->setCurrentRow(selectedRow);
    m_loading = false;
    showSelection();
}

void FlowEditor::showSelection() {
    if (m_loading)
        return;
    const int row = m_sequence->currentRow();
    const bool selected = row >= 0 && row < m_nodes.size();
    m_loading = true;
    m_title->setEnabled(selected);
    m_parameters->setEnabled(selected);
    m_apply->setEnabled(selected && m_editingEnabled);
    m_validation->clear();
    m_title->setText(selected ? m_nodes.at(row).title : QString());
    m_parameters->setPlainText(
        selected
            ? QString::fromUtf8(QJsonDocument(m_nodes.at(row).parameters).toJson(QJsonDocument::Indented))
            : QString());
    for (const QString &name : {QStringLiteral("moveFlowUp"), QStringLiteral("moveFlowDown"),
                                QStringLiteral("duplicateFlowNode"), QStringLiteral("removeFlowNode")})
        if (auto *button = findChild<QPushButton *>(name))
            button->setEnabled(selected && m_editingEnabled &&
                               (name != QLatin1String("moveFlowUp") || row > 0) &&
                               (name != QLatin1String("moveFlowDown") || row + 1 < m_nodes.size()));
    m_loading = false;
}

void FlowEditor::applyParameters() {
    const int row = m_sequence->currentRow();
    if (!m_editingEnabled || row < 0)
        return;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(m_parameters->toPlainText().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        m_validation->setStyleSheet(QStringLiteral("color:#c23b43;"));
        m_validation->setText(
            error.error == QJsonParseError::NoError
                ? tr("参数必须是 JSON 对象，例如 {\"ms\": 50}。")
                : tr("JSON 格式错误（位置 %1）：%2").arg(error.offset).arg(error.errorString()));
        return;
    }
    if (m_nodes.at(row).parameters != document.object()) {
        m_nodes[row].parameters = document.object();
        emit nodesChanged();
    }
    m_validation->setStyleSheet(QStringLiteral("color:#20815d;"));
    m_validation->setText(tr("参数已应用"));
}

void FlowEditor::moveSelected(int delta) {
    const int from = m_sequence->currentRow();
    const int to = from + delta;
    if (!m_editingEnabled || from < 0 || to < 0 || to >= m_nodes.size())
        return;
    m_nodes.move(from, to);
    rebuild(to);
    emit nodesChanged();
}
} // namespace ivs
