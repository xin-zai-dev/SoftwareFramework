#include "domain/Project.h"
#include "infrastructure/RecordRepository.h"
#include "ui/MainWindow.h"
#include "ui/RobotConsole.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>

static void answerDialog(QWidget *context, QMessageBox::StandardButton answer) {
    QTimer::singleShot(30, context, [answer] {
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *box = qobject_cast<QMessageBox *>(widget))
                if (auto *button = box->button(answer))
                    button->click();
    });
}
class MainWindowTest : public QObject {
    Q_OBJECT
  private slots:
    void manualConsoleTriggerDrivesRealPipelineAndOutput() {
        QTemporaryDir dir;
        ivs::MainWindow window(dir.path(), {}, false);
        window.show();
        auto *navigation = window.findChild<QListWidget *>("navigation");
        QVERIFY(navigation);
        QCOMPARE(navigation->count(), 8);
        navigation->setCurrentRow(6);
        auto *automatic = window.findChild<QCheckBox *>("automaticTriggerCheck");
        auto *trigger = window.findChild<QPushButton *>("triggerOnceButton");
        auto *single = window.findChild<QPushButton *>("singleRunButton");
        auto *count = window.findChild<QLabel *>("sessionRecordCount");
        QVERIFY(automatic && trigger && single && count);
        automatic->setChecked(false);
        single->click();
        QTest::qWait(150);
        QCOMPARE(count->text(), QStringLiteral("0"));
        QVERIFY(!single->isEnabled());
        QVERIFY(navigation->item(6)->flags().testFlag(Qt::ItemIsEnabled));
        trigger->click();
        QTRY_VERIFY_WITH_TIMEOUT(single->isEnabled(), 5000);
        QCOMPARE(count->text(), QStringLiteral("1"));
        auto *signalDevice = window.findChild<QComboBox *>("signalDeviceCombo");
        QVERIFY(signalDevice);
        signalDevice->setCurrentIndex(signalDevice->findData("plc-1"));
        auto *output = window.findChild<QSpinBox *>("signal0Holding");
        QVERIFY(output);
        QTRY_COMPARE(output->value(), 1);
        ivs::RecordRepository repo;
        QString error;
        QVERIFY2(repo.open({}, dir.path(), &error), qPrintable(error));
        QCOMPARE(repo.count({}, &error), 1);
        QCOMPARE(repo.query({}, &error).front().outcome, QStringLiteral("OK"));
    }
    void consoleLightOverrideSurvivesStartAndCanRecover() {
        QTemporaryDir dir;
        ivs::MainWindow window(dir.path(), {}, false);
        window.show();
        window.findChild<QListWidget *>("navigation")->setCurrentRow(6);
        auto *manual = window.findChild<QCheckBox *>("manualOverrideCheck");
        auto *light = window.findChild<QCheckBox *>("light0Enabled");
        auto *single = window.findChild<QPushButton *>("singleRunButton");
        auto *count = window.findChild<QLabel *>("sessionRecordCount");
        QVERIFY(manual && light && single && count);
        manual->setChecked(true);
        light->setChecked(false);
        single->click();
        QTRY_VERIFY_WITH_TIMEOUT(single->isEnabled(), 5000);
        QCOMPARE(count->text(), QStringLiteral("0"));
        light->setChecked(true);
        single->click();
        QTRY_VERIFY_WITH_TIMEOUT(single->isEnabled(), 5000);
        QCOMPARE(count->text(), QStringLiteral("1"));
    }
    void simulationConsoleRemainsAvailableInRuntimeMode() {
        QTemporaryDir dir;
        ivs::MainWindow window(dir.path(), {}, true);
        auto *navigation = window.findChild<QListWidget *>("navigation");
        QCOMPARE(navigation->count(), 8);
        QVERIFY(!navigation->item(6)->isHidden());
        QVERIFY(!navigation->item(7)->isHidden());
        QVERIFY(navigation->item(1)->isHidden());
    }
    void robotLaboratoryIsConnectedAndLocksDuringPipeline() {
        QTemporaryDir dir;
        ivs::MainWindow window(dir.path(), {}, false);
        window.show();
        auto *navigation = window.findChild<QListWidget *>("navigation");
        QCOMPARE(navigation->count(), 8);
        navigation->setCurrentRow(7);
        auto *robots = window.findChild<QComboBox *>("robotDeviceCombo");
        auto *move = window.findChild<QPushButton *>("robotMoveTcp");
        auto *stop = window.findChild<QPushButton *>("robotStopButton");
        QVERIFY(robots && move && stop);
        QCOMPARE(robots->currentData().toString(), QStringLiteral("robot-1"));
        QVERIFY(move->isEnabled());
        window.findChild<QCheckBox *>("automaticTriggerCheck")->setChecked(false);
        window.findChild<QPushButton *>("singleRunButton")->click();
        QVERIFY(!move->isEnabled());
        QVERIFY(stop->isEnabled());
        QVERIFY(navigation->item(7)->flags().testFlag(Qt::ItemIsEnabled));
        window.findChild<QPushButton *>("stop")->click();
        QTRY_VERIFY(move->isEnabled());
    }
    void olderProjectCanAddVirtualRobotFromLaboratory() {
        QTemporaryDir dir;
        auto project = ivs::defaultProject();
        for (int i = project.devices.size() - 1; i >= 0; --i)
            if (project.devices[i].category == "robot")
                project.devices.removeAt(i);
        QString error;
        QVERIFY2(ivs::saveProject(project, dir.filePath("older.ivproj"), &error), qPrintable(error));
        ivs::MainWindow window(dir.path(), dir.filePath("older.ivproj"), false);
        window.show();
        window.findChild<QListWidget *>("navigation")->setCurrentRow(7);
        auto *robots = window.findChild<QComboBox *>("robotDeviceCombo");
        auto *add = window.findChild<QPushButton *>("robotAddDeviceButton");
        QVERIFY(robots && add);
        QCOMPARE(robots->count(), 0);
        add->click();
        QCOMPARE(robots->count(), 1);
        QVERIFY(window.findChild<QPushButton *>("robotMoveTcp")->isEnabled());
        answerDialog(&window, QMessageBox::Discard);
        window.close();
    }
    void bundledTeachingLoadsAndNewProjectClearsPoints() {
        QTemporaryDir dir;
        ivs::MainWindow window(dir.path(), {}, false);
        window.show();
        window.findChild<QListWidget *>("navigation")->setCurrentRow(7);
        auto *console = window.findChild<ivs::RobotConsole *>();
        auto *table = window.findChild<QTableWidget *>("robotTeachTable");
        QVERIFY(console && table);
        const QString path = QFINDTESTDATA("../examples/scara-teaching-demo.ivrobot.json");
        QVERIFY(!path.isEmpty());
        QString error;
        QVERIFY2(console->loadTeaching(path, &error), qPrintable(error));
        QCOMPARE(table->rowCount(), 7);
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("料位上方"));
        auto *play = window.findChild<QPushButton *>("robotPlayButton");
        auto *part = window.findChild<QLabel *>("robotPartLabel");
        QVERIFY(play && part);
        play->click();
        QTRY_VERIFY_WITH_TIMEOUT(part->text().contains("OK 1 / NG 0"), 12000);
        QTRY_VERIFY_WITH_TIMEOUT(play->isEnabled(), 4000);
        window.findChild<QPushButton *>("newProjectButton")->click();
        QCOMPARE(table->rowCount(), 0);
    }
    void newProjectResetsConsoleState() {
        QTemporaryDir dir;
        ivs::MainWindow window(dir.path(), {}, false);
        window.show();
        auto *manual = window.findChild<QCheckBox *>("manualOverrideCheck");
        auto *automatic = window.findChild<QCheckBox *>("automaticTriggerCheck");
        auto *light = window.findChild<QCheckBox *>("light0Enabled");
        QVERIFY(manual && automatic && light);
        auto *target = window.findChild<QDoubleSpinBox *>("axis0Target");
        auto *speed = window.findChild<QDoubleSpinBox *>("axis0Speed");
        QVERIFY(target && speed);
        manual->setChecked(true);
        automatic->setChecked(false);
        light->setChecked(false);
        target->setValue(50);
        speed->setValue(2);
        window.findChild<QPushButton *>("newProjectButton")->click();
        QVERIFY(!manual->isChecked());
        QVERIFY(automatic->isChecked());
        QVERIFY(light->isChecked());
        QCOMPARE(target->value(), 0.0);
        QCOMPARE(speed->value(), 25.0);
    }
    void cancellationPreservesUnsavedSettings() {
        QTemporaryDir dir;
        ivs::MainWindow window(dir.path(), {}, false);
        window.show();
        auto *name = window.findChild<QLineEdit *>("projectNameInput");
        QVERIFY(name);
        name->setText("未保存的工件工程");
        answerDialog(&window, QMessageBox::Cancel);
        QVERIFY(!window.close());
        QCOMPARE(name->text(), QStringLiteral("未保存的工件工程"));
        answerDialog(&window, QMessageBox::Discard);
        QVERIFY(window.close());
    }
    void newProjectCanBeCancelled() {
        QTemporaryDir dir;
        ivs::MainWindow window(dir.path(), {}, false);
        window.show();
        auto *name = window.findChild<QLineEdit *>("projectNameInput");
        name->setText("不要覆盖");
        answerDialog(&window, QMessageBox::Cancel);
        window.findChild<QPushButton *>("newProjectButton")->click();
        QCOMPARE(name->text(), QStringLiteral("不要覆盖"));
        answerDialog(&window, QMessageBox::Discard);
        window.close();
    }
};
QTEST_MAIN(MainWindowTest)
#include "tst_mainwindow.moc"
