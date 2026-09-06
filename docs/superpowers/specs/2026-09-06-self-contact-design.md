# 联系人"自己"条目 设计规格

日期：2026-09-06
状态：已批准（用户确认全部设计决策）

## 背景与目标

每个用户的联系人列表（`contact.list`）中恒有一条名为"自己"的联系人——即该用户自身。**该"自己"联系人真实写入数据库**（contacts 表），而非仅返回时合成。

## 已确认决策（来自澄清）

| # | 决策 |
|---|---|
| Q1 | 落库形式：contacts 表插入 `(user_id=自己, contact_id=自己)` **单条**记录 |
| Q2 | 写入时机：**仅注册时插入**（`auth.register` 成功后 `INSERT IGNORE`）；**登录/重连不插入**（避免每次登录写库）；存量用户用一次性迁移脚本补齐 |
| Q3 | `contact.list` 中"自己"恒排第一位；`username` 显示自己的实际昵称，`account` 为自身账号 |

## 数据模型

`contacts` 表新增记录：`(user_id=自己, contact_id=自己)`，单条即可（查询 `user_id=自己 AND contact_id=自己` 命中）。

## 写入逻辑

### 注册时
`auth.register` 成功后调用 `ContactRepository::addSelfContact(userId)`：
```cpp
// INSERT IGNORE INTO contacts (user_id, contact_id) VALUES (?, ?)  // 两个参数均=userId
// 成功则更新 isContact 内存缓存 contacts_（键 "userId|userId"）
```

### 存量用户迁移
提供 `db/migrate_self_contact.sql`：
```sql
-- 为所有尚未有"自己"联系人的用户补齐（幂等，可重复执行）
INSERT IGNORE INTO contacts (user_id, contact_id)
SELECT id, id FROM users;
```

## 读取逻辑（contact.list）

`onContactList` 中：
- `listContactIds(userId)` 自然返回包含自己的 id
- 将"自己"（contact_id == 自己 userId）**移到数组第一位**，其余保持 DB 查询顺序
- 每条仍用 `accountOf`/`nickname` 合成 `{account, username}`（自己条目的 username 即自己昵称）

## 边界与既有逻辑兼容

| 操作 | 现状 | 是否受影响 |
|---|---|---|
| `contact.delete` 自己 | `other == self` → 400 已拦截 | 无 |
| `contact.request` 自己 | `to == self` → 400 已拦截 | 无 |
| `msg.send` 自己 | `to != self` 短路，不触发 isContact 权限 | 无 |
| `contact.requests` | 自己给自己发请求不可能（被拦截） | 无 |
| `isContact(自己,自己)=true` | 无副作用（上述路径均已短路/拦截） | 无 |
| `contact.delete` 普通联系人 | 不受影响 | 无 |

## 改动文件

| 文件 | 改动 |
|---|---|
| `src/contact_repository.hpp` | 新增 `void addSelfContact(int64_t userId)` 声明 |
| `src/contact_repository.cpp` | 实现 `addSelfContact`（INSERT IGNORE + 缓存更新） |
| `src/appserver.cpp` | `onRegister` 成功后调用 `addSelfContact`；`onContactList` 将"自己"排第一 |
| `db/migrate_self_contact.sql` | 存量用户一次性迁移 |
| `scripts/biz_test.py` | 新用例（注册后 list 含自己且排第一、昵称显示、存量用户迁移） |
| `docs/protocol.md` | `contact.list` 说明恒含"自己" |
| `docs/client_api.md` | §5.11 或对应章节补充说明 |

## 测试计划

- biz_test 新用例：
  - 注册后 `contact.list` 含自己且排第一位
  - 自己条目的 `account == 自己账号`、`username == 自己昵称`
  - 迁移脚本：手工插入一条无"自己"的老用户记录，执行迁移 SQL 后 `contact.list` 含自己
  - 登录不重复插入：两次登录后 contacts 表仍只有一条"自己"记录
- 双模式回归（内存 + Redis）
- 全量回归（biz + origin + CTest）

## 文档更新

- `docs/protocol.md`：`contact.list` 说明"恒含一条自己的联系人（account=自己、username=自己昵称），排第一位"
- `docs/client_api.md`：§5.11 `contact.list` 补充说明；§8 流程补充
- 同步 `docs/docs/` 副本
- `db/schema.sql` 无需改动（contacts 表结构不变）