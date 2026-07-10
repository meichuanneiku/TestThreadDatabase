#include "realtimedbwriter.h"

#include <QThread>
#include <QTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QXmlStreamReader>
#include <QFile>

// ---------------------------------------------------------------------------
// 后台写线程 worker：在独立线程中持有自己的 SQLite 连接
// （类声明见 realtimedbwriter.h，此处为 out-of-line 实现）
// ---------------------------------------------------------------------------
void RealtimeDbWriterWorker::init()
{
    QSqlDatabase db = openConnection();
    if (!db.isOpen()) {
        emit writeError("无法打开数据库连接: " + db.lastError().text());
        return;
    }
    ensureTable(db);
}

void RealtimeDbWriterWorker::doRegister(const QString &subsystem, const QStringList &keys)
{
    if (keys.isEmpty())
        return;
    QSqlDatabase db = openConnection();
    if (!db.isOpen()) { emit writeError("注册时数据库连接不可用"); return; }
    ensureTable(db);

    QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO %1 (subsystem, key, value, ts) VALUES (?,?,?,?)")
        .arg(m_table));
    for (const QString &k : keys) {
        q.addBindValue(subsystem);
        q.addBindValue(k);
        q.addBindValue("");
        q.addBindValue(ts);
        if (!q.exec()) {
            db.rollback();
            emit writeError("注册失败: " + q.lastError().text());
            return;
        }
    }
    db.commit();
    emit writeFinished(keys.size());
}

void RealtimeDbWriterWorker::doUpdate(const QString &subsystem, const QVariantMap &data)
{
    if (data.isEmpty())
        return;
    QSqlDatabase db = openConnection();
    if (!db.isOpen()) { emit writeError("更新时数据库连接不可用"); return; }
    ensureTable(db);

    QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO %1 (subsystem, key, value, ts) VALUES (?,?,?,?) "
        "ON CONFLICT(subsystem, key) DO UPDATE SET value=excluded.value, ts=excluded.ts")
        .arg(m_table));
    for (auto it = data.begin(); it != data.end(); ++it) {
        q.addBindValue(subsystem);
        q.addBindValue(it.key());
        q.addBindValue(it.value().toString());
        q.addBindValue(ts);
        if (!q.exec()) {
            db.rollback();
            emit writeError("更新失败: " + q.lastError().text());
            return;
        }
    }
    db.commit();
    emit writeFinished(data.size());
}

void RealtimeDbWriterWorker::ensureTable(QSqlDatabase &db)
{
    QSqlQuery q(db);
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS %1 ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "subsystem TEXT, "
        "key TEXT, "
        "value TEXT, "
        "ts TEXT, "
        "UNIQUE(subsystem, key))").arg(m_table));
}

QSqlDatabase RealtimeDbWriterWorker::openConnection()
{
    QString conn = QString("rt_writer_%1").arg((quintptr)QThread::currentThreadId());
    if (!QSqlDatabase::contains(conn)) {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
        db.setDatabaseName(m_dbPath);
        db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
        if (db.open()) {
            QSqlQuery(db).exec("PRAGMA journal_mode = WAL");
            QSqlQuery(db).exec("PRAGMA synchronous = NORMAL");
        }
    }
    return QSqlDatabase::database(conn);
}


// ---------------------------------------------------------------------------
// RealtimeDbWriter 实现
// ---------------------------------------------------------------------------
RealtimeDbWriter &RealtimeDbWriter::instance()
{
    static RealtimeDbWriter inst;
    return inst;
}

RealtimeDbWriter::RealtimeDbWriter(QObject *parent) : QObject(parent) {}

RealtimeDbWriter::~RealtimeDbWriter()
{
    shutdown();
}

bool RealtimeDbWriter::init(const QString &dbPath, const QString &table)
{
    m_dbPath = dbPath;
    m_table = table;

    if (m_thread)
        shutdown();

    m_thread = new QThread(this);
    m_worker = new RealtimeDbWriterWorker(dbPath, table);
    m_worker->moveToThread(m_thread);

    connect(this, &RealtimeDbWriter::enqueueRegister, m_worker, &RealtimeDbWriterWorker::doRegister);
    connect(this, &RealtimeDbWriter::enqueueUpdate, m_worker, &RealtimeDbWriterWorker::doUpdate);
    connect(m_thread, &QThread::started, m_worker, &RealtimeDbWriterWorker::init);
    connect(m_worker, &RealtimeDbWriterWorker::writeError, this, &RealtimeDbWriter::writeError);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_thread->start();
    return true;
}

void RealtimeDbWriter::registerKeys(const QString &subsystem, const QStringList &keys)
{
    if (m_worker)
        emit enqueueRegister(subsystem, keys);
}

bool RealtimeDbWriter::registerFromXml(const QString &xmlPath)
{
    QString subsystem;
    QStringList keys;
    if (!loadXmlConfig(xmlPath, subsystem, keys)) {
        emit writeError("解析 XML 失败: " + xmlPath);
        return false;
    }
    registerKeys(subsystem, keys);
    return true;
}

void RealtimeDbWriter::update(const QString &subsystem, const QString &key, const QVariant &value)
{
    QVariantMap m;
    m.insert(key, value);
    updateBatch(subsystem, m);
}

void RealtimeDbWriter::updateBatch(const QString &subsystem, const QVariantMap &data)
{
    if (m_worker)
        emit enqueueUpdate(subsystem, data);
}

// 读连接按调用线程隔离
static QSqlDatabase openReadConnection(const QString &dbPath)
{
    QString conn = QString("rt_read_%1").arg((quintptr)QThread::currentThreadId());
    if (!QSqlDatabase::contains(conn)) {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
        db.setDatabaseName(dbPath);
        db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
        db.open();
    }
    return QSqlDatabase::database(conn);
}

QVariantMap RealtimeDbWriter::querySubsystem(const QString &subsystem)
{
    QSqlDatabase db = openReadConnection(m_dbPath);
    QVariantMap result;
    if (!db.isOpen())
        return result;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT key, value, ts FROM %1 WHERE subsystem=:sub").arg(m_table));
    q.bindValue(":sub", subsystem);
    if (q.exec()) {
        while (q.next())
            result.insert(q.value("key").toString(), q.value("value").toString());
    }
    return result;
}

QVariantMap RealtimeDbWriter::queryKey(const QString &subsystem, const QString &key)
{
    QSqlDatabase db = openReadConnection(m_dbPath);
    QVariantMap result;
    if (!db.isOpen())
        return result;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT value, ts FROM %1 WHERE subsystem=:sub AND key=:key").arg(m_table));
    q.bindValue(":sub", subsystem);
    q.bindValue(":key", key);
    if (q.exec() && q.next()) {
        result.insert("value", q.value("value"));
        result.insert("ts", q.value("ts"));
    }
    return result;
}

QVariantMap RealtimeDbWriter::snapshot()
{
    QSqlDatabase db = openReadConnection(m_dbPath);
    QVariantMap result;
    if (!db.isOpen())
        return result;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT subsystem, key, value FROM %1").arg(m_table));
    if (q.exec()) {
        while (q.next()) {
            QString composite = q.value("subsystem").toString() + "/" + q.value("key").toString();
            result.insert(composite, q.value("value").toString());
        }
    }
    return result;
}

void RealtimeDbWriter::shutdown()
{
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
        m_thread->deleteLater();
        m_thread = nullptr;
        m_worker = nullptr;
    }
}

bool RealtimeDbWriter::loadXmlConfig(const QString &xmlPath,
                                     QString &subsystem,
                                     QStringList &keys)
{
    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QXmlStreamReader xml(&file);
    keys.clear();
    subsystem.clear();

    while (!xml.atEnd() && !xml.hasError()) {
        QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            QString name = xml.name().toString();
            QXmlStreamAttributes attrs = xml.attributes();
            if (name.compare("config", Qt::CaseInsensitive) == 0 ||
                name.compare("system", Qt::CaseInsensitive) == 0) {
                // subsystem 属性可能叫 subsystem / name / id
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
