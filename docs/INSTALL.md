# 安装 rws 扩展

构建产物在 `dist/`（未纳入版本库，用 `make release` 生成后拷贝过去）。
把下文的 `<repo>` 换成本仓库的绝对路径。

本目录里的 `rws.duckdb_extension` 是**未签名**的本地构建，因此加载时必须带
`-unsigned`（或 `allow_unsigned_extensions=true`）。

| 项 | 值 |
| --- | --- |
| DuckDB 版本 | v1.5.5 |
| 平台 | osx_arm64 |
| 校验 | 见 `rws.duckdb_extension.sha256` |

版本和平台必须与你运行的 DuckDB 完全一致，否则会被拒绝加载。用
`duckdb --version` 确认；不一致时见文末"为其他版本重新构建"。

## 方式一：装进扩展目录（推荐）

装一次，之后按名字加载：

```bash
duckdb -unsigned -c "INSTALL '<repo>/dist/rws.duckdb_extension';"
```

之后每次：

```bash
duckdb -unsigned
```
```sql
LOAD rws;
```

`-unsigned` 每次都要带——这个设置必须在数据库启动前生效，写进 `~/.duckdbrc`
无效。加个别名更省事：

```bash
echo "alias duckdb='duckdb -unsigned'" >> ~/.zshrc
```

安装位置是 `~/.duckdb/extensions/v1.5.5/osx_arm64/`，卸载直接删掉那里的
`rws.duckdb_extension` 和 `.info` 即可。

## 方式二：按路径直接加载

不落地到扩展目录：

```bash
duckdb -unsigned
```
```sql
LOAD '<repo>/dist/rws.duckdb_extension';
```

## 方式三：用随仓库构建的 duckdb

`build/release/duckdb` 已经把扩展静态链进去了，不需要 `-unsigned`，也不需要
`LOAD`：

```bash
<repo>/build/release/duckdb
```

## 验证

```sql
SELECT extension_name, loaded FROM duckdb_extensions() WHERE extension_name = 'rws';
SELECT * FROM rws_version();   -- 需要先建 secret
```

## 从 Python / 其他宿主使用

```python
import duckdb
con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.load_extension("<repo>/dist/rws.duckdb_extension")
```

Python 包自带的 DuckDB 版本要和构建版本一致（`duckdb.__version__`）。

## 为其他 DuckDB 版本重新构建

扩展和 DuckDB 是版本绑定的。换目标版本时：

```bash
cd <repo>
git -C duckdb fetch --tags --depth=1 origin refs/tags/v1.5.6:refs/tags/v1.5.6
git -C duckdb checkout v1.5.6
git -C duckdb submodule update --init --recursive
VCPKG_TOOLCHAIN_PATH=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  VCPKG_ROOT=/path/to/vcpkg GEN=ninja make release
cp build/release/extension/rws/rws.duckdb_extension dist/
```

源码不需要改动——本次从 v1.5.4 切到 v1.5.5 就是零改动直接编过的。

用法见仓库根目录的 `README.md`。
