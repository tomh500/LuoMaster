# Master 控制面接入契约 v1

默认管理地址为 `http://127.0.0.1:18776`。服务名为 `luo-master.service` 和 `luobackup-worker.service`，可执行文件分别为 `luo-master` 和 `luobackup-worker`。两者均使用 `--config /绝对路径/master.json` 并分别使用专用非 root 账户。worker 可以先启动并轮询，且两者都必须处于游戏服务以外的服务组。停机时先停止接收维护请求并等待状态结束，再停止 worker。配置、令牌、共享暂存目录及生命周期适配器由安装方按 [安装配置](README.md#安装配置) 提供。

| 方法和路径 | 约定 |
| --- | --- |
| `GET /healthz` | Master 进程存活时返回 200。 |
| `GET /readyz` | 启动检查和恢复完成后返回 200。需人工处理的恢复失败时返回 503。备份正常进行期间仍就绪。 |
| `GET /metrics` | Prometheus 文本格式，需要观察或备份令牌。 |
| `GET /v1/topology` | 配置拓扑、`worker_online` 及当前维护操作，需要观察或备份令牌。 |
| `POST /v1/backups` | 第一次请求，申请停服。请求体 `{"profile":"survival"}`。需备份令牌及 `Idempotency-Key`。接受或重放返回 202 和操作状态。相同标识用于不同配置或已有其他维护作业时返回 409。worker 离线时新申请返回 503。 |
| `GET /v1/backups/<id>` | 查询持久状态，需要观察或备份令牌。未知 ID 返回 404。 |
| `GET /v1/backup-worker/job` | 需要独立的 worker 令牌。停服确认后向 worker 返回任务和租约，空闲返回 204。 |
| `POST /v1/backups/<id>/resume` | 第二次请求，由独立 worker 持 worker 令牌提交 `{"lease":"…","result":"verified","sha256":"…"}`，归档失败提交 `{"lease":"…","result":"failed"}`。Master 只使用该任务的停服前快照恢复原拓扑。相同的回执可重放，错误租约、过期租约和不同回执返回 409。 |
| `POST /v1/commands` | QQ Bot 与 Proxy 的受限命令入口，请求体 `{"command":"status ID"}`。凭据分别绑定 `qq_commands.actions` 和 `proxy_actions`。无有效凭据返回 401，有效凭据的越权动作返回 403。 |

认证使用 `Authorization: Bearer <token>`。备份请求只选择服务端配置的白名单 profile，ID 允许 1 至 64 个 ASCII 字母、数字、`_` 和 `-`。状态以 `succeeded`、`failed` 或 `recovery_required` 结束。安装集成以 `GET /readyz` 及鉴权的 `/v1/topology` 的 `worker_online=true` 判定完整控制面就绪，避免使用固定睡眠。Master 不负责开机时拉起 Java 或 QQ 服务。正常关闭向 Master 发送 SIGTERM，停止接单并让进行中的维护作业收尾。若等待超时后被强制终止，使用同一个配置及持久状态目录重启以恢复拓扑。

生命周期适配器为管理员配置的绝对路径可执行文件，调用参数依次是 `<action> <node-id>`。`status` 返回 0 表示已就绪运行，3 表示确认退出，其他退出码表示未知或过渡状态。`save` 必须等保存确认，`stop` 须有序结束且等进程退出，`start` 须等服务可用。成功均返回 0，重复调用须安全。状态不明确时禁止归档。Master 按依赖逆序保存及停服，按正序恢复备份前运行的节点。`depends_on` 确定启停顺序，原先停着的节点保持停着，即使 Bungee 配置了该后端。受控拓扑必须覆盖所有会写 profile 选中内容的进程，且适配器在维护期间独占生命周期管理。外部数据库使用独立备份机制。

## 操作身份与命令

1. 下层服务通过回环 v1 API 按角色鉴权。观察令牌有只读拓扑、作业和指标权限。Proxy 提交者默认只获准 `backup` 和 `status`。归档 worker 只可领取与回执，其凭据无法提交初始请求。
2. 维护员可在服务器前台启动 `--console`，或由获准切换至服务账户的操作员使用 `--attach /run/luo-master/console.sock` 连接 Linux 私有控制台。socket 目录必须归服务账户所有且对组和其他账户无权限，TCP 服务无该控制台路由。`end` 仅出现在本机维护员权限集中。活动维护作业存在时 `end` 返回 409 并继续提供恢复通道。空闲时停止接单并退出 Master，Minecraft 进程继续运行。
3. QQ Bot 只响应官方回调中的 Bot 主人发起的 `/大发命令 master <命令>`。Bot 使用自己专用的凭据。Master 将其限制在与 Proxy 同级的独立动作白名单。`end` 永久排除在两者的服务动作之外。QQ 管理员、游戏绑定身份以及已有游戏指令队列均不能授予该入口权限。Master 记录角色、动作和授权结果，QQ 适配器记录事件对应 QQ 身份的摘要及返回状态。

三类入口使用封闭的动作模型。支持的控制台语法、权限与失败语义见 [维护命令](README.md#维护命令)。

## 统一经济接口预留

`/v1/economy/` 留给 Master 经济入口。公开调用仍经过 Master 鉴权。QQ Bot 和 BW 各自使用独立凭据，分别授予余额读取及限定业务的变更权限。经济凭据和备份、观察、归档 worker 凭据独立。调用方只为获准的业务提交账户标识、业务事件和幂等键。余额与账本由经济持久化组件在现有数据库中随同去重记录原子提交，调用方通过相同业务键重试。控制面备份用 SQLite 文件只记录维护状态。

拟为隔离的账本进程提供本机私有 Unix socket `/run/luo-economy/ledger.sock`。内部 `GET /healthz` 表示进程存活，`GET /readyz` 另验证数据库连通、账本结构版本和事务写入所需权限。Master 可通过受经济读取权限保护的 `GET /v1/economy/readyz` 汇报下游就绪状态。账本不可用时经济变更应返回 503，Master 维护接口仍按自身状态提供服务。以上路径是集成预留，原型尚未注册这些路由。
