# VPP + AF_XDP：Emby、nginx 与 Codex app-server 接入 / SSH Punt 方案

本文基于 2026-10-19 对 `s1255788` 的只读盘点编写。所有地址、接口名、容器名和端口均来自当前机器实际状态；落地前请在维护窗口内逐项复核。

## 0. 结论摘要

- 当前 WAN 是 `enp2s0f0`，公网地址 `184.107.84.201/26`，网关 `184.107.84.254`，Intel I350（`igb`）1GbE。
- `xdp-loader features enp2s0f0` 显示 `NETDEV_XDP_ACT_XSK_ZEROCOPY=no`，因此 AF_XDP 只能使用 **copy 模式**（`no-zero-copy`），不能使用零拷贝。
- 旧环境 80/443 由 **Caddy 2.11.4** 监听；新方案不迁移旧环境。新环境里的 nginx 是**另起的一套**（跑在 VPP host stack 上），与 Caddy 并存时用不同入口（lab 地址/端口），Phase 2 测试时才临时停 Caddy。
- Emby 保持现状：Docker 容器 `av-emby`（4.9.5.0），配置 `/root/emby/docker_app/amilys_embyserver/config`，媒体 `/root/emby/media`。新环境只把它当 **API 上游**（`127.0.0.1:8096`），不迁移、不换 systemd。
- 其他 Docker 服务（openlist、metatube、Prowlarr、Whisparr、av-subscriber）不在本次 Emby 迁移范围内；如果目标是彻底移除 Docker，需要单独逐项迁移。
- Codex app-server 当前使用 `--listen unix://`，只有 Unix socket。推荐让它只监听 `ws://127.0.0.1:8790`，本地通过 SSH 本地端口转发访问；这样 app-server 不需要单独做公网 punt，SSH 隧道本身就走在 punt 路径上。
- `vpp-web` TUN 只属于“保留内核 TCP”的路线。Phase 1/2 都是 **external app（nginx VCL）+ internal app（VPP media plugin）**：客户端到 nginx、nginx 到 internal app 都走 VPP host stack，不需要 `vpp-web` TUN。只有 nginx -> 真实 Emby 的 `127.0.0.1:8096` 那一段仍是内核 loopback。
- **当前优先 Phase 0：隔离 lab + 裸 HTTP internal app。** 独立 VPP 实例 + veth + `af_packet`，不碰现有 Caddy/Docker/Emby/`enp2s0f0`；AF_XDP 和 SSH punt 后置到 Phase 3。
- 推荐顺序：Phase 0 隔离 lab + 裸 HTTP 验证 -> Phase 1 本机 TUN 验证混合路由（nginx VCL + VPP internal app，纯 HTTP）-> Phase 2 同一套接到公网（TLS + `pornvpp.linqiankai.top` + 停 Caddy）-> Phase 3 AF_XDP + 强制 SSH punt。
- `pornvpp.linqiankai.top` 的 A 记录已生效（`184.107.84.201`），Let's Encrypt HTTP-01 证书已签发：
  `/etc/letsencrypt/live/pornvpp.linqiankai.top/{fullchain.pem,privkey.pem}`，ECDSA，有效期到 2027-01-17；Caddy 已恢复。
- Phase 0/1 的 lab 构建实测插件集：`af_packet,tap,http,http_static`（`./configure -P af_packet,tap,http,http_static -D none -S none -E none`），已包含 `vppctl` 和 VCL 库（`libvcl_ldpreload.so`）。Phase 3 再切 `af_xdp`，并补 `linux_cp/linux_nl/nat`。

## 1. 现状盘点

### 1.1 硬件与系统

| 项目 | 实际值 |
|---|---|
| 主机名 | `s1255788` |
| OS | Ubuntu 24.04.5 LTS (noble) |
| 内核 | `6.8.0-142-generic` |
| CPU | 2 × Intel Xeon E5-2690 v2，40 线程，2 NUMA 节点 |
| 内存 | 32 GB |
| Hugepages | 当前为 0；支持 2 MB 和 1 GB |
| 网络管理 | `systemd-networkd` + netplan，没有 NetworkManager |
| IPv4 转发 | `net.ipv4.ip_forward=1` |
| IPv6 转发 | `net.ipv6.conf.all.forwarding=0` |
| rp_filter | `2`（loose） |

### 1.2 网卡与 XDP 能力

```text
enp2s0f0  00:25:90:e0:2c:72  UP  1500  igb  8 RX/TX queues  NUMA 0
          184.107.84.201/26  default via 184.107.84.254
enp2s0f1  00:25:90:e0:2c:73  UP  9000  igb  8 RX/TX queues  NUMA 0
          仅 IPv6 link-local；DHCP 未取得 IPv4
```

- `enp2s0f0` 是当前公网 WAN，也是 AF_XDP 的目标接口。
- `enp2s0f1` 物理链路已 up，连接到交换机 `MTL-02-ROOM3-AF12-INT-SW01` 的 `ge-0/0/19`，但 netplan 的 DHCP 没有拿到 IPv4。它适合作为独立管理口/LAN 口，前提是确认交换机侧网段并能配置静态地址。
- `xdp-loader features enp2s0f0` 结果：

```text
NETDEV_XDP_ACT_BASIC:       yes
NETDEV_XDP_ACT_REDIRECT:    yes
NETDEV_XDP_ACT_XSK_ZEROCOPY: no
NETDEV_XDP_ACT_NDO_XMIT:    no
```

结论：可以使用 AF_XDP，但必须 `no-zero-copy`。I350 是 1GbE，实际吞吐上限约 1Gbps，且 copy 模式会额外占用 CPU。

### 1.3 地址与路由

```text
default via 184.107.84.254 dev enp2s0f0 proto static metric 100 onlink
184.107.84.192/26 dev enp2s0f0 proto kernel scope link src 184.107.84.201
172.17.0.0/16 dev docker0
172.18.0.0/16 dev br-80f099800b73
```

netplan 文件：`/etc/netplan/01-netcfg.yaml`

- `enp2s0f0`：静态 IPv4，默认路由，DNS `23.19.53.53`、`23.19.52.52`。
- `enp2s0f1`：`dhcp4: yes`，`mtu: 9000`，当前无 IPv4 租约。

### 1.4 当前监听端口（节选）

| 端口 | 进程 | 绑定 | 说明 |
|---|---|---|---|
| 22/tcp | `sshd` | `0.0.0.0` / `[::]` | 公网 SSH，当前开启密码和 root 登录 |
| 80, 443/tcp | `caddy` | `*` | 当前 Web 入口 |
| 8096/tcp | `docker-proxy` | `127.0.0.1` | Emby HTTP |
| 8920/tcp | `docker-proxy` | `127.0.0.1` | Emby HTTPS |
| 8080/tcp | `qbittorrent-nox` | `127.0.0.1` | qBittorrent WebUI |
| 30643/tcp+udp | `qbittorrent-nox` | `184.107.84.201` | BT 监听端口 |
| 111, 2049, 动态端口 | rpcbind/mountd/statd | `0.0.0.0` | NFS/RPC |
| 2019/tcp | `caddy` | `127.0.0.1` | Caddy admin API |

### 1.5 Docker 现状与去 Docker 范围

```text
av-stack_default  172.18.0.0/16  gateway 172.18.0.1
av-emby           172.18.0.3      amilys/embyserver:latest
openlist          172.18.0.2      openlistteam/openlist:latest
av-metatube       172.18.0.4      metatube-server
```

`av-emby` 关键属性（本次要移除的容器）：

```text
restart=unless-stopped
network=bridge，同时接入 av-stack_default 和 docker0
published: 127.0.0.1:8096->8096/tcp, 127.0.0.1:8920->8920/tcp
config: /root/emby/docker_app/amilys_embyserver/config
media:  /root/emby/media
entrypoint: /init (s6)，实际进程 /system/EmbyServer
version: 4.9.5.0
```

Emby 容器内监听 `8096`（HTTP）和 `8920`（HTTPS）；`system.xml` 中 `PublicPort=8096`、`PublicHttpsPort=8920`。容器还挂载 `/media`、`/media/AV`、`/downloads/incomplete`；这些是旧环境事实，新方案不迁移。

其他仍在 Docker 中的服务：`openlist`、`av-metatube`、`av-prowlarr`、`av-whisparr`、`av-subscriber`、`av-subscriber-scheduler`。本文只把 Emby 迁出 Docker；彻底移除 Docker 需要另做这些服务的 systemd/原生安装迁移。

### 1.6 Web 反代现状：是 Caddy，不是 nginx

- 已安装并运行：`caddy 2.11.4`，systemd 服务 `caddy.service`，配置 `/etc/caddy/Caddyfile`（212 行）。
- **未安装 nginx**，也没有 nginx systemd 服务或容器。
- Caddy 使用 Let's Encrypt 证书，证书目录：

```text
/var/lib/caddy/.local/share/caddy/certificates/acme-v02.api.letsencrypt.org-directory/<domain>/
```

当前站点及上游：

| 站点 | 上游 |
|---|---|
| `backup.linqiankai.top` | `127.0.0.1:8384` |
| `porn.linqiankai.top`, `cf-origin.linqiankai.top` | Emby 相关：`127.0.0.1:8096`、`127.0.0.1:5010`、`127.0.0.1:5244`，以及 `/var/lib/caddy` 下的 HLS 文件 |
| `prowlarr.linqiankai.top` | `127.0.0.1:9696`，basic_auth |
| `whisparr.linqiankai.top` | `127.0.0.1:6969`，basic_auth |
| `av.linqiankai.top` | `127.0.0.1:5010` |
| `oplist.linqiankai.top` | `127.0.0.1:5244` |
| `paseo.linqiankai.top` | `127.0.0.1:6767` |

Caddy 配置中包含 Emby token 和 basic_auth 口令哈希，属于敏感信息，迁移时不要复制到公开文档或 world-readable 文件。

### 1.7 VPP 构建现状

- 当前 release 构建目录：`build-vpp-release-afxdp`
- 安装树：`build-vpp-release-afxdp-install`
- 已构建：`vpp`、`vppctl`、`af_xdp_plugin.so`、`tap_plugin.so`
- **未构建 `nat_plugin.so`**；当前只有 `af_xdp` 和 `tap` 两个插件。
- VPP 当前没有运行，`/run/vpp` 和 `/opt/vpp` 不存在。

### 1.8 安全现状

- `ufw status` 为 `inactive`。
- iptables `INPUT` 策略为 `ACCEPT`，`FORWARD` 为 `DROP`；Docker 只对容器端口做了 localhost 限制。
- SSH 当前 `PermitRootLogin yes`、`PasswordAuthentication yes`，日志中已有大量公网爆破尝试。
- NFS/RPC、qBittorrent 30643 等端口在公网可达（是否被上游机房防火墙拦截需额外确认）。

### 1.9 Codex app-server 现状

```text
PID 1500035  codex -c features.code_mode_host=true app-server --listen unix://
PID 1500236  codex app-server proxy
control sock /root/.codex/app-server-control/app-server-control.sock
             -> /tmp/codex-daemon-0/<hash>
```

- 当前 app-server 只监听 Unix socket，没有 TCP/WebSocket 监听，所以**没有 IP 流量可以 punt**。
- `codex app-server --listen` 支持 `stdio://`、`unix://`、`ws://IP:PORT`、`off`。
- 若要以网络方式访问，需要显式使用 `ws://IP:PORT`，并配置 `--ws-auth capability-token`（配合 `--ws-token-file`）或 `--ws-auth signed-bearer-token`（配合 shared secret/issuer/audience）。
- Codex 的模型请求走 `http://127.0.0.1:15722/v1`，实际由本地 `cc-switch proxy serve --listen-address 127.0.0.1 --listen-port 15722` 代理转发；这部分是 localhost 流量，不经过 VPP。
- app-server 只支持 `ws://`（明文 WebSocket）。推荐只监听 `127.0.0.1:<port>`，本地通过 SSH 隧道访问；不建议直接把无 TLS、无认证的 app-server 暴露到 WAN。

## 2. 目标与约束

目标：

**这是一套新的隔离环境，不迁移、不改动现有 Caddy/Docker/Emby。**

1. **Phase 0**：隔离 lab 环境。用独立 VPP 实例 + veth/测试接口 + `af_packet`（或 Linux-CP）把链路跑通；不动 `enp2s0f0` 上的现有服务。
2. **Phase 1**：本机验证混合路由。external nginx（VCL/LDP）+ VPP internal media app，入口是 `tap` 插件的 TUN 接口 + **裸 HTTP**，在服务器本机用 curl 验证播放后端与路由。不动 WAN、不停 Caddy、不用证书。
3. **Phase 2**：把 Phase 1 的同一套 nginx 和路由表接到公网入口：VPP 接管 `enp2s0f0`，域名 `pornvpp.linqiankai.top`，TLS 在 VPP host stack 上终止。功能代码与 Phase 1 完全相同，只改入口。
4. **Phase 3**：`af_packet` 换成 AF_XDP；**SSH punt 必做**，不是可选。
5. TCP 使用 BBR + RACK/TLP：Phase 0 的 Linux 侧用内核参数；Phase 1 的 VPP TCP 用 `tcp { cc-algo bbr rack }`。
6. 从本地通过 SSH 隧道访问 Codex app-server。
7. 全程可回滚；新环境与旧环境隔离。

约束：

- I350 不支持 AF_XDP zero-copy，只能 copy 模式。
- 单公网 IP `184.107.84.201` 同时承载 80/443、SSH、BT 等，需要按端口分流。
- 现有 Caddy/Docker Emby 是旧环境，本方案不修改；新环境独立部署、独立 CLI socket、独立测试接口。
- app-server 当前是 Unix socket；推荐只改成 localhost `ws://` 并走 SSH 隧道，不建议把它的 `ws://` 直接暴露到 WAN。

## 3. 推荐方案

### 3.0 Phase 0：隔离 lab，不碰现有环境

目标：**独立 VPP 实例 + veth + `af_packet`**，先验证 VPP internal HTTP app 的裸 HTTP。现有 Caddy、Docker Emby、`enp2s0f0` 全部不动。

```text
test netns vpp-client
  10.10.0.2/24 (veth vpp-lab)
        |
  veth peer vpp-lab-peer
        |
  VPP af_packet host-vpp-lab-peer 10.10.0.1/24
        |
  VPP internal HTTP app tcp://10.10.0.1/8080
```

构建独立 lab：

```bash
./configure -b build-vpp-lab -t release \
  -i "$PWD/build-vpp-lab-install" \
  -P af_packet,tap,http,http_static -D none -S none -E none
```

创建隔离链路：

```bash
ip netns add vpp-client
ip link add vpp-lab type veth peer name vpp-lab-peer
ip link set vpp-lab netns vpp-client
ip link set vpp-lab-peer up
ip netns exec vpp-client ip link set lo up
ip netns exec vpp-client ip link set vpp-lab up
ip netns exec vpp-client ip addr add 10.10.0.2/24 dev vpp-lab
```

VPP 侧：

```text
create host-interface name vpp-lab-peer
set interface state host-vpp-lab-peer up
set interface ip address host-vpp-lab-peer 10.10.0.1/24
```

Phase 0 验证：

```bash
ip netns exec vpp-client ping -c3 10.10.0.1
ip netns exec vpp-client curl -v http://10.10.0.1:8080/health
```

注意：Phase 0 **不使用 Linux-CP、不修改 enp2s0f0、不启停 Caddy/Docker/Emby**。Linux-CP 只作为后续在独立测试接口上的可选项，不碰现有 WAN。

### 3.1 Phase 1：本机 TUN 验证混合路由（API -> Emby，播放 -> VPP）

Phase 1 只回答一个问题：**混合路由和 VPP 播放能不能工作，并且播放字节确实来自 VPP**。真实 Emby 只提供 API/元数据/鉴权，视频由 VPP internal media app 直接读文件返回。

Phase 1 的边界（与 Phase 2 的唯一区别就在这里）：

- 入口是 VPP 的 **`tap` 插件 TUN 接口**（`10.10.0.1`）+ **纯 HTTP**，只在本机测试，不开 TLS、不用域名。
- **不碰 `enp2s0f0`**，WAN 地址仍由内核持有。
- **Caddy 保持运行**，Docker Emby 保持不动，两个环境互不干扰。
- 功能代码（nginx 混合路由 + VPP internal media app）在 Phase 2 原样复用，一行不改。

```text
本机 curl（源 10.10.0.2）
   |  vpp-tun（内核 TUN 设备）
   v
tap0 (VPP, TUN 模式, 10.10.0.1/24)
   |
nginx (external VCL, 纯 HTTP, 10.10.0.1:80)
   |-- API  --> 127.0.0.1:8096   (真实 Emby，内核 loopback)
   `-- 播放 --> 10.255.0.1:8080  (VPP internal media app)
```

为什么 Phase 1 也要 nginx：Emby 客户端必须先登录、拿到 `PlaybackInfo` 才能点到播放 URL。没有 nginx 做混合路由，客户端连 API 都到不了真实 Emby。所以 Phase 1 和 Phase 2 的差别只在**入口**（TUN/HTTP vs WAN/HTTPS），不在路由和播放实现。

路由规则：

| 路径 | 目标 | 说明 |
|---|---|---|
| `/System/*`、`/Users/*`、`/Items/*`（非播放） | `127.0.0.1:8096` | 真实 Emby API |
| `/emby/System/*`、`/emby/Users/*`、`/emby/Items/*`（非播放） | `127.0.0.1:8096` | Emby base path API |
| `/(emby/)?Videos/*/stream`、`/(emby/)?Videos/*/original` | `10.255.0.1:8080` | VPP 播放 |
| `/(emby/)?Items/*/Download`、`/(emby/)?Audio/*/stream` | `10.255.0.1:8080` | VPP 播放 |
| `/hls/*`、`/(emby/)?Videos/*/master.m3u8` | `10.255.0.1:8080` | VPP HLS |
| 其他 | `127.0.0.1:8096` | 默认回 API |

**关键点**：播放 URL 必须命中 VPP，而不是被 nginx 默认反代到 Emby。Emby 的 `PlaybackInfo` 返回 `DirectStreamUrl` 时通常就是 `/Videos/{id}/stream`，nginx 按上面的规则切到 VPP；如果 Emby 返回的是转码 URL，MVP 不处理，先用 direct-play 兼容的媒体测试。

**itemId -> 文件路径**：VPP internal app 不能阻塞 worker 去查 Emby API。用外部 scanner 定期从 Emby API/数据库生成 `index.json`（itemId、path、size、mime），VPP app 启动加载并在 SIGHUP/CLI 时刷新；播放时只查内存索引并 `pread`/分块发送。

nginx VCL 关键配置：

```nginx
upstream emby_api  { server 127.0.0.1:8096; keepalive 32; }
upstream vpp_media { server 10.255.0.1:8080; keepalive 64; }

server {
    # Phase 1：只监听 TUN 地址，纯 HTTP
    listen 10.10.0.1:80;

    # Phase 2 换成公网 HTTPS 入口，其余配置不动：
    # listen 184.107.84.201:443 ssl http2;
    # server_name pornvpp.linqiankai.top;
    # ssl_certificate     /etc/letsencrypt/live/pornvpp.linqiankai.top/fullchain.pem;
    # ssl_certificate_key /etc/letsencrypt/live/pornvpp.linqiankai.top/privkey.pem;

    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header Range $http_range;
    proxy_set_header If-Range $http_if_range;
    proxy_buffering off;
    proxy_request_buffering off;

    # 播放：必须走 VPP
    location ~ ^/(emby/)?Videos/[^/]+/(stream|original) { proxy_pass http://vpp_media; }
    location ~ ^/(emby/)?Items/[^/]+/Download       { proxy_pass http://vpp_media; }
    location ~ ^/(emby/)?Audio/[^/]+/stream         { proxy_pass http://vpp_media; }
    location ~ ^/(emby/)?Videos/[^/]+/master\.m3u8  { proxy_pass http://vpp_media; }
    location /hls/                                  { proxy_pass http://vpp_media; }

    # API：真实 Emby
    location / { proxy_pass http://emby_api; }
}
```

VCL 配置 `/etc/vpp/vcl.conf`（Phase 1/2 通用）：

```text
vcl {
  rx-fifo-size 4000000
  tx-fifo-size 4000000
  app-socket-api /run/vpp-lab/app_ns_sockets/default
  use-mq-eventfd
}
```

启动 nginx（必须带 VCL preload，否则会退回内核 TCP 并连不上 `10.255.0.1`）：

```bash
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libvcl_ldpreload.so \
VCL_CONFIG=/etc/vpp/vcl.conf \
nginx -g 'daemon off;'
```

#### 3.1.1 播放后端：VPP internal media app

目标：独立 VPP 实例 + `tap` 插件 TUN 接口，VPP internal app 直接读 `/srv/media` 并提供 Range/HLS；现有 Caddy/Docker/Emby/`enp2s0f0` 全部不动。

#### 构建

```bash
./configure -b build-vpp-lab -t release \
  -i "$PWD/build-vpp-lab-install" \
  -P af_packet,tap,http,http_static -D none -S none -E none
```

#### Hugepages

```bash
sysctl -w vm.nr_hugepages=2048
```

```bash
mkdir -p /var/log/vpp-lab /run/vpp-lab
```

#### startup.conf

```text
unix {
  nodaemon
  cli-listen /run/vpp-lab/cli.sock
  log /var/log/vpp-lab/vpp.log
  full-coredump
  coredump-size unlimited
  runtime-dir /run/vpp-lab
}

api-segment {
  prefix vpp-lab
}

socksvr {
  socket-name /run/vpp-lab/api.sock
}

statseg {
  socket-name /run/vpp-lab/stats.sock
  per-node-counters on
  update-interval 5.0
}

cpu {
  main-core 1
  corelist-workers 2
  # 可选，只有在 CPU 隔离/实时调度准备好时再开：
  # scheduler-policy fifo
  # scheduler-priority 1
}

memory {
  main-heap-size 2G
  main-heap-page-size 2M
  default-hugepage-size 2M
}

buffers {
  buffers-per-numa 32768
  default data-size 2048
  page-size 2M
  layout packed
}

session {
  enable
  preallocated-sessions 64
  v4-session-table-buckets 64
  v4-halfopen-table-buckets 64
  wrk-mq-length 4096
}

tcp {
  cc-algo bbr
  rack
  byte-tracker
  mtu 1500
}

plugins {
  plugin af_packet_plugin.so { enable }
  plugin http_plugin.so { enable }
  plugin http_static_plugin.so { enable }
}
```

**不要写顶层 `plugin_path`**。Phase 0 实测：release + LTO 构建下，顶层 `plugin_path` 会在 `vlib_plugin_early_init()` 里以约 50% 概率启动段错误。VPP 会按自身二进制位置自动推导 `<prefix>/lib/x86_64-linux-gnu/vpp_plugins`，`build-vpp-lab-install/bin/vpp` 直接就能找到插件。如果确实要指定，用 `plugins { path <dir> }` 子选项，不要用顶层 `plugin_path`。

要点：

- `main-core 1` + `corelist-workers 2`：**一个 main + 一个 worker**，都在 NUMA 0（与 `enp2s0f0` 同 NUMA）；避开 CPU 0 给内核/中断。
- 当前版本没有 `session evt-threads` 配置项；session 事件处理跟随 VPP worker，单 worker 就只有一个处理线程。`preallocated-sessions`/table buckets 先给小值，后续按连接数调。
- `memory main-heap-size 2G` 需要至少 2G 大页；`vm.nr_hugepages=2048`（2M × 2048 = 4G）足够 main heap + buffers。
- `api-segment prefix vpp-lab` 和独立的 `cli/api/stats` socket 让 lab 实例与其它 VPP 隔离。
- `tcp { cc-algo bbr rack byte-tracker }` 只对新连接生效；`rack` 启用 RACK/TLP，3-dup-ack 不再是恢复触发条件。
- 启动后先看：

```text
show threads
show cpu
show memory
show buffers
show tcp config
show session
```

`vppctl` 连接 lab CLI：

```bash
vppctl -s /run/vpp-lab/cli.sock
```

#### 本地 TUN 链路（`tap` 插件）

用 `tap` 插件的 TUN 模式，**不要**用旧的内核 tuntap 路径（`src/vnet/unix/tuntap.c`、`ip tuntap add` 或旧 `tapcli`）。这里用的是 tapv2 API 的 `TUN` 标志，L3 点对点，没有 MAC/ARP。

VPP 侧：

```text
create tap id 0 if-name tap0 host-if-name vpp-tun tun host-mtu-size 1500
set interface state tap0 up
set interface ip address tap0 10.10.0.1/24

# nginx 与 internal media app 之间的内部地址，纯 VPP，不经内核
create loopback interface instance 0
set interface state loopback0 up
set interface ip address loopback0 10.255.0.1/32
```

内核侧（`vpp-tun` 由 VPP 创建，不要手工 `ip tuntap add`）：

```bash
ip link set vpp-tun up
ip addr add 10.10.0.2/24 dev vpp-tun
ip route get 10.10.0.1        # 应显示 dev vpp-tun
```

地址分工：

| 地址 | 谁在用 | 用途 |
|---|---|---|
| `10.10.0.1:80` | nginx（监听） | 本机测试入口，经 `vpp-tun` 进来 |
| `10.10.0.2` | 内核 `vpp-tun` | 本机测试源地址 |
| `10.255.0.1:8080` | internal media app（绑 `0.0.0.0:8080`） | nginx 的播放 upstream，纯 VPP 内部 |

#### 启用 internal HTTP app

`http_static` 有可用的 CLI（Phase 0 已验证），先用它把链路跑通：

```text
http static server www-root /srv/media cache-size 64m max-age 0 keepalive-timeout 60
http static listener add uri tcp://0.0.0.0/8080 www-root /srv/media cache-size 64m
```

等价的 API 方式是 `http_static_enable_v5`：

```python
from vpp_papi import VPPApiClient

vpp = VPPApiClient()
vpp.connect("media-bootstrap")
vpp.http_static_enable_v5(
    fifo_size=4 << 20,
    cache_size_limit=64 << 20,
    max_age=0,
    keepalive_timeout=60,
    max_body_size=8192,
    rx_buff_thresh=1 << 20,
    www_root="/srv/media",
    uri="tcp://0.0.0.0/8080",
)
```

**注意**：Phase 0 实测 `http_static` **不实现 `Range`** —— 带 `Range: bytes=0-1048575` 的请求仍返回 `200` + 整个文件。Phase 1 必须换成下面的 `media_server` 才有 `206`，否则 Emby 无法拖动进度。

#### Handler 与实现

MVP 直接扩展 `src/plugins/http_static/`，新增 `media_server.c`：

- `GET /health`：返回 `{"status":"ok"}`。
- `GET /media/<path>`：映射到 `/srv/media/<path>`，严格拒绝 `..`、绝对路径、符号链接逃逸。
- `GET /hls/<id>/<file>`：返回 `.m3u8`/`.m4s`/`.ts`。
- `Range: bytes=start-end`：返回 `206`、`Content-Range`、`Accept-Ranges: bytes`；无 Range 返回 `200`。
- 分块读取文件并通过 `hss_session_send_data`/`hss_session_send_fn` 发送；不要把整部电影读进内存。
- `.m3u8`/`.m4s`/`.ts`/`.mp4` 设置正确 Content-Type。
- 不要在 VPP worker 里做 ffmpeg/扫描；HLS 由外部 systemd 任务预生成到 `/srv/media/hls/`。

#### 验证

全部在服务器本机执行，流量走 `vpp-tun` -> VPP：

```bash
# 1. 直接打 internal media app（验证播放后端）
curl -sv http://10.10.0.1:8080/health
curl -sv -r 0-1023 http://10.10.0.1:8080/media/test.mp4 -o /tmp/part
curl -sv http://10.10.0.1:8080/hls/test/index.m3u8

# 2. 经过 nginx（验证混合路由）
curl -sv http://10.10.0.1/System/Info/Public
curl -sv -r 0-1023 http://10.10.0.1/media/test.mp4 -o /tmp/part2

# 3. 内核不应有 80/8080 的监听（TCP 由 VPP 终止）
ss -ltnp | grep -E ':(80|8080) ' || echo 'no kernel listener'

# 4. 确认流量确实进 VPP
ip -s link show vpp-tun      # RX/TX 计数应增长
```

`ping` 在这一版 VPP 里不通是正常的：ICMP echo 会被 `ip4-icmp-input` 送到 `ip4-punt`，而 Phase 1 没有 punt 回内核的通道。用 TCP 请求判断连通性即可。

Phase 1 **不做远程客户端测试**：入口不是 `enp2s0f0`，用 SSH 隧道把 lab 地址映射到本地只能证明 app 逻辑，证明不了真实入口。真实 Emby 客户端放到 Phase 2，直接连 `https://pornvpp.linqiankai.top`。

如果 Phase 1 阶段就想用真实客户端验证一次 app 行为，可以临时 `ssh -L 8080:10.10.0.1:80 root@184.107.84.201`，但这只是方便，不作为验收依据。

VPP 侧：

```text
show interface
show tcp config
show session
```

#### 回滚

```bash
# 在 VPP 内删除 tap，内核侧的 vpp-tun 会随之消失
vppctl -s /run/vpp-lab/cli.sock delete tap tap0
# 停掉独立 VPP lab 实例
sysctl -w vm.nr_hugepages=0
```

Phase 1 全程没有启停 Caddy、没有修改 `enp2s0f0`、没有动 Docker Emby；回滚只需要清掉上面这些 lab 资源。

### 3.2 Phase 2：把同一套路由接到公网入口（TLS + 域名）

Phase 2 **不改任何功能代码**，只把 Phase 1 的入口从 lab 换成公网。两者对照：

| 项目 | Phase 1 | Phase 2 |
|---|---|---|
| 监听 | `10.10.0.1:80`，纯 HTTP | `184.107.84.201:443`，TLS |
| 域名 | 无，本机直连 `10.10.0.1` | `pornvpp.linqiankai.top` |
| WAN 网卡 | 内核持有，不动 | VPP `af_packet` 绑 `enp2s0f0`，内核地址 flush |
| Caddy | 保持运行，互不干扰 | 临时停止（同一 IP:443 冲突） |
| nginx 配置 | 见 3.1 | 只改 `listen` / `server_name` / 证书三处 |
| 路由表、internal app | 3.1 的实现 | **完全相同** |

Phase 2 要回答的是：VPP 能不能在公网侧完成 TCP 握手、TLS 终止和 HTTP 服务，并在真实 Emby 客户端上完成播放。

补充说明：

- nginx 用 VCL/LDP 运行在 VPP host stack 上，客户端 TCP 由 VPP 终止；这一点 Phase 1 和 Phase 2 一样。
- 不是迁移旧 Caddy：Caddy 的配置、旧域名都不动，Phase 2 只是临时占用 80/443 做测试，测完恢复。

**网络前提（临时测试模式）**：这次只是试验，不要求 Caddy 和 VPP 并存。测试时直接停 Caddy，VPP/nginx 使用 `184.107.84.201:443`；测试结束后停 VPP/nginx，按需恢复 Caddy。同一 IP:443 不能同时由内核 Caddy 和 VPP nginx 接收 WAN 流量，所以不要期望两边同时工作。

**证书（已签发）**：

- DNS `A pornvpp -> 184.107.84.201` 已生效。
- 本次实际用 **Let's Encrypt HTTP-01**：临时停止 Caddy，`certbot certonly --standalone --preferred-challenges http -d pornvpp.linqiankai.top`；**不需要 TXT**。
- 证书：ECDSA，有效期到 2027-01-17；certbot timer 已启用。
- 路径：

```text
/etc/letsencrypt/live/pornvpp.linqiankai.top/fullchain.pem
/etc/letsencrypt/live/pornvpp.linqiankai.top/privkey.pem
```

- Caddy 已恢复并重新监听 80/443。
- DNS-01 只在 80 端口不可用时才需要；那时才需要 `_acme-challenge` TXT。
- 测试 VPP/nginx 前停止 Caddy 释放 443；测试结束后停 VPP/nginx、恢复 Caddy。

#### Phase 2 执行步骤

1. 停 Caddy，释放 80/443：

```bash
systemctl stop caddy
ss -ltnp | grep -E ':80 |:443 ' || true
```

2. 记录网关 MAC，然后让 VPP 接管 WAN IP：

```bash
ip neigh show 184.107.84.254
ip addr flush dev enp2s0f0
ip link set enp2s0f0 up
```

3. 启动 VPP lab（`af_packet` 绑 `enp2s0f0`）：

```text
create host-interface name enp2s0f0
set interface state host-enp2s0f0 up
set interface ip address host-enp2s0f0 184.107.84.201/26
ip route add 0.0.0.0/0 via 184.107.84.254 host-enp2s0f0
# ARP 不解析时，用第 2 步的 MAC 加静态邻居：
# ip neighbor add 184.107.84.254 <gateway-mac> host-enp2s0f0
```

4. 给 nginx 和 internal app 一个 VPP 内部地址：

```text
create loopback interface instance 0
set interface state loopback0 up
set interface ip address loopback0 10.255.0.1/32
```

5. internal media app 的监听地址与 Phase 1 完全相同（`tcp://0.0.0.0/8080`），不需要改：

```text
http static server www-root /srv/media cache-size 64m max-age 0 keepalive-timeout 60
http static listener add uri tcp://0.0.0.0/8080 www-root /srv/media cache-size 64m
```

Phase 2 唯一要改的是 nginx 的 `listen`/`server_name`/证书。

6. 启动 nginx VCL：

```bash
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libvcl_ldpreload.so \
VCL_CONFIG=/etc/vpp/vcl.conf \
nginx -g 'daemon off;'
```

`/etc/vpp/vcl.conf` 与 Phase 1 完全相同（见 3.1）。

7. 验证（从外部客户端，或从有公网路由的机器）：

```bash
curl -vk https://pornvpp.linqiankai.top/System/Info/Public
curl -vk https://pornvpp.linqiankai.top/health
curl -vk -r 0-1023 https://pornvpp.linqiankai.top/media/test.mp4 -o /tmp/part
openssl s_client -connect 184.107.84.201:443 -servername pornvpp.linqiankai.top </dev/null 2>/dev/null | openssl x509 -noout -subject -dates
```

Emby 客户端验证：

- 在 Emby 客户端添加服务器 `https://pornvpp.linqiankai.top`，登录、浏览媒体库、播放一个 direct-play 兼容的视频。
- 播放时真实 Emby 只应收到 API 请求；`/Videos/*/stream`、`/Items/*/Download` 不应出现在 Emby 的访问日志里，这些请求由 VPP internal app 处理。
- 服务端 `show session` 应能看到 playback TCP 会话；`ss -tnp | grep 8096` 只应看到 nginx -> Emby 的 API 连接，不应有视频流连接。
- 如果播放失败，先确认客户端走的是 direct play，且 `PlaybackInfo` 返回的 URL 命中 Phase 1 的播放路由。

8. 测试结束，恢复旧环境：

```bash
# 停 nginx / VPP lab
ip addr add 184.107.84.201/26 dev enp2s0f0
ip route add default via 184.107.84.254 dev enp2s0f0 onlink
systemctl start caddy
```

注意：`af_packet` 绑物理口是临时测试；如果收包/ARP 行为不稳定，直接提前切 AF_XDP，并按 Phase 3 强制启用 SSH punt。

nginx VCL 配置骨架：

> 与 3.1 的 Phase 1 配置相比，**只有 `listen` / `server_name` / 证书三处不同**，路由表原样保留。

```nginx
upstream emby_api  { server 127.0.0.1:8096; keepalive 32; }
upstream vpp_media { server 10.255.0.1:8080; keepalive 64; }

server {
    server_name pornvpp.linqiankai.top;

    listen 184.107.84.201:443 ssl http2;
    ssl_certificate     /etc/letsencrypt/live/pornvpp.linqiankai.top/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/pornvpp.linqiankai.top/privkey.pem;

    client_max_body_size 0;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header Range $http_range;
    proxy_set_header If-Range $http_if_range;

    # 播放：必须走 VPP
    location ~ ^/(emby/)?Videos/[^/]+/(stream|original) { proxy_pass http://vpp_media; }
    location ~ ^/(emby/)?Items/[^/]+/Download       { proxy_pass http://vpp_media; }
    location ~ ^/(emby/)?Audio/[^/]+/stream         { proxy_pass http://vpp_media; }
    location ~ ^/(emby/)?Videos/[^/]+/master\.m3u8  { proxy_pass http://vpp_media; }
    location /hls/                                  { proxy_pass http://vpp_media; }

    # API：真实 Emby
    location / { proxy_pass http://emby_api; }
}
```

启动方式：

```bash
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libvcl_ldpreload.so \
VCL_CONFIG=/etc/vpp/vcl.conf \
nginx -g 'daemon off;'
```

### 3.3 Phase 3：AF_XDP + SSH punt（强制）

Phase 2 稳定后再做：

- 把 `af_packet` 替换为 AF_XDP（I350 只能 copy 模式）。
- **SSH punt 是 AF_XDP 替换后的必选项，不是可选项。** AF_XDP 接管 NIC 后，内核不再直接收 WAN 包，必须把 SSH 流量显式送到内核：`vpp-punt` + `set punt tcp all` + `ip punt redirect add rx wan0 via 10.30.0.2 vpp-punt`。
- Linux-CP 可以继续负责控制面和部分 punt 流量，但 SSH 的 punt 规则必须明确配置并验证；不能依赖“LCP 可能覆盖”。
- 如果不用 LCP，按第 6 节的 `vpp-punt`/NAT 方案切换（`vpp-web` 只保留作内核 TCP 对照，不采用）。

## 4. 地址与端口规划

> 本节及第 5、6 节是 **Phase 2/3 的参考设计**（公网入口 + AF_XDP + SSH punt）。Phase 0 用 veth + `af_packet`，Phase 1 用 `tap` 插件的 TUN 接口，都是纯 HTTP，不需要 `vpp-web`/`vpp-punt` TUN。

| 用途 | VPP 侧 | Host/容器侧 | 说明 |
|---|---|---|---|
| WAN | `wan0 184.107.84.201/26` | 无 | AF_XDP copy mode |
| Web（路线 A，不采用） | `vpp-web 10.10.0.1/30` | `tun-web 10.10.0.2/30` | nginx 走内核 TCP，仅作对照 |
| Web（路线 B，采用） | 无 TUN | nginx VCL 监听 WAN IP | VPP host stack TCP |
| SSH punt | `vpp-punt 10.30.0.1/30` | `tun-punt 10.30.0.2/30` + `184.107.84.201/32` | 内核 sshd |
| app-server | 不需要 | `127.0.0.1:8790` | 本地 SSH 隧道访问 |
| Emby（旧环境，不动） | 无 | Docker `av-emby` 监听 `127.0.0.1:8096/8920` | 只做 nginx 的 API 上游 |
| 其他 Docker | 无 | `172.18.0.0/16` | 不在本次 Emby 迁移范围 |

公网映射建议：

| 公网端口 | 目标 | 备注 |
|---|---|---|
| 80/tcp | nginx（VPP host stack） | Phase 2 测试 |
| 443/tcp | nginx（VPP host stack） | Phase 2 测试，TLS 在 VPP 终止 |
| 22/tcp | punt -> tun-punt | 内核 sshd |
| app-server 端口 | 不开放 | 只监听 localhost，走 SSH 隧道 |
| 30643/tcp+udp | 10.10.0.2:30643 | 如仍需 BT |
| 8096/8920 | 不直接映射 | 由 nginx 反代 |

## 5. VPP 构建与启动

### 5.1 重新构建（加入 NAT）

当前只有 `af_xdp` 和 `tap`，需要至少加入 `nat`：

```bash
./configure -b build-vpp-release-afxdp -t release \
  -i "$PWD/build-vpp-release-afxdp-install" \
  -P af_xdp,tap,nat -D none -S none -E none
```

如要做 VPP 内精确 punt（只匹配 TCP/22），再加 `ip_session_redirect`：

```text
-P af_xdp,tap,nat,ip_session_redirect
```

构建后重新执行符号拆分：

```bash
ninja -C build-vpp-release-afxdp install
src/scripts/split-debug-symbols.sh build-vpp-release-afxdp-install
```

### 5.2 Hugepages

当前 hugepages 为 0。建议先分配 4 GB（2 MB × 2048）：

```bash
sysctl -w vm.nr_hugepages=2048
```

如需持久化，写入 `/etc/sysctl.d/80-vpp.conf`。Emby 转码和剩余容器也要内存，不要一次分配过多。

### 5.3 startup.conf 草案

Phase 1/2 的完整配置见 3.1：**一个 main + 一个 worker**（`main-core 1`、`corelist-workers 2`）、独立 API/stat/runtime、2G main heap、32768 buffers、session 预分配参数、`tcp cc-algo bbr rack byte-tracker`。Phase 3 只把 dataplane 插件换成 AF_XDP：

```text
plugins {
  plugin af_xdp_plugin.so { enable }
  plugin tap_plugin.so { enable }
  plugin nat_plugin.so { enable }
}
```

`enp2s0f0` 和 CPU 1/2 都在 NUMA 0；单 worker 足够这次功能验证，不要先上多 worker。

### 5.4 vppctl 配置草案

```text
# WAN
create interface af_xdp host-if enp2s0f0 name wan0 num-rx-queues 4 no-zero-copy mac-reuse
set interface state wan0 up
set interface ip address wan0 184.107.84.201/26
ip route add 0.0.0.0/0 via 184.107.84.254 wan0

# Web TUN：nginx 走这里
create tap id 1 tun if-name vpp-web host-if-name tun-web host-ip4-addr 10.10.0.2/30 host-ip4-gw 10.10.0.1
set interface state vpp-web up
set interface ip address vpp-web 10.10.0.1/30

# SSH punt TUN：不要设置 host-ip4-gw，默认路由由策略路由控制
create tap id 2 tun if-name vpp-punt host-if-name tun-punt host-ip4-addr 10.30.0.2/30
set interface state vpp-punt up
set interface ip address vpp-punt 10.30.0.1/30

# NAT：只对 Web/主机出站流量做 NAT；SSH 不走 NAT
nat44 plugin enable sessions 200000
nat44 add interface address wan0
set interface nat44 in vpp-web out wan0
nat44 add static mapping tcp local 10.10.0.2 80 external 184.107.84.201 80
nat44 add static mapping tcp local 10.10.0.2 443 external 184.107.84.201 443
# 如仍需 qBittorrent 公网端口：
# nat44 add static mapping tcp local 10.10.0.2 30643 external 184.107.84.201 30643
# nat44 add static mapping udp local 10.10.0.2 30643 external 184.107.84.201 30643

# SSH punt：VPP 当前不支持只按 TCP 22 punt，先 punt 所有未知 TCP
set punt ipv4 tcp all
ip punt redirect add rx wan0 via 10.30.0.2 vpp-punt

# Codex app-server 推荐只监听 127.0.0.1:8790，通过 SSH 隧道访问，
# 不进入 VPP punt；SSH 本身已由上面的规则 punt。
```

说明：`set punt ipv4 tcp all` 会把所有“VPP 本地没有监听的 TCP”送到内核。nginx 的 80/443 是转发到 TUN，不是 VPP 本地监听；进入内核的主要是 SSH 和其他扫描流量，后续用 nftables 只放行 22。

若必须在 VPP 内只匹配 TCP/22，使用 `ip_session_redirect` 插件（实验性）：

```text
set punt ipv4 tcp all
classify table miss-next drop mask l3 ip4 l4 tcp dst_port buckets 1024
set interface input acl intfc wan0 ip4-punt-table 0
ip session redirect punt table 0 match l3 ip4 dst 184.107.84.201 l4 tcp dst_port 22 via 10.30.0.2 vpp-punt
```

该语法需按实际插件版本验证，不建议作为第一版。

## 6. Linux 侧配置

### 6.1 netplan 调整

当前 `/etc/netplan/01-netcfg.yaml` 给 `enp2s0f0` 配置了静态 IP 和默认路由。VPP 接管后，必须移除这部分，否则 systemd-networkd 会把地址/路由抢回去。

建议保留 `enp2s0f1` 作为管理/LAN 口；如果确认交换机网段，可给它配置静态地址：

```yaml
enp2s0f1:
  dhcp4: no
  mtu: 9000
  addresses: ['<管理网段地址>/<prefix>']
```

在维护窗口内执行 `netplan apply`。如果 `enp2s0f1` 暂时没有可用网段，至少要准备 IPMI/串口/机房远程手，避免 VPP 异常时失去 SSH。

### 6.2 TUN 与策略路由

`vpp-web` 由 tap 插件设置 `10.10.0.2/30` 和默认路由 `via 10.10.0.1`，因此主机、systemd Emby 和剩余 Docker 容器的出站流量默认走 VPP NAT。

`vpp-punt` 不设置默认路由。SSH 回包源地址是 `184.107.84.201`，用策略路由把它送回 `tun-punt`：

```bash
ip addr add 184.107.84.201/32 dev tun-punt
ip link set tun-punt up
ip rule add from 184.107.84.201/32 table 200 priority 1000
ip route add default via 10.30.0.1 dev tun-punt table 200
```

SSH 隧道场景不需要为 app-server 增加策略路由；只有把 app-server 直接监听到 WAN 时才需要为它的源地址增加 `ip rule`/路由表。

验证：

```bash
ip route get 8.8.8.8
ip route get 8.8.8.8 from 184.107.84.201
```

预期：普通出站走 `tun-web`；源地址为公网 IP 的 SSH 回包走 `tun-punt`。

### 6.3 Emby systemd 与剩余 Docker

- Emby 使用 systemd，监听 `127.0.0.1:8096/8920`；nginx 继续反代到这两个端口。
- Emby 不再经过 Docker bridge；它和其他主机进程一样，出站走主默认路由（`tun-web`），由 VPP NAT44 SNAT 到公网 IP。
- 其他仍在 Docker 中的服务（openlist、metatube、Prowlarr、Whisparr、av-subscriber）仍使用 Docker MASQUERADE；它们会使用主默认路由（`tun-web`），VPP NAT44 再 SNAT。
- 如果 Docker 出站异常，检查 `ip route get <目标> from 172.18.0.3` 和 Docker 的 MASQUERADE 规则；如果后续彻底去 Docker，这一段可以删除。

Phase 2 直连 Emby 时：

```text
create tap id 3 tun if-name vpp-emby host-if-name tun-emby host-ip4-addr 10.20.0.2/30
set interface state vpp-emby up
set interface ip address vpp-emby 10.20.0.1/30
```

如果后续要做 Phase 2，把 `tun-emby` 移入 Emby systemd 服务的 netns，并在该 netns 内配置 `10.20.0.2/30`、默认路由 `via 10.20.0.1`。性能不是目标时，这一步可以不做，保持 Emby 走 host 路由即可。

### 6.4 nftables

当前 UFW 未启用且 INPUT 默认 ACCEPT。VPP 接管 WAN 后，建议至少在主 netns 对 `tun-punt` 做 SSH 限制：

```bash
nft add table inet punt
nft add chain inet punt input '{ type filter hook input priority 0; policy drop; }'
nft add rule inet punt input iifname "lo" accept
nft add rule inet punt input iifname "tun-punt" tcp dport 22 accept
nft add rule inet punt input iifname "tun-punt" ct state established,related accept
# 仅当 app-server 直接监听 WAN 时才需要：
# nft add rule inet punt input iifname "tun-punt" ip saddr <trusted-prefix> tcp dport <app-server-port> accept
```

如果确有可信来源网段，把 22 规则改成 `ip saddr <trusted-prefix> tcp dport 22 accept`。同时：

- 关闭 SSH 密码登录和 root 登录，改为密钥。
- NFS/RPC 只允许 `enp2s0f1` 管理/LAN 口，禁止从 WAN 进入。
- qBittorrent 30643 只在需要时映射。
- Caddy admin API `127.0.0.1:2019` 在迁移后消失；如仍保留 Caddy 作为回退，则继续只监听 localhost。

### 6.5 为什么有 `vpp-web` TUN，HTTP 是否还在用内核 TCP

是的，当前方案里 nginx/Emby 的 TCP 仍然由 Linux 内核终止。原因不是 VPP 不能做 TCP，而是：

- nginx 和 Emby 都是普通 Linux 用户态进程，使用内核 socket API；AF_XDP 只接管网卡，不会透明接管 host 进程的 socket。
- `vpp-web` 是一个 L3 点对点 TUN：VPP 把 80/443 的包送到内核，nginx 在 `10.10.0.2` 上用内核 TCP 接受连接；Emby 同理。
- VPP 的 `tcp` 配置（BBR/RACK/TLP）只对 **VPP host stack** 的 TCP 会话生效，也就是使用 VCL/LDP 的应用或 VPP 插件；不会作用于 TUN 后面的 nginx/Emby。

因此有两条路线：

| 路线 | 客户端 TCP | nginx -> Emby | 是否需要 `vpp-web` TUN | 适用场景 |
|---|---|---|---|---|
| A. TUN + 内核 TCP | Linux 内核 | 内核 loopback | 需要 | 应用不改，最稳；性能不是目标时首选 |
| B. VCL/LDP + VPP TCP | VPP host stack | 仍是内核 loopback | 不需要 | 明确要求客户端 TCP 由 VPP 终止 |

### 6.6 BBR + RACK/TLP 配置

#### 路线 A：保留 TUN，使用内核 BBR + RACK/TLP（推荐，简单）

Linux 6.8 支持 BBR/RACK/TLP。写入 `/etc/sysctl.d/90-tcp-bbr.conf`：

```text
net.core.default_qdisc = fq
net.ipv4.tcp_congestion_control = bbr
net.ipv4.tcp_recovery = 1
net.ipv4.tcp_early_retrans = 3
net.ipv4.tcp_sack = 1
net.ipv4.tcp_timestamps = 1
```

应用并验证：

```bash
sysctl --system
sysctl net.ipv4.tcp_available_congestion_control
sysctl net.ipv4.tcp_congestion_control
ss -ti | grep -E 'bbr|rack|tlp' | head
```

说明：

- `tcp_recovery=1` 启用 RACK 丢失检测；`tcp_early_retrans=3` 启用 TLP。
- Linux 没有“关闭 3 dup ack”的单独 sysctl；RACK/TLP 是替代 dupack 触发的损失检测路径。开了 RACK 后，重传决策以时间/顺序信息为主，而不是等 3 个重复 ACK。
- BBR 需要 SACK/timestamps；上面的配置已包含。

#### 路线 B：让 nginx 走 VPP host stack

如果明确要求 VPP 终止 TCP，则需要：

1. 删除 nginx 的 `vpp-web` TUN；nginx 不再通过内核 TUN 收包。
2. nginx 使用 VCL/LDP 运行，监听 VPP 的 WAN IP（或 VPP host-stack 地址）。
3. VPP 启动时启用 host stack 和 BBR/RACK：

```text
session {
  preallocated-sessions 64
}

tcp {
  cc-algo bbr
  rack
  byte-tracker
  mtu 1500
}
```

或运行时：

```text
set tcp cc-algo bbr
set tcp rack
show tcp config
```

4. nginx 通过 LDP 启动，示意：

```bash
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libvcl_ldpreload.so \
VCL_CONFIG=/etc/vpp/vcl.conf \
nginx -g 'daemon off;'
```

VCL 配置示例（需按 VPP 版本校验）：

```text
vcl {
  rx-fifo-size 4000000
  tx-fifo-size 4000000
  app-socket-api /run/vpp-lab/app_ns_sockets/default
  use-mq-eventfd
}
```

5. Emby 是 .NET 应用，不能直接 VCL；让 nginx 反代 `127.0.0.1:8096` 后，只有**客户端到 nginx**的 TCP 由 VPP 终止；nginx 到 Emby 的 loopback 这一段仍然是内核 TCP。

**边界说明（重要）**：

- 路线 B 能做到的是“WAN 客户端 TCP 由 VPP 终止，BBR/RACK/TLP 作用于这条连接”。
- nginx -> `127.0.0.1:8096` 是 local loopback，仍然走内核 TCP；它不经过 WAN，也不是公网拥塞路径，因此通常不需要 BBR/RACK/TLP。
- 如果要求“任何 TCP 都不能走内核”，Emby 必须自己变成 VCL-aware（直接使用 VPP host stack）。.NET Emby 目前没有受支持的 VCL 集成；`LD_PRELOAD=libvcl_ldpreload.so` 运行 .NET 只能算实验，Kestrel/.NET runtime 可能使用 VCL 不支持的 syscall。
- 在不换媒体服务器的前提下，无法做到“Emby 自身 TCP 也走 VPP”。可选路径只有：实验性 LDP + .NET、换成能接 VCL 的自研/C++/Rust 服务、或接受 loopback 这一段走内核。

注意事项：

- `set tcp` 只影响 **新连接**；已有连接要断开/重启 nginx 才会使用 BBR/RACK。
- BBR/RACK 需要 SACK；对端不支持 SACK 时 BBR 初始化会回退到 CUBIC，RACK 也不会启用。
- VPP 里启用 `rack` 后，丢失恢复由 RACK 驱动（`tcp_loss_should_enter_recovery` 直接看 `ac->last_lost`），3-dup-ack 不再是触发条件；TLP 属于 RACK 路径。VPP 仍会计数 dupack 用于拥塞控制统计，但不靠它触发重传。
- nginx + VCL/LDP 的兼容性需要实测（`epoll`、`accept4`、`sendfile` 等）；性能不是目标时，路线 A 更稳妥。

## 7. 旧环境参考：Caddy 配置（不执行）

> 以下只记录旧环境现状，新方案不改动 Caddy，也不执行 Caddy -> nginx 迁移。

### 7.1 迁移顺序

1. 备份 `/etc/caddy/Caddyfile` 和证书目录。
2. 安装 nginx，但先让它在 `8080/8443` 或备用地址上验证，不抢 Caddy 的 80/443。
3. 迁移站点、证书、Emby 路径、HLS、basic_auth，逐项用 `curl --resolve` 验证。
4. 停止并禁用 Caddy，nginx 接管 80/443。
5. 保留 Caddy 配置和证书至少一个维护周期，便于回退。
6. 确认 nginx 稳定后，再执行 VPP/AF_XDP 切换。

```bash
apt install nginx certbot python3-certbot-nginx apache2-utils
cp -a /etc/caddy /root/caddy-backup-$(date +%F)
nginx -t
systemctl stop caddy
systemctl disable caddy
systemctl enable --now nginx
```

### 7.2 证书

Caddy 证书当前在：

```text
/var/lib/caddy/.local/share/caddy/certificates/acme-v02.api.letsencrypt.org-directory/<domain>/
```

两种方式：

- **复用 Caddy 证书**：复制到 `/etc/nginx/tls/`，改属主/权限，并设置 Caddy 续期后的同步 hook（否则证书更新后 nginx 仍用旧文件）。
- **改用 certbot**：`certbot --nginx -d <domain>`，或先用 DNS challenge 申请，再写 nginx `ssl_certificate`。Caddy 停止后自动续期不再工作。

```bash
install -d -m 750 /etc/nginx/tls
for d in /var/lib/caddy/.local/share/caddy/certificates/acme-v02.api.letsencrypt.org-directory/*; do
  n=$(basename "$d")
  cp "$d/$n.crt" "/etc/nginx/tls/$n.crt"
  cp "$d/$n.key" "/etc/nginx/tls/$n.key"
done
chown root:www-data /etc/nginx/tls/*
chmod 640 /etc/nginx/tls/*
```

`porn.linqiankai.top` 与 `cf-origin.linqiankai.top` 共用 `cf-origin` 证书；使用前用 `openssl x509 -text` 确认 SAN 包含两个域名。

### 7.3 Caddyfile -> nginx 映射

| Caddy | nginx |
|---|---|
| `reverse_proxy 127.0.0.1:8096` | `proxy_pass http://emby;` |
| `reverse_proxy 127.0.0.1:5010` | `proxy_pass http://av;` |
| `reverse_proxy 127.0.0.1:5244` | `proxy_pass http://openlist;` |
| `reverse_proxy 127.0.0.1:9696/6969/6767/8384` | 对应 upstream |
| `encode zstd gzip` | `gzip on;`（zstd 需要额外模块） |
| `path_regexp` / `handle` | `location ~` / `location`（注意顺序，先具体后通用） |
| `header` 缓存策略 | `add_header` |
| `basic_auth` | `auth_basic` + htpasswd |
| `file_server` HLS | `alias`/`root` + `try_files` |
| `flush_interval -1` | `proxy_buffering off; proxy_request_buffering off;` |
| `X-Emby-Token` | `proxy_set_header X-Emby-Token "<token>";` |

**basic_auth 注意**：Caddy 使用 bcrypt 哈希；nginx 的 `auth_basic_user_file` 取决于系统的 `crypt()` 实现。最稳妥是重新生成 htpasswd：

```bash
htpasswd -B -c /etc/nginx/htpasswd/prowlarr avadmin
htpasswd -B -c /etc/nginx/htpasswd/whisparr avadmin
```

### 7.4 nginx 配置骨架

以下只保留关键路径，不能直接无脑覆盖生产配置；`<EMBY_TOKEN>` 必须从受控 secret 文件读取或由部署脚本注入。

```nginx
map $http_upgrade $connection_upgrade {
    default upgrade;
    ''      close;
}

upstream emby      { server 127.0.0.1:8096; keepalive 64; }
upstream av        { server 127.0.0.1:5010; keepalive 64; }
upstream openlist  { server 127.0.0.1:5244; keepalive 32; }
upstream prowlarr  { server 127.0.0.1:9696; keepalive 16; }
upstream whisparr  { server 127.0.0.1:6969; keepalive 16; }
upstream paseo     { server 127.0.0.1:6767; keepalive 16; }
upstream backup    { server 127.0.0.1:8384; keepalive 16; }
upstream codex_ws  { server 127.0.0.1:8790; keepalive 16; }

server {
    listen 80;
    listen [::]:80;
    server_name porn.linqiankai.top cf-origin.linqiankai.top;
    return 301 https://$host$request_uri;
}

server {
    listen 443 ssl http2;
    listen [::]:443 ssl http2;
    server_name porn.linqiankai.top cf-origin.linqiankai.top;

    ssl_certificate     /etc/nginx/tls/cf-origin.linqiankai.top.crt;
    ssl_certificate_key /etc/nginx/tls/cf-origin.linqiankai.top.key;

    gzip on;
    gzip_types text/plain text/css application/json application/javascript
               application/vnd.apple.mpegurl video/mp4 video/iso.segment;
    client_max_body_size 0;

    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header X-Emby-Token "<EMBY_TOKEN>";
    proxy_set_header Connection "";

    # HLS：保持原 /var/lib/caddy 布局，或复制到 /var/lib/nginx/emby-hls
    location /hls/av/ {
        alias /var/lib/caddy/hls/av/;
        add_header Access-Control-Allow-Origin "*" always;
        add_header Cache-Control "public, max-age=21600, immutable, no-transform" always;
        types { application/vnd.apple.mpegurl m3u8; video/mp4 mp4; video/iso.segment m4s; }
        try_files $uri =404;
    }

    location /d/ { proxy_pass http://openlist; }

    # 具体路径必须放在通用路径之前
    location ~ ^/api/emby/(chunks|manifest)/ { proxy_pass http://av; proxy_buffering off; }
    location ~ ^/(emby/)?Items/.*/PlaybackInfo$ { proxy_pass http://av; proxy_buffering off; }
    location ~ ^/(emby/)?(Items|Users)/.*/(Images|Items)/ { proxy_pass http://emby; }
    location ~ ^/(emby/)?Items/ { proxy_pass http://av; proxy_buffering off; }
    location ~ ^/(emby/)?(videos|audio)/ {
        proxy_pass http://emby;
        proxy_buffering off;
        proxy_request_buffering off;
        proxy_read_timeout 3600s;
        proxy_send_timeout 3600s;
    }
    location / { proxy_pass http://emby; proxy_buffering off; proxy_read_timeout 300s; }
}

server {
    listen 443 ssl http2;
    server_name prowlarr.linqiankai.top;
    ssl_certificate     /etc/nginx/tls/prowlarr.linqiankai.top.crt;
    ssl_certificate_key /etc/nginx/tls/prowlarr.linqiankai.top.key;
    auth_basic "Restricted";
    auth_basic_user_file /etc/nginx/htpasswd/prowlarr;
    location / { proxy_pass http://prowlarr; proxy_http_version 1.1; proxy_set_header Host $host; }
}

server {
    listen 443 ssl http2;
    server_name whisparr.linqiankai.top;
    ssl_certificate     /etc/nginx/tls/whisparr.linqiankai.top.crt;
    ssl_certificate_key /etc/nginx/tls/whisparr.linqiankai.top.key;
    auth_basic "Restricted";
    auth_basic_user_file /etc/nginx/htpasswd/whisparr;
    location / { proxy_pass http://whisparr; proxy_http_version 1.1; proxy_set_header Host $host; }
}

server {
    listen 443 ssl http2;
    server_name av.linqiankai.top;
    ssl_certificate     /etc/nginx/tls/av.linqiankai.top.crt;
    ssl_certificate_key /etc/nginx/tls/av.linqiankai.top.key;
    location / { proxy_pass http://av; proxy_http_version 1.1; proxy_set_header Host $host; }
}

server {
    listen 443 ssl http2;
    server_name oplist.linqiankai.top;
    ssl_certificate     /etc/nginx/tls/oplist.linqiankai.top.crt;
    ssl_certificate_key /etc/nginx/tls/oplist.linqiankai.top.key;
    location / { proxy_pass http://openlist; proxy_http_version 1.1; proxy_set_header Host $host; }
}

server {
    listen 443 ssl http2;
    server_name paseo.linqiankai.top;
    ssl_certificate     /etc/nginx/tls/paseo.linqiankai.top.crt;
    ssl_certificate_key /etc/nginx/tls/paseo.linqiankai.top.key;
    location / { proxy_pass http://paseo; proxy_http_version 1.1; proxy_set_header Host $host; }
}

server {
    listen 443 ssl http2;
    server_name backup.linqiankai.top;
    ssl_certificate     /etc/nginx/tls/backup.linqiankai.top.crt;
    ssl_certificate_key /etc/nginx/tls/backup.linqiankai.top.key;
    location / { proxy_pass http://backup; proxy_http_version 1.1; proxy_set_header Host $host; }
}
```

### 7.5 迁移注意事项

- `porn/cf-origin` 的 Caddy 配置包含 Emby 缓存头、HLS 静态文件、正则路径和 `X-Emby-Token`，必须逐项验证，尤其大文件 Range、转码流和 HLS 分段。
- nginx 的 `location ~` 按出现顺序匹配，`Items/.../Images` 必须排在通用 `Items/` 之前。
- `proxy_buffering off` 对流媒体很重要；但会增加后端连接占用，注意 `proxy_read_timeout` 和 upstream keepalive。
- HLS 目录原属主是 `caddy`，nginx（`www-data`）未必有读权限；复制到 `/var/lib/nginx/emby-hls` 并修正权限，或给 `www-data` 只读 ACL。
- Caddy admin API `127.0.0.1:2019` 属于旧环境；外部脚本如依赖它需自行调整。
- Caddy 的自动 ACME 不再工作；必须建立 certbot 或证书同步 + nginx reload 的续期流程。

## 8. 从本地访问 Codex（推荐：SSH 隧道）

### 8.1 结论

你的目标是“从本地访问 Codex”，不是把 app-server 暴露到公网。最合理的方式是：

- app-server 只监听 `ws://127.0.0.1:8790`，不开 WAN 端口。
- 本地通过 SSH 本地端口转发访问它。
- SSH 本来就是唯一从 WAN punt 到内核的服务；app-server 流量全部装在 SSH 隧道里。

这样不需要给 app-server 单独做 punt，不需要公网 TLS/证书，也不增加公网攻击面。

### 8.2 服务端

生成一个长期 token：

```bash
install -d -m 700 /etc/codex
openssl rand -hex 32 > /etc/codex/ws-token
chmod 600 /etc/codex/ws-token
```

启动 app-server，只绑定 localhost：

```bash
codex app-server \
  --listen ws://127.0.0.1:8790 \
  --ws-auth capability-token \
  --ws-token-file /etc/codex/ws-token
```

说明：

- 当前运行的 `codex ... app-server --listen unix://` 只有 Unix socket。建议在维护窗口内停掉现有实例，直接用 `ws://127.0.0.1:8790` 启动，而不是同时跑两个 app-server，避免争用 `/root/.codex` 下的状态数据库；本机客户端改用 `codex --remote ws://127.0.0.1:8790`。
- 8790 只是示例端口，选一个未占用的高位端口即可。
- app-server 的模型流量仍然走 `127.0.0.1:15722` 的 `cc-switch`，不受影响。

### 8.3 本地客户端

在本地机器上建立 SSH 隧道：

```bash
ssh -N -L 8790:127.0.0.1:8790 root@184.107.84.201
```

把 token 取回本地（只取一次，保存在本地受控文件或环境变量中）：

```bash
export CODEX_REMOTE_TOKEN="$(ssh root@184.107.84.201 cat /etc/codex/ws-token)"
```

然后让本地 Codex 连接隧道：

```bash
codex --remote ws://127.0.0.1:8790 \
  --remote-auth-token-env CODEX_REMOTE_TOKEN
```

如果要长期使用，可以把 `ssh -N -L ...` 放到本地 systemd user service、`autossh` 或终端复用器里；不关心性能时，SSH 隧道的开销完全可接受。

### 8.4 为什么不让 app-server 直接走 punt

- app-server 只支持明文 `ws://`，没有内置 TLS；直接暴露 WAN 会让 token 和会话内容明文传输。
- app-server 可以驱动 shell/agent，是高风险入口；公网监听需要额外的认证、TLS、来源 ACL 和审计。
- `ip punt redirect` 保留原始目的地址，app-server 必须绑定公网 IP，进一步扩大暴露面。
- SSH 隧道已经能满足“本地访问”，而且 SSH 本来就是 punt 到内核的；没必要再给 app-server 开一个公网 punt 端口。

### 8.5 如果客户端不支持 SSH 隧道

优先考虑 WireGuard/Tailscale 或使用 `enp2s0f1` 的私网管理地址；如果只能走 443，再让 nginx 终止 TLS，把 `wss://codex.<domain>/` 反代到 `ws://127.0.0.1:8790`，并保留 token 认证。仍不建议直接暴露 app-server 的 `ws://` 端口。

## 9. 旧环境参考：Docker Emby（不执行）

> 以下只记录旧环境现状，新方案不改动 Docker Emby，也不执行 Docker -> systemd 迁移。

### 9.1 方案

使用官方 `emby-server-deb_4.10.1.0_amd64.deb`。它自带：

```text
/opt/emby-server/
/opt/emby-server/bin/emby-server        # wrapper
/opt/emby-server/system/EmbyServer
/opt/emby-server/bin/{ffmpeg,ffprobe,ffdetect}
/usr/lib/systemd/system/emby-server.service
/etc/emby-server.conf
```

官方 unit 默认 `User=emby`、`EMBY_DATA=/var/lib/emby`。当前容器版本是 4.9.5.0，官方包是 4.10.1.0，迁移会顺带升级；先备份 `/config`。

### 9.2 备份与停容器

```bash
cp -a /root/emby/docker_app/amilys_embyserver/config \
  /root/emby/config-backup-$(date +%F)
docker update --restart=no av-emby
docker stop av-emby
```

### 9.3 安装官方包（先禁止自动启动）

```bash
systemctl mask emby-server
curl -fL -o /tmp/emby-server-deb_4.10.1.0_amd64.deb \
  https://github.com/MediaBrowser/Emby.Releases/releases/download/4.10.1.0/emby-server-deb_4.10.1.0_amd64.deb
dpkg -i /tmp/emby-server-deb_4.10.1.0_amd64.deb
systemctl unmask emby-server
```

### 9.4 迁移配置与媒体路径

把容器配置复制到官方默认数据目录：

```bash
install -d -o emby -g emby -m 755 /var/lib/emby
cp -a /root/emby/docker_app/amilys_embyserver/config/. /var/lib/emby/
chown -R emby:emby /var/lib/emby
```

Emby 数据库里存的是容器内绝对路径 `/media` 和 `/downloads/incomplete`。为了不改库，使用 bind mount 保持原路径：

```bash
install -d -m 755 /media /downloads/incomplete
mount --bind /root/emby/media /media
mount --bind /root/emby/media/incomplete /downloads/incomplete
```

写入 `/etc/fstab` 以便重启后自动挂载：

```text
/root/emby/media              /media                  none bind,nofail 0 0
/root/emby/media/incomplete   /downloads/incomplete   none bind,nofail 0 0
```

注意：`/root` 是 `700`，`emby` 用户不能直接穿过 `/root`；但 bind mount 到 `/media` 后，访问走的是挂载点，不再需要遍历 `/root`。配置已经复制到 `/var/lib/emby`，也不需要访问 `/root/emby/docker_app/...`。

如果 Emby 需要写 `/downloads/incomplete`，确认底层目录对 `emby` 可写，或加 ACL/组权限；容器原来是 root，权限可能偏宽。

### 9.5 配置 systemd

编辑 `/etc/emby-server.conf`：

```text
EMBY_DATA=/var/lib/emby
```

建议加一个 drop-in 提高文件描述符上限：

```bash
install -d /etc/systemd/system/emby-server.service.d
cat >/etc/systemd/system/emby-server.service.d/override.conf <<'EOF'
[Service]
LimitNOFILE=1048576
EOF
systemctl daemon-reload
```

启动并验证：

```bash
systemctl enable --now emby-server
systemctl status emby-server --no-pager
journalctl -u emby-server -n 100 --no-pager
ss -ltnp | grep -E ':8096|:8920'
```

### 9.6 最小改动模式（如果权限迁移太麻烦）

如果暂时不想复制 269MB 配置、也不想处理 `emby` 用户权限，可以让官方 unit 以 root 运行，并继续使用原配置目录：

```bash
install -d /etc/systemd/system/emby-server.service.d
cat >/etc/systemd/system/emby-server.service.d/override.conf <<'EOF'
[Service]
User=root
Group=root
Environment=EMBY_DATA=/root/emby/docker_app/amilys_embyserver/config
LimitNOFILE=1048576
EOF
systemctl daemon-reload
systemctl enable --now emby-server
```

媒体路径仍建议用上面的 `/media`、`/downloads/incomplete` bind mount 保持兼容。这个模式与当前容器行为一致（容器也是 root），但不是长期推荐；验证稳定后再迁移到 `emby` 用户。

### 9.7 防火墙与 nginx

Emby 默认可能监听 `0.0.0.0:8096/8920`。nginx 在宿主机上，只需要 loopback 访问，因此应阻止非 loopback 直接访问：

```bash
nft add rule inet filter input tcp dport { 8096, 8920 } iifname != "lo" drop
```

nginx 的 upstream 不需要改，仍然是：

```nginx
upstream emby { server 127.0.0.1:8096; keepalive 64; }
```

### 9.8 验证与清理

- 访问 nginx 的 Emby 站点，播放视频，确认 4.10.1.0 数据库升级成功。
- 检查 `/var/lib/emby/logs` 和 `journalctl -u emby-server`。
- 稳定后移除容器：`docker rm av-emby`；镜像可保留一段时间做回退。
- 如果 Emby 启动失败：`systemctl stop emby-server`，恢复备份配置，重新 `docker start av-emby`（容器还在时）。

### 9.9 彻底去 Docker 的剩余项

本文只把 Emby 迁出 Docker。以下容器仍在运行，需要分别迁移后才能停掉 Docker：

```text
openlist                 openlistteam/openlist:latest
av-metatube              metatube-server
av-prowlarr              linuxserver/prowlarr:latest
av-whisparr              ghcr.io/hotio/whisparr:latest
av-subscriber            av-subscriber:local
av-subscriber-scheduler  av-subscriber:local
```

其中 openlist、metatube 可以作为原生二进制 + systemd；Prowlarr/Whisparr 需要迁移配置和数据库；av-subscriber 是自定义镜像，需要先找到它的二进制/启动参数。全部迁移并验证后，再 `systemctl disable --now docker containerd` 并卸载 Docker。

## 10. 验证清单

VPP：

```text
show interface
show hardware-interfaces
show tcp config
show tcp stats
show session
```

Linux：

```bash
ip netns exec vpp-client ping -c3 10.10.0.1
ip netns exec vpp-client curl -v http://10.10.0.1:8080/health
ip netns exec vpp-client curl -v -r 0-1023 http://10.10.0.1:8080/media/test.mp4 -o /tmp/part
ip netns exec vpp-client curl -v http://10.10.0.1:8080/hls/test/index.m3u8
ss -ltnp | grep ':8080'        # 服务端不应出现内核监听
```

业务：

- 裸 HTTP 直连 `http://10.10.0.1:8080/media/test.mp4`，验证 Range/206、seek、HLS 分片。
- 确认服务端 VPP internal app 在 `show session` 中有 TCP 会话，Linux 侧没有 8080 内核监听。
- Phase 2 再做 AF_XDP + 强制 SSH punt；到时用 `show tcp punt`、`show ip punt redirect`、`tcpdump -ni tun-punt tcp port 22` 验证。

## 11. 回滚

1. 停止 VPP。
2. 恢复 `/etc/netplan/01-netcfg.yaml` 中 `enp2s0f0` 的静态 IP、默认路由和 DNS。
3. 执行 `netplan apply`。
4. 删除 `tun-web`/`tun-punt` 和策略路由（`ip rule del ...`、`ip route flush table 200`）。
5. 如 nginx 有问题：停止 nginx，恢复并启动 Caddy，确认 80/443 回到 Caddy。
6. 如 app-server ws 监听有问题：停止该监听，恢复原来的 Unix socket 方式。
7. 确认 systemd Emby、sshd、nginx/Caddy 正常。

维护窗口内务必保留 IPMI/串口或 `enp2s0f1` 管理通道；单网口切换 AF_XDP 时，SSH 锁死风险最高。

## 12. 风险与待确认

1. **AF_XDP copy 模式**：I350 不支持 zero-copy，1Gbps 上限，CPU 拷贝成本需要实测。
2. **Emby 路径**：Emby 已改为 systemd，但仍经过 host 内核；性能不是目标时不需要 Phase 2 直连 TAP。
3. **旧环境不参与**：Caddy/Docker Emby 不迁移、不改动；新方案只在新环境做 internal HTTP app。
4. **SSH punt 范围**：`set punt tcp all` 会把所有未知 TCP 送到内核，必须用 nftables/ACL 限流；严格只匹配 22 需要 `ip_session_redirect`（实验性）。
5. **app-server 安全**：它当前只监听 Unix socket；推荐改成 localhost `ws://` 并只通过 SSH 隧道访问，不要暴露公网。若要公网访问，必须加 token 认证、TLS 和来源 ACL。
6. **app-server 兼容性**：改变 `--listen` 可能影响现有 Codex 客户端；建议保留 Unix socket 供本机使用，另起一个 localhost ws 监听。
7. **TCP 栈选择**：TUN 路线下 BBR/RACK/TLP 在内核；VPP 路线下 nginx 必须走 VCL/LDP，兼容性和 `set tcp` 只影响新连接这两点都要实测。Emby .NET 不直接走 VCL，建议由 nginx 反代。
8. **管理通道**：`enp2s0f1` 当前无 IPv4；如果交换机侧有可用网段，建议配置为独立管理口。
9. **CPU/内存**：E5-2690 v2 较老，Emby 转码会与 VPP worker 争抢 CPU；hugepages 分配要兼顾 Emby。
10. **安全**：当前 SSH 允许 root+密码、UFW 未启用、NFS/RPC 公网可达；迁移前应先收紧。
11. **IPv6**：当前只有 link-local，无全局 IPv6；本方案只覆盖 IPv4。

## 附录 A：当前 Docker 容器

```text
av-subscriber-scheduler  av-subscriber:local            host
av-subscriber            av-subscriber:local            host
openlist                 openlistteam/openlist:latest   av-stack_default  172.18.0.2
av-prowlarr              linuxserver/prowlarr:latest    host
av-whisparr              ghcr.io/hotio/whisparr:latest  host
av-metatube              metatube-server                 av-stack_default  172.18.0.4
av-emby                  amilys/embyserver:latest        bridge            172.18.0.3 / 172.17.0.2
```

## 附录 B：迁移前 Caddy 站点

```text
backup.linqiankai.top
porn.linqiankai.top, cf-origin.linqiankai.top
prowlarr.linqiankai.top
whisparr.linqiankai.top
av.linqiankai.top
oplist.linqiankai.top
paseo.linqiankai.top
```

## 附录 C：Emby 与替代品的语言/集成

| 项目 | 核心语言 | 说明 |
|---|---|---|
| Emby Server | C# / .NET | 当前使用；没有官方 C++/Rust 版本 |
| Jellyfin | C# / .NET | Emby 的 fork，核心仍是 .NET |
| Plex Media Server | C++（闭源） | 成熟的 C++ 媒体服务器，但不是开源，也不原生支持 VPP |
| Kodi | C++ | 媒体中心，不是 Emby 这种服务端 |
| Gerbera | C++ | UPnP/DLNA 媒体服务器，不是 Emby 等价物 |
| MiniDLNA / ReadyMedia | C | 轻量 DLNA，不是 Emby 等价物 |
| Navidrome | Go | 音乐服务器，不处理视频媒体库 |
| Dim | Rust | 早期 Rust 媒体服务器项目，功能和生态与 Emby 差距较大 |

结论：**目前没有成熟的 C++/Rust Emby 等价物**。如果目标只是让客户端 TCP 走 VPP，保留 Emby（.NET）并由 nginx/VCL 反代即可；但 nginx 到 Emby 的 loopback 仍是内核 TCP。如果要求 Emby 自身也不用内核 TCP，就必须把 Emby 换成 VCL-aware 的实现，目前没有成熟替代。

## 附录 D：VPP internal app + nginx external app 是否可行

可行，但要先把两个概念分开：

- **VPP internal app**：作为 VPP 插件跑在 VPP 进程内，使用 `vnet/session` host stack。现成例子是 `src/plugins/http` 和 `src/plugins/http_static`；后者就是内建的静态 HTTP(S) 服务器，支持 GET/POST、LRU 文件缓存和可插拔 URL handler。
- **VPP external app**：独立进程，通过 VCL/LDP 使用 VPP host stack。nginx 属于这一类；如果 nginx 走 TUN，则又回到内核 TCP。

推荐的 MVP 架构：

```text
Client --TCP/VPP(BBR+RACK+TLP)--> nginx(VCL, TLS/HTTP edge)
                                      |
                                      | VPP host stack TCP（不经过内核）
                                      v
                          VPP internal media app (C plugin)
                          基于 http/http_static：
                          - 直链文件 / HLS 分片
                          - 简单鉴权 / URL 映射
                          - Range/206（需要自己补）
                                      |
                         外部 scanner/ffmpeg 进程
                         （扫描、转码、生成 HLS，不放进 VPP 进程）
```

关键取舍：

1. **nginx 必须走 VCL/LDP**。如果 nginx 只是挂在 TUN 上，nginx -> internal app 仍然可以走 VPP host stack，但 nginx 自身的客户端侧仍是内核 TCP。要端到端 VPP，nginx 必须用 VCL。
2. **internal app 只做薄层**。VPP 插件不能阻塞 worker，不能把 ffmpeg、数据库、复杂文件 I/O 放进数据面；扫描/转码/元数据放外部进程，插件只读文件并回 HTTP。
3. **Emby API 不是“简单”能兼容的**。Emby 客户端依赖完整的 API、用户/库/元数据、转码和 HLS 语义。写一个直链/HLS 播放器可行，写一个 Emby 兼容服务器是另一个量级。
4. **Range/206 要单独实现**。`http` 插件解析 Range 头，但 `http_static` 的静态文件路径不一定实现完整 206；媒体 seek 需要自己补。
5. **崩溃隔离**。插件崩溃会带走 VPP；这是把媒体服务器放进 VPP 进程的最大风险。薄插件 + 外部转码是更稳的边界。

结论：**如果只是“简单的直链/HLS 媒体服务”，这个架构可行，而且能做到客户端到 internal app 全程 VPP TCP；如果目标是完整 Emby 兼容，不建议把 Emby 重写成 VPP 插件，保留 Emby + nginx/VCL + 内核 loopback 更现实。**

## 附录 E：VPP internal media app 具体实施方案

### E.1 范围

MVP 只做“直链播放 + HLS 静态分片”，不做转码、不做完整 Emby API：

- `GET /media/<path>`：按路径返回媒体文件，支持 `Range`/`206`。
- `GET /hls/<id>/<file>`：返回外部 ffmpeg 生成的 `.m3u8`/`.m4s`/`.ts`。
- `GET /health`：返回 `{"status":"ok"}`。
- 可选 `GET /api/items`：返回简单 JSON 索引。
- 鉴权、TLS、限流由 nginx 负责；internal app 只信任 nginx 注入的 `X-Auth-User`，并剥离客户端自带的同名头。

### E.2 组件与地址

```text
WAN: enp2s0f0 / AF_XDP
VPP wan0 184.107.84.201/26

VPP loopback0 10.255.0.1/32
  http_static / media handler 监听 tcp://10.255.0.1/8080

nginx (external VCL app)
  监听 wan0:443（VPP host stack）
  proxy_pass http://10.255.0.1:8080

外部 scanner/ffmpeg (systemd)
  扫描 /srv/media，生成 /srv/media/hls/<id>/* 和 index.json

SSH: punt 到 tun-punt -> 内核 sshd
```

这里没有 `vpp-web` TUN：客户端 TCP 和 nginx -> internal app 都在 VPP host stack 内。

### E.3 VPP 内部实现（推荐先扩展 `http_static`）

`http_static` 已经是内建 host-stack HTTP 服务器，支持 GET/POST、LRU 缓存和可插拔 URL handler。MVP 直接给它加一个 `media` handler，避免跨插件初始化顺序问题：

```text
src/plugins/http_static/
  media_server.c        # 新增：路径解析、Range、文件发送
  media_index.c         # 可选：index.json 加载/刷新
  CMakeLists.txt        # SOURCES 增加 media_server.c
```

核心逻辑：

1. 注册 `GET /media/` 和 `GET /hls/` 路径（或扩展 `static_server.c` 的文件路径）。
2. 严格限制根目录为 `/srv/media`，拒绝 `..`、绝对路径、符号链接逃逸。
3. 解析 `Range: bytes=start-end`，返回 `206 Partial Content`、`Content-Range`、`Accept-Ranges: bytes`。
4. 分块读取文件并通过 `hss_session_send_data`/`hss_session_send_fn` 发送；不要在 worker 里一次性读几 GB。
5. 对 `.m3u8`、`.m4s`、`.ts`、`.mp4` 设置正确 `Content-Type`。
6. 大文件不要依赖 `http_static` 的 LRU 缓存；缓存只用于小文件/索引。

如果不想改 `http_static`，也可以新建 `media_server` 插件，用 `vlib_get_plugin_symbol("http_static_plugin.so", "hss_register_url_handler")` 注册 handler。但要注意 `docs/usecases/webapp.rst` 的顺序要求：先启动 `http_static`，再注册 handler，否则注册会丢失。MVP 更推荐直接扩展 `http_static`。

### E.4 启用 internal HTTP server

`http_static` 目前主要通过二进制 API 启用（`http_static_enable_v5`），没有稳定的 CLI。MVP 写一个 `vpp_papi` bootstrap 脚本：

```python
from vpp_papi import VPPApiClient

vpp = VPPApiClient()
vpp.connect("media-bootstrap")
vpp.http_static_enable_v5(
    fifo_size=4 << 20,
    cache_size_limit=64 << 20,
    max_age=0,
    keepalive_timeout=60,
    max_body_size=8192,
    rx_buff_thresh=1 << 20,
    www_root="/srv/media",
    uri="tcp://10.255.0.1/8080",
)
```

如果 fork 后愿意加 CLI，可以增加 `http static enable ...` 命令，避免维护 Python bootstrap。

### E.5 nginx external VCL

nginx 用 VCL/LDP 运行，监听 WAN IP；不需要 TUN：

```bash
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libvcl_ldpreload.so \
VCL_CONFIG=/etc/vpp/vcl.conf \
nginx -g 'daemon off;'
```

关键 nginx 配置：

```nginx
server {
    listen 443 ssl http2;
    server_name media.example.com;

    ssl_certificate     /etc/nginx/tls/media.example.com.crt;
    ssl_certificate_key /etc/nginx/tls/media.example.com.key;

    client_max_body_size 0;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Auth-User $remote_user;
    proxy_set_header Range $http_range;
    proxy_set_header If-Range $http_if_range;

    location /media/ {
        proxy_pass http://10.255.0.1:8080;
        proxy_buffering off;
        proxy_request_buffering off;
        proxy_read_timeout 3600s;
    }

    location /hls/ {
        proxy_pass http://10.255.0.1:8080;
        proxy_buffering off;
        add_header Access-Control-Allow-Origin "*" always;
    }

    location /health { proxy_pass http://10.255.0.1:8080; }
}
```

### E.6 外部 scanner/transcoder

不要放进 VPP 进程：

- scanner：遍历 `/srv/media`，生成 `index.json`（id、path、mime、size、mtime）。
- transcoder：按需或预生成 HLS 到 `/srv/media/hls/<id>/`（ffmpeg）。
- 两者用 systemd service/timer 管理；internal handler 只读文件/索引。
- 更新索引后通过 CLI/API 通知 VPP reload，或按 mtime 懒加载。

### E.7 构建

在 `VPP_PLUGINS` 中加入 `http,http_static` 以及（如果新建插件）`media_server`：

```text
./configure -b build-vpp-media -t release \
  -i "$PWD/build-vpp-media-install" \
  -P af_xdp,tap,nat,http,http_static -D none -S none -E none
```

VCL 默认构建；确认安装树里有 `libvcl_ldpreload.so` 和 `libvppcom.so`。

### E.8 验证

```text
show tcp config          # cc-algo bbr + rack
show interface
show session
```

```bash
curl -k -r 0-1023 https://media.example.com/media/test.mp4 -o /tmp/part
curl -k https://media.example.com/hls/<id>/index.m3u8
ss -ltnp | grep ':443'        # 不应出现内核 nginx 监听
tcpdump -ni tun-punt tcp port 22   # 只有 SSH punt
```

### E.9 分阶段

1. **P0**：`http_static` 提供测试文件；nginx VCL 反代；验证客户端 TCP 在 VPP、无内核 443 监听。
2. **P1**：媒体路径 + Range/206 + HLS 静态分片；验证 seek 和播放。
3. **P2**：scanner/ffmpeg + 索引 + 鉴权 + TLS + 限流。
4. **P3**：可选的 Emby API 子集；在此之前不要声称兼容 Emby 客户端。

