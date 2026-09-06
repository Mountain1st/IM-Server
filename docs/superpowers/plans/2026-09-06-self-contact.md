# 联系人『自己』条目 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 每个用户的联系人列表（`contact.list`）恒含一条"自己"联系人（落库 contacts 表 `(自己,自己)` 单条），注册时插入、存量用户一次性迁移，`contact.list` 中排第一位。

**架构：** ContactRepository 新增 `addSelfContact`（INSERT IGNORE + 缓存更新）；`onRegister` 成功后调用；`onContactList` 将 contact_id==自己 的条目移到第一位；提供 `db/migrate_self_contact.sql` 一次性迁移存量用户。

**技术栈：** C++17、MySQL（libmysqlclient）、CMake、Python 测试（biz_test.py）。

**规格：** `docs/superpowers/specs/2026-09-06-self-contact-design.md`

**测试基线：** `bash scripts/test_biz.sh`（内存模式 102/102）、`IM_REDIS_ENABLED=1 IM_REDIS_PORT=6380 bash scripts/test_biz.sh`（Redis 模式 102/102）、`bash scripts/test_origin.sh`、`cd build && ctest`

---

## 文件结构

| 文件 | 职责 |
|---|---|
| `src/contact_repository.hpp` | 新增 `addSelfContact` 声明（修改） |
| `src/contact_repository.cpp` | 实现 `addSelfContact`（修改） |
| `src/appserver.cpp` | `onRegister` 调用 addSelfContact；`onContactList` 自己排第一（修改） |
| `db/migrate_self_contact.sql` | 存量用户一次性迁移（新增） |
| `scripts/biz_test.py` | 新用例（注册后含自己排第一、登录不重复插入、迁移）（修改） |
| `docs/protocol.md` | `contact.list` 恒含"自己"说明（修改） |
| `docs/client_api.md` | `contact.list` 补充说明（修改） |

---

## 任务 1：ContactRepository 新增 addSelfContact

**文件：**
- 修改：`src/contact_repository.hpp`
- 修改：`src/contact_repository.cpp`

- [ ] **步骤 1：contact_repository.hpp 加声明**

在 `bool addContactPair(...)` 声明后追加：
```cpp
  bool addContactPair(int64_t a, int64_t b);                 // 双向插入（幂等：已存在不报错）
  void addSelfContact(int64_t userId);                       // 自己联系人（INSERT IGNORE (uid,uid)）
```

- [ ] **步骤 2：contact_repository.cpp 实现**

在 `addContactPair` 实现后追加：
```cpp
void ContactRepository::addSelfContact(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "INSERT IGNORE INTO contacts (user_id, contact_id) VALUES (?, ?)",
      {std::to_string(userId), std::to_string(userId)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 添加自己联系人失败 errno=" << errNo << std::endl;
    return;
  }
  mysql_stmt_close(stmt);
  contacts_.emplace(std::to_string(userId) + "|" + std::to_string(userId));
}
```

- [ ] **步骤 3：编译验证**

运行：`cmake --build build -j4 2>&1 | grep -iE 'error|warning' | head`
预期：无输出（当前 AppServer 未调用新方法，仅接口扩展，编译通过）。

- [ ] **步骤 4：Commit**

```bash
git add src/contact_repository.hpp src/contact_repository.cpp
git commit -m "feat: ContactRepository::addSelfContact（INSERT IGNORE 自己联系人 + 缓存更新）"
```

---

## 任务 2：AppServer 集成

**文件：** 修改 `src/appserver.cpp`

- [ ] **步骤 1：onRegister 成功后调用 addSelfContact**

在 `onRegister` 的注册成功响应之后（`session->reply(makeResult("auth.register_res", 0, ...))` 之后、函数结尾 `}` 之前）追加：
```cpp
  // 新用户注册即拥有"自己"联系人（联系人列表恒含自己）
  contactRepo_->addSelfContact(userId);
```

- [ ] **步骤 2：onContactList 将"自己"排第一位**

修改 `onContactList`（当前为 appserver.cpp:817-827）：
```cpp
void AppServer::onContactList(const std::shared_ptr<Session>& session, const json& data) {
  const auto ids = contactRepo_->listContactIds(session->userId());
  json arr = json::array();
  const int64_t self = session->userId();
  // "自己"联系人恒排第一位（contact_id == 自己 userId）
  for (const int64_t id : ids) {
    if (id == self) continue;
    std::string account, username;
    userStore_->accountOf(id, &account);
    userStore_->nickname(id, &username);
    arr.push_back(json{{"account", account}, {"username", username}});
  }
  std::string selfAccount, selfNick;
  userStore_->accountOf(self, &selfAccount);
  userStore_->nickname(self, &selfNick);
  json selfEntry = json{{"account", selfAccount}, {"username", selfNick}};
  json out = json::array();
  out.push_back(selfEntry);
  for (const auto& e : arr) out.push_back(e);
  session->reply(makeResult("contact.list_res", 0, "ok", json{{"contacts", out}}));
}
```

- [ ] **步骤 3：编译验证**

运行：`cmake --build build -j4 2>&1 | grep -iE 'error|warning' | head`
预期：无输出。

- [ ] **步骤 4：Commit**

```bash
git add src/appserver.cpp
git commit -m "feat: 注册即建自己联系人；contact.list 自己恒排第一位"
```

---

## 任务 3：存量用户迁移脚本

**文件：** 创建 `db/migrate_self_contact.sql`

- [ ] **步骤 1：创建迁移脚本**

```sql
-- 存量用户补齐"自己"联系人（幂等，可重复执行）
-- 用法: MYSQL_PWD='<密码>' mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_self_contact.sql
-- 说明: 新用户注册时由服务器自动插入；本脚本仅为存量用户一次性补齐。
INSERT IGNORE INTO contacts (user_id, contact_id)
SELECT id, id FROM users;
```

- [ ] **步骤 2：在现有库执行迁移**

运行：
```bash
MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_self_contact.sql 2>&1
```
预期：无输出（幂等）。重复执行也无输出。

- [ ] **步骤 3：验证存量用户已补齐**

运行：
```bash
MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -e "USE IMS_reactor_db; SELECT COUNT(*) AS total FROM users; SELECT COUNT(*) AS selfed FROM contacts c JOIN users u ON c.user_id=u.id AND c.contact_id=u.id;" 2>/dev/null
```
预期：`selfed == total`（所有存量用户都有"自己"联系人）。

- [ ] **步骤 4：Commit**

```bash
git add db/migrate_self_contact.sql
git commit -m "feat: 存量用户『自己』联系人一次性迁移脚本"
```

---

## 任务 4：biz_test.py 新用例

**文件：** 修改 `scripts/biz_test.py`

- [ ] **步骤 1：新增"自己联系人"用例**

在联系人测试段（`d.close(); e.close(); f.close()` 之前）追加。当前该位置已有互相请求用例。在互相请求用例之后追加：
```python
    # ---- v1.8.1 自己联系人 ----
    # 注册用户 contact.list 恒含"自己"且排第一位
    selfc = WS(); register(selfc, base + "50", "pw1234", nick="小己")
    login(selfc, base + "50", "pw1234")
    selfc.send(env("contact.list", {}))
    r = json.loads(selfc.recv())
    contacts = r["data"]["data"]["contacts"]
    check("contact list contains self", r["data"]["code"] == 0
          and any(x["account"] == base + "50" for x in contacts))
    check("contact self first", contacts and contacts[0]["account"] == base + "50"
          and contacts[0]["username"] == "小己")
    # 登录不重复插入：再登一次（换连接），contact.list 仍只有一条自己
    selfc2 = WS(); login(selfc2, base + "50", "pw1234")
    selfc2.send(env("contact.list", {}))
    r = json.loads(selfc2.recv())
    cnt = sum(1 for x in r["data"]["data"]["contacts"] if x["account"] == base + "50")
    check("self contact not duplicated on login", r["data"]["code"] == 0 and cnt == 1)
    selfc.close(); selfc2.close()
```
注意：注册时用了 `nick="小己"`，用于验证自己条目的 username 显示昵称。

- [ ] **步骤 2：清理列表补账号**

在清理段 `used` 列表追加 `base + "50"`（确保清理时删除该测试账号及关联联系人记录）。

- [ ] **步骤 3：运行内存模式测试**

运行：`bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'`
预期：全部 PASS，用例数 102 → 106（`106 tests, 0 failures`）。

- [ ] **步骤 4：Commit**

```bash
git add scripts/biz_test.py
git commit -m "test: 自己联系人用例（注册后含自己排第一、登录不重复插入）"
```

---

## 任务 5：文档更新

**文件：**
- 修改：`docs/protocol.md`
- 修改：`docs/client_api.md`
- 同步：`docs/docs/` 副本

- [ ] **步骤 1：protocol.md 更新 contact.list 说明**

在 §4 指令表 `contact.list` 行追加"恒含一条自己的联系人（account=自己、username=自己昵称，排第一位）"。

在版本变更列表顶部追加 v1.8.1 条目：
```markdown
- v1.8.1 **联系人列表恒含"自己"**：`contact.list` 返回中恒有一条 `account=自己账号、username=自己昵称` 的联系人，排第一位；新用户注册时自动建立（写入 contacts 表），存量用户可用 `db/migrate_self_contact.sql` 一次性补齐；登录不重复写入
```

- [ ] **步骤 2：client_api.md 更新**

§5.11 `contact.list` 部分补充说明"返回恒含自己的条目（排第一位）"。

- [ ] **步骤 3：同步 docs/docs/**

运行：`cp docs/protocol.md docs/client_api.md docs/docs/`

- [ ] **步骤 4：Commit**

```bash
git add docs/protocol.md docs/client_api.md
git commit -m "docs: v1.8.1 联系人列表恒含『自己』（注册自动建、存量迁移、登录不重复写）"
```

---

## 任务 6：全量回归与提交

**文件：** 无新改动（仅验证）

- [ ] **步骤 1：内存模式全量**

运行：
```bash
bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'
bash scripts/test_origin.sh 2>&1 | tail -1
(cd build && ctest 2>&1 | grep 'tests passed')
```
预期：biz 全 PASS（106）、origin OK、CTest 4/4。

- [ ] **步骤 2：Redis 模式全量**

运行：
```bash
IM_REDIS_ENABLED=1 IM_REDIS_PORT=6380 bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'
```
预期：全 PASS（106）。

- [ ] **步骤 3：确认无残留进程**

运行：`pkill -x imserver; sleep 0.5; pgrep -x imserver || echo "no server"`

- [ ] **步骤 4：确认 git 状态**

运行：`git status --short | grep -vE '^\?\?'`
预期：src/CMakeLists/docs 无未提交改动（各任务已分别 commit）。

---

## 自检

- **规格覆盖度**：落库 `(自己,自己)` 单条（任务 1）；注册插入（任务 2）；登录不插入（任务 2——仅 onRegister 调用）；存量迁移（任务 3）；list 自己排第一 + username 显示昵称（任务 2）；文档（任务 5）；测试（任务 4）。全覆盖。
- **占位符扫描**：无 TODO/待定。
- **类型一致性**：`addSelfContact(int64_t)` 声明与实现签名一致；`listContactIds` 返回 `std::vector<int64_t>`，任务 2 用 `const int64_t id : ids` 迭代一致；`onContactList` 重建 `out` 数组保持结构 `{contacts: [...]}` 与协议一致。