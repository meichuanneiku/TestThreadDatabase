# Task Plan: TestThreadDatabase — Qt 多线程实时键值数据库工具

## Goal
提供一个表无关、线程安全的通用数据库服务 `DbService`：所有写操作经单一后台写线程异步串行执行，读操作走各调用线程的 WAL 只读连接，支撑 5 个子系统通过 UDP 线程实时更新、WebSocket 低频查询。

## Current Phase
Phase 0（稳定 / 待扩展）

## Phases

### Phase 0: 现状（已完成）
- [x] `DbService` 通用化重构（替代 `RealtimeDbWriter`），提供 insert/update/remove/batch/transaction/exec/select
- [x] 持久写连接 `m_db` 成员化，修复「每次调用返回栈对象 → 连接关闭 → 单写线程崩溃」的问题
- [x] 构建通过；5 子系统 `updateBatch` + 查询验证通过
- **Status:** complete

### Phase 1: 未来可能的扩展（待定）
- [ ] 是否需要连接池 / 失败重试 / metrics（视需求）
- [ ] 视业务补充
- **Status:** pending

## Key Questions
1. 规划文件是否提交进仓库？→ 结论：个人开发使用，不提交（靠 `.gitignore` 或手动不 add）。
2. 后续是否有新子系统 / 新表结构需要纳入？

## Decisions Made
| Decision | Rationale |
|----------|-----------|
| 写连接作为 `DbServiceWorker` 成员 `m_db` 持久持有 | 每次调用返回栈对象会在析构时关闭连接，导致单写线程崩溃 (0xC0000005) |
| 绑定在 `prepare()` 之后 `addBindValue()` | prepare 前绑定会报 "Parameter count mismatch" |
| XML 解析留在 demo 层 (`mainwindow.cpp`) 而非工具类 | 工具类保持表无关、通用 |
| `planning-with-files` 仅用文件层 (`task_plan.md`/`progress.md`)，去 hook | openCode 不解析 SKILL.md 的 hooks；只需跨会话续上下文 |
| skill 只装全局、删项目级双保险 | 个人项目，避免 skill 文件进仓库影响协作 |

## Errors Encountered
| Error | Attempt | Resolution |
|-------|---------|------------|
| 写线程崩溃 EXIT -1073741819 (0xC0000005) | - | `openConnection()` 返回栈对象，析构关闭连接；改为 `m_db` 成员持久连接 |
| 编译报错 "missing terminating " 中文乱码 | - | 文件被以错误编码保存导致中文误码；重建为 UTF-8 |

## Notes
- Qt 5.12.8: `qmake` 在 `Qt5.12.8/5.12.8/mingw73_64/bin`，`mingw32-make` 在 `Tools/mingw730_64/bin`
- 全局 skill: `~/.config/opencode/skills/planning-with-files/`（不进仓库）
- 更新节奏：每阶段完成后更新 `progress.md` + 本文件状态；会话压缩 / 新会话先读本文件续上下文
