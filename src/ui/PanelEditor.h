#pragma once
#include "domain/Types.h"
#include <QWidget>

class QListWidget;
class QLineEdit;
class QComboBox;
class QLabel;

namespace ivs {
class PanelEditor : public QWidget {
    Q_OBJECT
  public:
    explicit PanelEditor(QWidget *parent = nullptr);
    void setPanels(const QList<PanelConfig> &panels);
    QList<PanelConfig> panels() const;
  signals:
    void panelsChanged();

  private:
    void addPanel(const QString &type, int row = -1);
    void rebuild(int selectedRow = -1);
    void showSelection();
    void moveSelected(int delta);
    QList<PanelConfig> m_panels;
    QListWidget *m_palette = nullptr;
    QListWidget *m_list = nullptr;
    QLineEdit *m_title = nullptr;
    QComboBox *m_field = nullptr;
    QLabel *m_bindingHint = nullptr;
    bool m_loading = false;
};
} // namespace ivs
