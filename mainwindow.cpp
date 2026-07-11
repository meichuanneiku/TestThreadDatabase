#include "mainwindow.h"
#include "dbservice.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QThread>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QRandomGenerator>
#include <QXmlStreamReader>
#include <QFile>
#include <QMap>

// 解析 XML 配置，提取 subsystem 名称与 key 列表（与具体表结构无关，属于演示层）
static bool parseXmlKeys(const QString &xmlPath, QString &subsystem, QStringList &keys)
{
    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QXmlStreamReader xml(&file);
    keys.clear();
    subsystem.clear();
    while (!xml.atEnd() && !xml.hasError()) {
        if (xml.readNext() == QXmlStreamReader::StartElement) {
            QString name = xml.name().toString();
            QXmlStreamAttributes attrs = xml.attributes();
            if (name.compare("config", Qt::CaseInsensitive) == 0 ||
                name.compare("system", Qt::CaseInsensitive) == 0) {
                subsystem = attrs.value("subsystem").toString();
                if (subsystem.isEmpty()) subsystem = attrs.value("name").toString();
                if (subsystem.isEmpty()) subsystem = attrs.value("id").toString();
            } else if (name.compare("item", Qt::CaseInsensitive) == 0 ||
                       name.compare("key", Qt::CaseInsensitive) == 0 ||
                       name.compare("param", Qt::CaseInsensitive) == 0) {
                QString k = attrs.value("key").toString();
                if (k.isEmpty()) k = attrs.value("name").toString();
                if (!k.isEmpty() && !keys.contains(k))
                    keys.append(k);
            }
        }
    }
    file.close();
    return !subsystem.isEmpty() && !keys.isEmpty() && !xml.hasError();
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Qt 通用数据库服务演示（DbService）");
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

    // 初始化通用数据库服务（单写线程 + 队列）
    DbService::instance().init("realtime.db");
    connect(&DbService::instance(), &DbService::error,    this, &MainWindow::onError);
    connect(&DbService::instance(), &DbService::executed, this, &MainWindow::onWritten);

    connect(m_btnLoad,   &QPushButton::clicked, this, &MainWindow::onLoadXml);
    connect(m_btnUpdate, &QPushButton::clicked, this, &MainWindow::onSimulateUpdate);
    connect(m_btnStress, &QPushButton::clicked, this, &MainWindow::onStressTest);
    connect(m_btnQuery,  &QPushButton::clicked, this, &MainWindow::onQuery);
    connect(m_btnSnap,   &QPushButton::clicked, this, &MainWindow::onSnapshot);
}

MainWindow::~MainWindow()
{
    DbService::instance().shutdown();
}

void MainWindow::onLoadXml()
{
    // 建表（通用 DDL，通过 exec 异步执行；与具体业务解耦）
    DbService::instance().exec(
        "CREATE TABLE IF NOT EXISTS kv ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "subsystem TEXT, key TEXT, value TEXT, ts TEXT, "
        "UNIQUE(subsystem, key))");

    QDir dir(QCoreApplication::applicationDirPath() + "/config");
    QStringList files = dir.entryList(QStringList() << "*.xml", QDir::Files);
    if (files.isEmpty()) { m_log->appendPlainText("未找到 config/*.xml"); return; }

    m_subsystems.clear();
    m_keys.clear();
    for (const QString &f : files) {
        QString subsystem; QStringList keys;
        if (parseXmlKeys(dir.filePath(f), subsystem, keys)) {
            // 用通用原语 insertBatch 注册 key（忽略已存在）
            QList<QVariantMap> rows;
            for (const QString &k : keys)
                rows.append({{"subsystem", subsystem}, {"key", k}, {"value", ""}, {"ts", ""}});
            DbService::instance().insertBatch("kv", rows, /*ignoreExisting=*/true);
            m_subsystems.append(subsystem);
            m_keys.insert(subsystem, keys);
            m_log->appendPlainText(QDateTime::currentDateTime().toString("hh:mm:ss.zzz")
                                   + " 已注册配置: " + f + " (key 数 " + QString::number(keys.size()) + ")");
        }
    }
    m_label->setText(QString("已加载 %1 个分系统配置").arg(m_subsystems.size()));
}

void MainWindow::onSimulateUpdate()
{
    if (m_subsystems.isEmpty()) { m_log->appendPlainText("请先加载XML配置"); return; }
    // 用内存缓存的分系统A真实 key，挑 3 个更新（保证 key 名对得上，且不依赖异步写库时序）
    QStringList keys = m_keys.value("分系统A");
    if (keys.isEmpty()) { m_log->appendPlainText("分系统A 暂无已注册 key"); return; }
    QList<QVariantMap> rows;
    QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    for (int n = 0; n < 3 && n < keys.size(); ++n)
        rows.append({{"subsystem", "分系统A"}, {"key", keys[n]},
                     {"value", QRandomGenerator::global()->generateDouble() * 100.0}, {"ts", ts}});
    DbService::instance().updateBatch("kv", rows, {"subsystem", "key"});
    m_log->appendPlainText("已发送分系统A更新: " + keys.mid(0, 3).join(", "));
}

void MainWindow::onStressTest()
{
    if (m_subsystems.isEmpty()) { m_log->appendPlainText("请先加载XML配置"); return; }
    m_log->appendPlainText("启动 5 个线程模拟 5 个分系统并发更新（每线程100次）...");
    int *done = new int(0);
    int *total0 = new int(0);
    for (const QString &sub : m_subsystems) {
        // 用内存缓存的本分系统真实 key（不依赖异步写库时序）
        QStringList keys = m_keys.value(sub);
        if (keys.isEmpty()) { m_log->appendPlainText(sub + " 无可用 key，跳过"); continue; }
        ++(*total0);
        QThread *th = QThread::create([sub, keys]() {
            for (int i = 0; i < 100; ++i) {
                QList<QVariantMap> rows;
                QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
                for (int n = 0; n < 3; ++n) {
                    QString k = keys.at((i * (n + 3)) % keys.size());
                    rows.append({{"subsystem", sub}, {"key", k},
                                 {"value", QRandomGenerator::global()->generateDouble() * 100.0}, {"ts", ts}});
                }
                DbService::instance().updateBatch("kv", rows, {"subsystem", "key"});
            }
        });
        connect(th, &QThread::finished, th, &QThread::deleteLater);
        connect(th, &QThread::finished, this, [this, done, total0]() {
            if (++(*done) == *total0) {
                int total = DbService::instance().select("kv", QStringList() << "key").size();
                m_log->appendPlainText(QString("全部线程完成，全量 key 数: %1").arg(total));
                delete done;
                delete total0;
            }
        });
        th->start();
    }
    if (*total0 == 0) { delete done; delete total0; }
}

void MainWindow::onQuery()
{
    QList<QVariantMap> rows = DbService::instance()
            .select("kv", {"key", "value"}, {{"subsystem", "分系统A"}});
    m_log->appendPlainText(QString("分系统A 共 %1 个 key（仅显示有值的）:").arg(rows.size()));
    int shown = 0;
    for (const QVariantMap &row : rows) {
        QString v = row.value("value").toString();
        if (!v.isEmpty() && shown < 10) {
            m_log->appendPlainText(QString("  %1 = %2").arg(row.value("key").toString(), v));
            ++shown;
        }
    }
}

void MainWindow::onSnapshot()
{
    QList<QVariantMap> rows = DbService::instance().select("kv", {"subsystem", "key", "value"});
    m_log->appendPlainText(QString("全量快照共 %1 个 key/value").arg(rows.size()));
}

void MainWindow::onError(const QString &msg)
{
    m_log->appendPlainText("错误: " + msg);
}

void MainWindow::onWritten(const QString &op, int affected)
{
    m_totalOps += affected;
    m_label->setText(QString("累计写/更新操作: %1 次（最近: %2 +%3）")
                     .arg(m_totalOps).arg(op).arg(affected));
}
