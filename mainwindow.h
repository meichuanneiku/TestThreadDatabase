#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onLoadXml();        // 读取 config/*.xml 并把 key 注册进数据库
    void onSimulateUpdate(); // 模拟某分系统解析后按 key 更新 value
    void onStressTest();     // 5 个线程模拟 5 个分系统并发更新
    void onQuery();          // 查询某分系统
    void onSnapshot();       // 全量快照

    void onWritten(qint64 batch); // 实时累计已写入/更新次数
    void onError(const QString &msg);

private:
    QLabel *m_label;
    QPushButton *m_btnLoad;
    QPushButton *m_btnUpdate;
    QPushButton *m_btnStress;
    QPushButton *m_btnQuery;
    QPushButton *m_btnSnap;
    QPlainTextEdit *m_log;

    QStringList m_subsystems; // 已加载的分系统名
    qint64 m_totalOps = 0;    // 累计写/更新操作次数（实时显示）
};

#endif // MAINWINDOW_H
