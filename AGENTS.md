# obs-zcamera · AI agent 工作约定

OBS Studio 插件：SSP 网络相机源 + 零拷贝硬件解码（Windows D3D11VA / macOS VideoToolbox）
+ 相机控制 dock。**改动代码前先读完本文件**，它规定了提交格式和缺陷库用法。

## 缺陷库（每个改动都要落到 issue 上）

- 地址：`http://192.168.10.147:3300`（**必须用 http**，浏览器会自动升 https 导致打不开）
- 本仓库对应：`imvt/obs-zcamera` —— 就是仓库名本身，不要再拼路径
- 工具（MCP 服务 `gitea-bugtracker`）：`issue_read` / `issue_write` / `list_issues` / `search_issues`

### 提交信息必须带页脚

```
修复弱光下采集黑屏

根因：stride 未按 64 字节对齐。

Fixes: 12
```

| 页脚 | 含义 | 合入后自动变成 |
|---|---|---|
| `Bug: 12` | 只是关联，不改状态（直推 main 时用这个） | 状态不变 |
| `Fixes: 12` | 代码已修好，等验证 | `status/fixed`（待验证），**不关单** |
| `Verified: 12` | 验证通过 | `status/verified` + 关闭 |
| `Closes: 12` | 跳过验证直接关（慎用） | `status/verified` + 关闭 |

一次修多个就写多行，一行一个编号。编号是 `imvt/obs-zcamera` 里的 issue 号。

> `Fixes:` **不再直接关单**。修完只是"待验证"，关单要等 `Verified:` 或人工验证。

### 状态怎么走

Gitea 只有 open/closed 两个按钮，真正的阶段是一组**互斥**的 `status/*` 标签：

| 状态 | 含义 | 什么时候打 |
|---|---|---|
| `status/triage` | 待定性 | 新建 issue 的默认状态 |
| `status/confirmed` | 已确认，等排期 | 复现过、判定确实是缺陷 |
| `status/in-progress` | 修复中 | 开始动手改之前 |
| `status/fixed` | 待验证 | 代码已合入（`Fixes:` 自动打） |
| `status/verified` | 已验证 | 验证通过（`Verified:` 自动打） |
| `status/wontfix` | 不修复 | 判定不改 |

**一个 issue 同时只能有一个 `status/*`。** 通过 MCP 改标签时：

- `add_labels` 只追加不删除，**会搞出两个状态并存** —— 改状态不要用它
- `replace_labels` 是安全的，但必须传**完整**的标签 id 列表（原有的 P/type 标签别丢）
- 最省事：`python D:\services\gitea\bin\bugctl.py status imvt/obs-zcamera#<N> in-progress`

看状态分布：`bugctl.py statuses imvt/obs-zcamera`

### push 之后补一条评论

```
issue_write(method="comment", owner="imvt", repo="obs-zcamera", index=12,
            body="已推送 main，commit a1b2c3d4")
```

状态更新（`Fixes:` → 待验证 / `Verified:` → 关单）由服务端桥自动完成，**不要手动 close**，
也不要把状态直接改成 `status/verified` —— 那等于替验证人签字。改状态只改到 `in-progress` / `fixed` 这一档。

> **直推 `main` 时没有自动回写**：`bridge_gerrit_ssh.py` 只扫 Gerrit `refs/for` 的 change，
> 而 Gitea 侧的 `imvt/<repo>` 是空仓库（只存 issue，不存代码），页脚没人接。直推完自己补：
>
> ```bash
> python D:\services\gitea\bin\bugctl.py --repo imvt/<repo> status imvt/<repo>#N fixed \
>        -m "已推送 main，commit abc1234：<改了什么都写清楚>"
> ```
>
> `bugctl.py statuses` 的分页计数会少算（2026-10-19 实测 fixed 报 95、实际 116），要准数
> 就自己分页拉 `/api/v1/repos/imvt/<repo>/issues?state=all&type=issues&limit=50&page=N` 统计。

### 提新 issue

- 先查重：`list_issues(state="all", q="关键词")` / `search_issues`
- 标题：`<模块> <现象>（<触发条件>）`
- 标签必填三项：`P0-critical|P1-major|P2-minor` + `type/bug|type/regression|type/feature` + `agent:filed`
- 正文：复现步骤 / 期望与实际 / 环境（OBS 版本、相机型号、固件）/ 线索（日志、抓包）
- 一次提超过 5 条，先列清单等人工确认

## 构建

Windows（VS 生成器，RelWithDebInfo）：

```sh
cmake -S . -B build --preset windows-x64
cmake --build build --config RelWithDebInfo
```

macOS 用 Xcode 生成器，见 `CMakePresets.json` 里的 `macos` / `macos-ci`。

- 依赖：FFmpeg（avcodec/avutil/swscale/swresample）+ OBS 打过补丁的 Qt6；
  `CMakePresets.json` 与 `.deps/` 里已有既有配置，**别随手改 CMakeCache 里的 deps 路径**
- `build_x64/`、`build*/` 是本机历史构建目录，不进版本库
- WebSocket 通知用的是树内自带的 RFC 6455 客户端，**不需要** Qt WebSockets 模块

## 部署到本机 OBS 测试

```powershell
.\scripts\deploy-obs.ps1              # 默认 -Target dev，装到 %ProgramData%\obs-studio\plugins\
```

两种布局不通用（`-Target dev` 与 OBS 安装目录直装），部署后要重启 OBS 才看得到插件。

## 代码地图

| 路径 | 内容 |
|---|---|
| `src/zc_source.cpp` | 源注册、帧输出、属性面板 |
| `src/hwdecode/` | 零拷贝解码链路（D3D11VA / CUDA-NVDEC / VideoToolbox），改动影响最大 |
| `src/cameractrl/` | 相机控制：HTTP Digest、`/ctrl/*` 设置、WebSocket 81 通知 |
| `src/ui/` | dock 面板（相机栏、设置、PTZ、debug） |
| `src/ssp-mdns.cpp` `src/ssp-client-iso.cpp` | mDNS 发现、SSP 子进程管道 |
| `doc/progress.md` | **当前真实状态**（哪些链路已验证、哪些只是编译通过） |
| `doc/plan.md` | 路线图 |
| `doc/ui-spec.md` | 给客户的 UI 规格，改 dock 前对照 |

改硬件解码路径前先看 `doc/progress.md`：CUDA 路径目前**只在独立测试里验证过**，在 OBS 内会绿屏，
所以 UI 里没有暴露它——不要顺手“修好”就打开。

## 代码风格与提交卫生

- C/C++ 按仓库根的 `.clang-format`，不要引入新的风格
- 不提交：`build*/`、`.deps/`、`*.log`、`dist/` 产物、个人 OBS 配置
- 单个改动保持聚焦；buglist 修复请一个 issue 一个 commit，方便回溯

## 不要做

- 不要建仓库、不要把代码推到缺陷库（`imvt/*` 只存 issue，推代码会 403）
- 不要删 issue、不要改他人评论
- 不要自行把 issue 标成 `closed`
- 不要把 `doc/*.xlsx` 里的历史 buglist 当权威——权威是缺陷库
