# DuckDB RWS 扩展设计

状态：已实现并在真实环境（一个 Rave UAT 沙箱，RWS 1.16.0）验证。本文记录已落地的设计与其依据；带“未实现”标注的部分是明确的缺口，不是计划承诺。

## 1. 目标与技术路线

把 Medidata Rave Web Services 暴露成 DuckDB 表，让用户直接用 SQL 查研究、受试者和表单数据，再自行 JOIN、聚合或落地成本地表/Parquet。

两个入口并行提供，共用同一套端点构造、认证、解析和校验代码：

- **表函数**：显式取数，参数可逐查询变化。
- **ATTACH 只读目录**：把一个研究挂成数据库，直接暴露 subjects、sites、forms 等主数据表和每个表单一张宽表。

范围是只读。数据写回、自动同步和全量复刻 rwslib 不在范围内。

原生 C++ 扩展，不内嵌 Python；rwslib 只作为接口语义的参考。依赖限于 DuckDB 自带的 cpp-httplib（本扩展以 OpenSSL 支持重新编译，因为 DuckDB 链接的那份关闭了 TLS）与 vcpkg 的 OpenSSL。XML 解析是自带的命名空间感知拉取式解析器（`src/rws_xml.cpp`），没有引入 libxml2。目标版本固定为 DuckDB v1.5.4：存储扩展基类的虚函数签名跨版本变动明显，升级需要单独适配。

## 2. 目标环境的实测结论

设计初稿假设可以走 Biostats Gateway 的 CSV 宽表和 Clinical View 元数据。实测账户上这两条路都不存在，这直接决定了下面的数据模型，因此先记录事实：

| 端点 | 结果 |
| --- | --- |
| `GET /version` | 200，`1.16.0` |
| `GET /studies` | 200，ODM 研究列表 |
| `GET /studies/{oid}/subjects` | 200，支持 `status=all`、`links=all`、`include=inactive`、`include=inactiveAndDeleted`、`subjectKeyType=SubjectUUID` |
| `GET /studies/{oid}/datasets/{regular\|raw}` | 200，完整 ODM 临床数据；返回 `X-MWS-CV-Last-Updated` |
| `GET /studies/{oid}/subjects/{key}/datasets/{type}` | 200，受试者范围的同结构数据 |
| `GET /datasets/ClinicalViewMetadata.odm` | **IIS 404**，Biostats Gateway 未部署 |
| `GET /datasets/{form}.csv` | **IIS 404**，同上 |
| `GET /metadata/studies` | 200，但为空（账户无 Architect 可见性） |
| `GET /metadata/studies/{project}/versions[/{oid}]` | 404 `RWS00014` Study does not exist |
| `GET /sites`、`GET /studies/{oid}/users` | 404 `RWS00055` RWS URL does not exist |
| `?start=...` | **501 Not Implemented** |
| `?formOid=...` | 被服务端忽略，返回完整数据集 |

三个后果：

1. **没有 CSV 宽表**，宽表必须由 ODM 数据自己透视出来。
2. **没有元数据端点**，`forms`、`items`、`study_events`、`sites` 只能从临床数据和受试者列表反推。
3. **没有服务端按表单过滤**，一个研究就是一次全量下载；因此缓存不是优化，是可用性前提。

`start` 的 501 说明增量在该部署上不可用；扩展会把它翻译成明确的 `NotImplementedException` 而不是静默降级。

## 3. 数据模型

### 3.1 行的粒度

ODM 的 `ItemGroupData` 实例是临床视图暴露的最细粒度，也是本扩展所有表的行基准：

```text
ClinicalData → SubjectData → StudyEventData → FormData → ItemGroupData → ItemData
```

- **长表 `clinical_items`**：每个 `ItemData` 一行，完整保留层级路径与重复键。
- **宽表 `<FORM_OID>`**：每个 `ItemGroupData` 实例一行，列 = 该表单在整份数据里出现过的全部 `ItemOID` 的并集，按首次出现顺序排列。

宽表的透视是确定的：键是完整层级路径，一个 `ItemGroupData` 内 `ItemOID` 不重复。真的重复时不挑赢家，直接报错并指向长表。

### 3.2 类型与空值

所有列都是 `VARCHAR`（计数列除外），不按前几行采样猜类型，避免前导零、不完整日期和后续混合值丢失。空值有三种来源，互不混淆：

| 源 | SQL |
| --- | --- |
| 属性不存在 | `NULL` |
| `Value=""` | 空字符串 |
| `IsNull="Yes"` 且无 `Value` | `value` 为 `NULL`，`is_null` 为 `true` |
| 源里没有 `IsNull` 标记 | `is_null` 为 `NULL`，不推断为 `false` |

`Yes`/`No` 才映射成布尔；其他取值一律保持未知。重复键（`StudyEventRepeatKey="ALL[1]/AE[1]"`）保留原始文本，不转整数。

### 3.3 列名映射

Item OID 带表单前缀（`AE.AETERM`）。宽表默认剥掉 `<form_oid>.` 前缀得到 `AETERM`；剥离后为空、与上下文列冲突或与其他列大小写冲突时，退回完整 OID，再冲突则追加 `__2`、`__3`。映射完整记录在 `rws_form_columns` 与目录的 `form_columns` 表里，不静默覆盖。

上下文列固定在前：`record_id`、`study_oid`、`metadata_version_oid`、`subject_key`、`site_oid`、`study_event_oid`、`study_event_repeat_key`、`form_oid`、`form_repeat_key`、`item_group_oid`、`item_group_repeat_key`。

## 4. 表函数

| 函数 | 位置参数 | 粒度 |
| --- | --- | --- |
| `rws_version` | 无 | 1 行，服务端版本 |
| `rws_studies` | 无 | 每个可访问研究一行 |
| `rws_subjects` | project, environment | 每个受试者一行 |
| `rws_sites` | project, environment | 每个被受试者引用的站点一行 |
| `rws_forms` | project, environment | 每个出现过的表单一行 |
| `rws_study_events` | project, environment | 每个访视一行 |
| `rws_items` | project, environment | 每个 (表单, item group, item) 一行 |
| `rws_clinical_items` | project, environment | 每个 ItemData 一行 |
| `rws_form` | project, environment, form_oid | 每条记录一行，动态宽表 |
| `rws_form_columns` | project, environment, form_oid | 每个输出列一行 |
| `rws_clear_cache` | 无 | 清空进程内响应缓存 |
| `rws_refresh_catalog` | catalog_name | 清空一个挂载对应的缓存 |

共同命名参数 `secret`、`timeout_seconds`、`max_retries`、`refresh`；数据集类函数另有 `dataset_type`、`subject_key`、`start`、`form_oid`；受试者类另有 `include`、`status`、`links`、`subject_key_type`。

project 与 environment 分开传参，内部拼成 `PROJECT(ENV)` 这一 RWS 约定的 study OID，路径按 RFC 3986 逐段编码（`(`、`)` 是合法 pchar，保持可读）。反向拆分只用于派生 `environment` 列，`project_name` 优先取服务端的 ProtocolName。

参数值一律先校验再发请求：RWS 会**静默忽略**未知的 `include` 值，所以扩展只接受实测支持的取值；请求了 `subject_key_type` 而服务端返回了别的标识体系时报错，不悄悄换标识。

## 5. ATTACH：只读目录

### 5.1 定位与边界

目录表没有参数。表函数里逐查询变化的参数，在 ATTACH 下要么上升为挂载级选项，要么不支持。两个入口长期并存。

目录不改变一致性模型：远端没有事务快照，目录只是命名层。

### 5.2 语法

```sql
ATTACH 'MYSTUDY/Prod' AS study (TYPE rws, SECRET rave);          -- 研究模式
ATTACH '' AS study2 (TYPE rws, SECRET rave, PROJECT 'My Study', ENVIRONMENT 'Prod');
ATTACH '' AS rave_all (TYPE rws, SECRET rave, ENVIRONMENTS 'Prod'); -- 服务器模式
```

路径只识别 `PROJECT/ENVIRONMENT`；含 `/` 的项目名必须用 `PROJECT`/`ENVIRONMENT` 选项，两者同时给会报错。空路径即服务器模式。

选项：`SECRET`、`PROJECT`、`ENVIRONMENT`、`DATASET_TYPE`、`DEFAULT_SCHEMA`、`SUBJECT_INCLUDE`、`SUBJECT_STATUS`、`SUBJECT_LINKS`、`SUBJECT_KEY_TYPE`、`STUDIES`、`ENVIRONMENTS`。

### 5.3 布局

```text
study
├── main                 studies / subjects / sites / forms / items / study_events /
│                        clinical_items / form_columns + 每个 FormDef 一张宽表
└── _rws                 settings / tables
```

服务器模式下每个 study/environment 一个 schema，名字是 `<project>__<environment>`，外加 `_rws`。schema 名会进用户 SQL，所以生成名冲突时报错并要求用 `STUDIES` 缩小范围，不自动加序号；原始 OID 始终保留在 `_rws.tables` 里。

表单 OID 与主数据保留名冲突时，宽表加 `__form` 后缀，映射写进 `_rws.tables`。

### 5.4 请求预算

一次临床数据请求就填满整个 schema——所有宽表的列、以及 forms/items/study_events 全部由同一份响应派生。这不是取巧：该部署没有按表单过滤的能力，逐表探测反而会把一次下载变成 N 次。因此：

1. `ATTACH` 本身不发请求。
2. 服务器模式首次枚举 schema 取一次 `/studies`。
3. 首次访问某 schema 取一次该研究的临床数据集，之后全部命中缓存。
4. `subjects`/`sites` 另有一次受试者列表请求。

代价是首次访问要等整份数据集下载并解析完，且整份数据集常驻内存直到缓存过期。响应缓存按 (连接身份, 端点) 分区，凭据不同的挂载互不共享。

`_rws.tables` 在研究模式下会解析那一个数据 schema（成本就是下一条查询本来要付的那次请求）；服务器模式下只列已解析的 schema，不跨研究扇出。

### 5.5 事务、缓存与刷新

- TransactionManager 是占位实现：没有远端事务，也没有快照隔离。需要一致性时显式 CTAS 落地。
- `rws_cache_seconds`（默认 300，`0` 关闭）控制响应复用时长。关闭缓存意味着每张表各自重新下载整个研究。
- `rws_refresh_catalog('study')` 只清缓存，**不重建目录项**：已绑定的执行计划持有目录项指针，重建会造成悬垂。源端新增或删除表单需要 `DETACH` + `ATTACH`，这一步对用户可见，好过让已绑定查询悄悄换形状。
- schema 的表集合在锁外构建、构建完再加锁提交，因为 `_rws` 的内容需要回调目录本身；构建中的 schema 对这些回调表现为空。

### 5.6 写操作

`CREATE TABLE`/`CREATE TABLE AS`/`INSERT`/`CREATE VIEW`/`DROP`/`ALTER`/`CREATE SCHEMA`/`CREATE INDEX` 全部抛出明确错误。`UPDATE`/`DELETE` 由 DuckDB 绑定器更早拒绝（“Can only update base table”）。`InMemory()` 为 false、`GetDatabaseSize()` 不编造数字。

## 6. HTTP、解析与安全

组件分层：

```text
src/rws_extension.cpp          注册 Secret、表函数、存储扩展、设置
src/rws_secret.cpp             TYPE rws 的 Secret 定义、校验与凭据读取
src/rws_url.cpp                路径/查询编码与 study OID 拼装
src/rws_client.cpp             HTTPS、Basic 认证、重试、错误分类、取消
src/rws_xml.cpp                命名空间感知的 XML 拉取式解析器
src/rws_odm.cpp                ODM 文档到内存模型
src/rws_request.cpp            请求规格、响应缓存、各表的行构建
src/functions/                 表函数的 bind / init / scan
src/storage/                   Catalog / SchemaEntry / TableEntry / TransactionManager / ATTACH
test/sql/                      无网络的 SQL 行为测试
test/http/                     本地模拟服务与协议错误测试
```

安全与健壮性上的具体决定：

- 默认验证 TLS（macOS 从钥匙串取信任根）；`BASE_URL` 不允许内嵌凭据。
- **不跟随重定向**：跨源跳转会把 Basic 凭据送到别处，直接按错误处理。
- 只对可重试状态（408/429/5xx）与传输失败做有上限退避，遵守 `Retry-After`（上限 60 秒）；401/403 和解析错误不重试。
- 错误消息只带端点、HTTP 状态、RWS `ReasonCode` 与服务端消息，**从不带响应正文或凭据**——正文可能含受试者数据。
- XML 解析拒绝 `<!DOCTYPE>`（外部实体攻击的入口）和未知实体引用；按命名空间 URI 而非前缀匹配；文档未闭合即报错，不返回部分行。
- 遵守 `enable_external_access`；关闭时直接拒绝。
- 密码只存在于 Secret 中，`redact_keys` 覆盖，`_rws.settings` 不输出。

## 7. 下推与增量

参数只映射到服务端实际支持的选项。WHERE、JOIN、聚合、LIMIT 全部由 DuckDB 执行；不承诺 `WHERE subject_key = ...` 或 `LIMIT 10` 能减少网络传输——服务端没有对应能力。支持投影裁剪，但它只减少输出开销，不减少传输。

`subject_key` 参数走受试者范围端点，是目前唯一真正能缩小下载量的过滤（实测 389 行 vs 7445 行）。

增量在该部署上返回 501。真要做增量，还需要先验证边界包含性、时区、父级删除和标识稳定性；`X-MWS-CV-Last-Updated` 是 CV 更新时间线索，不是 CDC 位点。

## 8. 验证情况

真实环境（一个 UAT 研究，22 名受试者、21 个表单、1027 条记录、7445 个 ItemData）：所有表函数与两种 ATTACH 模式均已跑通，长表行数与源文档中 `<ItemData` 出现次数逐一相符。

`test/http/run_mock_tests.sh` 用本地模拟服务覆盖 24 项协议行为，无需凭据或网络：实体与 CDATA 解码、BOM、非默认命名空间前缀、空字符串与 NULL 的区分、跨记录列并集、200 状态下的业务错误、DOCTYPE 拒绝、截断文档、跨源重定向不跟随、5xx 重试、401 消息不含密码、目录与表函数结果一致、写操作拒绝、服务器模式 schema 生成。

`make test` 跑无网络的 SQL 测试：Secret 校验、密码脱敏、各参数的取值校验、ATTACH 选项校验。

## 9. 已知缺口

- **增量**：`start` 在目标部署上是 501，未做过任何真实验证。
- **元数据表**：没有 CRF 版本、item 定义、代码表、站点主数据等结构信息，因为该部署不提供元数据端点。现有的 `items`、`forms`、`sites` 是从数据反推的，只覆盖“出现过的”对象——从未录入数据的表单和站点不会出现。
- **MAuth**：只实现了 Basic 认证。
- **执行元数据**：没有 `rws_scan_history()`；`X-MWS-CV-Last-Updated` 已解析但尚未暴露成列。
- **内存**：整份数据集常驻内存，且行以 `Value` 物化。当前研究只有 7445 个值，量级更大的研究需要改成流式或落临时文件。
- **并发**：单线程扫描；两个线程同时首次加载同一 schema 会各自构建一次（HTTP 由缓存吸收）。
- **目录刷新**：只清缓存，不重建目录项；表单增删需要重新 ATTACH。
