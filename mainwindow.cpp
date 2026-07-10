#include "mainwindow.h"
#include "realtimedbwriter.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QThread>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QRandomGenerator>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Qt 实时键值数据库工具类演示");
    resize(700, 450);

    QWidget *central = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(central);

    m_label = new QLabel("请先点击“加载XML配置”", central);
    m_log = new QPlainTextEdit(central);
    m_log->setReadOnly(true);

    QHBoxLayout *btnLayout = new QHBoxLayout;
    m_btnLoad   = new QPushButton("加载XML配置", central);
    m_btnUpdate = new QPushButton("模拟UDP更新", central);
    m_btnStress = new QPushButton("5线程并发", central);
    m_btnQuery  = new QPushButton("查询分系统A", central);
    m_btnSnap   = new QPushButton("全量快照", central);
    btnLayout->addWidget(m_btnLoad);
    btnLayout->addWidget(m_btnUpdate);
    btnLayout->addWidget(m_btnStress);
    btnLayout->addWidget(m_btnQuery);
    btnLayout->addWidget(m_btnSnap);

    layout->addWidget(m_label);
    layout->addLayout(btnLayout);
    layout->addWidget(m_log);
    setCentralWidget(central);

    RealtimeDbWriter::instance().init("realtime.db", "kv");
    connect(&RealtimeDbWriter::instance(), &RealtimeDbWriter::writeError,
            this, &MainWindow::onError);
    connect(&RealtimeDbWriter::instance(), &RealtimeDbWriter::writeFinished,
            this, &MainWindow::onWritten);

    connect(m_btnLoad,   &QPushButton::clicked, this, &MainWindow::onLoadXml);
    connect(m_btnUpdate, &QPushButton::clicked, this, &MainWindow::onSimulateUpdate);
    connect(m_btnStress, &QPushButton::clicked, this, &MainWindow::onStressTest);
    connect(m_btnQuery,  &QPushButton::clicked, this, &MainWindow::onQuery);
    connect(m_btnSnap,   &QPushButton::clicked, this, &MainWindow::onSnapshot);
}

MainWindow::~MainWindow()
{
    RealtimeDbWriter::instance().shutdown();
}

void MainWindow::onLoadXml()
{
    // 读取 config 目录下所有 XML，把每个分系统的 key 注册进数据库
    QDir dir(QCoreApplication::applicationDirPath() + "/config");
    QStringList files = dir.entryList(QStringList() << "*.xml", QDir::Files);
    if (files.isEmpty()) {
        m_log->appendPlainText("未找到 config/*.xml");
        return;
    }
    m_subsystems.clear();
    for (const QString &f : files) {
        QString path = dir.filePath(f);
        if (RealtimeDbWriter::instance().registerFromXml(path)) {
            // 从文件名推断分系统名用于演示（实际以 XML 内 subsystem 为准）
            m_subsystems.append(QFileInfo(f).baseName());
            m_log->appendPlainText(QDateTime::currentDateTime().toString("hh:mm:ss.zzz")
                                   + " 已注册配置: " + f);
        }
    }
    m_label->setText(QString("已加载 %1 个分系统配置").arg(m_subsystems.size()));
}

void MainWindow::onSimulateUpdate()
{
    if (m_subsystems.isEmpty()) { m_log->appendPlainText("请先加载XML配置"); return; }
    // 从数据库读回“分系统A”真实注册的 key，挑 3 个更新（保证 key 名对得上）
    QStringList keys = RealtimeDbWriter::instance().querySubsystem("分系统A").keys();
    if (keys.isEmpty()) { m_log->appendPlainText("分系统A 暂无已注册 key"); return; }
    QVariantMap data;
    for (int n = 0; n < 3 && n < keys.size(); ++n)
        data.insert(keys[n], QRandomGenerator::global()->generateDouble() * 100.0);
    RealtimeDbWriter::instance().updateBatch("分系统A", data);
    m_log->appendPlainText("已发送分系统A更新: " + data.keys().join(", "));
}

void MainWindow::onStressTest()
{
    if (m_subsystems.isEmpty()) { m_log->appendPlainText("请先加载XML配置"); return; }
    m_log->appendPlainText("启动 5 个线程模拟 5 个分系统并发更新（每线程100次）...");
    QStringList subs = {"分系统A", "分系统B", "分系统C", "分系统D", "分系统E"};
    // 先读回每个分系统真实注册的 key，传给对应线程，避免 key 名写错
    QMap<QString, QStringList> subKeys;
    for (const QString &sub : subs)
        subKeys[sub] = RealtimeDbWriter::instance().querySubsystem(sub).keys();

    int *done = new int(0);
    for (const QString &sub : subs) {
        QStringList keys = subKeys[sub];
        QThread *th = QThread::create([sub, keys]() {
            for (int i = 0; i < 100; ++i) {
                QVariantMap data;
                // 从本分系统真实 key 中随机挑 3 个更新，模拟解析后的实时数据
                for (int n = 0; n < 3; ++n) {
                    QString k = keys.at((i * (n + 3)) % keys.size());
                    data.insert(k, QRandomGenerator::global()->generateDouble() * 100.0);
                }
                RealtimeDbWriter::instance().updateBatch(sub, data);
            }
        });
        connect(th, &QThread::finished, th, &QThread::deleteLater);
        connect(th, &QThread::finished, this, [this, done, subs]() {
            if (++(*done) == subs.size()) {
                QVariantMap snap = RealtimeDbWriter::instance().snapshot();
                m_log->appendPlainText(QString("5 个线程全部完成，全量 key 数: %1（应为 2500）")
                                       .arg(snap.size()));
                delete done;
            }
        });
        th->start();
    }
}

void MainWindow::onQuery()
{
    QVariantMap row = RealtimeDbWriter::instance().querySubsystem("分系统A");
    m_log->appendPlainText(QString("分系统A 共 %1 个 key:").arg(row.size()));
    for (auto it = row.begin(); it != row.end(); ++it)
        m_log->appendPlainText(QString("  %1 = %2").arg(it.key()).arg(it.value().toString()));
}

void MainWindow::onSnapshot()
{
    QVariantMap snap = RealtimeDbWriter::instance().snapshot();
    m_log->appendPlainText(QString("全量快照共 %1 个 key/value").arg(snap.size()));
}

void MainWindow::onError(const QString &msg)
{
    m_log->appendPlainText("错误: " + msg);
}

void MainWindow::onWritten(qint64 batch)
{
    m_totalOps += batch;
    m_label->setText(QString("累计写/更新操作: %1 次").arg(m_totalOps));
}
