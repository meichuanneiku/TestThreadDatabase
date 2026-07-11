#include "dbservice.h"

#include <QSqlError>
#include <QSqlRecord>
#include <QDateTime>
#include <QMutex>

// ---------------------------------------------------------------------------
// 后台单写线程 worker：在独立线程中持有唯一 SQLite 连接，串行执行所有写操作
// （类声明见 dbservice.h，此处为 out-of-line 实现）
// ---------------------------------------------------------------------------

void DbServiceWorker::init()
{
    QString conn = QString("dbw_writer_%1").arg((quintptr)QThread::currentThreadId());
    if (!QSqlDatabase::contains(conn))
        QSqlDatabase::addDatabase("QSQLITE", conn).setDatabaseName(m_dbPath);
    m_db = QSqlDatabase::database(conn);
    m_db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
    if (!m_db.open())
        emit error("无法打开数据库连接 " + m_db.lastError().text());
    else {
        QSqlQuery(m_db).exec("PRAGMA journal_mode = WAL");
        QSqlQuery(m_db).exec("PRAGMA synchronous = NORMAL");
    }
}

// where 条件构建 "k1=? AND k2=?"，值按顺序填入 outValues（绑定在 prepare 之后进行）
QString DbServiceWorker::buildWhere(const QVariantMap &where, QVariantList &outValues)
{
    if (where.isEmpty())
        return QString();
    QStringList parts;
    for (auto it = where.begin(); it != where.end(); ++it) {
        parts.append(it.key() + "=?");
        outValues.append(it.value());
    }
    return parts.join(" AND ");
}

int DbServiceWorker::execInsert(QSqlDatabase &db, const QString &table,
                                const QVariantMap &row, bool ignoreExisting)
{
    if (row.isEmpty())
        return 0;
    QSqlQuery q(db);
    QStringList cols, ph;
    QVariantList vals;
    for (auto it = row.begin(); it != row.end(); ++it) {
        cols.append(it.key());
        ph.append("?");
        vals.append(it.value());
    }
    QString sql = QString("INSERT %1INTO %2 (%3) VALUES (%4)")
            .arg(ignoreExisting ? "OR IGNORE " : "")
            .arg(table).arg(cols.join(",")).arg(ph.join(","));
    q.prepare(sql);
    for (const QVariant &v : vals) q.addBindValue(v);
    if (!q.exec()) {
        emit error("insert 失败: " + q.lastError().text());
        return -1;
    }
    return q.numRowsAffected();
}

int DbServiceWorker::execUpdate(QSqlDatabase &db, const QString &table,
                                const QVariantMap &row, const QVariantMap &where)
{
    if (row.isEmpty())
        return 0;
    QSqlQuery q(db);
    QStringList sets;
    QVariantList setVals;
    for (auto it = row.begin(); it != row.end(); ++it) {
        sets.append(it.key() + "=?");
        setVals.append(it.value());
    }
    QVariantList whereVals;
    QString w = buildWhere(where, whereVals);
    QString sql = QString("UPDATE %1 SET %2").arg(table).arg(sets.join(","));
    if (!w.isEmpty())
        sql += " WHERE " + w;
    q.prepare(sql);
    for (const QVariant &v : setVals) q.addBindValue(v);
    for (const QVariant &v : whereVals) q.addBindValue(v);
    if (!q.exec()) {
        emit error("update 失败: " + q.lastError().text());
        return -1;
    }
    return q.numRowsAffected();
}

int DbServiceWorker::execRemove(QSqlDatabase &db, const QString &table, const QVariantMap &where)
{
    QSqlQuery q(db);
    QVariantList whereVals;
    QString w = buildWhere(where, whereVals);
    QString sql = QString("DELETE FROM %1").arg(table);
    if (!w.isEmpty())
        sql += " WHERE " + w;
    q.prepare(sql);
    for (const QVariant &v : whereVals) q.addBindValue(v);
    if (!q.exec()) {
        emit error("remove 失败: " + q.lastError().text());
        return -1;
    }
    return q.numRowsAffected();
}

int DbServiceWorker::execUpdateBatch(QSqlDatabase &db, const QString &table,
                                     const QList<QVariantMap> &rows, const QStringList &keyColumns)
{
    if (rows.isEmpty())
        return 0;
    QStringList cols = rows.first().keys();   // 假定所有行列一致
    QStringList ph;
    for (int i = 0; i < cols.size(); ++i) ph.append("?");
    QStringList setClauses;
    for (const QString &c : cols)
        if (!keyColumns.contains(c))
            setClauses.append(QString("%1=excluded.%1").arg(c));

    QSqlQuery q(db);
    QString sql = QString("INSERT INTO %1 (%2) VALUES (%3) "
                          "ON CONFLICT(%4) DO UPDATE SET %5")
            .arg(table).arg(cols.join(",")).arg(ph.join(","))
            .arg(keyColumns.join(",")).arg(setClauses.join(", "));
    if (!q.prepare(sql)) {
        emit error("updateBatch 准备失败: " + q.lastError().text());
        return -1;
    }
    for (const QVariantMap &row : rows) {
        for (const QString &c : cols)
            q.addBindValue(row.value(c));
        if (!q.exec()) {
            emit error("updateBatch 失败: " + q.lastError().text());
            return -1;
        }
    }
    return rows.size();
}

void DbServiceWorker::doInsert(const QString &table, const QVariantMap &row, bool ignoreExisting)
{
    QSqlDatabase &db = m_db;
    if (!db.isOpen()) { emit error("连接不可用"); return; }
    db.transaction();
    int n = execInsert(db, table, row, ignoreExisting);
    if (n < 0) { db.rollback(); return; }
    db.commit();
    emit executed("insert", n);
}

void DbServiceWorker::doUpdate(const QString &table, const QVariantMap &row, const QVariantMap &where)
{
    QSqlDatabase &db = m_db;
    if (!db.isOpen()) { emit error("连接不可用"); return; }
    db.transaction();
    int n = execUpdate(db, table, row, where);
    if (n < 0) { db.rollback(); return; }
    db.commit();
    emit executed("update", n);
}

void DbServiceWorker::doRemove(const QString &table, const QVariantMap &where)
{
    QSqlDatabase &db = m_db;
    if (!db.isOpen()) { emit error("连接不可用"); return; }
    db.transaction();
    int n = execRemove(db, table, where);
    if (n < 0) { db.rollback(); return; }
    db.commit();
    emit executed("remove", n);
}

void DbServiceWorker::doInsertBatch(const QString &table, const QList<QVariantMap> &rows, bool ignoreExisting)
{
    QSqlDatabase &db = m_db;
    if (!db.isOpen()) { emit error("连接不可用"); return; }
    db.transaction();
    int total = 0;
    for (const QVariantMap &row : rows) {
        int n = execInsert(db, table, row, ignoreExisting);
        if (n < 0) { db.rollback(); return; }
        total += n;
    }
    db.commit();
    emit executed("insertBatch", total);
}

void DbServiceWorker::doUpdateBatch(const QString &table, const QList<QVariantMap> &rows, const QStringList &keyColumns)
{
    QSqlDatabase &db = m_db;
    if (!db.isOpen()) { emit error("连接不可用"); return; }
    db.transaction();
    int n = execUpdateBatch(db, table, rows, keyColumns);
    if (n < 0) { db.rollback(); return; }
    db.commit();
    emit executed("updateBatch", n);
}

void DbServiceWorker::doRemoveBatch(const QString &table, const QList<QVariantMap> &wheres)
{
    QSqlDatabase &db = m_db;
    if (!db.isOpen()) { emit error("连接不可用"); return; }
    db.transaction();
    int total = 0;
    for (const QVariantMap &w : wheres) {
        int n = execRemove(db, table, w);
        if (n < 0) { db.rollback(); return; }
        total += n;
    }
    db.commit();
    emit executed("removeBatch", total);
}

void DbServiceWorker::doTransaction(const QList<DbCommand> &cmds)
{
    QSqlDatabase &db = m_db;
    if (!db.isOpen()) { emit error("连接不可用"); return; }
    db.transaction();
    int total = 0;
    for (const DbCommand &c : cmds) {
        int n = -1;
        if (c.type == DbCommand::Insert)      n = execInsert(db, c.table, c.data, false);
        else if (c.type == DbCommand::Update) n = execUpdate(db, c.table, c.data, c.where);
        else if (c.type == DbCommand::Remove) n = execRemove(db, c.table, c.where);
        if (n < 0) { db.rollback(); return; }
        total += n;
    }
    db.commit();
    emit executed("transaction", total);
}

void DbServiceWorker::doExec(const QString &sql)
{
    QSqlDatabase &db = m_db;
    if (!db.isOpen()) { emit error("连接不可用"); return; }
    QSqlQuery q(db);
    if (!q.exec(sql)) {
        emit error("exec 失败: " + q.lastError().text());
        return;
    }
    emit executed("exec", q.numRowsAffected());
}

// ---------------------------------------------------------------------------
// DbService 实现
// ---------------------------------------------------------------------------
DbService &DbService::instance()
{
    static DbService inst;
    return inst;
}

DbService::DbService(QObject *parent) : QObject(parent) {}

DbService::~DbService()
{
    shutdown();
}

bool DbService::init(const QString &dbPath)
{
    m_dbPath = dbPath;
    if (m_thread)
        shutdown();

    qRegisterMetaType<QVariantMap>("QVariantMap");
    qRegisterMetaType<QList<QVariantMap>>("QList<QVariantMap>");
    qRegisterMetaType<DbCommand>("DbCommand");
    qRegisterMetaType<QList<DbCommand>>("QList<DbCommand>");

    m_thread = new QThread(this);
    m_worker = new DbServiceWorker(dbPath);
    m_worker->moveToThread(m_thread);

    connect(this, &DbService::enqueueInsert,       m_worker, &DbServiceWorker::doInsert);
    connect(this, &DbService::enqueueUpdate,       m_worker, &DbServiceWorker::doUpdate);
    connect(this, &DbService::enqueueRemove,       m_worker, &DbServiceWorker::doRemove);
    connect(this, &DbService::enqueueInsertBatch,  m_worker, &DbServiceWorker::doInsertBatch);
    connect(this, &DbService::enqueueUpdateBatch, m_worker, &DbServiceWorker::doUpdateBatch);
    connect(this, &DbService::enqueueRemoveBatch, m_worker, &DbServiceWorker::doRemoveBatch);
    connect(this, &DbService::enqueueTransaction,  m_worker, &DbServiceWorker::doTransaction);
    connect(this, &DbService::enqueueExec,         m_worker, &DbServiceWorker::doExec);

    connect(m_thread, &QThread::started, m_worker, &DbServiceWorker::init);
    connect(m_worker, &DbServiceWorker::executed, this, &DbService::executed);
    connect(m_worker, &DbServiceWorker::error,    this, &DbService::error);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_thread->start();
    return true;
}

void DbService::insert(const QString &table, const QVariantMap &row, bool ignoreExisting)
{
    if (m_worker) emit enqueueInsert(table, row, ignoreExisting);
}

void DbService::update(const QString &table, const QVariantMap &row, const QVariantMap &where)
{
    if (m_worker) emit enqueueUpdate(table, row, where);
}

void DbService::remove(const QString &table, const QVariantMap &where)
{
    if (m_worker) emit enqueueRemove(table, where);
}

void DbService::insertBatch(const QString &table, const QList<QVariantMap> &rows, bool ignoreExisting)
{
    if (m_worker) emit enqueueInsertBatch(table, rows, ignoreExisting);
}

void DbService::updateBatch(const QString &table, const QList<QVariantMap> &rows, const QStringList &keyColumns)
{
    if (m_worker) emit enqueueUpdateBatch(table, rows, keyColumns);
}

void DbService::removeBatch(const QString &table, const QList<QVariantMap> &wheres)
{
    if (m_worker) emit enqueueRemoveBatch(table, wheres);
}

void DbService::transaction(const QList<DbCommand> &cmds)
{
    if (m_worker) emit enqueueTransaction(cmds);
}

void DbService::exec(const QString &sql)
{
    if (m_worker) emit enqueueExec(sql);
}

// 读连接按调用线程隔离（独立于写线程）
static QSqlDatabase openReadConnection(const QString &dbPath)
{
    QString conn = QString("dbw_read_%1").arg((quintptr)QThread::currentThreadId());
    if (!QSqlDatabase::contains(conn)) {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
        db.setDatabaseName(dbPath);
        db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
        db.open();
    }
    return QSqlDatabase::database(conn);
}

QList<QVariantMap> DbService::select(const QString &table,
                                      const QStringList &cols,
                                      const QVariantMap &where,
                                      const QString &orderBy,
                                      int limit)
{
    QSqlDatabase db = openReadConnection(m_dbPath);
    QList<QVariantMap> result;
    if (!db.isOpen())
        return result;

    QSqlQuery q(db);
    QString colStr = cols.isEmpty() ? "*" : cols.join(",");
    QString sql = QString("SELECT %1 FROM %2").arg(colStr).arg(table);
    if (!where.isEmpty()) {
        QStringList parts;
        for (auto it = where.begin(); it != where.end(); ++it)
            parts.append(it.key() + "=?");
        sql += " WHERE " + parts.join(" AND ");
    }
    if (!orderBy.isEmpty())
        sql += " ORDER BY " + orderBy;
    if (limit > 0)
        sql += QString(" LIMIT %1").arg(limit);

    if (!q.prepare(sql)) {
        emit error("select 准备失败: " + q.lastError().text());
        return result;
    }
    for (auto it = where.begin(); it != where.end(); ++it)
        q.addBindValue(it.value());

    if (q.exec()) {
        while (q.next()) {
            QVariantMap row;
            for (int i = 0; i < q.record().count(); ++i)
                row.insert(q.record().fieldName(i), q.value(i));
            result.append(row);
        }
    }
    return result;
}

void DbService::shutdown()
{
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
        m_thread->deleteLater();
        m_thread = nullptr;
        m_worker = nullptr;
    }
}
