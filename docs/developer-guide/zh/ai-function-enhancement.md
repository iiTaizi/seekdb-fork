# 赛题一：AI 函数增强详细操作指南

适用对象：在 seekdb SQL 引擎内完成六个子题的开发者。目标为全部 60 分，不调用真实模型，不依赖应用层包装，不针对模型名、输入或响应内容编写特例。

本文依据 2026-11-01 的本地源码检查编写。文中的“现状”是已检查的事实；“实施步骤”“验收条件”是后续工作，不能作为已经实现或测试通过的证明。

## 目录

- [一、工作范围、分支和当前进度](#一工作范围分支和当前进度)
- [二、先跑通 WSL 与构建路线](#二先跑通-wsl-与构建路线)
- [三、公共调用链和实现约定](#三公共调用链和实现约定)
- [四、子题一：AI_PROMPT 类型与模板增强](#四子题一ai_prompt-类型与模板增强)
- [五、子题二：AI_EMBED 单行语义](#五子题二ai_embed-单行语义)
- [六、子题三：AI_COMPLETE 请求与响应](#六子题三ai_complete-请求与响应)
- [七、子题四：Ollama Provider](#七子题四ollama-provider)
- [八、子题五：AI_RERANK 结果正确性](#八子题五ai_rerank-结果正确性)
- [九、子题六：AI_EMBED 跨行批量](#九子题六ai_embed-跨行批量)
- [十、测试材料、执行流程与最终交付](#十测试材料执行流程与最终交付)

## 一、工作范围、分支和当前进度

### 1.1 唯一开发仓库

| 项目 | 约定 |
| --- | --- |
| Windows 仓库 | `D:\AAAlab\oceanbase\seekdb\seekdb-fork` |
| WSL 挂载路径 | `/mnt/d/AAAlab/oceanbase/seekdb/seekdb-fork` |
| 基线 | 本地 `master`，`9065fb8c2aa9e5d5831f03672b8a41eb10c83685` |
| 工作分支 | `feat/ai-function-enhancement` |
| 比赛材料参考 | 官方 `oceanbase/seekdb` 的 `2026-competition`；已检查的材料提交为 `5bce9e06d1281347ad61344e4b01fab9158c58a2` |
| 构建入口 | 本仓库 `python3 build.py`，使用 Bazel；不用比赛分支的 CMake 流程 |

该工作分支已从本地 master 创建。不要重复创建，不要从之前的修复分支继续开发，不合并整个比赛分支，不更新 master 来改变本题基线。

已检查的兼容性：AI 表达式目录 14 个文件中 13 个完全一致；唯一差异是 master 的 HTTP 客户端增加 WebAssembly 条件分支，原生 Linux 代码一致。AI Endpoint 结构、管理服务、Schema 服务和已检查解析接口也一致。全仓仍有 1,379 个路径不同，主要构建和其他模块差异不能整体带入。

### 1.2 执行边界

- 修改规范时先改 `AGENTS.md` 和本文，再改实现；不新增没有结构约定的目录。
- 保留 SQL 函数名、参数个数和现有返回形式，不新增数据库表或修改系统表 Schema。
- 删除文件或目录、数据库 DDL、清理实例、修改密钥、系统配置、全局安装、推送或公开发布，均先取得用户许可。
- SQL 测试文件中包含建表、删表、创建和删除模型或 Endpoint；编写或引入文件不等于获准执行其中的数据库操作。
- 下载缓存、依赖解包、构建产物和运行日志不进入提交；密钥及带认证信息的代理 URL 不进入文档、代码或日志。

### 1.3 开始前检查

在 Windows PowerShell 的仓库根目录执行：

```powershell
git status --short --branch
git branch --show-current
git rev-parse master
git diff --stat
```

应位于 `feat/ai-function-enhancement`，master 应与表中的基线一致。当前已允许存在本题规范和指南改动；如发现其他人或其他任务的修改，保留并先辨明来源，不执行 reset、stash、清理或覆盖操作。

建议顺序：环境基线 → Prompt → Embedding 单行及公共校验 → Completion → Ollama → Rerank → Embedding 跨行批量 → 全面回归。每阶段独立记录差异、命令、退出码和未验证项。

实施细化：原始 master 快照固定后，耗时的依赖准备可与不依赖运行实例的代码修改穿插进行；基线与修改版均须独立验证，未完成基线不得把环境错误归因于本题代码。Prompt 递归上限复用 `JSON_DOCUMENT_MAX_DEPTH`（当前为 100）。公共工具新增有限数值、Embedding 数量/维度和 Rerank index/score 校验，Provider 先规范化响应，调用层再按请求数量检查。Completion 配置不得覆盖 model、messages、prompt、stream 等协议结构字段；Ollama 采样项进入 options，最终 stream 固定 false。

## 二、先跑通 WSL 与构建路线

### 2.1 已检查结果及其边界

| 项目 | 已观察到的结果 |
| --- | --- |
| WSL | Ubuntu 24.04.4 LTS，WSL2，运行中；通过获准的 WSL 命令可接入 |
| 资源 | WSL 配置 12 GB 内存、8 GB swap；首次检查约有 5.5 GiB 可用内存 |
| 网络模式 | 现有配置启用 mirrored、dnsTunneling、autoProxy |
| 代理路径 | WSL 中 HTTP/HTTPS 代理指向 `http://127.0.0.1:10808` |
| 外网 | GitHub、raw.githubusercontent.com、OceanBase 镜像均取得过 HTTP 200 |
| 波动 | 并行重复检查中出现过连接超时，随后重试成功 |
| 实际依赖 | 已探测锁定的 gtest RPM 的 HTTP 地址，响应成功；不代表所有包已下载 |
| 运行服务 | 检查时 `127.0.0.1:8000` 和 `127.0.0.1:2881` 未监听；其他端口未据此排除 |
| 构建 | 尚未初始化本题依赖、完成编译或部署实例 |

现有网络路线已能工作，先沿用。不要为了偶发超时直接改默认路由、禁用 VPN、改全局 DNS 或重启 WSL。网络设置和代理端口可能变化，每次恢复工作时重新检查。

### 2.2 只读网络预检

PowerShell：

```powershell
wsl --list --verbose
wsl -d Ubuntu -- bash -lc 'uname -sr; ip route; getent ahostsv4 github.com'
```

进入 Ubuntu 后，分别检查 DNS、代理是否可达、目标站点是否返回成功。只输出状态码和耗时，不输出认证头、Cookie 或完整代理环境变量：

```bash
curl --retry 2 --retry-delay 2 --connect-timeout 8 --max-time 30 \
  -sS -L -o /dev/null -w 'github: %{http_code} %{time_total}\n' \
  https://github.com

curl --retry 2 --retry-delay 2 --connect-timeout 8 --max-time 30 \
  -sS -L -o /dev/null -w 'mirror: %{http_code} %{time_total}\n' \
  http://mirrors.oceanbase.com/oceanbase/development-kit/el/8/x86_64/devdeps-gtest-1.8.0-242026020914.el8.x86_64.rpm
```

优先按“DNS → 代理监听 → HTTP CONNECT/TLS → 目标 HTTP 状态 → 实际包下载”定位。GitHub 首页成功不代表 release 资产、Bazel registry、Rust 或 Cargo 下载链路都成功。失败日志记录站点、错误类型和耗时，不凭一个超时认定路由错误。

### 2.3 本题独立目录与环境

拟使用 `/var/tmp/seekdb-ai-enhancement-20261101`，与旧任务的 `/var/tmp/seekdb-fix1469-20261028` 分开。创建前检查目标不存在；如已存在，先阅读其规范和状态，不覆盖或删除。

新目录先建立 `AGENTS.md`，约定以下结构和归属，然后才生成其他文件：

```text
seekdb-ai-enhancement-20261101/
  AGENTS.md             # 本题环境规则，不修改系统或旧任务
  source-00-baseline/   # master 原始快照
  source-01-prompt/     # 后续阶段的源码快照，依阶段编号命名
  builds/               # 各阶段独立 Bazel output_base
  cache/                # Bazel、依赖和下载缓存
  toolchain/            # 本题使用的项目级工具
  logs/                 # 命令、退出码、错误摘要及测试证据
  instances/            # 仅本题新建的隔离实例
  tmp/                  # 本题临时文件
```

源码只在 Windows 的 `seekdb-fork` 编辑。Linux 快照只用于构建，不能成为第二个开发仓库。基线从 `master` 导出；后续快照必须包含本题尚未提交的修改和新增文件，不能仅复制 HEAD 后误测旧代码。每个快照记录基线 SHA、工作区 diff 和新增文件清单。

复制使用新建的空目录，不使用 `rsync --delete` 或目录清理。后续源码不计划删除文件；若出现删除需求，先按用户红线处理。快照和输出各阶段分开，以免基线缓存掩盖代码差异。

本题 shell 使用任务级变量，以下命令在目录及规则已准备后执行；不写入 `.bashrc`、`.wslconfig` 或系统代理设置：

```bash
export TASK_RUN_ROOT=/var/tmp/seekdb-ai-enhancement-20261101
export XDG_CACHE_HOME="$TASK_RUN_ROOT/cache"
export DEP_CACHE_DIR="$TASK_RUN_ROOT/cache/deps"
export CARGO_HOME="$TASK_RUN_ROOT/toolchain/cargo"
export RUSTUP_HOME="$TASK_RUN_ROOT/toolchain/rustup"
export CARGO_BUILD_JOBS=2
export TMPDIR="$TASK_RUN_ROOT/tmp"
export NO_PROXY=localhost,127.0.0.1,::1,0.0.0.0
export no_proxy="$NO_PROXY"
```

继承当前可用的外网代理，不把真实代理凭据保存到文件。`CARGO_HOME`/`RUSTUP_HOME` 应指向已准备好的本题工具目录；不能只设置空目录就声称 Rust 可用。旧缓存只有在锁定版本匹配时才可只读复用或复制，不升级依赖来绕过编译问题。

如果 Mock 在其他主机，另将其明确地址加入两种大小写的 `NO_PROXY`，先验证实际 HTTP 库支持该写法。不要把全网加入免代理列表。

### 2.4 基线构建步骤

1. 检查 `python3`、`git`、`tar`、`rpm2cpio`、`cpio`、Rust 工具是否存在，确认仓库固定的 `.bazelversion` 和 `rust/rust-toolchain.toml`。
2. 当前 master 固定 Bazel `9.3.0`、Rust `1.98.1`；以源码配置为准，不替换为系统随机版本。缺失工具先采用项目级准备方式；需要全局安装时单独确认。
3. 在空的 `source-00-baseline` 中导出 master，记录 SHA。不要包含新分支的功能改动。
4. 阅读 `deps/init/prepare.py` 后才初始化。脚本会替换依赖目录并清理临时目录，因此任何涉及删除的执行必须先获准，只使用本题路径。
5. 准备锁定依赖；对大包使用独立下载缓存、有限次数重试、断点续传，并检查包可解析性。有官方校验值时必须核对，不能把文件存在等同于完整。
6. 初始化、编译并保留完整日志。以下是本分支支持的命令形态，不代表本轮已执行：

```bash
cd "$TASK_RUN_ROOT/source-00-baseline"
python3 build.py init
python3 build.py build -j2 \
  -o "$TASK_RUN_ROOT/builds/baseline" \
  --repository_cache="$TASK_RUN_ROOT/cache/bazel-repository" \
  seekdb
```

`DEP_CACHE_DIR` 控制依赖准备缓存；`XDG_CACHE_HOME` 控制默认 Bazel 下载及结果缓存的根目录。不要假设 `build.py init` 自带断点续传或充分重试：当前下载代码失败会清理 `.download` 文件；断点恢复需在任务级缓存准备阶段完成，不改本题无关的构建脚本来掩盖问题。

使用 `set -o pipefail` 保证日志经过 `tee` 时仍保留真实退出码。构建中途网络失败、缺失宿主工具、编译错误和链接错误分别记录。首轮固定 `-j2`，如内存不足降低并发；不修改系统 swap 或关闭校验。

本机实测的基线兼容问题：SDK 的 GNU ld 2.30 无法链接 Ubuntu 24.04 glibc 的 `.relr.dyn` 段。Bazel 原生 C++ 配置已经指定 LLD，但 Cargo 直接调用 Clang 时没有继承该链接选项。本题以命令行 `--action_env=RUSTFLAGS=-Clink-arg=-fuse-ld=lld` 选择同一锁定 SDK 附带的 LLD；基线与修改版均使用该参数，保留第一次失败日志，不修改系统链接器、SDK 包版本或仓库构建脚本。

初轮 `-j2` 的实测资源余量允许后续生产构建提高至 `-j3`；原生单测独立运行时限制 `-j1`，持续检查内存及 swap，出现明显压力即降低并发，不调整系统内存设置。中断本题构建客户端仅用于改变构建参数，复用已完成缓存，不终止其他任务或 WSL 服务。

### 2.5 基线通过标准

- 锁定依赖准备完成，生产二进制编译并链接成功。
- 获准后启动本题独立实例，数据目录和端口与已有实例不冲突。
- 普通查询、字符串、JSON 及无网络的已有 `AI_PROMPT` 用例可运行，记录当前预期失败点。
- 未运行的 Mock 网络用例标记为“等待官方服务”，而非“通过”。

部署步骤沿用本分支 [编译运行](build-and-run.md) 和 [mysqltest](mysqltest.md) 文档。不要复制其中的 destroy、删除目录或全局安装命令执行。部署包装器可能改写模板文件或准备运行依赖，执行前先确认其具体目标。

## 三、公共调用链和实现约定

### 3.1 先按层定位问题

```text
SQL 参数与类型推导
  → ObExprAI* 的标量 / batch 求值
  → 模型元数据和 Endpoint 解析
  → ObAIFuncModel
  → Provider：请求构造 / 响应解析
  → ObAIFuncClient：HTTP 状态 / 超时 / 资源回收
  → 公共结果校验、顺序恢复
  → 复制到 SQL 结果内存
```

四个表达式的入口在 `src/sql/engine/expr/ob_expr_ai/`。主要公共文件是 `ob_ai_func_utils.h/.cpp`、`ob_ai_func_client.h/.cpp`；Endpoint 校验在 `src/share/ai_service/ob_ai_service_struct.cpp`。

源码阅读入口：[AI 表达式目录](../../../src/sql/engine/expr/ob_expr_ai/)、[公共工具实现](../../../src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp)、[Endpoint 结构与校验](../../../src/share/ai_service/ob_ai_service_struct.cpp)。

### 3.2 全题必须保持的行为

| 层次 | 操作规则 |
| --- | --- |
| 类型推导 | 不盲目将 LOB 转为短 VARCHAR；SQL JSON 二进制不能按普通字符串读取 |
| NULL | Datum 指针存在不代表值非 NULL；先 `is_null()`，再取值或判断可选参数是否生效 |
| JSON | 先检查指针和节点类型，再向数组、对象、整数等类型转换 |
| Provider | 对大小写的容忍只用于比较；不要为了比较强制改写展示值 |
| HTTP | 保留超时和取消路径；检查 HTTP 失败、非法 JSON、业务错误，不能返回成功外观 |
| 结果 | 校验原始字段再序列化，不能用序列化后的非空字符判断内容有效 |
| 生命周期 | 不把临时 allocator 的指针直接留给 SQL 结果；检查容器追加和深拷贝的返回值 |
| 日志 | 不输出 Authorization、access key、完整敏感请求或响应；诊断信息使用受限长度 |
| 兼容 | 修改客户端时保留 `__EMSCRIPTEN__` 分支；不把 Linux HTTP 行为强行用于浏览器后端 |

用户输入非法沿用 `OB_INVALID_ARGUMENT` 等现有 SQL 错误路径；Provider 响应格式损坏沿用 `OB_INVALID_DATA` 或相应现有错误码。具体 SQL 错误编号依据本分支错误映射和测试确认，不提前猜造编号。

## 四、子题一：AI_PROMPT 类型与模板增强

### 4.1 目标与得分点（10 分）

JSON Array/Object 参数 4 分；重复占位符 3 分；TEXT/LONGTEXT 模板与参数 3 分。同时覆盖嵌套 Prompt、非法下标、Unicode、转义和长输出。

### 4.2 当前缺口与修改入口

| 源码入口 | 现状 | 改动方向 |
| --- | --- | --- |
| `ob_expr_ai_prompt.cpp::calc_result_typeN` | 使用字符串类型类判断；明确拒绝 JSON 参数 | 接受文本/LOB 和 JSON 参数，保持模板本身为文本 |
| `ObExprAIPrompt::eval_ai_prompt` | 文本直接取 `get_string()`，JSON 参数返回不支持 | 正确读取 LOB 和 JSON，先处理 NULL，再保存参数 |
| `ObAIFuncPromptObjectUtils::is_valid_prompt_object` | 只容忍部分参数节点类型 | 统一容忍合法 JSON 参数，严格检查 Prompt 外壳 |
| `replace_all_str_args_in_template` | 预估缓冲仅为模板长加每个参数一次，重复替换可能不足；下标累乘未防溢出 | 按模板扫描动态追加，增加边界检查 |
| `ob_expr_ai_complete.cpp` | 额外要求 Prompt args 全是字符串 | 后续子题三移除这个限制并复用统一展开逻辑 |

### 4.3 固定语义

`AI_PROMPT` 返回结构化 JSON，而不是立即返回展开文本。保持如下外壳：

```json
{"template":"{0}/{0}/{1}","args":[[1,true],{"k":2}]}
```

- Array/Object 参数保留原始 JSON 类型，不能先转成带引号的 JSON 文本再存入 `args`。
- 普通字符串在展开时直接插入其内容；普通 JSON 参数在展开时通过 JSON 库序列化，不手写转义。
- 参数中满足严格 Prompt 外壳校验的对象按嵌套 Prompt 展开；其他对象按普通 JSON 处理。不能因为含有一个 `template` 字段就认作 Prompt。
- 构造与展开分开。已有 `AI_PROMPT('{0}+{1}', '1')` 可构造一个尚未补齐的对象，不在构造阶段强制展开所有占位符；实际展开遇到不存在的下标时才报错。
- 数字占位符为零起始的 `{0}`、`{1}` 等；下标越界或超过整数表示范围时报错。其他不能构成数字占位符的花括号内容沿用当前字面处理，不新增 Python format 之类的转义语法。
- SQL NULL 模板或参数仍按现有规则报错；空模板报错，空字符串参数允许。JSON 内部的 null 是 JSON 值，不与 SQL NULL 混为一谈。

### 4.4 具体操作

1. 调整类型推导：识别字符、二进制字符串以及已有文本/LOB 类型；沿用当前不支持普通数值、日期参数的错误行为，避免无意扩大隐式转换。
2. 读取模板和文本参数时采用 `ObTextStringHelper::read_real_string_data`，传入实际 Datum metadata 和 `has_lob_header()`；不要读取到 LOB locator 或头部字节。
3. JSON 参数通过 `ObJsonExprHelper::get_json_doc` 等现有接口取树；明确树的 allocator 和归属，必要时深拷贝到本次构造的 allocator。
4. 构造 `args` 数组，字符串用 JSON string 节点，JSON 用原节点的安全副本；再复用 `construct_prompt_object` 和现有 JSON 打包结果路径。
5. 重构展开函数：一次扫描原始模板，识别有效数字 token，校验下标后将参数展开文本追加到 `ObStringBuffer` 或等价 allocator 缓冲；不要重新扫描已插入的参数字符串。
6. 解析数字时，在 `index * 10 + digit` 前检查溢出；最终长度转换成 `ObString` 长度前检查可表示范围。
7. 嵌套展开复用同一参数转换逻辑并设置受控深度：使用现有 JSON 深度上限；若无法复用，则在实现与测试中显式固定上限并更新本文，不能无限递归。
8. 统一 Prompt 校验和展开入口，避免 `AI_PROMPT` 能构造、`AI_COMPLETE` 却拒绝同一对象。

### 4.5 验证案例与预期

以下 SELECT 可先用于无网络的构造验证；只是指南示例，本轮未执行：

```sql
SELECT AI_PROMPT('{0}/{0}/{1}', JSON_ARRAY(1, TRUE), JSON_OBJECT('k', 2));
SELECT AI_PROMPT('outer:{0}', AI_PROMPT('inner:{0}', '世界'));
SELECT AI_PROMPT('{0}:{0}:{0}', REPEAT('x', 4096));
SELECT AI_PROMPT('line\n{0}', '引号"、反斜线\\与中文');
```

| 场景 | 预期及检查点 |
| --- | --- |
| Array/Object | `args` 保留数组/对象结构；展开后是合法 JSON 文本片段 |
| 重复占位符 | 实际展开每次都替换，不因参数只计算一次而缓冲不足 |
| 嵌套 Prompt | 构造保留结构，展开得到 `outer:inner:世界` |
| 参数含 `{1}` | 作为参数内容插入，不能被外层再次解释 |
| `{999999999999999999999999}` | 展开报参数错误，不溢出或越界 |
| 缺少参数、负数字样、普通花括号 | 按上文构造/展开及字面语义分别验证 |
| TEXT/LONGTEXT/BLOB 系列 | 从表列读取真实数据；长于短 VARCHAR 的内容不截断 |
| 内嵌 NUL、emoji、换行、引号 | 长度按实际字节管理，JSON 序列化正确 |

验收：本题 `ai_prompt.test` 与既有错误类型、NULL、空模板案例一致；展开的重复与嵌套行为还需公共工具测试或官方 Mock 请求内容证明，不能只看到结构化 JSON 返回就判定展开通过。

## 五、子题二：AI_EMBED 单行语义

### 5.1 目标与得分点（10 分）

默认维度 4 分；显式 dimension 3 分；返回 JSON 与维度校验 3 分。单条输入返回一个向量，SQL 多行调用时各行结果独立正确。

### 5.2 当前缺口与入口

- `ob_expr_ai_embed.cpp::calc_result_typeN` 已限制 2～3 个参数，第三参数必须为整数类型；不要删除这类约束。
- `eval_ai_embed` 把 NULL 内容作为错误；显式 dimension 在取整数前未检查 Datum 是否为 NULL；内容读取也要覆盖 LOB。
- `ObOpenAIEmbed::parse_output` 只按响应数组顺序 append embedding，没有读取 index，部分转换前未检查数组类型。
- `ObAIFuncModel::call_dense_embedding_vector_v2` 已检查条数及显式维度，但尚缺完整节点、元素和默认维度一致性校验。

### 5.3 固定语义

- `AI_EMBED(model, text)` 不主动填入固定 dimensions，让 Provider 决定默认维度。比赛样例的 768 只属于该样例模型和服务，不能写入生产判断。
- `AI_EMBED(model, text, dimension)` 对参与请求的非 NULL 输入要求 dimension 是非 NULL 的正整数；不得把 1.5 截断成 1，也不得让超大无符号值溢出成负数。
- 内容为 SQL NULL 时返回 SQL NULL，不产生模型请求。标量和 batch 的 NULL 分支顺序保持一致；模型本身为 NULL/空值等其他错误不要无意全面改成 NULL。
- 沿用现有返回形式：包含一个向量的 JSON 文本，例如 `[0.1,0.2]`，不是 `[[0.1,0.2]]`，也不擅自改变 SQL 类型元数据。
- 省略维度时校验向量非空、元素合法，同一请求内维度一致；不跨模型推断固定维度，不使用进程全局维度缓存。

### 5.4 具体操作

1. 在表达式层先完成 NULL、模型名、文本和 dimension 校验，再解析 Endpoint；内容通过 LOB helper 读取，空字符串沿用已有错误语义。
2. 显式 dimension 才构造 `{"dimensions":N}`。缺省时不把 0、NULL 或样例默认值传给 Provider。
3. OpenAI 响应必须依次检查 root object、`data` array、每条 object、整数 `index` 和 `embedding` array。
4. 在 Provider 解析层读取响应条数 N，建立 N 个位置和已出现标记；index 必须落在 `[0,N)` 且不重复，按 index 放回输入顺序。模型调用层再要求 N 等于本次输入数量。
5. 统一向量校验：数组非空；每个元素必须为可表示的有限数值，拒绝字符串、bool、null、对象、数组及溢出；显式维度严格相等，无显式维度则同请求维度一致。
6. Ollama 的无 index 响应使用协议规定的输入顺序，仍走相同的数量与向量校验。其他 Provider 的原有格式保留，不能套用 OpenAI 字段路径。
7. 先验证完整个响应，再序列化各向量到临时结果集合，最终复制到 SQL 结果内存。任何一步错误都不能返回部分结果作为完整成功。
8. 所有容器追加、分配和序列化检查返回值；空响应、错误根类型不得进入不安全的 `static_cast`。

### 5.5 验证案例与预期

下列 `model` 为测试中预先创建的模型标识；这里用 SQL 变量表示，禁止把变量文本当作生产特殊模型名：

```sql
SELECT JSON_VALID(AI_EMBED(@embed_model, 'single-row'));
SELECT JSON_LENGTH(AI_EMBED(@embed_model, 'dimension-row', 768));
SELECT AI_EMBED(@embed_model, NULL);
SELECT AI_EMBED(@embed_model, 'bad-dimension', 0);
SELECT AI_EMBED(@embed_model, 'bad-dimension', -1);
SELECT AI_EMBED(@embed_model, 'bad-dimension', 1.5);
SELECT AI_EMBED(@embed_model, 'bad-dimension', NULL);
```

显式 768 仅示范样例调用；还要使用服务实际支持的另一维度证明没有写死。

| 响应/输入场景 | 预期 |
| --- | --- |
| 单条、默认维度 | 返回一个合法非空向量，不人为指定默认维度 |
| 两条响应倒序 index=1、0 | 按 index 恢复，并以每个元素的内容证明对应关系 |
| 少一条、多一条、重复或缺失 index | SQL 报格式错误，不静默补齐 |
| embedding 为 null/对象/字符串 | SQL 报格式错误，无崩溃 |
| 维度不符、默认向量长度不一致 | SQL 报错 |
| 元素是整数、合法浮点 | 允许；不要只接受 JSON double |
| 元素是 bool、字符串、null、非有限/溢出值 | 拒绝；含 NaN 的非法 JSON 也应在解析时拒绝 |
| NULL 内容 | 返回 NULL，请求数不增加 |

验收：先通过单行公共校验，再进入跨行批量。仅检查 `JSON_LENGTH` 不能证明向量元素合法或行映射正确。

## 六、子题三：AI_COMPLETE 请求与响应

### 6.1 目标与得分点（10 分）

模型与 Endpoint 配置 3 分；基础调用与 NULL config 3 分；temperature/max_tokens 2 分；响应非空校验 2 分。

### 6.2 当前缺口与入口

`ob_expr_ai_complete.cpp` 已支持字符串和 JSON Prompt，但把结构化 Prompt 限制为全字符串 args。可选 config 的读取判断是指针非空，未充分区分 SQL NULL；且通过字符串读取路径处理 config，原生 JSON config 必须单独正确取树。公共 `call_completion` 解析后直接序列化，缺少原始内容的严格非空与类型判断。

主要改动入口：`ObExprAIComplete::calc_result_typeN`、`eval_ai_complete`、`ObOpenAIComplete::get_body/parse_output`、`ObAIFuncModel::call_completion`。

### 6.3 具体操作

1. 可选参数按实际参数个数求值；缺省 config 和显式 SQL NULL 都表示不覆盖默认配置，不对 NULL 调用文本读取或 JSON 解析。
2. config 为原生 JSON 时使用 JSON helper，为字符/LOB 文本时先读取真实字节再解析 JSON；两条路径最终都要求非 NULL 的 JSON object。
3. JSON null、array、number、string 等不是 config object，报参数错误；SQL NULL 与 JSON null 的区别写入回归测试。
4. 普通 Prompt 使用文本读取路径；`AI_PROMPT` 对象经统一外壳检查及子题一的展开函数处理，不保留“args 必须全字符串”的旧限制。
5. 保留现有模型元数据和 Endpoint 解析路径；模型类型须为 completion，请求模型名遵循现有 `get_request_model_name()` 的 Endpoint 覆盖逻辑，不能替换为 SQL 逻辑模型名。
6. OpenAI 请求沿用 `model`、`messages` 外壳，在 JSON 中按数值类型加入 temperature/max_tokens。新增配置不能破坏 model/messages 等必要字段；检查当前 `compact_json_object` 的合并方向，避免重复键或覆盖后验收失效。
7. 对已传入的 temperature 要求有限数值，max_tokens 要求正整数；不把 Provider 的特定采样范围写死到所有 Provider。未出现的配置不凭空填值。
8. 响应解析前检查 HTTP 和 JSON；OpenAI 按 `choices[0].message.content` 的结构验证每层类型、非空数组、非 NULL 字符串、内容字节长度大于 0。
9. 必须检查原始 content。空字符串序列化后是两个引号，不能因为输出字符串长度为 2 就判作非空响应。是否全为空白不另加题面未要求的拒绝规则。
10. 保留当前 SQL 返回序列化约定，长内容通过已有结果分配工具写入，不截断成短 VARCHAR。共享调用点也要确认没有绕开校验的向量路径。

### 6.4 请求检查样例

```sql
SELECT LENGTH(AI_COMPLETE(@complete_model, 'hello')) > 0;
SELECT LENGTH(AI_COMPLETE(@complete_model, 'hello', NULL)) > 0;
SELECT LENGTH(AI_COMPLETE(@complete_model, 'hello',
                         '{"temperature":0.2,"max_tokens":16}')) > 0;
SELECT LENGTH(AI_COMPLETE(@complete_model,
                         AI_PROMPT('describe {0}', JSON_OBJECT('text','你好')),
                         JSON_OBJECT('temperature',0.2,'max_tokens',16))) > 0;
```

从服务请求记录检查 Prompt 最终文本、model 覆盖、配置数值类型和长文本实际字节数。调用成功不代表配置已传递。

| 场景 | 预期 |
| --- | --- |
| 省略配置、SQL NULL、空 object | 均可调用，行为不因 NULL 指针误读失败 |
| 原生 JSON object 和同内容 JSON 文本 | 请求配置一致 |
| 非 object、非法 JSON、非法已支持参数类型 | 参数错误，不发出错误格式请求 |
| choices 缺失/为空、message 错类型、content 缺失/null/空串 | SQL 报响应错误 |
| HTTP 4xx/5xx、超时、非 JSON 响应 | 返回 SQL 错误并释放资源 |
| 超长 Prompt/响应、Unicode/转义 | 内容不截断，序列化后可正确解释 |

验收：检查原始响应字段和实际出站请求，不能只运行 `LENGTH(...) > 0` 就覆盖本题全部要求。

## 七、子题四：Ollama Provider

### 7.1 目标与得分点（10 分）

Endpoint 创建与 Provider 修改 3 分；空 access key 2 分；原生 Completion 3 分；原生 Embedding 2 分。

### 7.2 当前已有能力和阻塞点

已有 `ObOllamaUtils::ObOllamaComplete`、`ObOllamaEmbed` 和大小写无关的 Provider 分发比较，不必再造一套 Provider 框架。真正需要打通的是：

1. `ObAiModelEndpointInfo::VALID_PROVIDERS` 不含 OLLAMA。
2. `check_valid()` 对所有 Provider 一律拒绝空 access key。
3. Ollama `get_header()` 返回空集合，但 HTTP 客户端 `init()` 把空 headers 当成非法参数。
4. Completion 请求尚未固定 `stream:false`，现有响应解析较宽松。
5. Embedding 已有 `input` 和 `embeddings` 的基本路径，但要接入子题二的完整数量/维度校验。

### 7.3 Endpoint 操作步骤

1. 在统一白名单加入 OLLAMA，继续使用 `case_compare`，同时允许 `ollama`、`Ollama`、`OLLAMA`。
2. 检查顺序先确认 Provider 非空、受支持，再决定 key 是否必填。只有 Ollama 允许空 key；其他 Provider 的现有要求保持。
3. 创建时允许显式空 key；如没有提供 key 且其余必填信息完整，也可按免 key Provider 处理，不放松 URL、模型和 scope 校验。
4. 修改走 `merge_delta_endpoint()`，先合并当前值与增量，再验证最终对象。由 Ollama 空 key 改成要求 key 的 Provider 应报错；同时提供有效 key 时才允许。
5. Provider 的大小写保持用户存入值，因为官方结果文件会展示 `ollama` 和修改后的 `Ollama`。只在比较时忽略大小写。
6. 保留现有创建、查询、修改、删除与权限路径；不新增 Schema、不修改系统视图列。

### 7.4 HTTP 和协议操作步骤

- HTTP 客户端允许无认证头请求；通用 JSON Content-Type 可由既有客户端机制统一处理。不要添加假的 Bearer key 只为绕过 headers.empty()。
- 保留 native 和 `__EMSCRIPTEN__` 各自实现范围。
- Completion 请求最小结构如下，模型和 Prompt 取自调用参数：

```json
{"model":"<request model>","prompt":"<expanded prompt>","stream":false}
```

- 在配置合并完成后保证最终 `stream` 仍是布尔 false，避免配置重启流式输出。解析一个完整 JSON object 的 `response` 非空字符串，拒绝 NDJSON 流混入普通响应。
- 如果支持 Completion 采样配置，按 Ollama 原生 options 字段映射：temperature 进入 `options.temperature`，max_tokens 对应 `options.num_predict`；不把 OpenAI 的 messages/max_tokens 外壳原样发送。不额外引入本题没有要求的配置体系。
- Embedding 请求最小结构是 `{"model":"<request model>","input":["a","b"]}`，从 `embeddings` 读取每条向量。无 index 时按输入顺序，数量必须与 input 长度一致。
- URL 由 Endpoint 提供，官方样例是 `/api/generate` 和 `/api/embed`；不根据模型名偷偷重写路径，不改走 OpenAI 兼容路径。

### 7.5 验证与完成标准

| 场景 | 检查点 |
| --- | --- |
| 小写创建、混合大小写修改、大写创建 | 都接受；查询展示原存值 |
| 空 key / 省略 key 的 Ollama | 可以创建和请求，出站不携带伪造认证信息 |
| 空 key 的 OpenAI 等原 Provider | 保持拒绝 |
| Ollama 空 key 改成 OpenAI | 最终对象校验失败，不保存半成品 |
| Completion | 路径正确，prompt 正确，最终 stream=false，response 非空 |
| Embedding 多输入 | input 是数组，embeddings 条数/维度/元素正确 |
| 缺字段、空 response、错误类型、错误 HTTP | SQL 报错，不崩溃 |
| Endpoint 删除与再次查询 | 旧生命周期规则、错误码和权限行为不变 |

所有创建、修改和删除 Endpoint 的实际执行均放在获准的隔离实例中。此题验收必须通过原生协议请求证明，不能只证明白名单接受 OLLAMA。

## 八、子题五：AI_RERANK 结果正确性

### 8.1 目标与得分点（10 分）

字符串数组 3 分；对象数组及文本字段 3 分；数量、顺序与原始 ID 映射 4 分。

### 8.2 当前缺口与修改入口

- `ob_expr_ai_rerank.cpp::eval_ai_rerank` 用 `arg_doc_key` 指针是否存在判断对象模式，显式 SQL NULL 会被误认为有效文本字段。
- 字符串路径每批 20 条；对象路径 `eval_ai_rerank_with_doc_key` 直接整体调用 `model.call_rerank`，两种模式行为不一致。
- `batch_result_add_base`、`sort_document_array_by_model_result` 等函数存在检查类型前转换的情况。
- `compact_json_array_by_key` 依赖两批各自有序，而且合并边界使用 `min_count`；不能保证任意响应下的全局稳定排序，错误路径还需要检查循环是否终止。

### 8.3 统一数据流

```text
原始文档数组
  → 校验并提取文本（字符串自身 / 指定对象字段）
  → 每批最多 20 条，记录 batch_start
  → Provider 返回局部 index + relevance_score
  → 校验本批数量、index 完整性及 score
  → global_index = batch_start + local_index
  → 所有结果按 score 降序、global_index 升序排序
  → 字符串结果沿用现有字段；对象结果映射回原对象并附 _model_score
```

### 8.4 具体操作

1. `doc_key` 缺省或 SQL NULL 进入字符串数组模式；仅非 NULL 的有效字符串字段名进入对象模式。不能用 `OB_NOT_NULL(arg_doc_key)` 代替值判断。
2. 文档根节点必须为非空 array。字符串模式逐条检查 string；对象模式逐条检查 object、指定字段存在且为 string。空/非法字段名、混合数组、错误元素类型及时报参数错误。
3. 对象模式保留原数组和原始对象完整内容，只提取指定字段发送。不能把对象序列化成字符串后当作文本，也不能丢弃 id 或其他业务字段。
4. 两种模式共用同一个分批调用路径，继续沿用现有 20 条上限，不为对象模式保留绕过路径。
5. 每批响应结果必须为 array，数量等于本批文档数；每条为 object，index 是本批合法整数且唯一，score 为有限数值，允许整数和浮点形式。
6. 在进行偏移前完成局部 index 校验。禁止负数、越界、重复、缺失 index；将合法局部 index 加本批起点，映射到原始数组。
7. 汇总所有批次后统一排序，比较键固定为 `(-score, global_index)`。不要假设服务已按分数排序；即便使用非稳定排序算法，第二键也必须保证同分原始顺序。
8. 字符串模式保留现有 index/score 等输出约定。对象模式深拷贝原对象后附加现有 `_model_score`，不能修改输入树；原始 id 仅是业务字段，不能作为数组下标。
9. 任意一批响应非法或请求失败，整体调用返回错误，不吞掉该批、不把前面成功批次作为完整结果。

### 8.5 重点案例

```sql
SELECT JSON_LENGTH(AI_RERANK(@rerank_model, 'apple',
  '["apple","banana","fruit","vegetable"]', NULL));

SELECT AI_RERANK(@rerank_model, 'apple',
  '[{"id":"a","text":"apple"},{"id":"b","text":"banana"}]', 'text');
```

| 案例 | 预期 |
| --- | --- |
| 3 参数和第四参数 NULL 的字符串数组 | 两者行为一致，返回全部文档结果 |
| 指定 content 而非 text 字段 | 使用指定字段；其余字段和 id 保留 |
| 19、20、21、40、41 条 | 验证无越界、无漏项，跨批次 index 正确 |
| 各批响应局部乱序、后一批高分 | 全局排序正确，不简单拼接 |
| 多批次同分 | 按原始文档顺序输出，重复执行顺序稳定 |
| 业务 id 为字符串、大整数或重复值 | 按原位置映射，不用业务 id 寻址 |
| 少结果、重复 index、越界 index、score null/string | 明确报错；不能排序到一半挂住 |
| 第二批超时或错误 | 返回 SQL 错误，后续独立调用仍可运行 |

验收：不仅比较数量，还逐项核对排序、全局 index、原始字段和 score 的配对关系。只测试两条对象不够覆盖分批路径。

## 九、子题六：AI_EMBED 跨行批量

### 9.1 目标与得分点（10 分）

表扫描多行调用 3 分；NULL 传播 3 分；原行顺序与维度映射 4 分。必须一个请求携带多条输入，不能把多次单行 HTTP 请求并发发送当作完成批量。

### 9.2 已有接口与应选路线

| 入口 | 当前行为 | 本题用途 |
| --- | --- | --- |
| `ObExprAIEmbed::cg_expr` | 只注册 `eval_func_` | 保留标量回调，新增 batch 回调 |
| `call_dense_embedding_vector` | 每条输入构造一个 body，再 `send_post_batch` | 这是多 HTTP 请求，不作为本题跨行合并实现 |
| `call_dense_embedding_vector_v2` | 多条输入构造一个 body，只调用一次 `send_post` | 复用此路径，并先补完子题二的响应校验 |
| `ob_expr_array_concat.cpp::eval_array_concat_batch` | 展示 skip、evaluated flags、DatumVector 用法 | 参考本分支 batch API，不复制其提前标记结果的时机 |
| `ObEvalCtx::BatchInfoScopeGuard` | 管理 batch 索引上下文 | 如逐行读取/写回需要，使用 RAII 恢复索引 |

### 9.3 新增表达式接口

在 `ob_expr_ai_embed.h/.cpp` 增加与当前引擎匹配的静态回调：

```cpp
static int eval_ai_embed_batch(const ObExpr &expr,
                              ObEvalCtx &ctx,
                              const ObBitVector &skip,
                              const int64_t batch_size);
```

在 `cg_expr` 中注册 `rt_expr.eval_batch_func_`，同时保留 `eval_func_`。不得只定义函数却没有挂入执行器，也不得将整个 batch 回调写成循环调用单行 `eval_ai_embed`。

### 9.4 批次算法

1. 取得输出 DatumVector 与 evaluated flags。计算真正需要处理的行：不在 skip 中、尚未求值；子表达式求值时也传递相应 mask，避免重复触发已有副作用表达式。
2. 使用引擎的 `eval_batch` 求模型、内容和可选 dimension。通过本分支 DatumVector 访问抽象读取常量和逐行参数，不假设常量参数也有 batch_size 个独立存储槽。
3. NULL 内容的行记为 NULL，不加入请求。对参与请求的其他行执行与标量相同的文本、模型和 dimension 校验，使用 LOB helper 读取内容。
4. 建立本批内分组：相同逻辑模型、相同 dimension 状态/值共用一次解析得到的模型与 Endpoint。缺省 dimension 与显式值必须分开；不能因“通常模型是常量”而忽略逐行模型参数。
5. 每组保存 `contents`、`request_to_row`。如 Provider 现有明确上限需要再分块，每块都保留自己的请求内 index；不把一个模型的响应写进另一组。
6. 每组调用一次多输入 `call_dense_embedding_vector_v2`。Provider 层先按 index 恢复请求内顺序，模型层再校验数量和维度。
7. 结果先暂存在本批 allocator。当前回调涉及的所有请求均成功并校验通过后，才逐行复制到 SQL 所拥有的结果内存；写回失败也要返回错误，不把尚未完成的行标记为成功。
8. 正确设置每行 evaluated flag；skip 或先前已求值的行不修改、不重发。完整批次失败时不能留下可以被当作本次成功的陈旧 flags。
9. 函数退出前恢复 batch 索引上下文，释放请求和临时结果；下一执行批次重新建立局部映射，不保存指向上批次临时内存的内容或向量。

本题不增加跨查询缓存、后台请求合并器或并发队列；合并范围是 SQL 引擎当前提供的执行批次。多批次表示多次回调连续正确执行，不要求把整个查询结果攒在内存中。

### 9.5 映射示例

```text
原行位置       0       1       2       3
内容         first   NULL    third   fourth
skip         false   false   false   true

HTTP input   [first, third]
映射表       [0, 2]
响应顺序     index=1 → third_vector, index=0 → first_vector

恢复后       row0=first_vector, row1=NULL, row2=third_vector
             row3 沿用 skip 语义，不求值、不写回
```

必须依次恢复“响应 index → 请求位置 → 原行位置”。不能直接拿服务返回数组位置当成原行位置，也不能因为过滤或 NULL 被压缩就丢失行号。

### 9.6 批量验收矩阵

| 场景 | SQL 结果检查 | 请求检查 |
| --- | --- | --- |
| 多行同模型 | 每行向量正确 | 至少一次 input 长度大于 1 |
| 第一/中间/最后一行 NULL | NULL 位置正确，其余不移位 | NULL 不进入 input |
| 全 NULL | 全部 NULL | 0 个模型请求 |
| WHERE 过滤 | 返回行与原表 id 对应 | 引擎传入 skip 的行不请求 |
| LIMIT 0、1、跨批边界 | 条数和内容正确 | 不额外扫描全表攒批次；考虑计划允许的上游预取 |
| ORDER BY、子查询、重复引用 | id 与向量内容正确 | 已求值行不重复请求 |
| 数据量超过一次执行批次 | 每批尾部及下一批开头都正确 | 每批独立请求和映射，无上一批残留 |
| 不同模型/不同 dimension | 各行维度及内容正确 | 按组分开发送 |
| 返回乱序 index | 与原 id/文本一一匹配 | 服务 index 确实乱序 |
| 少结果、重复 index、某组失败 | SQL 返回错误，后续查询仍正常 | 不静默退化为逐行请求掩盖错误 |

LIMIT 本身不保证执行计划只求值最终返回的几行，测试按实际算子批次和 skip 语义判断，不编造固定请求次数。但必须证明没有为了合并请求而绕开引擎、额外取完整表。

验收证据必须包含请求数量、各请求 input 长度、原行 id/输入摘要和对应向量。只看向量长度相等无法证明跨行映射正确。

## 十、测试材料、执行流程与最终交付

### 10.1 官方材料怎么使用

官方分支已公开：

- `tools/deploy/mysql_test/test_suite/ai_function/t/ai_function_enhancement.test`
- 对应 `r/mysql/ai_function_enhancement.result`
- `ai_prompt.test` 及结果文件的增强变更

官方分支检查入口：[AI 函数测试](https://github.com/oceanbase/seekdb/tree/2026-competition/tools/deploy/mysql_test/test_suite/ai_function)、[单元测试目录](https://github.com/oceanbase/seekdb/tree/2026-competition/unittest/sql)。

这次检查未在公开分支找到题面所列的 `ob_expr_ai_prompt_test.cpp`、`test_ob_openai_utils.cpp` 和赛题一 Mock。按用户要求视为官方保管，不为寻找或重造这些材料阻塞全部实现，也不借用 AI 列的 Mock 冒充本题官方服务。

测试迁移只做四个文件范围：新增 enhancement 的 test/result，移植 ai_prompt 的 test/result 题目相关改动。保留 master 的 Endpoint 测试，不覆盖比赛分支里涉及 MAX_CONCURRENCY 等其他能力的变更。

从官方 GitHub 分支读取固定提交的文件并校验差异；不要从其他本地开发仓库复制实现，不合并比赛分支，不让材料迁移顺带改变构建、Schema 或 CI。

### 10.2 本地无官方服务时能验证什么

| 验证层次 | 可以执行的内容 | 不能据此声称的结论 |
| --- | --- | --- |
| 源码与文档检查 | 类型分支、调用链、错误处理、差异检查 | 不能证明编译通过 |
| 原生编译 | 生产目标及实际存在的受影响目标 | 不能证明 SQL 已运行 |
| 本地 SQL | 普通 SQL、Prompt 构造、可在网络前确定的参数错误 | 不能证明 Provider 请求和响应正确 |
| 官方网络测试 | 官方 Mock 可访问时跑 enhancement 等用例 | 服务不可用时必须明确待验 |
| 扩展负例 | 官方提供故障能力或可用测试接口时验证乱序、错误和超时 | 不虚构服务一定支持注入所有异常 |

需要新增本地 C++ 回归时放在 `unittest/sql/` 的合适位置，先定新目录约定，再按本分支 Bazel 规则显式声明真实依赖。当前没有可直接运行的 `sql_tests` 聚合目标，不能照抄比赛分支 CMake 命令或编造缺失测试目标。

可以先查询实际目标：

```bash
python3 build.py query 'kind(cc_test, //unittest/...)'
```

本题新增本地目标 `//unittest/sql/engine/expr:ai_function_validation_test`，通过 SQL 模块内受限的 test-only 接口调用真实生产工具，覆盖 Prompt、向量解析、Completion 和 Ollama 协议。该目标是独立编写的回归测试，不冒充官方未公开单测，也不启动替代 Mock。新目录的结构和规则见 `unittest/sql/AGENTS.md`。运行时使用与生产构建一致的任务缓存、LLD 环境参数及 `python3 build.py test ... --test_output=errors`。

测试接口单独编译生产文件 `ob_ai_func_utils.cpp`，使用真实 OBLib/JSON 依赖及函数段回收，不引入服务器桩或复制实现；因此协议工具回归不必先链接整个 SQL 执行器。完整表达式、batch、排序与数据库对象行为仍由生产构建和 SQL 测试验证。

官方 C++ 源码后来提供时再核对测试依赖和构建入口，不能仅复制文件后认为已接入。

### 10.3 SQL 测试执行方法

先完成隔离实例及测试 DDL 的许可；确认本题部署名、端口、数据库和数据目录。包装器会部署或调用数据库操作，因此以下仅为命令模板，不在未获准的实例上运行：

```bash
cd "$TASK_SOURCE_ROOT/tools/deploy"
bash obd.sh mysqltest -n "$TASK_DEPLOYMENT" \
  --suite ai_function --test-set ai_prompt --disable-reboot

# 官方 Mock 服务可用、且从数据库进程所在 WSL 可访问后再运行：
bash obd.sh mysqltest -n "$TASK_DEPLOYMENT" \
  --suite ai_function --test-set ai_function_enhancement --disable-reboot
```

`TASK_SOURCE_ROOT` 为当前已同步并构建的阶段快照，`TASK_DEPLOYMENT` 为本题实际隔离部署名；测试前必须赋值并校验，不能指向已有工作实例。按该分支实际包装器帮助确认选项后执行，不添加 `--record` 来覆盖期望输出以“修复”失败。

官方样例 Endpoint 使用 `http://0.0.0.0:8000/...`；Mock 应与数据库处于可达的同一网络环境，且本机地址绕过代理。`0.0.0.0` 是服务监听通配地址，不是可以跨机器访问的目标主机名；若服务放在另一主机，要使用明确可达地址并记录本地适配，不能改生产代码特判该 URL。

### 10.4 每阶段统一检查表

1. `git diff --check`，确认没有空白错误和意外文件。
2. 检查本阶段文件范围，不包含其他问题修复、系统配置、密钥、缓存或日志。
3. 从当前工作树同步新的阶段快照，确认新增文件及未提交改动已进入快照，记录对应 diff。
4. 使用本分支锁定工具链编译，保留退出码及日志；失败先定位，不通过注释、忽略错误或关闭检查绕过。
5. 执行可用的本阶段正例、边界和错误案例，注明数据库是否重启并加载了新二进制。
6. 回归上一阶段涉及的公共函数、Provider 和普通 SQL；仅在新变更或失败需要时扩展测试。
7. 记录未执行的官方用例、缺失材料和环境问题；修复代码与失败证据分别保留。

### 10.5 最终完成标准与交付内容

- 六项能力均在 SQL 引擎中生效，生产代码没有针对比赛模型名、固定维度、输入文本或预期响应的特例。
- 成功构建实际交付的源码版本；官方可用用例按预期通过，无法执行的部分如实列出，不宣称已获 60 分。
- 向量与 Rerank 不仅数量正确，还能证明索引、顺序、业务 ID、分数及维度一一对应。
- 请求失败、响应格式损坏和多批次执行不会导致崩溃、错位、无限循环或资源遗留；关闭/取消路径纳入检查。
- 交付分支差异、操作指南、实际测试摘要、日志位置和待验项；提交和推送另按用户授权执行，不自动发布。

### 10.6 当前实施记录（2026-11-01）

- 分支：`feat/ai-function-enhancement`，基于本地 master `9065fb8c2`。2026-11-01 验证结束时尚未提交；2026-11-02 用户已授权将现有改动保存为本地检查点，供后续 AI 列分支复用。远端推送不在本轮授权内。
- 六个子题的主要源码改动已写入工作树，修改版完整构建及当前可执行的本地回归已通过；官方网络端到端验收仍待 Mock 服务。
- 已从官方固定提交引入 enhancement 的 test/result、Prompt 的题目相关变化，并增加无需 Endpoint 的 JSON 结构、嵌套结构和 70000 字符参数案例。修改版数据库执行完整 `ai_prompt.test`，对照已有及新增期望结果返回 `ok`，未使用录制模式。
- 独立验证目录：`/var/tmp/seekdb-ai-enhancement-20261101`。已保存 master 基线快照、修改版快照、任务规则和日志；源码修改仍只发生在 Windows `seekdb-fork`。
- 28 个锁定依赖包已准备并完成 RPM 内容检查、项目级解包。Bazel 9.3.0 已下载且 SHA256 匹配仓库清单；复制的 Rust 工具链已实际输出 `rustc 1.98.1`。
- `git diff --check` 通过；7 个修改的 C++ 实现文件经锁定工具链的 clang-format 解析，均未报告 incomplete format。这只是格式解析，不是 C++ 编译或功能测试。
- 最初在正式生成依赖之前尝试直接原生语法探测，遇到缺失 Rust 生成的 `logger.h`。该前置依赖现已由正常构建生成；未使用桩头文件、注释或关闭检查绕过。
- 后续正式基线构建已生成真实的 Rust/C++ 桥接和系统表头文件。使用相同锁定 Clang、C++20 与这些真实生成文件，7 个修改的实现文件及新增单测源文件均通过原生类型/语法检查；这与完整 Bazel 生产链接、SQL 执行分别记录。
- 本地原生目标 `//unittest/sql/engine/expr:ai_function_validation_test` 已完成 Bazel 编译和链接，10 个 Google Test 用例全部通过（0 failures、0 errors）：长参数重复替换、结构化/嵌套 Prompt、非法下标、Unicode/NUL/JSON null、Embedding 乱序及非法响应、Completion 内容和配置、Ollama 原生协议、Rerank index/score。日志为 `logs/test-ai-function-validation-final.log`，真实测试 XML 位于 `builds/validation/.../testlogs/unittest/sql/engine/expr/ai_function_validation_test/test.xml`。
- 用户已明确允许构建工具清理本题目录内的自动临时缓存，并允许仅在该目录内新建的隔离实例中执行测试 DDL、模型/Endpoint 增删改。旧任务、已有数据库、全局安装和系统配置不在授权内；每次递归清理前仍检查最终路径。
- 基线与修改版 `python3 build.py init` 已执行成功。master 基线正式 Bazel 构建及生产链接已成功（`logs/build-baseline-j3.log`，退出码 0），数据库验证和官方 Mock 验收分别记录。
- 基线首次启动报告缺少 `libaio.so.1`。该运行库已经包含在锁定 SDK 的 `usr/local/oceanbase/deps/devel/lib` 内：只为本题数据库和 SQL 客户端进程补充 `LD_LIBRARY_PATH`，不安装系统包，也不把该运行环境混入编译环境。SQL 客户端显式使用 `utf8mb4`，避免本机默认字符集影响 Unicode 验证。
- 修改版正式 Bazel 构建及生产链接成功（`logs/build-enhancement.log`，退出码 0）。两版均使用相同锁定依赖与上述 Cargo LLD 参数；本机确认资源余量后以 `-j3` 执行，编译器、缓存和输出仍限制在本题目录。
- 基线隔离实例的普通算术、JSON、原有 AI_PROMPT 冒烟通过。修改版隔离实例的 Prompt mysqltest、1500 行 NULL 输入传播、过滤/排序/LIMIT、非法 dimension、溢出 Prompt 下标、非法 Completion 配置，以及免 key 的 Ollama Endpoint 创建、Provider 大小写修改和失败修改回滚均通过。后两类仅检查 SQL/管理行为，不发送 HTTP 请求。
- Endpoint 回归保留 master 完整 test/result。单机隔离夹具不预设框架的 admin 账户，因此单独执行原文件 `### test_ai_user` 之前的 DDL 段：测试和期望结果均从 master 原文件按相同边界逐字截取到本题 `tmp`，仅补本机连接变量；基线和修改版都对照原期望结果通过。来源摘要为 `logs/endpoint-ddl-prefix-source.json`，两版日志为 `logs/mysqltest-endpoint-ddl-baseline.log` 和 `logs/mysqltest-endpoint-ddl-enhancement.log`。该结果不包含权限账户用例，未录制或改写期望结果。
- SQL 工作流记录在 `logs/validation-workflow.json`，Prompt 日志为 `logs/mysqltest-ai-prompt.log`。所有数据库都使用本题新建的实例目录，验证后已停止；该版本的 SIGTERM 处理会自行发送 SIGKILL，因此停止退出码为 -9，未改动上游停止逻辑。历史环境和夹具错误单独保留，不计入通过结果。
- 官方赛题一 Mock 与题面两份 C++ 单测仍未公开，不另造替代服务，不宣称官方验收或 60 分已经通过。

待官方环境验证：完整 `ai_function_enhancement.test`、非 NULL 跨行多输入请求的实际请求记录、乱序回包的原行映射、多批请求失败恢复、Rerank 跨批全局索引与同分排序，以及 OpenAI/Ollama 的 HTTP 端到端协议。当前原生单测只证明所覆盖的生产解析与校验函数，不替代这些网络和完整 SQL 路径的验收。master Endpoint 权限账户用例也尚未执行。
