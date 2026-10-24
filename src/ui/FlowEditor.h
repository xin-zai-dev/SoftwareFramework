#pragma once
#include "domain/Types.h"
#include <QWidget>

class QListWidget;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;

namespace ivs {
class FlowEditor : public QWidget {
    Q_OBJECT
  public:
    explicit FlowEditor(QWidget *parent = nullptr);
    void setNodes(const QList<FlowNode> &nodes);
    QList<FlowNode> nodes() const;
    void setEditingEnabled(bool enabled);
  signals:
    void nodesChanged();

  private:
    void addNode(const QString &type, int row = -1);
    void rebuild(int selectedRow = -1);
    void showSelection();
    void applyParameters();
    void moveSelected(int delta);
    QList<FlowNode> m_nodes;
    QListWidget *m_palette = nullptr;
    QListWidget *m_sequence = nullptr;
    QLineEdit *m_title = nullptr;
    QPlainTextEdit *m_parameters = nullptr;
    QLabel *m_validation = nullptr;
    QPushButton *m_apply = nullptr;
    bool m_loading = false;
    bool m_editingEnabled = true;
};
} // namespace ivs
