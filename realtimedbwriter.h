#ifndef REALTIMEDBWRITER_H
#define REALTIMEDBWRITER_H

#include <QObject>
#include <QThread>
#include <QVariantMap>
#include <QList>
#include <QStringList>
#include <QSqlDatabase>

class RealtimeDbWriterWorker : public QObject
{
    Q_OBJECT
public:
    explicit RealtimeDbWriterWorker(const QString &dbPath,
                                    const QString &table,
                                    QObject *parent = nullptr)
        : QObject(parent), m_dbPath(dbPath), m_table(table) {}

public slots:
    void init();
    void doRegister(const QString &subsystem, const QStringList &keys);
    void doUpdate(const QString &subsystem, const QVariantMap &data);

signals:
    void writeFinished(qint64 batchCount);
    void writeError(const QString &msg);

private:
    void ensureTable(QSqlDatabase &db);
    QSqlDatabase openConnection();

    QString m_dbPath;
    QString m_table;
};

/**
 * @brief 实时键值数据库工具类（可在线程间安全使用）
 *
 * 适用场景：
 *  - 启动时从 XML 配置把各分系统的 key 注册进数据库；
 *  - 多个线程（如 5 个 UDP 接收线程）解析数据后，按 key 更新对应的 value；
 *  - 通过 snapshot() 取全量最新值，供 WS 推送（<1s）。
 *
 * 表结构（key/value 分列存储）：
 *   CREATE TABLE kv (
 *     id        INTEGER PRIMARY KEY AUTOINCREMENT,
 *     subsystem TEXT,
 *     key       TEXT,
 *     value     TEXT,
 *     ts        TEXT,
 *     UNIQUE(subsystem, key)
 *   );
 *
 * 所有写操作（注册 / 更新）都通过内部单条后台写线程 + 队列完成，
 * 任意线程调用均线程安全，不阻塞调用方；读操作走各线程自己的只读连接。
 */
class RealtimeDbWriter : public QObject
{
    Q_OBJECT
public:
    static RealtimeDbWriter &instance();

    /// 初始化数据库与后台写线程（建议主线程调用一次）。
    bool init(const QString &dbPath, const QString &table = "kv");

    /// 注册某分系统的若干 key（已存在则忽略）。通常在启动时从 XML 调用。
    void registerKeys(const QString &subsystem, const QStringList &keys);

    /// 解析 XML 配置并把其中的 key 注册进数据库。XML 格式见 loadXmlConfig()。
    bool registerFromXml(const QString &xmlPath);

    /// 更新某分系统单个 key 的 value。
    void update(const QString &subsystem, const QString &key, const QVariant &value);

    /// 批量更新某分系统多个 key（map 的键为 key，值为 value）。
    void updateBatch(const QString &subsystem, const QVariantMap &data);

    /// 查询某分系统所有 key 的最新值（含 ts）。
    QVariantMap querySubsystem(const QString &subsystem);
    /// 查询单个 key 的最新值（含 ts），返回 map 含 "value" 与 "ts"。
    QVariantMap queryKey(const QString &subsystem, const QString &key);
    /// 取全量最新值，key 形如 "subsystem/key"，供 WS 推送使用。
    QVariantMap snapshot();

    /// 停止后台线程并等待退出（析构时自动调用）。
    void shutdown();

signals:
    void writeFinished(qint64 totalWritten);
    void writeError(const QString &msg);

    // 内部信号：跨线程把操作交给写线程
    void enqueueRegister(const QString &subsystem, const QStringList &keys);
    void enqueueUpdate(const QString &subsystem, const QVariantMap &data);

private:
    explicit RealtimeDbWriter(QObject *parent = nullptr);
    ~RealtimeDbWriter();

    /**
     * @brief 解析 XML 配置，提取 subsystem 名称与 key 列表。
     * 支持的 XML 格式（每项 item 的 key 属性即为要注册的 key）：
     *   <config subsystem="分系统A">
     *     <item key="voltage"  desc="电压"/>
     *     <item key="current"  desc="电流"/>
     *   </config>
     * 也可写成 <key name="...">，两种属性名都兼容。
     */
    static bool loadXmlConfig(const QString &xmlPath,
                              QString &subsystem,
                              QStringList &keys);

    QString m_dbPath;
    QString m_table;
    QThread *m_thread = nullptr;
    RealtimeDbWriterWorker *m_worker = nullptr;

    Q_DISABLE_COPY(RealtimeDbWriter)
};

#endif // REALTIMEDBWRITER_H
