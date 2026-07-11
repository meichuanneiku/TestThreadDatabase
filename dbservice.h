#ifndef DBSERVICE_H
#define DBSERVICE_H

#include <QObject>
#include <QThread>
#include <QVariantMap>
#include <QList>
#include <QStringList>
#include <QSqlDatabase>
#include <QSqlQuery>

// 事务命令：一条原子操作的描述（与具体表结构无关）
struct DbCommand {
    enum Type { Insert, Update, Remove };
    Type type = DbCommand::Insert;
    QString table;   // 目标表名
    QVariantMap data; // 写入值（insert/update）
    QVariantMap where; // 条件（update/remove），等值 AND
};
Q_DECLARE_METATYPE(DbCommand)

class DbServiceWorker; // 后台单写线程中的具体执行者

// 后台单写线程中的具体执行者（类声明在此，实现见 dbservice.cpp）。
// 持有唯一的 SQLite 写连接，串行执行所有写操作。
class DbServiceWorker : public QObject
{
    Q_OBJECT
public:
    explicit DbServiceWorker(const QString &dbPath, QObject *parent = nullptr)
        : QObject(parent), m_dbPath(dbPath) {}

public slots:
    void init(); // 在子线程中打开连接
    void doInsert(const QString &table, const QVariantMap &row, bool ignoreExisting);
    void doUpdate(const QString &table, const QVariantMap &row, const QVariantMap &where);
    void doRemove(const QString &table, const QVariantMap &where);
    void doInsertBatch(const QString &table, const QList<QVariantMap> &rows, bool ignoreExisting);
    void doUpdateBatch(const QString &table, const QList<QVariantMap> &rows, const QStringList &keyColumns);
    void doRemoveBatch(const QString &table, const QList<QVariantMap> &wheres);
    void doTransaction(const QList<DbCommand> &cmds);
    void doExec(const QString &sql);

signals:
    void executed(const QString &op, int affected);
    void error(const QString &msg);

private:
    QSqlDatabase m_db; // 持久写连接：在 init() 中打开，工作线程内一直复用（不随每次调用关闭）
    QString buildWhere(const QVariantMap &where, QVariantList &outValues);
    int execInsert(QSqlDatabase &db, const QString &table, const QVariantMap &row, bool ignoreExisting);
    int execUpdate(QSqlDatabase &db, const QString &table, const QVariantMap &row, const QVariantMap &where);
    int execRemove(QSqlDatabase &db, const QString &table, const QVariantMap &where);
    int execUpdateBatch(QSqlDatabase &db, const QString &table, const QList<QVariantMap> &rows, const QStringList &keyColumns);

    QString m_dbPath;
};

/**
 * @brief 通用数据库服务（多线程序安全）
 *
 * 设计要点：
 *  - 所有“写”操作（insert/update/remove/batch/transaction/exec）都通过内部
 *    唯一的后台写线程串行执行，任意线程调用均线程安全，不阻塞调用方，
 *    且从根本上避免多线程序直写 SQLite 的锁冲突；
 *  - 所有“读”操作（select）走调用线程自己的只读连接（WAL 模式），与写互不阻塞；
 *  - 对外只提供与表结构无关的通用原语：单体 CRUD + 批量 + 事务。
 *
 * 业务语义（如“按 key 更新”“注册 key”）由上层用这些原语组合，不在本类内耦合。
 */
class DbService : public QObject
{
    Q_OBJECT
public:
    static DbService &instance();

    /// 初始化数据库与后台写线程（建议主线程调用一次）。
    bool init(const QString &dbPath);

    // —— 单体 CRUD（异步，投递写线程，立即返回）——
    void insert(const QString &table, const QVariantMap &row, bool ignoreExisting = false);
    void update(const QString &table, const QVariantMap &row, const QVariantMap &where);
    void remove(const QString &table, const QVariantMap &where);

    // —— 批量（内部单事务）——
    void insertBatch(const QString &table, const QList<QVariantMap> &rows, bool ignoreExisting = false);
    void updateBatch(const QString &table, const QList<QVariantMap> &rows, const QStringList &keyColumns);
    void removeBatch(const QString &table, const QList<QVariantMap> &wheres);

    // —— 事务：多条命令作为一个原子操作（一个 BEGIN/COMMIT）——
    void transaction(const QList<DbCommand> &cmds);

    // —— 任意 SQL（如建表 DDL），异步在写线程执行 ——
    void exec(const QString &sql);

    // —— 查询（同步，调用线程的只读连接；WAL 下不阻塞写）——
    QList<QVariantMap> select(const QString &table,
                               const QStringList &cols = QStringList() << "*",
                               const QVariantMap &where = {},
                               const QString &orderBy = {},
                               int limit = -1);

    /// 停止后台线程并等待退出（析构时自动调用）。
    void shutdown();

signals:
    void executed(const QString &op, int affected); // 某操作完成：操作名 + 影响行数
    void error(const QString &msg);

    // 内部信号：跨线程把操作交给写线程
    void enqueueInsert(const QString &table, const QVariantMap &row, bool ignoreExisting);
    void enqueueUpdate(const QString &table, const QVariantMap &row, const QVariantMap &where);
    void enqueueRemove(const QString &table, const QVariantMap &where);
    void enqueueInsertBatch(const QString &table, const QList<QVariantMap> &rows, bool ignoreExisting);
    void enqueueUpdateBatch(const QString &table, const QList<QVariantMap> &rows, const QStringList &keyColumns);
    void enqueueRemoveBatch(const QString &table, const QList<QVariantMap> &wheres);
    void enqueueTransaction(const QList<DbCommand> &cmds);
    void enqueueExec(const QString &sql);

private:
    explicit DbService(QObject *parent = nullptr);
    ~DbService();

    QString m_dbPath;
    QThread *m_thread = nullptr;
    DbServiceWorker *m_worker = nullptr;

    Q_DISABLE_COPY(DbService)
};

#endif // DBSERVICE_H
