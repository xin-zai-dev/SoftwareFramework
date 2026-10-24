#pragma once
#include "domain/Types.h"
#include <QWidget>

namespace ivs {
class ImageView : public QWidget {
    Q_OBJECT
  public:
    explicit ImageView(QWidget *parent = nullptr);
    void setResult(const InspectionResult &result);
    void clear();

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    InspectionResult m_result;
};
} // namespace ivs
