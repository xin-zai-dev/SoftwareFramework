#include "ui/PanelEditor.h"
#include "domain/Project.h"
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QSplitter>
#include <QUuid>
#include <QVBoxLayout>
#include <functional>

namespace {
constexpr auto typeMime = "application/x-ivs-panel-type";
constexpr auto panelMime = "application/x-ivs-panel-id";

QString panelTitle(const QString &type) {
    if (type == QLatin1String("image"))
        return QStringLiteral("检测图像");
    if (type == QLatin1String("table"))
        return QStringLiteral("结果表格");
    if (type == QLatin1String("trend"))
        return QStringLiteral("趋势图");
    if (type == QLatin1String("scatter"))
        return QStringLiteral("散点图");
    if (type == QLatin1String("log"))
        return QStringLiteral("运行日志");
    return type;
}

class PanelList final : public QListWidget {
  public:
    explicit PanelList(bool palette, QWidget *parent) : QListWidget(parent), m_palette(palette) {
        setSelectionMode(QAbstractItemView::SingleSelection);
        setDragEnabled(true);
        setAcceptDrops(!palette);
        setDropIndicatorShown(!palette);
        setDragDropMode(palette ? QAbstractItemView::DragOnly : QAbstractItemView::DragDrop);
    }
    std::function<void(bool, const QString &, int)> dropped;

  protected:
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override {
        auto *mime = new QMimeData;
        if (!items.isEmpty())
            mime->setData(m_palette ? typeMime : panelMime,
                          items.first()->data(Qt::UserRole).toString().toUtf8());
        return mime;
    }
    Qt::DropActions supportedDropActions() const override {
        return Qt::CopyAction | Qt::MoveAction;
    }
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (!m_palette && (event->mimeData()->hasFormat(typeMime) || event->mimeData()->hasFormat(panelMime)))
            event->acceptProposedAction();
        else
            event->ignore();
    }
    void dragMoveEvent(QDragMoveEvent *event) override {
        if (!m_palette && (event->mimeData()->hasFormat(typeMime) || event->mimeData()->hasFormat(panelMime)))
            event->acceptProposedAction();
        else
            event->ignore();
    }
    void dropEvent(QDropEvent *event) override {
        const bool external = event->mimeData()->hasFormat(typeMime);
        const QString mime = QString::fromLatin1(external ? typeMime : panelMime);
        if (m_palette || !event->mimeData()->hasFormat(mime) || !dropped) {
            event->ignore();
            return;
        }
        int row = indexAt(event->position().toPoint()).row();
        if (row < 0)
            row = count();
        else if (event->position().y() > visualItemRect(item(row)).center().y())
            ++row;
        dropped(external, QString::fromUtf8(event->mimeData()->data(mime)), row);
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }

  private:
    bool m_palette;
};
} // namespace

namespace ivs {
PanelEditor::PanelEditor(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *heading = new QLabel(tr("检测看板布局"), this);
    heading->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;"));
    layout->addWidget(heading);
    auto *description = new QLabel(tr("添加展示面板并绑定检测字段。列表顺序就是运行看板的显示顺序。"), this);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *splitter = new QSplitter(this);
    layout->addWidget(splitter, 1);

    auto *palettePane = new QWidget(splitter);
    auto *paletteLayout = new QVBoxLayout(palettePane);
    paletteLayout->setContentsMargins(0, 0, 8, 0);
    paletteLayout->addWidget(new QLabel(tr("面板库"), palettePane));
    m_palette = new PanelList(true, palettePane);
    m_palette->setObjectName(QStringLiteral("panelPalette"));
    for (const QString &type : supportedPanelTypes()) {
        auto *item = new QListWidgetItem(panelTitle(type), m_palette);
        item->setData(Qt::UserRole, type);
        item->setSizeHint(QSize(130, 40));
    }
    m_palette->setCurrentRow(0);
    paletteLayout->addWidget(m_palette, 1);
    auto *add = new QPushButton(tr("添加面板 →"), palettePane);
    add->setObjectName(QStringLiteral("addPanel"));
    paletteLayout->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] {
        if (auto *item = m_palette->currentItem())
            addPanel(item->data(Qt::UserRole).toString());
    });
    connect(m_palette, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) { addPanel(item->data(Qt::UserRole).toString()); });

    auto *listPane = new QWidget(splitter);
    auto *listLayout = new QVBoxLayout(listPane);
    listLayout->setContentsMargins(0, 0, 8, 0);
    listLayout->addWidget(new QLabel(tr("显示顺序"), listPane));
    auto *list = new PanelList(false, listPane);
    m_list = list;
    m_list->setObjectName(QStringLiteral("panelSequence"));
    listLayout->addWidget(m_list, 1);
    list->dropped = [this](bool external, const QString &value, int row) {
        if (external) {
            addPanel(value, row);
            return;
        }
        int from = -1;
        for (int i = 0; i < m_panels.size(); ++i)
            if (m_panels.at(i).id == value) {
                from = i;
                break;
            }
        if (from < 0)
            return;
        if (row > from)
            --row;
        row = qBound(0, row, int(m_panels.size()) - 1);
        if (from == row)
            return;
        m_panels.move(from, row);
        rebuild(row);
        emit panelsChanged();
    };
    auto *actions = new QHBoxLayout;
    for (const auto &pair : QList<QPair<QString, QString>>{{QStringLiteral("movePanelUp"), tr("上移")},
                                                           {QStringLiteral("movePanelDown"), tr("下移")},
                                                           {QStringLiteral("removePanel"), tr("删除")}}) {
        auto *button = new QPushButton(pair.second, listPane);
        button->setObjectName(pair.first);
        actions->addWidget(button);
        if (pair.first == QLatin1String("movePanelUp"))
            connect(button, &QPushButton::clicked, this, [this] { moveSelected(-1); });
        else if (pair.first == QLatin1String("movePanelDown"))
            connect(button, &QPushButton::clicked, this, [this] { moveSelected(1); });
        else
            connect(button, &QPushButton::clicked, this, [this] {
                const int row = m_list->currentRow();
                if (row < 0)
                    return;
                m_panels.removeAt(row);
                rebuild(qMin(row, int(m_panels.size()) - 1));
                emit panelsChanged();
            });
    }
    listLayout->addLayout(actions);

    auto *detailPane = new QWidget(splitter);
    auto *details = new QVBoxLayout(detailPane);
    details->setContentsMargins(4, 0, 0, 0);
    details->addWidget(new QLabel(tr("面板属性"), detailPane));
    auto *form = new QFormLayout;
    m_title = new QLineEdit(detailPane);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    form->addRow(tr("标题"), m_title);
    m_field = new QComboBox(detailPane);
    m_field->setObjectName(QStringLiteral("panelField"));
    for (const QString &field : {QStringLiteral("diameterMm"), QStringLiteral("offsetXmm"),
                                 QStringLiteral("offsetYmm"), QStringLiteral("durationMs")})
        m_field->addItem(fieldTitle(field), field);
    form->addRow(tr("检测字段"), m_field);
    details->addLayout(form);
    m_bindingHint = new QLabel(detailPane);
    m_bindingHint->setWordWrap(true);
    details->addWidget(m_bindingHint);
    details->addStretch();
    connect(m_list, &QListWidget::currentRowChanged, this, &PanelEditor::showSelection);
    connect(m_title, &QLineEdit::editingFinished, this, [this] {
        const int row = m_list->currentRow();
        if (m_loading || row < 0)
            return;
        const QString title = m_title->text().trimmed();
        if (title.isEmpty()) {
            m_title->setText(m_panels.at(row).title);
            return;
        }
        if (m_panels.at(row).title == title)
            return;
        m_panels[row].title = title;
        rebuild(row);
        emit panelsChanged();
    });
    connect(m_field, &QComboBox::currentIndexChanged, this, [this](int index) {
        const int row = m_list->currentRow();
        if (m_loading || row < 0 || index < 0)
            return;
        const QString field = m_field->itemData(index).toString();
        if (m_panels.at(row).field == field)
            return;
        m_panels[row].field = field;
        rebuild(row);
        emit panelsChanged();
    });
    splitter->setSizes({160, 330, 300});
    rebuild();
}

void PanelEditor::setPanels(const QList<PanelConfig> &panels) {
    m_panels = panels;
    rebuild(panels.isEmpty() ? -1 : 0);
}
QList<PanelConfig> PanelEditor::panels() const {
    return m_panels;
}

void PanelEditor::addPanel(const QString &type, int row) {
    if (!supportedPanelTypes().contains(type))
        return;
    PanelConfig panel;
    panel.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    panel.type = type;
    panel.title = panelTitle(type);
    if (row < 0)
        row = m_panels.size();
    row = qBound(0, row, int(m_panels.size()));
    m_panels.insert(row, panel);
    rebuild(row);
    emit panelsChanged();
}

void PanelEditor::rebuild(int selectedRow) {
    m_loading = true;
    m_list->clear();
    for (int i = 0; i < m_panels.size(); ++i) {
        const auto &panel = m_panels.at(i);
        auto *item =
            new QListWidgetItem(QStringLiteral("%1  %2\n     %3 · %4")
                                    .arg(i + 1, 2, 10, QLatin1Char('0'))
                                    .arg(panel.title, panelTitle(panel.type), fieldTitle(panel.field)),
                                m_list);
        item->setData(Qt::UserRole, panel.id);
        item->setSizeHint(QSize(260, 58));
    }
    m_list->setCurrentRow(selectedRow);
    m_loading = false;
    showSelection();
}

void PanelEditor::showSelection() {
    if (m_loading)
        return;
    const int row = m_list->currentRow();
    const bool selected = row >= 0 && row < m_panels.size();
    m_loading = true;
    m_title->setEnabled(selected);
    m_field->setEnabled(selected);
    m_title->setText(selected ? m_panels.at(row).title : QString());
    m_field->setCurrentIndex(selected ? m_field->findData(m_panels.at(row).field) : -1);
    if (selected) {
        const QString type = m_panels.at(row).type;
        if (type == QLatin1String("scatter"))
            m_bindingHint->setText(tr("散点图：横轴固定为 X 偏移 (mm)，纵轴使用所选检测字段。"));
        else if (type == QLatin1String("trend"))
            m_bindingHint->setText(
                tr("趋势图：横轴为最近检测序号，纵轴使用所选检测字段，保留最近 1,000 条。"));
        else if (type == QLatin1String("table"))
            m_bindingHint->setText(tr("结果表格展示检测记录；所选字段作为当前面板的主要测量字段。"));
        else
            m_bindingHint->setText(tr("图像与日志面板展示对应内容。字段绑定会随工程保存。"));
    } else
        m_bindingHint->setText(tr("从左侧添加面板，或选择一个面板设置标题与字段。"));
    for (const QString &name :
         {QStringLiteral("movePanelUp"), QStringLiteral("movePanelDown"), QStringLiteral("removePanel")})
        if (auto *button = findChild<QPushButton *>(name))
            button->setEnabled(selected && (name != QLatin1String("movePanelUp") || row > 0) &&
                               (name != QLatin1String("movePanelDown") || row + 1 < m_panels.size()));
    m_loading = false;
}

void PanelEditor::moveSelected(int delta) {
    const int from = m_list->currentRow();
    const int to = from + delta;
    if (from < 0 || to < 0 || to >= m_panels.size())
        return;
    m_panels.move(from, to);
    rebuild(to);
    emit panelsChanged();
}
} // namespace ivs
