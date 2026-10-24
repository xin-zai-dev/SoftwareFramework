#include "ui/ChartWidget.h"
#include "ui/FlowEditor.h"
#include "ui/ImageView.h"
#include "ui/PanelEditor.h"
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>
#include <limits>

class EditorTests : public QObject {
    Q_OBJECT
  private slots:
    void flowPreservesParametersAndRejectsInvalidJson();
    void flowActionsKeepOrderAndUniqueIds();
    void flowReadOnlyBlocksMutations();
    void flowDragInsertionAndReorderPreserveParameters();
    void panelsPersistFieldBindingsAndOrder();
    void panelsAddFromPalette();
    void chartsRenderNonfiniteAndSingleSamples();
    void chartsDiscardOldestRecords();
    void imageFitsFrameAndClears();
};

void EditorTests::flowPreservesParametersAndRejectsInvalidJson() {
    ivs::FlowEditor editor;
    ivs::FlowNode node{QStringLiteral("delay-1"),
                       QStringLiteral("delay"),
                       QStringLiteral("等待"),
                       true,
                       {{QStringLiteral("ms"), 80}}};
    QSignalSpy changed(&editor, &ivs::FlowEditor::nodesChanged);
    editor.setNodes({node});
    QCOMPARE(editor.nodes().size(), 1);
    QCOMPARE(editor.nodes().first().parameters.value(QStringLiteral("ms")).toInt(), 80);
    QCOMPARE(changed.count(), 0);
    auto *parameters = editor.findChild<QPlainTextEdit *>(QStringLiteral("flowParameters"));
    auto *apply = editor.findChild<QPushButton *>(QStringLiteral("applyFlowParameters"));
    QVERIFY(parameters);
    QVERIFY(apply);
    parameters->setPlainText(QStringLiteral("{broken"));
    apply->click();
    QCOMPARE(editor.nodes().first().parameters.value(QStringLiteral("ms")).toInt(), 80);
    QCOMPARE(changed.count(), 0);
    parameters->setPlainText(QStringLiteral("{\"ms\":125,\"deviceId\":\"axis\"}"));
    apply->click();
    QCOMPARE(editor.nodes().first().parameters.value(QStringLiteral("ms")).toInt(), 125);
    QCOMPARE(editor.nodes().first().parameters.value(QStringLiteral("deviceId")).toString(),
             QStringLiteral("axis"));
    QCOMPARE(changed.count(), 1);
}

void EditorTests::flowActionsKeepOrderAndUniqueIds() {
    ivs::FlowEditor editor;
    editor.setNodes(
        {{QStringLiteral("capture-1"), QStringLiteral("capture"), QStringLiteral("拍照"), true, {}}});
    auto *palette = editor.findChild<QListWidget *>(QStringLiteral("flowPalette"));
    auto *list = editor.findChild<QListWidget *>(QStringLiteral("flowSequence"));
    auto *add = editor.findChild<QPushButton *>(QStringLiteral("addFlowNode"));
    QVERIFY(palette && list && add);
    for (int row = 0; row < palette->count(); ++row)
        if (palette->item(row)->data(Qt::UserRole).toString() == QStringLiteral("delay"))
            palette->setCurrentRow(row);
    add->click();
    QCOMPARE(editor.nodes().size(), 2);
    QCOMPARE(editor.nodes().last().type, QStringLiteral("delay"));
    QCOMPARE(editor.nodes().last().parameters.value(QStringLiteral("ms")).toInt(), 50);
    editor.findChild<QPushButton *>(QStringLiteral("duplicateFlowNode"))->click();
    QCOMPARE(editor.nodes().size(), 3);
    QVERIFY(editor.nodes().at(1).id != editor.nodes().at(2).id);
    editor.findChild<QPushButton *>(QStringLiteral("moveFlowUp"))->click();
    const QString duplicateId = editor.nodes().at(1).id;
    QVERIFY(duplicateId != editor.nodes().last().id);
    list->item(1)->setCheckState(Qt::Unchecked);
    QVERIFY(!editor.nodes().at(1).enabled);
    editor.findChild<QPushButton *>(QStringLiteral("removeFlowNode"))->click();
    QCOMPARE(editor.nodes().size(), 2);
    QVERIFY(editor.nodes().at(1).id != duplicateId);
}

void EditorTests::flowReadOnlyBlocksMutations() {
    ivs::FlowEditor editor;
    editor.setNodes({{QStringLiteral("n1"), QStringLiteral("capture"), QStringLiteral("拍照"), true, {}}});
    editor.setEditingEnabled(false);
    auto *add = editor.findChild<QPushButton *>(QStringLiteral("addFlowNode"));
    auto *parameters = editor.findChild<QPlainTextEdit *>(QStringLiteral("flowParameters"));
    QVERIFY(add && parameters);
    QVERIFY(!add->isEnabled());
    QVERIFY(parameters->isReadOnly() || !parameters->isEnabled());
    add->click();
    QCOMPARE(editor.nodes().size(), 1);
}

void EditorTests::flowDragInsertionAndReorderPreserveParameters() {
    ivs::FlowEditor editor;
    editor.resize(950, 450);
    editor.setNodes({{QStringLiteral("n1"),
                      QStringLiteral("capture"),
                      QStringLiteral("拍照"),
                      true,
                      {{QStringLiteral("deviceId"), QStringLiteral("camera1")}}},
                     {QStringLiteral("n2"), QStringLiteral("inspect"), QStringLiteral("检测"), true, {}}});
    editor.show();
    QApplication::processEvents();
    auto *list = editor.findChild<QListWidget *>(QStringLiteral("flowSequence"));
    QVERIFY(list);
    QMimeData insertion;
    insertion.setData(QStringLiteral("application/x-ivs-flow-type"), QByteArrayLiteral("delay"));
    const QPoint top = list->visualItemRect(list->item(0)).topLeft() + QPoint(12, 2);
    QDragEnterEvent enter(top, Qt::CopyAction, &insertion, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop(top, Qt::CopyAction, &insertion, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &drop);
    QCOMPARE(editor.nodes().size(), 3);
    QCOMPARE(editor.nodes().first().type, QStringLiteral("delay"));
    QMimeData reorder;
    reorder.setData(QStringLiteral("application/x-ivs-flow-id"), QByteArrayLiteral("n1"));
    const QPoint bottom(12, list->visualItemRect(list->item(2)).bottom() + 10);
    QDragEnterEvent secondEnter(bottom, Qt::MoveAction, &reorder, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &secondEnter);
    QDropEvent secondDrop(bottom, Qt::MoveAction, &reorder, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &secondDrop);
    QCOMPARE(editor.nodes().last().id, QStringLiteral("n1"));
    QCOMPARE(editor.nodes().last().parameters.value(QStringLiteral("deviceId")).toString(),
             QStringLiteral("camera1"));
}

void EditorTests::panelsPersistFieldBindingsAndOrder() {
    ivs::PanelEditor editor;
    editor.setPanels({{QStringLiteral("p1"), QStringLiteral("trend"), QStringLiteral("直径走势"),
                       QStringLiteral("diameterMm")},
                      {QStringLiteral("p2"), QStringLiteral("scatter"), QStringLiteral("偏移分布"),
                       QStringLiteral("offsetYmm")}});
    QCOMPARE(editor.panels().size(), 2);
    QSignalSpy changed(&editor, &ivs::PanelEditor::panelsChanged);
    auto *list = editor.findChild<QListWidget *>(QStringLiteral("panelSequence"));
    auto *field = editor.findChild<QComboBox *>(QStringLiteral("panelField"));
    auto *title = editor.findChild<QLineEdit *>(QStringLiteral("panelTitle"));
    QVERIFY(list && field && title);
    list->setCurrentRow(1);
    field->setCurrentIndex(field->findData(QStringLiteral("durationMs")));
    title->setText(QStringLiteral("耗时分布"));
    QMetaObject::invokeMethod(title, "editingFinished", Qt::DirectConnection);
    QCOMPARE(editor.panels().last().field, QStringLiteral("durationMs"));
    QCOMPARE(editor.panels().last().title, QStringLiteral("耗时分布"));
    editor.findChild<QPushButton *>(QStringLiteral("movePanelUp"))->click();
    QCOMPARE(editor.panels().first().id, QStringLiteral("p2"));
    QCOMPARE(editor.panels().first().field, QStringLiteral("durationMs"));
    editor.findChild<QPushButton *>(QStringLiteral("removePanel"))->click();
    QCOMPARE(editor.panels().size(), 1);
    QCOMPARE(editor.panels().first().id, QStringLiteral("p1"));
    QVERIFY(changed.count() >= 4);
}

void EditorTests::panelsAddFromPalette() {
    ivs::PanelEditor editor;
    auto *palette = editor.findChild<QListWidget *>(QStringLiteral("panelPalette"));
    auto *add = editor.findChild<QPushButton *>(QStringLiteral("addPanel"));
    QVERIFY(palette && add);
    for (int row = 0; row < palette->count(); ++row)
        if (palette->item(row)->data(Qt::UserRole).toString() == QStringLiteral("scatter"))
            palette->setCurrentRow(row);
    add->click();
    add->click();
    QCOMPARE(editor.panels().size(), 2);
    QCOMPARE(editor.panels().first().type, QStringLiteral("scatter"));
    QVERIFY(!editor.panels().first().id.isEmpty());
    QVERIFY(editor.panels().first().id != editor.panels().last().id);
    QCOMPARE(editor.panels().first().field, QStringLiteral("diameterMm"));
}

void EditorTests::chartsRenderNonfiniteAndSingleSamples() {
    for (const auto kind : {ivs::ChartWidget::Kind::Trend, ivs::ChartWidget::Kind::Scatter}) {
        ivs::ChartWidget chart(kind);
        chart.resize(480, 280);
        ivs::DetectionRecord bad;
        bad.diameterMm = std::numeric_limits<double>::quiet_NaN();
        bad.offsetXmm = std::numeric_limits<double>::infinity();
        ivs::DetectionRecord valid;
        valid.diameterMm = 20;
        valid.offsetXmm = 0.2;
        chart.setRecords({bad, valid});
        QImage image(chart.size(), QImage::Format_RGB32);
        image.fill(Qt::magenta);
        chart.render(&image);
        QVERIFY(image.pixelColor(5, 5) != QColor(Qt::magenta));
        int coloredPoints = 0;
        for (int y = 40; y < 230; ++y)
            for (int x = 65; x < 450; ++x) {
                const QColor color = image.pixelColor(x, y);
                if (color.blue() > color.red() + 40)
                    ++coloredPoints;
            }
        QVERIFY2(coloredPoints > 5,
                 "The finite sample must remain visible when adjacent records contain NaN/Inf");
        for (int n = 0; n < 1100; ++n)
            chart.appendRecord(valid);
        chart.clear();
        chart.render(&image);
        int remainingPoints = 0;
        for (int y = 40; y < 230; ++y)
            for (int x = 65; x < 450; ++x) {
                const QColor color = image.pixelColor(x, y);
                if (color.blue() > color.red() + 40)
                    ++remainingPoints;
            }
        QCOMPARE(remainingPoints, 0);
    }
}

void EditorTests::chartsDiscardOldestRecords() {
    ivs::ChartWidget chart(ivs::ChartWidget::Kind::Trend);
    chart.resize(480, 280);
    QList<ivs::DetectionRecord> records;
    ivs::DetectionRecord old;
    old.diameterMm = 900;
    records.append(old);
    ivs::DetectionRecord current;
    current.diameterMm = 20;
    for (int n = 0; n < 1000; ++n)
        records.append(current);
    chart.setRecords(records);
    QImage image(chart.size(), QImage::Format_RGB32);
    chart.render(&image);
    int blueAtCenter = 0;
    for (int y = 127; y < 141; ++y)
        for (int x = 90; x < 435; ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.blue() > color.red() + 40)
                ++blueAtCenter;
        }
    QVERIFY2(blueAtCenter > 200,
             "The discarded outlier must not compress the latest 1,000 points toward the bottom axis");
    chart.appendRecord(old);
    for (int n = 0; n < 1000; ++n)
        chart.appendRecord(current);
    chart.render(&image);
    blueAtCenter = 0;
    for (int y = 127; y < 141; ++y)
        for (int x = 90; x < 435; ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.blue() > color.red() + 40)
                ++blueAtCenter;
        }
    QVERIFY2(blueAtCenter > 200, "Appending must also evict records older than the latest 1,000");
}

void EditorTests::imageFitsFrameAndClears() {
    ivs::ImageView view;
    view.resize(400, 300);
    ivs::InspectionResult result;
    result.frame = QImage(100, 50, QImage::Format_RGB32);
    result.frame.fill(QColor(25, 75, 210));
    result.record.outcome = QStringLiteral("OK");
    result.objectBounds = QRectF(40, 15, 20, 20);
    view.setResult(result);
    QImage image(view.size(), QImage::Format_RGB32);
    view.render(&image);
    QCOMPARE(image.pixelColor(50, 150), QColor(25, 75, 210));
    QVERIFY(image.pixelColor(50, 20) != QColor(25, 75, 210));
    const QColor center = image.pixelColor(200, 134);
    QVERIFY2(center.green() > center.blue() + 40 && center.green() > center.red() + 60,
             "The measurement center must follow the image's aspect-fit transform");
    view.clear();
    view.render(&image);
    QVERIFY(image.pixelColor(50, 150) != QColor(25, 75, 210));
}

QTEST_MAIN(EditorTests)
#include "tst_editors.moc"
