# TestThreadDatabase — Qt 实时键值数据库写入工具

一个用于 **多线程实时数据入库** 的 Qt 工具类 + 演示工程。适用于"多个线程/多个分系统并发产生数据，需要实时、安全地写入同一 SQLite 数据库"的场景。

典型场景：

- 5 个分系统各有一个 XML 配置文件，启动时把其中的 key 注册进数据库；
- 5 个 UDP 接收线程分别解析各自分系统的实时数据，按 key 更新 value；
- 数据解析 → 入库 → 查询 → WebSocket 推送，全程低于 1 秒。

---

## ✨ 主要功能点（亮点）

1. **线程安全的实时写入**
   任意线程（主线程、UDP 线程、工作线程）都可以直接调用写接口，无需自己加锁，不会阻塞调用方。

2. **单写线程 + 队列架构，彻底规避 SQLite 锁冲突**
   所有数据库写操作由唯一一个后台线程串行执行，从根本上消灭 `database is locked`，多线程序并发写也稳。

3. **key/value 分列存储 + 按 key 更新（upsert）**
   表结构为 `(subsystem, key, value, ts)`，支持"key 预先注册、后续按 key 更新 value"，而非简单追加。

4. **XML 配置驱动，key 带分系统前缀**
   启动即从 XML 把各分系统的 key 注册进库；不同分系统的 key 以字母前缀区分（如 `A0001~A0500`、`B0001~B0500`），互不串。

5. **读隔离 + WAL，写入查询互不阻塞**
   查询走各线程独立的只读连接，配合 WAL 模式，写入的同时仍可并发查询，满足实时推送需求。

6. **极简 API**
   初始化一次后，核心调用仅 4 个：`init` / `registerFromXml` / `updateBatch` / `querySubsystem`（另含 `snapshot`、`shutdown`）。

---

## 🏗 架构总览

```
业务线程(生产者)  ──updateBatch()──▶  内存事件队列  ──doUpdate()──▶  后台单写线程  ──▶  SQLite(kv 表)
   UDP 线程 ×5           (emit 信号)      (FIFO 排队)      (单连接事务)     (唯一写者)        WAL 模式
                                                                              ▲
查询线程 ──────────────────────────────────────────────────────────── 独立只读连接 ┘
```

> 设计要点：**多个生产者只负责把数据丢进队列，唯一的消费者线程负责落库。** 生产者与消费者之间通过 Qt 信号槽的队列连接解耦，线程安全由框架保证，无需手写互斥锁。

---

## 📦 表结构

```sql
CREATE TABLE kv (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    subsystem TEXT,      -- 分系统名，如 "分系统A"
    key       TEXT,      -- 键名，如 "A0001"
    value     TEXT,      -- 值（统一以文本存储）
    ts        TEXT,      -- 最近一次写入时间
    UNIQUE(subsystem, key)
);
```

- 一个分系统内 key 唯一；
- `INSERT ... ON CONFLICT(subsystem, key) DO UPDATE`：key 不存在则插入，存在则更新 value 与时间戳。

---

## 🧩 对外 API（工具类 `RealtimeDbWriter`）

单例使用：`RealtimeDbWriter::instance()`

| 接口 | 说明 |
|------|------|
| `bool init(dbPath, table="kv")` | 初始化数据库并启动后台写线程（建议主线程调用一次） |
| `bool registerFromXml(xmlPath)` | 解析 XML 配置并把其中的 key 注册进库（已存在则忽略） |
| `void registerKeys(subsystem, keys)` | 直接注册一批 key |
| `void update(subsystem, key, value)` | 更新单个 key 的 value |
| `void updateBatch(subsystem, QVariantMap)` | 批量按 key 更新多个 value（**任意线程调用都安全**） |
| `QVariantMap querySubsystem(subsystem)` | 查询某分系统所有 key 的最新值 |
| `QVariantMap queryKey(subsystem, key)` | 查询单个 key 的最新值（含 ts） |
| `QVariantMap snapshot()` | 取全量最新值，key 形如 `分系统A/A0001`，供 WS 推送 |
| `void shutdown()` | 停止后台线程并等待退出（析构时自动调用） |

信号：`writeFinished(qint64)`（累计写/更新次数）、`writeError(QString)`。

---

## 🚀 使用方法

### 方式一：作为工具类引入自己的工程

1. 把 `realtimedbwriter.h` / `realtimedbwriter.cpp` 复制到你的工程；
2. `.pro` 中加入 `QT += sql`；
3. 代码：

```cpp
#include "realtimedbwriter.h"

// 程序启动处初始化一次
RealtimeDbWriter::instance().init("data.db", "kv");

// 连接结果/错误信号（可选）
connect(&RealtimeDbWriter::instance(), &RealtimeDbWriter::writeFinished,
        [](qint64 total){ /* 已写入 total 次 */ });
connect(&RealtimeDbWriter::instance(), &RealtimeDbWriter::writeError,
        [](const QString& e){ /* 错误处理 */ });

// 启动时从 XML 注册 key
RealtimeDbWriter::instance().registerFromXml("config/分系统A.xml");

// —— 任意线程里调用，立即返回不阻塞 ——
QVariantMap data;
data["A0001"] = 36.5;
data["A0002"] = 101.2;
RealtimeDbWriter::instance().updateBatch("分系统A", data);

// 查询（同样可在任意线程）
QVariantMap rows = RealtimeDbWriter::instance().querySubsystem("分系统A");

// 程序退出前
RealtimeDbWriter::instance().shutdown();
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
| 加载XML配置 | 读取 `config/*.xml`，将 5 个分系统各 500 个 key 注册进库 |
| 模拟UDP更新 | 从库读回"分系统A"真实 key，挑几个更新其 value |
| 5线程并发 | 启动 5 个线程，各从本分系统真实 key 中随机更新，模拟 5 个分系统并发 |
| 查询分系统A | 查询并打印"分系统A"所有 key 的最新值 |
| 全量快照 | 取全量 key/value（用于 WS 推送的数据源） |

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

---

## 🔧 多线程更新逻辑（简述）

1. 业务线程（UDP 线程等）调用 `updateBatch()`，内部只 `emit enqueueUpdate` 信号并立即返回；
2. 信号以**队列连接**方式被后台单写线程接收，数据按 FIFO 排队；
3. 后台线程的 `doUpdate()` 逐条以事务 upsert 进数据库，提交后发出 `writeFinished`；
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
| `realtimedbwriter.h/.cpp` | **核心工具类**：单例、后台单写线程、XML 解析、注册/更新/查询 |
| `mainwindow.h/.cpp` | 演示界面，展示 XML 加载、模拟更新、并发压测、查询 |
| `main.cpp` | 程序入口 |
| `config/*.xml` | 5 个分系统的 key 配置（各 500 个，带前缀） |
| `TestThreadDatabase.pro` | qmake 工程文件 |

---

## 🔮 后续可扩展

- 5 个 UDP 接收线程框架（解析 → `updateBatch`）；
- WebSocket 推送（`snapshot()` 定时 <1s 推送全量最新值）；
- 写入失败重试 / 本地缓存队列持久化。
