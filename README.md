# TestThreadDatabase — Qt 通用数据库服务（DbService）

一个用于 **多线程实时数据入库** 的 Qt 工具类 + 演示工程。核心是一个**表结构无关的通用数据库服务** `DbService`：对外只提供统一的 **增 / 删 / 改 / 查（单体）+ 批量 + 事务** 原语，业务语义（如"按 key 更新""注册 key"）由上层用这些原语组合。

典型场景：

- 5 个分系统各有一个 XML 配置文件，启动时把其中的 key 注册进数据库；
- 5 个 UDP 接收线程分别解析各自分系统的实时数据，按 key 更新 value；
- 数据解析 → 入库 → 查询 → WebSocket 推送，全程低于 1 秒。

---

## ✨ 主要功能点（亮点）

1. **通用数据库原语，不耦合表结构**
   对外只有 `insert / update / remove / select` + `insertBatch / updateBatch / removeBatch` + `transaction`，不认识具体表与字段。

2. **线程安全的实时写入**
   任意线程（主线程、UDP 线程、工作线程）都可以直接调用写接口，无需自己加锁，不阻塞调用方。

3. **单写线程 + 队列架构，彻底规避 SQLite 锁冲突**
   所有数据库写操作由唯一一个后台线程串行执行，从根本上消灭 `database is locked`，多线程序并发写也稳。

4. **读隔离 + WAL，写入查询互不阻塞**
   查询走各线程独立的只读连接，配合 WAL 模式，写入的同时仍可并发查询。

5. **事务原子性**
   `transaction(QList<DbCommand>)` 把多条命令作为一个整体（一个 `BEGIN/COMMIT`）原子执行。

6. **极简 API**
   初始化一次后，核心调用仅几个通用方法。

---

## 🏗 架构总览

```
应用层 (MainWindow / UDP 线程 / WS 推送)
        │ 调用通用原语
        ▼
┌──────────────────────────────────────────┐
│  DbService（单例，对外统一 API，线程安全）  │
│   单体: insert / update / remove / select  │
│   批量: insertBatch / updateBatch / removeBatch │
│   事务: transaction(QList<DbCommand>)       │
│   任意SQL: exec（如建表 DDL）               │
└──────┬─────────────────────────┬───────────┘
       │ 写(异步, 队列投递)       │ 读(同步, 每线程连接)
       ▼                         ▼
┌──────────────┐         ┌──────────────────────┐
│ WriteWorker  │         │ ReadConn(每线程独立)  │
│ 单线程执行SQL │         │ WAL 模式, 直接 SELECT │
└──────┬───────┘         └──────────────────────┘
       │ 唯一写连接
       ▼
   SQLite 数据库
```

> 设计要点：**多个生产者只负责把写操作丢进队列，唯一的消费者线程负责落库。** 生产者与消费者之间通过 Qt 信号槽的队列连接解耦，线程安全由框架保证，无需手写互斥锁。

---

## 📦 对外 API（工具类 `DbService`）

单例使用：`DbService::instance()`。数据统一用 `QVariantMap`（列名→值）表示一行；条件用 `QVariantMap`（列名→值，等值 AND）。

| 接口 | 说明 |
|------|------|
| `bool init(dbPath)` | 初始化数据库并启动后台写线程（建议主线程调用一次） |
| `void insert(table, row, ignoreExisting=false)` | 插入一行（`ignoreExisting` 时 `INSERT OR IGNORE`） |
| `void update(table, row, where)` | 按条件更新 |
| `void remove(table, where)` | 按条件删除 |
| `void insertBatch(table, rows, ignoreExisting=false)` | 批量插入（内部单事务） |
| `void updateBatch(table, rows, keyColumns)` | 批量更新（按 `keyColumns` 定位行，`ON CONFLICT ... DO UPDATE`） |
| `void removeBatch(table, wheres)` | 批量删除 |
| `void transaction(cmds)` | 多条命令原子执行（`DbCommand` 列表） |
| `void exec(sql)` | 执行任意 SQL（如建表 DDL），异步在写线程执行 |
| `QList<QVariantMap> select(table, cols, where, orderBy, limit)` | 查询（同步，调用线程的只读连接） |
| `void shutdown()` | 停止后台线程并等待退出（析构时自动调用） |

信号：`executed(const QString &op, int affected)`（操作名 + 影响行数）、`error(const QString &msg)`。

`DbCommand` 结构（事务用）：
```cpp
struct DbCommand {
    enum Type { Insert, Update, Remove };
    Type type = Insert;
    QString table;
    QVariantMap data;   // 写入值
    QVariantMap where;  // 条件
};
```

---

## 🚀 使用方法

### 方式一：作为工具类引入自己的工程

1. 把 `dbservice.h` / `dbservice.cpp` 复制到你的工程；
2. `.pro` 中加入 `QT += sql`；
3. 代码：

```cpp
#include "dbservice.h"

// 程序启动处初始化一次
DbService::instance().init("data.db");
// 连接结果/错误信号（可选）
connect(&DbService::instance(), &DbService::executed, [](const QString &op, int n){ /* op 完成, 影响 n 行 */ });
connect(&DbService::instance(), &DbService::error,    [](const QString &e){ /* 错误处理 */ });

// —— 任意线程里调用，立即返回不阻塞 ——
// 建表（通用 DDL）
DbService::instance().exec("CREATE TABLE IF NOT EXISTS kv ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "subsystem TEXT, key TEXT, value TEXT, ts TEXT, "
    "UNIQUE(subsystem, key))");

// 插入一行
DbService::instance().insert("kv", {{"subsystem","分系统A"},{"key","A0001"},{"value",36.5},{"ts",""}});

// 按 key 更新（upsert）
DbService::instance().updateBatch("kv",
    {{{"subsystem","分系统A"},{"key","A0001"},{"value",36.5},{"ts","..."}}},
    {"subsystem","key"});

// 事务：一条事务内做多个操作
DbCommand u; u.type=DbCommand::Update; u.table="kv";
u.data={{"value",36.5}}; u.where={{"subsystem","分系统A"},{"key","A0001"}};
DbService::instance().transaction({u});

// 查询（同样可在任意线程，同步返回）
QList<QVariantMap> rows = DbService::instance().select("kv", {"key","value"}, {{"subsystem","分系统A"}});

// 程序退出前
DbService::instance().shutdown();
```

### 方式二：运行本演示程序

```bash
qmake
mingw32-make
release\TestThreadDatabase.exe
```

界面按钮：

| 按钮 | 作用 |
|------|------|
| 加载XML配置 | 读取 `config/*.xml`，用 `exec` 建表 + `insertBatch` 将 5 个分系统各 500 个 key 注册进库 |
| 模拟UDP更新 | 用 `select` 读回"分系统A"真实 key，挑几个用 `updateBatch` 更新 |
| 5线程并发 | 启动 5 个线程，各用内存缓存的 key 列表后 `updateBatch` 模拟并发（不依赖异步写库时序） |
| 查询分系统A | 用 `select` 查询并打印"分系统A"有值的 key |
| 全量快照 | 用 `select` 取全量 key/value（用于 WS 推送的数据源） |

> 点击"5线程并发"后，顶部标签会实时累加"累计写/更新操作"次数，结束后日志显示全量 key 数（应为 2500 = 5 × 500）。

---

## 📄 XML 配置格式

每个分系统一个 XML，放在 `config/` 目录。key 以分系统字母前缀开头：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<config subsystem="分系统A">
  <item key="A0001" desc="信号A0001"/>
  <item key="A0002" desc="信号A0002"/>
  ...
  <item key="A0500" desc="信号A0500"/>
</config>
```

解析规则（兼容多种写法）：

- 根标签：`config` 或 `system`；分系统名取 `subsystem` / `name` / `id` 属性；
- key 标签：`item` / `key` / `param`；
- key 属性名：`key` 或 `name`。

> XML 解析属于演示层（在 `mainwindow.cpp` 的 `parseXmlKeys` 中），不放在通用工具类里。

---

## 🔧 多线程写入逻辑（简述）

1. 业务线程（UDP 线程等）调用 `insert/update/...`，内部只 `emit` 对应信号并立即返回；
2. 信号以**队列连接**方式被后台单写线程接收，操作按 FIFO 排队；
3. 后台线程的 `doInsert/doUpdate/...` 在**唯一持久写连接**上逐条（或事务内）执行 SQL，完成后发出 `executed`；
4. 查询走各线程独立的只读连接（WAL 模式），与写入互不阻塞。

好处：**生产者之间、生产者与消费者之间完全解耦，线程安全，无锁冲突，调用方零阻塞。**

---

## 🛠 构建环境

- Qt 5.12.8（MinGW 64-bit）
- 模块：`core` `gui` `widgets` `sql`
- 编译器：MinGW 7.3.0 (`mingw73_64`)

```bash
qmake TestThreadDatabase.pro
mingw32-make
```

---

## 📁 工程文件

| 文件 | 说明 |
|------|------|
| `dbservice.h/.cpp` | **核心工具类**：单例、后台单写线程（持久连接）、通用 CRUD + 批量 + 事务 |
| `mainwindow.h/.cpp` | 演示界面，展示 XML 加载、模拟更新、并发压测、查询（全部用 `DbService` 原语组合） |
| `main.cpp` | 程序入口 |
| `config/*.xml` | 5 个分系统的 key 配置（各 500 个，带前缀 A~/B~/C~/D~/E~） |
| `TestThreadDatabase.pro` | qmake 工程文件 |

---

## 🔮 后续可扩展

- 5 个 UDP 接收线程框架（解析 → `updateBatch`）；
- WebSocket 推送（`select`/`snapshot` 定时 <1s 推送全量最新值）；
- 写入失败重试 / 本地缓存队列持久化。
