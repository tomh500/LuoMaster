# Luo Master 与 LuoBackup 两阶段维护原型

Master 是回环地址上的 C++20 控制服务。Java 客户端和基岩客户端继续经既有接入代理、Geyser、BungeeCord 进入 Spigot。QQ Bot 与已有 API、数据库和存档继续由现有组件负责。`SpigotMaster-GAL` 是现有跨端插件，部署和依赖关系与 `luo-master.service` 独立。

## 公开资料与实现边界

网易开服工具 2.0 的公开概述将接入、协议转换、控制协调和 Java 游戏节点分层。原 SpigotMaster 的公开文档描述 Java 插件提供的客户端事件和游戏扩展 API。该项目沿用分层与显式服务接口，提供自己的维护 v1 API 和 C++ SDK。已有 Bukkit/GAL 插件继续使用原 Java 接口。网易文档的公开镜像仓库未声明许可证，公开的部署和 API 页面也没有给出原 SpigotMaster 实现或资源的可验证再分发授权。当前代码、示例和配置均为本项目实现。 [开服工具 2.0 架构概述](https://mcwiki.easecation.net/mcguide/27-%E6%89%8B%E6%9C%BA%E7%BD%91%E7%BB%9C%E6%B8%B8%E6%88%8F/%E8%AF%BE%E7%A8%8B10%EF%BC%9A%E4%BD%BF%E7%94%A8Spigot%E5%BC%80%E6%9C%8D/10-%E6%94%AF%E6%8C%81%E5%9F%BA%E5%B2%A9%E7%89%88%E5%AE%A2%E6%88%B7%E7%AB%AF%E7%9A%84Java%E7%89%88%E7%BD%91%E7%BB%9C%E6%B8%B8%E6%88%8F%E6%A6%82%E8%BF%B0.html) · [SpigotMaster API 文档](https://mcwiki.easecation.net/mcguide/27-%E7%BD%91%E7%BB%9C%E6%B8%B8%E6%88%8F/%E8%AF%BE%E7%A8%8B10%EF%BC%9A%E4%BD%BF%E7%94%A8Spigot%E5%BC%80%E6%9C%8D/81-SpigotMasterAPI%E6%96%87%E6%A1%A3.html) · [公开文档仓库](https://github.com/MCNeteaseDevs/netease-bedrock-wiki)

## 维护流程

1. LuoBackup 的 Java 命令或 SDK 客户端向 Master 提交 profile 和幂等键。Master 只有近期探测到独立 worker 时才接受新任务，接受后立即返回操作编号。
2. Master 与生命周期适配器确认全部游戏节点状态，持久化原运行集合，再按依赖逆序保存、停止和确认进程退出。所有 profile 共用同一组受控游戏节点。配置独立接入层时先停接入，再停 BungeeCord 和后端。外部存储和 QQ 服务保留独立运行。
3. 游戏节点退出后，独立 `luobackup-worker` 获得同一任务的限时租约。它从配置的来源打包到暂存 ZIP，排除 `session.lock` 及 `exclude` 中选定的子树，拒绝链接及特殊文件。它逐条读回 ZIP、比对 SHA-256 并同步文件到磁盘。
4. worker 向同一个 Master 提交带任务租约的恢复请求。Master 固化回执并再次校验整个 ZIP 的 SHA-256，按正序恢复原先运行的节点，确认原先停止的节点仍停止。恢复成功后原子发布归档。任一归档步骤失败时走同一恢复通道，作业最终标为失败。

两个操作入口及服务命名见 [安装契约](CONTRACT.md)。Master 留存 SQLite 维护记录，用来回放同一操作 ID 和恢复未完成任务。作业的正常并发上限为一个，因为全部 profile 共用停服范围。HTTP 有独立固定大小的线程池及有界队列，归档压缩和校验由另一个进程承担。`GET /readyz` 表示 Master 可接受维护请求，完整接入还需检查 `worker_online`。日志包含阶段、时间戳、作业 ID 和失败原因，凭据与租约不出现在查询结果和日志中。

`recovery_required` 表示 Master 无法安全确认拓扑，继续拒绝新申请。检查失败日志，修复适配器、进程状态或归档锁后，用原配置和状态目录重新启动 Master。若其在停服中被强制终止，随后启动会用 SQLite 快照恢复。归档 worker 若因阻塞 I/O 持锁，Master 等待短暂释放窗口后仍无法获取锁时进入 `recovery_required`，此时需先让 worker 退出及释放锁，再重启 Master。恢复进程状态不会覆盖游戏文件。生产适配器必须阻止其他启停入口绕过维护流程。

## 构建和本地验证

需要 CMake 3.25+、支持 C++20 的编译器、nlohmann-json、SQLite 3.31+、OpenSSL Crypto、libarchive 和 zlib。cpp-httplib 使用固定的 0.67.0 源码 SHA-256，可用已安装的同版本 CMake 包或由 CMake 下载。Python 3 用于测试和基准。Ubuntu 24.04 的命令如下。 [cpp-httplib v0.67.0 发布记录](https://github.com/yhirose/cpp-httplib/releases/tag/v0.67.0)

```bash
sudo apt-get install build-essential cmake nlohmann-json3-dev libsqlite3-dev libssl-dev libarchive-dev zlib1g-dev python3 ca-certificates
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 tests/benchmark.py build/luo-master build/fake-lifecycle build/luo-master-client build/luobackup-worker
```

Windows x64 使用 Visual Studio 2022、CMake 和 vcpkg 的静态库。以下配置使用 Release 版 libarchive，以匹配 Release 运行库。

```powershell
& "$env:VCPKG_ROOT/vcpkg.exe" install nlohmann-json:x64-windows-static sqlite3:x64-windows-static 'libarchive[core]:x64-windows-static' openssl:x64-windows-static zlib:x64-windows-static
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  "-DLibArchive_LIBRARY=$env:VCPKG_ROOT/installed/x64-windows-static/lib/archive.lib"
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python tests/benchmark.py build/Release/luo-master.exe build/Release/fake-lifecycle.exe build/Release/luo-master-client.exe build/Release/luobackup-worker.exe
./luobackup/build.ps1 -BungeeJar /path/to/BungeeCord.jar
```

Linux 可用 `bash luobackup/build.sh /path/to/BungeeCord.jar` 构建 Java 桥接。两种构建都需要支持 `javac --release 8` 的 JDK，并只将自研插件类及配置打入生成的 `luobackup/build/LuoBackup.jar`。

测试自行创建临时目录、回环监听端口及假的生命周期适配器。SDK 的 `luo-master-client` 支持 `topology`、`submit ID PROFILE` 与 `status ID`，起点是 `sdk/example.cpp`。旧 `/luobackup now <server>` 命令保留，并可附已有任务 ID 进行重试。`/luobackup status <operation-id>` 查询状态。插件新默认配置保持关闭，旧配置需按下方方式显式迁移。

## 安装配置

`examples/` 含脱离生产环境的 JSON 模板与两个 systemd 单元。模板以 `luo-master` 和 `luobackup-worker` 两个专用身份运行，只有 Master 账户可进入控制台 `RuntimeDirectory`。`state_dir` 和 `archive_dir` 归 Master 所有并设为 0700。`staging_dir` 和 `archive_dir` 需要同一文件系统以支持原子发布。`staging_dir` 归 `luo-master:luo-backup` 所有并设为 3770，启用 setgid 继承归档共享组及 sticky bit 保护锁文件。首次安装时创建归 `luo-master:luo-backup` 所有、模式 0640 的 `staging_dir/.archive-io.lock`。两个服务共享 worker bearer 文件的读取权限，其他令牌按客户端身份单独发放。Linux 令牌使用 0600 或限定组的 0640。Windows 为令牌、状态和归档目录设置服务账户专用 ACL，本地单账户试验可省略 `staging_dir`。

由安装方按生命周期契约提供适配器并配置各节点。启动整组服务时允许 worker 提前轮询，实际放行条件为 Master `/readyz` 成功且 `worker_online=true`。整组关闭应保持 worker 存活直到 Master 收尾，示例单元的 `After=` 提供对应的 systemd 停止顺序。依据实际游戏启动和归档时限调整停止等待时间。

为 LuoBackup 原 `servers.<name>` 的选中内容创建同名或独立 profile，逐项转换 `include`/`exclude`，包括世界路径、Bedrock/Java 实际存档位置和玩家数据路径。手动和定时备份如需不同范围，可使用不同 profile。应用新 Bungee 客户端前需先完成真实生命周期适配，验证每个节点的保存 ACK、退出和启动探测，并把其他启停脚本纳入同一互斥边界。示例 adapter 是测试替身。原型未迁移旧插件的定时调度、容量和槽位轮换。定时触发需用独立运行的调度器持稳定幂等键调用同一提交 API。原型未包含数据还原命令，真实备份还需进行解压和独立恢复演练。统一经济的接口预留见 [经济契约](CONTRACT.md#统一经济接口预留)。

## 单独启动与连接

完成上面的 CMake 构建并准备对应的非生产配置后，可启动 Master 进程和本机前台控制台。需要受理备份时，在第二个终端单独启动 worker。

```bash
./build/luo-master --config /absolute/path/master.json --console
./build/luobackup-worker --config /absolute/path/master.json
curl --fail --silent http://127.0.0.1:18776/healthz
curl --fail --silent http://127.0.0.1:18776/readyz
./build/luo-master-client 18776 /absolute/path/observe.token topology
```

按模板配置好程序和两个 unit 后，单独启动或停止 Master 服务可使用以下命令。示例 unit 会拉起自己的 worker。连接命令在远端主机的本机 SSH 终端运行，需要事先获准切换至专用服务账户。

```bash
sudo systemctl start luo-master.service
sudo systemctl status luo-master.service luobackup-worker.service
sudo -u luo-master /usr/local/bin/luo-master --attach /run/luo-master/console.sock
# 在控制台输入 help、jobs、end。Ctrl-D 退出已连接的客户端。
sudo systemctl stop luo-master.service
```

`end` 在空闲时停止 Master，保留 worker 和 Minecraft 的当前状态。`systemctl stop` 向 Master 发 SIGTERM，停止接单并等待已受理的作业完成恢复。处理活动任务期间保留 worker 进程；单独停止 worker 可在 Master 已退出后执行。

## 维护命令

下表是原型实现的完整命令集。所有命令可从本机控制台使用。QQ 和 Proxy 凭据仅可执行对应白名单中的普通动作，表中的共享权限仍须在服务配置中单独授予。

| 命令 | 权限 | 作用与副作用 | 失败行为 |
| --- | --- | --- | --- |
| `help` | 本机或获授权服务 | 显示调用方获准命令及参数提示。只读。 | 多余参数返回 400。 |
| `topology` | 本机或获授权服务 | 查看配置节点、依赖和 worker 心跳。只读。 | 多余参数返回 400。 |
| `jobs` | 本机或获授权服务 | 查看当前维护记录，无活动任务时为 `null`。只读。 | 多余参数返回 400。 |
| `status ID` | 本机或获授权服务 | 按 ID 查询持久作业状态。只读。 | ID 无效返回 400，未知 ID 返回 404。 |
| `backup ID PROFILE` | 本机或获授权服务 | 申请维护窗口，Master 批准后短暂停掉整组已配置游戏节点。使用相同参数和 ID 可重放。 | 未知 profile 返回 404，任务冲突返回 409，Master 或 worker 不可用返回 503。 |
| `metrics` | 本机或获授权服务 | 显示控制面指标。只读。 | 多余参数返回 400。 |
| `end` | 仅本机维护员 | 空闲时停止接单并正常退出 Master，已提交的 SQLite 状态已同步持久化。 | 存在活动或待修复的作业时返回 409，Master 继续运行。QQ 和 Proxy 永远返回 403。 |

## QQ Bot 适配

`integrations/mcqq/master_commands.py` 提供与现有 `console_command` 解析结果对接的 Python 异步适配器。集成到 MCQQBridge 时，把模块放入 `mcqq` 包，从现有命令分支优先对 `target == "master"` 调用 `dispatch_master(message, action, platform, name, owners, master_config)`。`owners` 取自运维配置的官方 QQ `member_openid` 主人列表，`master_config` 包含 QQ 专用 `token_file` 绝对路径与可选 `port`。对 `target == "master"` 必须直接返回适配器结果，防止落入游戏控制台指令队列。当前适配器交接尚未合入 MCQQBridge。

在 Master 的 JSON 中显式添加 `"qq_commands": {"token_file": "/etc/luo-master/qq.token", "actions": ["help", "jobs", "status"]}`。QQ 端示例命令为 `/大发命令 master help` 和 `/大发命令 master status <operation-id>`。需要 QQ 发起备份时，管理员可以显式加入 `backup` 动作，同样适用活动任务互斥、profile 白名单及幂等要求。

## 许可

自研源码采用 [Apache-2.0](LICENSE)。第三方依赖及公开架构参考见 [THIRD_PARTY.md](THIRD_PARTY.md)。
