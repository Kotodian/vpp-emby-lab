# VPP + AF_XDP：Emby、nginx 与 Codex app-server 接入 / SSH Punt 方案

本文基于 2026-10-19 对 `s1255788` 的只读盘点编写。所有地址、接口名、容器名和端口均来自当前机器实际状态；落地前请在维护窗口内逐项复核。

## 0. 结论摘要

- 当前 WAN 是 `enp2s0f0`，公网地址 `184.107.84.201/26`，网关 `184.107.84.254`，Intel I350（`igb`）1GbE。
- **当前内核 6.8**：`xdp-loader features enp2s0f0` 显示 `NETDEV_XDP_ACT_XSK_ZEROCOPY=no`，只能 copy 模式（`no-zero-copy`）。这是**内核版本限制，不是 I350 硬件的限制**：igb 在 6.14 之前**完全没有 AF_XDP 支持**（不只是 zero-copy），6.13 的 igb 源码里没有任何 `xsk` 引用；6.14 才加入（LWN 986006 就是这组 patch 的 v6）。升级到 ≥6.14 后同一张网卡可以开 zero-copy。
- 旧环境 80/443 由 **Caddy 2.11.4** 监听；新方案不迁移旧环境。新环境里的 nginx 是**另起的一套**（跑在 VPP host stack 上），与 Caddy 并存时用不同入口（lab 地址/端口），Phase 2 测试时才临时停 Caddy。
- Emby 保持现状：Docker 容器 `av-emby`（4.9.5.0），配置 `/root/emby/docker_app/amilys_embyserver/config`，媒体 `/root/emby/media`。新环境只把它当 **API 上游**（`127.0.0.1:8096`），不迁移、不换 systemd。
- 其他 Docker 服务（openlist、metatube、Prowlarr、Whisparr、av-subscriber）不在本次 Emby 迁移范围内；如果目标是彻底移除 Docker，需要单独逐项迁移。
- Codex app-server 当前使用 `--listen unix://`，只有 Unix socket。推荐让它只监听 `ws://127.0.0.1:8790`，本地通过 SSH 本地端口转发访问；这样 app-server 不需要单独做公网 punt，SSH 隧道本身就走在 punt 路径上。
- `vpp-web` TUN 只属于“保留内核 TCP”的路线。Phase 1/2 都是 **external app（nginx VCL）+ internal app（VPP media plugin）**：客户端到 nginx、nginx 到 internal app 都走 VPP host stack，不需要 `vpp-web` TUN。只有 nginx -> 真实 Emby 的 `127.0.0.1:8096` 那一段仍是内核 loopback。
- **当前优先 Phase 0：隔离 lab + 裸 HTTP internal app。** 独立 VPP 实例 + veth + `af_packet`，不碰现有 Caddy/Docker/Emby/`enp2s0f0`。Phase 2 在当前 6.8 内核上用 **af_packet** 跑功能（内核不受影响）；升级到 ≥6.14 后再切 AF_XDP（zero-copy）。
- 推荐顺序：Phase 0 隔离 lab + 裸 HTTP 验证 -> Phase 1 本机 TUN 验证混合路由（nginx VCL + VPP internal app，纯 HTTP）-> Phase 2 同一套接到公网（TLS + `pornvpp.linqiankai.top` + 停 Caddy）-> Phase 3 AF_XDP + 强制 SSH punt。
- `pornvpp.linqiankai.top` 的 A 记录已生效（`184.107.84.201`），Let's Encrypt HTTP-01 证书已签发：
  `/etc/letsencrypt/live/pornvpp.linqiankai.top/{fullchain.pem,privkey.pem}`，ECDSA，有效期到 2027-01-17；Caddy 已恢复。
- Phase 0/1 的 lab 构建实测插件集：`af_packet,tap,http,http_static`（`./configure -P af_packet,tap,http,http_static -D none -S none -E none`），已包含 `vppctl` 和 VCL 库（`libvcl_ldpreload.so`）。Phase 2 当前用 `af_packet`；内核 ≥6.14 后可切 `af_xdp`（含 zero-copy）。linux-cp/nat/ip_session_redirect 已编进 lab 构建。

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

结论：当前 6.8 内核上必须 `no-zero-copy`；Linux ≥6.14 的 igb 已支持 zero-copy，升级内核后可以完全去掉这次拷贝。判断依据始终以 `xdp-loader features` 为准。

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
4. **Phase 3**：性能与加固（可选）：压测、调 worker/队列、评估 DPDK 与 linux-cp。AF_XDP 和 SSH punt 已在 Phase 2 完成。
5. TCP 使用 BBR + RACK/TLP：Phase 0 的 Linux 侧用内核参数；Phase 1 的 VPP TCP 用 `tcp { cc-algo bbr rack }`。
6. 从本地通过 SSH 隧道访问 Codex app-server。
7. 全程可回滚；新环境与旧环境隔离。

约束：

- AF_XDP zero-copy 取决于内核：当前 6.8 不支持（只能 copy）；Linux ≥6.14 的 igb 已合入支持，升级后可开。
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

#### 实测结论（2026-10-19，`tap` TUN + nginx VCL）

已验证可用的完整链路：

| 路径 | 结果 |
|---|---|
| 本机 -> `vpp-tun` -> VPP internal app（`10.10.0.1:8080`） | 200 |
| 本机 -> `vpp-tun` -> nginx VCL（`10.10.0.1:80`） | 200 |
| nginx -> 真实 Emby（`10.10.0.2:8096`，内核 DNAT 到 `127.0.0.1:8096`） | 200 |
| nginx -> VPP media app（`10.255.0.1:8080`，**全程 VPP，不经内核**） | 200 |

`create tap id 0 host-if-name vpp-tun tun` 正常，内核侧是 `POINTOPOINT,NOARP` 的 TUN 设备。VCL 必须同时配置 `api-socket-name` 和 `app-socket-api`，只配后者会静默超时。

上游 VPP 默认会丢弃"本机地址 -> 本机地址"的连接：`ip4-local` 的反欺骗检查（IPv6 是 uRPF）把源地址是本机地址的包当成 spoof 丢掉。表现是 `show session` 里 `10.255.0.1:xxxxx -> 10.255.0.1:8080 SYN_SENT`，`ip4 spoofed local-address packet dropped` 递增。

修复方式见下面的源码补丁：本机协议栈生成的包已经带 `VNET_BUFFER_F_LOCALLY_ORIGINATED`（`tcp_output.c` 设置），让反欺骗检查对它放行即可。修完后 `nginx -> 10.255.0.1:8080` 返回 200，媒体路径**全程在 VPP host stack 内**，不经过内核。

真实 Emby 是内核里的 Docker 进程，API 那一段必须出 VPP：nginx 连 `10.10.0.2:8096`，内核 DNAT 到 `127.0.0.1:8096`。这是唯一需要内核的地方，且只承载 API/元数据，不承载视频流。

```text
本机 curl（源 10.10.0.2）
   |  vpp-tun（tap 插件 TUN）
   v
VPP: 10.10.0.1:80  nginx (VCL)
   |-- API  --> 10.10.0.2:8096 --[内核 DNAT]--> 127.0.0.1:8096 (真实 Emby)
   `-- 播放 --> 10.255.0.1:8080 -------------> VPP internal app（纯 VPP）
```

为什么 Phase 1 也要 nginx：Emby 客户端必须先登录、拿到 `PlaybackInfo` 才能点到播放 URL。没有 nginx 做混合路由，客户端连 API 都到不了真实 Emby。所以 Phase 1 和 Phase 2 的差别只在**入口**（TUN/HTTP vs WAN/HTTPS），不在路由和播放实现。

路由规则：

| 路径 | 目标 | 说明 |
|---|---|---|
| `/System/*`、`/Users/*`、`/Items/*`（非播放） | `10.10.0.2:8096` | 真实 Emby API（内核 DNAT） |
| `/emby/System/*`、`/emby/Users/*`、`/emby/Items/*`（非播放） | `10.10.0.2:8096` | Emby base path API（内核 DNAT） |
| `/(emby/)?Videos/*/stream`、`/(emby/)?Videos/*/original` | `10.255.0.1:8080` | VPP 播放（纯 VPP） |
| `/(emby/)?Items/*/Download`、`/(emby/)?Audio/*/stream` | `10.255.0.1:8080` | VPP 播放（纯 VPP） |
| `/hls/*`、`/(emby/)?Videos/*/master.m3u8` | `10.255.0.1:8080` | VPP HLS（纯 VPP） |
| 其他 | `10.10.0.2:8096` | 默认回 API |

**关键点**：播放 URL 必须命中 VPP，而不是被 nginx 默认反代到 Emby。Emby 的 `PlaybackInfo` 返回 `DirectStreamUrl` 时通常就是 `/Videos/{id}/stream`，nginx 按上面的规则切到 VPP；如果 Emby 返回的是转码 URL，MVP 不处理，先用 direct-play 兼容的媒体测试。

**itemId -> 文件路径**：VPP internal app 不能阻塞 worker 去查 Emby API。用外部 scanner 定期从 Emby API/数据库生成 `index.json`（itemId、path、size、mime），VPP app 启动加载并在 SIGHUP/CLI 时刷新；播放时只查内存索引并 `pread`/分块发送。

nginx VCL 关键配置：

```nginx
upstream emby_api  { server 10.10.0.2:8096; keepalive 32; }
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
    location ~ ^/(emby/)?Videos/[^/]+/(.+)$         { proxy_pass http://vpp_media; }
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

#### 源码补丁：允许本机 app 互连

VPP 原本会丢弃"本机地址 -> 本机地址"的 TCP 连接。本机协议栈生成的包已经带 `VNET_BUFFER_F_LOCALLY_ORIGINATED`（`src/vnet/tcp/tcp_output.c` 设置），只是反欺骗/uRPF 检查没看这个标记。补三处：

```c
/* src/vnet/ip/ip4_forward.c - ip4_local_check_src() */
if (PREDICT_FALSE (b->flags & VNET_BUFFER_F_LOCALLY_ORIGINATED))
  {
    vnet_buffer (b)->ip.adj_index[VLIB_RX] =
      vnet_buffer (b)->ip.adj_index[VLIB_TX];
    return;
  }

/* src/vnet/ip/ip4_forward.c - ip4_local_check_src_x2() */
if (PREDICT_FALSE ((b[0]->flags | b[1]->flags) &
                   VNET_BUFFER_F_LOCALLY_ORIGINATED))
  {
    ip4_local_check_src (b[0], ip[0], last_check, &error[0], is_receive_dpo);
    ip4_local_check_src (b[1], ip[1], last_check, &error[1], is_receive_dpo);
    return;
  }

/* src/vnet/ip/ip6_forward.c - ip6_urpf_loose_check() */
if (PREDICT_FALSE (b->flags & VNET_BUFFER_F_LOCALLY_ORIGINATED))
  return 1;
```

语义：反欺骗/uRPF 检查针对的是**从接口收到的包**，本机协议栈自己生成的包不可能伪造本机源地址，放行是安全的。现有代码已经在 TTL/跳数处理里用同一个标记区分本机包（`ip4_ttl_inc`、`ip6_rewrite`）。

实测：打补丁后 `nginx(VCL) -> 10.255.0.1:8080` 返回 200，`show errors` 无 spoof 计数，媒体路径全程在 VPP 内。

#### nginx 上游

- 播放：`10.255.0.1:8080`（VPP loop0，**纯 VPP**）
- API：`10.10.0.2:8096`，内核 DNAT 到 `127.0.0.1:8096`。真实 Emby 是内核里的 Docker 进程，这一段无法避免出 VPP，但它只承载 API/元数据，不承载视频流。

```bash
sysctl -w net.ipv4.ip_forward=1
sysctl -w net.ipv4.conf.vpp-tun.route_localnet=1
iptables -t nat -A PREROUTING -i vpp-tun -p tcp -d 10.10.0.2 --dport 8096 -j DNAT --to-destination 127.0.0.1:8096
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
  corelist-workers 2-3
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
  v4-session-table-buckets 1024
  v4-halfopen-table-buckets 1024
  wrk-mq-length 4096
}

tcp {
  cc-algo bbr
  rack
  byte-tracker
  mtu 1500
  tso
}

plugins {
  plugin af_packet_plugin.so { enable }
  plugin http_plugin.so { enable }
  plugin http_static_plugin.so { enable }
}
```

**不要写顶层 `plugin_path`**。Phase 0 实测：release + LTO 构建下，顶层 `plugin_path` 会在 `vlib_plugin_early_init()` 里以约 50% 概率启动段错误。VPP 会按自身二进制位置自动推导 `<prefix>/lib/x86_64-linux-gnu/vpp_plugins`，`build-vpp-lab-install/bin/vpp` 直接就能找到插件。如果确实要指定，用 `plugins { path <dir> }` 子选项，不要用顶层 `plugin_path`。

要点：

- `main-core 1` + `corelist-workers 2-3`：main 在 CPU 1，两个 worker 在 CPU 2/3，都在 NUMA 0；避开 CPU 0 给内核/中断。DPDK 侧用 `num-rx-queues 2` 并启用 RSS 才能把流量分到两个 worker。
- 当前版本没有 `session evt-threads` 配置项；session 事件处理跟随 VPP worker。不要设置 `preallocated-sessions`：它会把 worker session pool 固定为该上限；不设置时 pool 会通过 `pool_program_safe_realloc_rpc` 动态扩展。
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

# loop0：internal media app 的内部地址，纯 VPP
create loopback interface instance 0
set interface state loop0 up
set interface ip address loop0 10.255.0.1/32
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
| `10.10.0.2` | 内核 `vpp-tun` | API upstream 的目标地址（DNAT 到 127.0.0.1:8096） |
| `10.255.0.1:8080` | internal media app（绑 `0.0.0.0:8080`） | nginx 的播放 upstream，纯 VPP |

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

#### 插件边界与 206 支持

不要把媒体业务写进 `http_static`。分层如下：

| 层 | 组件 | 职责 |
|---|---|---|
| 传输 | `http` 插件 + session | HTTP 解析/序列化、TLS、连接管理 |
| 通用静态文件 | `http_static` | 目录映射、缓存、通用 Range/206 语义 |
| 业务 | 独立插件 `media_server` | itemId -> 路径索引、`/media` `/hls` 路由、路径安全、鉴权头 |

`media_server` 是独立插件（`src/plugins/media_server/`），通过 `http_static` 的公开接口 `hss_register_url_handler()` 挂 URL，业务代码不进入 `http_static`。

**206 现状**：`http` 层已经定义了 `HTTP_STATUS_PARTIAL_CONTENT`、`HTTP_HEADER_CONTENT_RANGE`、`HTTP_HEADER_ACCEPT_RANGES`、`HTTP_HEADER_RANGE`，但还缺两块通用能力：

1. `http_static` 的静态文件路径没有解析 Range。需要补：解析 `Range: bytes=a-b`，返回 `206` + `Content-Range`；无 Range 时返回 `Accept-Ranges: bytes`；越界返回 `416`。
2. `hss_url_handler_args_t` 的 reply 只能设置 `sc`/`ct`/`data`，不能加 `Content-Range`。需要给 reply 增加 `headers` 字段（预格式化 header 块），让任何 handler 都能设置响应头。`sc` 已经能设 `206`。

这两处都是 HTTP 语义，与媒体无关；补完后独立插件就能直接做 206，不需要改 `http_static` 的业务代码。

**media_server 职责**（独立插件）：

- `GET /health`：返回 `{"status":"ok"}`。
- `GET /media/<itemId>[/<file>]`：查内存索引 -> 打开文件 -> 按 Range 做 `pread`。
- `GET /hls/<id>/<file>`：返回 `.m3u8`/`.m4s`/`.ts`。
- 只做业务：索引、路径校验（拒绝 `..`、绝对路径、符号链接逃逸）、Range 参数换算；HTTP 解析/序列化交给 `http`/`http_static`。
- 分块读取并发送，不要把整部电影读进内存。
- `.m3u8`/`.m4s`/`.ts`/`.mp4` 设置正确 Content-Type。
- 不在 VPP worker 里做 ffmpeg/扫描；HLS 由外部 systemd 任务预生成。

**构建**：

```cmake
add_vpp_plugin(media_server
  SOURCES
  media_server.c
  LINK_LIBRARIES
  http vnet vppinfra
)
```

**注册**（实现已落地在 `src/plugins/media_server/`）：

```c
/* http1 解析出的 target_path 不带前导 '/'，注册键也必须不带 */
extern void hss_register_url_handler (hss_url_handler_fn fp, const char *url,
				      int is_post);
extern void hss_register_url_handler_prefix (hss_url_handler_fn fp,
					     const char *prefix, int is_post);

hss_register_url_handler (media_server_get_handler, "health", 0);
hss_register_url_handler_prefix (media_server_get_handler, "media/", 0);
hss_register_url_handler_prefix (media_server_get_handler, "hls/", 0);
```

两个容易踩的点：

- listener 必须带 `url-handlers`（`http static listener add ... url-handlers`），否则 `try_url_handler` 根本不会被调用。
- 注册键不要写 `/media/`，要写 `media/`；http1 解析器交给 app 的 `target_path` 是 `"media/x"`。

**Range 由 transport 统一处理**：handler 只需要返回完整 body（`args->data`/`args->data_len`）+ `200`，`hss_session_send_data()` 会读请求里的 `Range`，自动切成 `206` + `Content-Range`，越界返回 `416`。业务插件不解析 Range。

已实测（`build-vpp-lab`）：`health` 返回 200，`media/test.mp4` 的 `Range: bytes=100-199` 返回 `206` + `Content-Range: bytes 100-199/4194304`，`--path-as-is /media/../etc/passwd` 返回 404。

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
| WAN 网卡 | 内核持有，不动 | 6.8 用 `af_packet` 旁路；≥6.14 用 `af_xdp` 接管 `enp2s0f0` |
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

3. 数据面接管 `enp2s0f0`：

**当前内核 6.8：用 af_packet**（功能可用，内核仍能收包，SSH 不会断）：

```text
create host-interface name enp2s0f0
set interface state host-enp2s0f0 up
set interface ip address host-enp2s0f0 184.107.84.201/26
ip route add 0.0.0.0/0 via 184.107.84.254 host-enp2s0f0
```

**内核 ≥6.14：用 af_xdp**：

```text
create interface af_xdp host-if enp2s0f0 name wan0 zero-copy mac-reuse num-rx-queues all
set interface state wan0 up
set interface ip address wan0 184.107.84.201/26
ip route add 0.0.0.0/0 via 184.107.84.254 wan0
```

两个必须注意的点：

- `num-rx-queues` 必须显式写 `all`（或具体数字）。不写时默认是 0，VPP 会创建 0 个 RX 队列，`show interface` 里 rx 永远为 0。
- 接口 MTU 必须是 1500。igb 的 XDP 不支持 jumbo，MTU 9000 时 attach 会报 `RX buffer size is too small`，并且触发 af_xdp 失败路径的空指针崩溃。

**7.0 内核复测（2026-10-20）**：已安装 HWE `7.0.0-38-generic`，`xdp-loader features` 返回 `NETDEV_XDP_ACT_XSK_ZEROCOPY: yes`。但 VPP 的 af_xdp 插件仍然 **0 RX**：

- `xdp_dispatcher` + `xsk_def_prog` 以 native 模式挂载；
- `xsks_map` 有 8 个条目，程序的 `map_ids` 指向同一个 map（66）；
- `af_xdp-input` 节点在 polling，调用 720 万次，但 `xsk_ring_cons__peek` 始终是 0；
- 去掉 `XDP_USE_NEED_WAKEUP` 后仍是 0；
- 同一张网卡上 `af_packet` 正常收包（3 秒 331 包）。

已用独立 XSK 测试程序进一步定位：

- 独立程序绑定 8 个队列，用 `XDP_COPY` + `xsk_def_xdp_prog.o`，在 queue 0 上收到了 3 个包。**内核和 igb 的 AF_XDP 本身是正常的**，队列覆盖也不是问题（8 个队列全绑，流量确实落在 queue 0 的 XSK 上）。
- 差异在 UMEM：独立程序用 **frame-aligned** 的独立 mmap 区；VPP 插件把 **VPP buffer pool** 直接注册成 UMEM，并设置 `XDP_UMEM_UNALIGNED_CHUNK_FLAG`。igb 的 XSK 路径不接受这种非对齐 chunk，所以 `xsk_ring_cons__peek` 永远为 0。
- 去掉 `XDP_UMEM_UNALIGNED_CHUNK_FLAG` 后，`xsk_umem__create()` 因为 VPP buffer 地址不是 frame-aligned 而失败，插件在错误路径上还会触发 `vnet_delete_hw_interface` 的空指针崩溃。

结论：**VPP 的 af_xdp 插件需要改成独立的 frame-aligned UMEM（在 UMEM 和 VPP buffer 之间做一次 copy），或者改用 DPDK**；当前内核无需再升级。Phase 2 先用 `af_packet`。

**为什么当前 6.8 不能用 AF_XDP**：Linux 6.13 及以前的 igb 驱动**完全没有 AF_XDP 代码**（没有 `xsk`、没有 `ndo_xsk_wakeup`），6.14 才加入（含 zero-copy）。6.8 上 XDP 程序能挂成 native，但 XSK redirect 不工作：VPP rx=0，内核也收不到包，WAN 直接断。已实测排除队列数（8/1 队列）和 memlock（4GB）因素。

**SSH punt**：af_packet 下内核仍能收包，SSH 不受影响；切到 AF_XDP 后必须按第 6 节配 `vpp-punt` + `set punt tcp all` + `ip punt redirect`，否则 SSH 直接断。

**切换 watchdog**：见 6.2.1，切换失败会自动回滚。

4. 内部 app 地址（纯 VPP）+ 一条只给 API 用的内核 TUN：

```text
# 播放：nginx -> internal app，纯 VPP
create loopback interface instance 0
set interface state loop0 up
set interface ip address loop0 10.255.0.1/32

# API：真实 Emby 在内核里，必须留一条出口 TUN
create tap id 1 if-name svc0 host-if-name vpp-svc tun host-mtu-size 1500
set interface state svc0 up
set interface ip address svc0 10.40.0.1/24
```

内核侧：

```bash
ip link set vpp-svc up
ip addr add 10.40.0.2/24 dev vpp-svc
sysctl -w net.ipv4.ip_forward=1
sysctl -w net.ipv4.conf.vpp-svc.route_localnet=1
iptables -t nat -A PREROUTING -i vpp-svc -p tcp -d 10.40.0.2 --dport 8096 -j DNAT --to-destination 127.0.0.1:8096
```

nginx 的 `emby_api` upstream 用 `10.40.0.2:8096`；`vpp_media` 仍用 `10.255.0.1:8080`。

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

注意：Phase 2 直接用 AF_XDP（≥6.14 优先 zero-copy，6.8 回退 copy），`af_packet` 只用于 Phase 0 lab。SSH punt 不是 Phase 3 的事，必须在 Phase 2 一起做完并验证。

nginx VCL 配置骨架：

> 与 3.1 的 Phase 1 配置相比，**只有 `listen` / `server_name` / 证书三处不同**，路由表和播放上游原样保留；`emby_api` 改成 Phase 2 出口 TUN 的 `10.40.0.2:8096`。

```nginx
upstream emby_api  { server 10.40.0.2:8096; keepalive 32; }
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
    location ~ ^/(emby/)?Videos/[^/]+/(.+)$         { proxy_pass http://vpp_media; }
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

### 3.3 Phase 3：性能与加固（可选）

Phase 2 已经用 AF_XDP（zero-copy 或 copy）接管 WAN，SSH punt 也已启用。Phase 3 只做可选优化：

- 压测 `af_xdp` 的 pps/CPU（先确认 `xdp-loader features` 是否给出 zero-copy），按需增加 worker、调整 RX 队列和 `buffers-per-numa`。
- 评估 DPDK：唯一能去掉拷贝的方案，但会独占网卡；单网卡机器上 SSH/管理流量必须全部走 punt，风险高。1GbE 场景不建议。
- 评估 linux-cp：让内核接管 ARP/ND，减少手工邻居配置；SSH 的 TCP punt 规则仍然必须显式配置和验证。
- SSH punt 回归测试（Phase 2 已启用，Phase 3 只验证和加固）。

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

#### Phase 2 数据面：DPDK 26.07

AF_XDP 在这个组合上放弃：VPP 的 `vlib_buffer_pool_t.alloc_size` 与 AF_XDP 的 `frame_size` 不一致（VPP 的 `vlib_buffer_alloc_size()` 会 `|= VLIB_BUFFER_ALIGN`），igb 又不消费非对齐 chunk。改用 DPDK。

**安装 DPDK（系统级，不触发 VPP external 重编）**

```bash
curl -fL https://fast.dpdk.org/rel/dpdk-26.07.tar.xz -o /tmp/dpdk-26.07.tar.xz
tar -xf /tmp/dpdk-26.07.tar.xz -C /opt
# 关键补丁：16 个 ops 槽会被 PMD 注册满，VPP 的 "vpp" mempool ops 注册失败
sed -i 's/#define RTE_MEMPOOL_MAX_OPS_IDX 16/#define RTE_MEMPOOL_MAX_OPS_IDX 64/' \
  /opt/dpdk-26.07/lib/mempool/rte_mempool.h
cd /opt/dpdk-26.07
PATH=/usr/bin:$PATH meson setup build --prefix=/opt/dpdk --buildtype=release \
  -Dplatform=generic -Denable_docs=false -Dtests=false
PATH=/usr/bin:$PATH ninja -C build -j$(nproc)
PATH=/usr/bin:$PATH ninja -C build install
echo /opt/dpdk/lib/x86_64-linux-gnu > /etc/ld.so.conf.d/dpdk.conf && ldconfig
```

**VPP 配置与构建**

```bash
PKG_CONFIG_PATH=/opt/dpdk/lib/x86_64-linux-gnu/pkgconfig \
cmake -G Ninja -DCMAKE_BUILD_TYPE=release \
  -DVPP_USE_SYSTEM_DPDK=ON -DDPDK_SRC_DIR=/opt/dpdk-26.07 \
  -DVPP_PLUGINS=af_packet,tap,http,http_static,media_server,nat,linux_cp,linux_cp_unittest,linux_nl,ip_session_redirect,dpdk \
  -S src -B build-vpp-lab
ninja -C build-vpp-lab
```

`src/plugins/dpdk/CMakeLists.txt` 的 system-DPDK 分支额外加了 DPDK 源码树里的内部头文件路径和
`link_directories(${DPDK_LIBRARY_DIRS})`，否则会误链系统旧版 DPDK。验证：

```bash
readelf -d build-vpp-lab/lib/x86_64-linux-gnu/vpp_plugins/dpdk_plugin.so | grep rte_eal
# 应为 librte_eal.so.26
```

**绑定与启动**

```bash
modprobe vfio-pci
dpdk-devbind.py --bind=vfio-pci 0000:02:00.1    # enp2s0f1，测试口
```

```text
dpdk {
  dev 0000:02:00.1
}
```

```text
set interface state GigabitEthernet2/0/1 up
```

实测：1 Gbps、carrier up full duplex、RX 计数增长。`enp2s0f0`（WAN）仍由内核 `igb` 持有，未动。

**f0 实测切换（已验证）**

DPDK 启动时会跳过 host interface 仍是 `IFF_UP` 的 PCI 设备（`vlib_pci_bind_to_uio`），所以顺序必须是：

1. `ip link set enp2s0f0 down`
2. 启动 VPP（`dpdk { dev 0000:02:00.0 }`），DPDK 绑定 f0，得到 `GigabitEthernet2/0/0`
3. VPP 内：
   ```text
   create tap id 10 if-name punt0 host-if-name vpp-punt tun host-mtu-size 1500
   set interface state punt0 up
   set interface ip address punt0 10.30.0.1/30
   set punt tcp all
   set punt udp all
   ip punt redirect add rx GigabitEthernet2/0/0 via 10.30.0.2 punt0

   set interface state GigabitEthernet2/0/0 up
   set interface ip address GigabitEthernet2/0/0 184.107.84.201/26
   ip route add 0.0.0.0/0 via 184.107.84.254 GigabitEthernet2/0/0
   ip neighbor GigabitEthernet2/0/0 184.107.84.254 00:1c:73:00:00:02
   ```
4. 内核：
   ```bash
   ip link set vpp-punt up
   ip addr add 184.107.84.201/32 dev vpp-punt
   ip addr add 10.30.0.2/30 dev vpp-punt
   ip route add default via 10.30.0.1 dev vpp-punt src 184.107.84.201
   sysctl -w net.ipv4.conf.vpp-punt.rp_filter=2
   ```
   源地址必须显式 `src 184.107.84.201`，否则内核会用 10.30.0.2 出 WAN。
5. DNS：f0 消失后 systemd-resolved 没有上游，必须把上游挂到 vpp-punt：
   ```bash
   resolvectl dns vpp-punt 23.19.53.53 23.19.52.52
   resolvectl domain vpp-punt '~.'
   resolvectl default-route vpp-punt yes
   ```
   漏了这步的表现是：`ping` 网关正常，但 `curl https://域名` 返回 000。

实测结果：

- DPDK f0：1 Gbps、carrier up full duplex；
- `tcp4-input Packets punted` 6000+，SSH 会话 `184.107.84.201:22` established；
- `curl https://api.deepseek.com` 返回 401，DNS 正常；
- 本机 Emby `127.0.0.1:8096` 仍返回 200。

回滚：

```bash
pkill -x vpp_main
ip link del vpp-punt 2>/dev/null
dpdk-devbind.py --bind=igb 0000:02:00.0
ip link set enp2s0f0 up
ip addr add 184.107.84.201/26 dev enp2s0f0
ip route add default via 184.107.84.254 dev enp2s0f0 onlink
```

watchdog 已按这套回滚实现，切换失败 30 秒内自动恢复。

#### Linux-CP 与 af_packet/af_xdp 的关系

两者不是二选一，是不同层：

| 组件 | 层 | 作用 |
|---|---|---|
| `af_packet` / `af_xdp` | 数据面驱动 | 把 Linux netdev（如 `enp2s0f0`）接进 VPP |
| `linux_cp` + `linux_nl` | 控制面镜像 | 给 VPP 接口创建 Linux TAP/TUN 镜像，把 ARP/ND 等未转发流量 punt 给内核，并用 netlink 同步 IP 配置 |

- 当前 lab（`tap` 插件 TUN）不需要 linux-cp，两端地址手工配好即可。
- Phase 2 用 `af_xdp` 接管 WAN 时，可以叠加 linux-cp 让内核继续处理 ARP/ND，但不是必须。
- Phase 3 切 AF_XDP 后，linux-cp 能覆盖 ARP/ND/未转发流量的 punt，但 **SSH 的 TCP punt 仍必须显式配置**，不能依赖 linux-cp。
- 本仓库支持：`src/plugins/linux-cp/` 提供 `linux_cp`（CLI/API）和 `linux_nl`（netlink 同步）两个插件，状态是 experimental。
- 构建时要显式加：`-P af_xdp,tap,nat,linux_cp,linux_nl`，并安装 `libnl-3-dev`、`libmnl-dev`；缺依赖时 CMake 只打 warning，插件会被静默跳过。
- 用法：`lcp create enp2s0f0 host-if wan0`，然后在 Linux 侧配置 `wan0`。

### 5.2 Hugepages

当前 hugepages 为 0。建议先分配 4 GB（2 MB × 2048）：

```bash
sysctl -w vm.nr_hugepages=2048
```

如需持久化，写入 `/etc/sysctl.d/80-vpp.conf`。Emby 转码和剩余容器也要内存，不要一次分配过多。

### 5.3 startup.conf 草案

Phase 1/2 的完整配置见 3.1：**一个 main + 两个 worker**（`main-core 1`、`corelist-workers 2-3`）、独立 API/stat/runtime、256M main heap、32768 buffers、动态 session pool、`tcp cc-algo bbr rack byte-tracker tso`。Phase 3 只把 dataplane 插件换成 AF_XDP：

```text
plugins {
  plugin af_xdp_plugin.so { enable }
  plugin tap_plugin.so { enable }
  plugin nat_plugin.so { enable }
}
```

`enp2s0f0` 和 CPU 1/2/3 都在 NUMA 0。功能验证用双 worker；VCL app 本身仍可能是单线程，连接数上限和聚合吞吐要分开评估。

### 5.4 vppctl 配置草案

```text
# WAN
create interface af_xdp host-if enp2s0f0 name wan0 num-rx-queues 4 zero-copy mac-reuse
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

### 6.2.1 WAN 切换 watchdog

`/usr/local/sbin/wan-watchdog.sh` + `wan-watchdog.service`（已安装并 enable）：

- 每 10s 检查一次 `https://api.deepseek.com`（失败再回退到 gateway ping）。
- 只有 `/run/wan-cutover` 存在时才生效，正常环境不会误杀。
- 连续 3 次失败：kill VPP、`xdp-loader unload enp2s0f0 -a`、`ip link set enp2s0f0 xdp off`、恢复 f0 地址/默认路由、删除标记。
- 日志：`/var/log/wan-watchdog.log`。
- 实测：AF_XDP 切换失败时约 30s 自动回滚，WAN 恢复。

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

1. **AF_XDP**：内核 ≥6.14 可 zero-copy；6.8 只能 copy，CPU 拷贝成本需要实测。1Gbps 是网卡上限。
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

### E.3 VPP 内部实现（独立 `media_server` 插件）

`http_static` 只作为 HTTP transport 和通用静态文件层；媒体业务放独立插件：

```text
src/plugins/media_server/
  media_server.c        # 路由、Range 参数换算、文件发送
  media_index.c         # index.json 加载/刷新
  CMakeLists.txt        # add_vpp_plugin(media_server ...)
```

核心逻辑：

1. 通过 `hss_register_url_handler()` 注册 `/media/`、`/hls/`、`/health`；注册前必须先 enable `http_static`，否则 handler 会丢。
2. 严格限制根目录为 `/srv/media`，拒绝 `..`、绝对路径、符号链接逃逸。
3. Range/206 是 transport 的通用能力：`http_static` 解析 `Range` 并返回 206，handler 通过 reply 的 `headers` 设置 `Content-Range`。业务插件只负责把 `start-end` 换算成文件偏移。
4. 分块读取文件并通过 transport 发送；不要在 worker 里一次性读几 GB。
5. 对 `.m3u8`、`.m4s`、`.ts`、`.mp4` 设置正确 `Content-Type`。
6. 大文件不要依赖 `http_static` 的 LRU 缓存；缓存只用于小文件/索引。

### E.4 启用 internal HTTP server

Phase 0 验证了 CLI 可直接启用：`http static server ...` + `http static listener add ...`。需要自动化时也可以用二进制 API（`http_static_enable_v5`）：

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

## 13. 2026-10-22 最终验证配置

本节记录当前实际跑通的环境，优先于前面的早期草案。

### 13.1 f0 DPDK + 双 worker

```text
cpu { main-core 1 corelist-workers 2-3 }

session {
  enable
  v4-session-table-buckets 1024
  v4-halfopen-table-buckets 1024
  wrk-mq-length 4096
}

tcp { cc-algo bbr rack byte-tracker mtu 1500 tso }

dpdk {
  enable-tcp-udp-checksum
  dev 0000:02:00.0 {
    num-rx-queues 2
    num-rx-desc 2048
    num-tx-desc 4096
    tso on
  }
}
```

关键点：

- 不要设置 `preallocated-sessions`。该参数会调用 `pool_init_fixed()`，把 worker session pool 固定成硬上限；不设置时 pool 通过 `pool_program_safe_realloc_rpc` 动态扩展。
- `num-rx-queues 2` 同时启用 RSS；否则第二个 worker 没有 RX 队列。
- `enable-tcp-udp-checksum` 必须显式开启，否则 DPDK 日志会报 `TCP/UDP checksum offload must be enabled`，即使 `tso on` 也不会把 `tcp-tso` 置为 active。
- 当前 `show hardware-interfaces` 的 TX active 包含 `tcp-tso`；tap `punt0` 的 `gso-enabled/csum-enabled/packet-coalesce` 为 1。
- i350 的 DPDK RX 没有 `tcp-lro/gro` capability；WAN 侧无法开 GRO，只能开 TSO 和 tap 侧 GSO/coalesce。

### 13.2 VCL 与连接规模

VCL 每个 session 的 fifo 当前为 8MB。4MB 和 8MB 都实测过，单条长流没有可见提升；继续加到 16MB 会显著增加 200 连接时的 app segment 压力，不建议无脑加大：

```text
vcl {
  rx-fifo-size 8000000
  tx-fifo-size 8000000
  api-socket-name /run/vpp-dpdk/api.sock
  app-socket-api /run/vpp-dpdk/app_ns_sockets/default
  use-mq-eventfd
}
```

实测结论：

- 200 条并发小请求（每条 64KB Range）全部成功，session pool 动态扩展，没有 fixed-size pool OOM。
- **外部 144 内核 TCP 客户端**跑 200 条并发 8MB 长流：195 条成功，5 条在约 4–5MB 处 60s 超时。nginx 对 5 条的 upstream 耗时也约 61s；这是客户端/链路长尾导致 nginx 反压，不是 session 连接数上限。
- 本机 `LD_PRELOAD` VCL 客户端跑 200 条长流不能作为服务端容量结论：客户端和服务端都在同一台 VPP 上，会同时占用 VCL app worker 和 fifo。此前本机测试的 140 条失败应视为测试污染。
- nginx 可以 master + 2 workers，但**不能加 `reuseport`**：那会让每个 worker 各自向 VCL bind，报 `ip port pair already listened on`。正确做法是 `master_process on; worker_processes 2;`，listen 不带 `reuseport`，由 master 建一个 VCL listener、worker 继承：

```text
master_process on;
worker_processes 2;
worker_rlimit_nofile 65535;
events { worker_connections 4096; use epoll; }
listen 184.107.84.201:443 ssl http2;
```

- nginx 有两个 worker 时，VCL app session 仍主要落在一个 VPP worker（`show session` 可见 Thread 1 几百条、Thread 2 只有极少）；这会影响极高聚合吞吐，但不能据此断言 200 条外部长流必然失败。外部 195/200 的结果说明连接数和 session pool 已经足够，剩余 5 条需要从客户端/链路侧继续定位（`ss -tin` 的 rwnd/cwnd/retrans、分多台客户端复测）。

### 13.3 Emby API DNAT 与 SSH punt

VPP 接管 f0 后，Emby API 上游必须保留 `vpp-punt` DNAT：

```bash
sysctl -w net.ipv4.conf.vpp-punt.route_localnet=1
iptables -t nat -A PREROUTING -i vpp-punt -p tcp -d 10.30.0.2 --dport 8096 \
  -j DNAT --to-destination 127.0.0.1:8096
```

该规则已固化到 `/usr/local/sbin/vpp-f0-start.sh` 调用的 `vpp-emby-dnat.sh`。漏掉时表现为 Emby `/System/Info/Public` 502，但 media 8080 仍正常。

SSH 走 `set punt tcp all` + `ip punt redirect` 到 `vpp-punt`。**重启 VPP 必然断开 SSH**，因为 punt TAP 和主机路由会消失；生产环境应通过 `enp2s0f1` 独立管理口、IPMI 或串口操作，不能只依赖 f0 上的 SSH。

### 13.4 heap 生命周期与 show memory

`show memory` 的 SIGSEGV 根因不是并发锁：

- CLI 命令默认不是 mp-safe，vlib 已自动持有 worker barrier。
- `svm` / `ssvm` / private API segment 在 `munmap` 前没有调用已有的 `clib_mem_destroy_heap()`，导致已 unmap 的 heap 指针留在 `clib_mem_main.heaps`。
- 修复方式是生产者路径在 unmap 前注销 heap，不新增全局锁、不新增 `__clib_export`、不改 `show memory` 的生命周期语义。

### 13.5 验证基线

- `codex` 登录 Emby：HTTP 200
- `/emby/System/Info/Public`：HTTP 200
- `/emby/videos/21082/original.mp4` Range：HTTP 206
- VPP、VCL nginx、watchdog 同时运行
- WAN `https://api.deepseek.com`：401
- hugepages：`vm.nr_hugepages=1024`，持久化到 `/etc/sysctl.d/99-vpp-hugepages.conf`

## 14. 多用户生产架构与 CDN

### 14.1 结论：控制面与数据面拆开

当前 `客户端 -> VPP host stack -> VCL nginx -> VPP media plugin` 拓扑适合验证“播放字节确实走 VPP、Range/206 正确、SSH punt 可用”，但不适合直接作为多用户公网入口。原因是数据面被收窄成一个 VCL nginx app worker，TLS、HTTP、文件读取和大规模并发全部压在同一个 VPP app worker 上；继续调 fifo、增 nginx worker 或加 VPP worker，都不能改变这个串行点。

生产建议把职责拆开：

- **控制面**：Emby 用 systemd 运行，只负责登录、鉴权、元数据、`PlaybackInfo`、进度同步和转码决策。
- **数据面**：direct play 的文件字节不再经过 Emby。由独立 edge 服务读取本地盘/NFS/对象存储，使用内核 `sendfile`、`aio` 或多进程/多线程文件服务发送。
- **VPP 定位**：负责 WAN 的 L3/L4、NAT、SSH punt、ACL、限速和故障切换；如果必须由 VPP 终止 TCP，则需要单独的 host-stack 扩展设计，不能沿用当前单 VCL nginx 作为唯一 TLS 前端。
- **CDN**：只缓存可缓存的媒体对象和 HLS 分片，不缓存 Emby 登录、鉴权、用户信息和动态 API。

当前 lab 的 Emby 仍是 Docker 容器 `av-emby`，这是验证环境；生产基线是 systemd Emby。两者不要混写，也不要求在验证阶段先迁移 Docker。

### 14.2 推荐拓扑

```text
客户端
  │ HTTPS + Range
  ▼
DNS / GeoDNS / CDN
  │ 签名 URL、Range/206、缓存
  ▼
L4 LB / HAProxy（多 edge 时）
  │
  ├── edge-1: 内核 nginx 多 worker ──┐
  ├── edge-2: 内核 nginx 多 worker ──┤
  └── edge-N: 内核 nginx 多 worker ──┤
                                      ▼
                     本地 NVMe / NFS / MinIO / S3

Emby（systemd）
  ├── /emby/*        登录、元数据、PlaybackInfo、进度
  ├── /Items/*       媒体库浏览
  └── 转码 worker    需要转码时输出 HLS
```

URL 按职责分开，避免把动态 API 和可缓存媒体混在同一个 location：

```text
/emby/*        -> Emby 控制面，默认 no-store，不进入 CDN 缓存
/media/*       -> edge 文件服务，支持 Range/206，可缓存
/hls/*/*.m3u8  -> 播放列表，短 TTL 或 no-cache
/hls/*/*.ts    -> HLS 分片，内容寻址后可长期缓存
```

单机也可以落地，但不要把“公网 TLS 终止”继续放在 VCL nginx 里：

- **单机、优先稳定**：VPP 继续接管 `enp2s0f0`，把 443 和 SSH 一起 punt 给内核；内核 nginx 多 worker 负责 TLS 和文件发送。VPP 仍做 NAT、ACL 和链路切换，但 HTTP 不再经过 VPP host stack。
- **单机、坚持 VPP 终止 TCP**：需要把 external app 拆成多个可独立调度的实例，并解决 VCL listener 与 app worker 的分配问题；这是独立的 host-stack 工程，不是当前 lab 的配置调整。
- **多机、VPP 必须接管 WAN**：VPP 作为边缘路由器，把流量转发到独立 edge 主机；edge 主机用内核 nginx 多 worker。不要把文件和 TLS 再塞回 VPP 单 app worker。

### 14.3 鉴权与 `itemId -> path` 索引

direct play 的关键不是让 nginx 直接暴露文件路径，而是建立一个受控的媒体索引：

1. 客户端先通过 Emby 完成登录，拿到 Emby 用户身份。
2. 控制面调用 Emby API 获取 `PlaybackInfo`，确认该用户能播放哪个 `itemId`、是否需要转码。
3. 控制面签发短 TTL 的播放 token，内容至少包含 `userId`、`itemId`、`exp`、`deviceId`，用 HMAC 签名；不要把永久 Emby API key 放进 URL。
4. edge 在本地校验 token，不要求每个 Range 请求都回源 Emby。
5. edge 用 `itemId` 查询索引，得到不可猜测的媒体 ID 或对象 key，再映射到真实文件；真实文件系统路径不返回给客户端。
6. nginx 通过 `X-Accel-Redirect` 进入 internal location 发送文件，或由 Go/Rust 文件服务直接 `sendfile`。

索引可以由 `src/scripts/emby-index.py` 先做全量 bootstrap，生产再增加增量同步：媒体库扫描完成、文件替换、条目删除时更新 `itemId -> object key -> size/mtime/etag`。索引只保存元数据和对象 key，不把大文件塞进数据库。

如果暂时不做独立控制面，也可以让 nginx `auth_request` 调 Emby 校验；但要加本地 token 缓存，否则每个 Range 请求都会打 Emby，无法支撑多用户拖动进度。

### 14.4 容量计算：先算出口带宽，再谈 VPP

链路带宽和 TCP 在途数据量必须先分开算：

```text
1 Gbps = 125 MB/s（理想值，实际可用按 110-115 MB/s 估算）
BDP(Bytes) = 带宽(B/s) × RTT(s)
```

1 Gbps 链路的 BDP 参考：

| RTT | BDP | 单流接收窗口建议 |
|---:|---:|---:|
| 20 ms | 2.5 MB | 4-8 MB |
| 69 ms | 8.6 MB | 16-32 MB |
| 150 ms | 18.75 MB | 32 MB 或更高 |

这里的窗口是**单条长流**所需，不是每用户固定分配。100 条同时跑满的长流不能简单乘以 32MB；真正能同时跑满的流数由出口带宽决定。客户端 `rmem` 太小会把单流吞吐限制在 `窗口 / RTT`，这与 VPP session 数无关；此前 144 客户端提高接收 buffer 后单流跑满，已经验证了这一点。

并发用户数按码率和余量估算：

```text
N ≈ 可用带宽(Mbps) × 0.7 / 单用户码率(Mbps)
```

1 Gbps 出口的粗略上限：

| 内容 | 单用户码率 | 70% 带宽下的理论并发 |
|---|---:|---:|
| 1080p direct play | 8 Mbps | 约 87 |
| 1080p 高码率 | 20 Mbps | 约 35 |
| 4K | 40 Mbps | 约 17 |
| 4K 高码率/HDR | 80 Mbps | 约 8 |

这些数字是链路上限，不是 Emby 或 VPP 的处理上限。实际还要扣掉：

- 重传、协议开销和突发流量，按 20%-30% 余量预留。
- 转码输出通常比源文件更高码率；转码和 direct play 不能共用同一份带宽预算。
- 多用户同时拖动会产生 Range 请求和短时突发，CDN/edge 要有缓存和限速。
- 机械盘随机读在多条 4K 流下会先于网络成为瓶颈；热门内容放 NVMe 或对象存储。
- Emby 的扫描、刮削、转码和 API 不应与文件发送争同一批 CPU/IO。

### 14.5 VPP 在当前架构中的边界

| 方案 | 数据路径 | 适用场景 | 主要问题 |
|---|---|---|---|
| 当前 VCL nginx + VPP media app | 公网 -> VPP session -> VCL nginx -> VPP internal app | 单用户/少量用户验证、协议和 Range 验证 | TLS、HTTP、文件发送集中在一个 app worker；不适合多用户公网入口 |
| VPP L3/L4 + 443/SSH punt 到内核 nginx | 公网 -> VPP NAT/punt -> 内核 nginx 多 worker -> 本地盘 | 单机、1 Gbps、优先稳定 | HTTP 不走 VPP host stack，但内核 nginx 很容易跑满 1 Gbps |
| VPP 边缘路由器 + 独立 edge 主机 | 公网 -> VPP -> LAN -> edge nginx -> 存储 | 多机、需要 VPP 接管 WAN | 需要独立 LAN/管理网络和 edge 运维 |
| 内核 nginx 直接暴露 | 公网 -> 内核 nginx -> 存储 | 回滚、对照基线 | 放弃 VPP 数据面能力 |

如果目标只是“让客户端 TCP 走 VPP”，当前 lab 已经证明可行；如果目标是“多用户稳定看片”，应优先选择后两种，把 VPP 留在它擅长的 L3/L4 和 punt 路径上。不要在数据面里增加 `clib_warning`，也不要用固定 `preallocated-sessions` 掩盖容量问题。

生产机的 SSH 不能只依赖被 VPP 接管的 `enp2s0f0`。应使用独立的 `enp2s0f1` 管理地址、IPMI 或串口；VPP 重启时 punt TAP 和主机路由会消失，f0 上的 SSH 必然断，watchdog 只能负责恢复，不能保证会话不断。

### 14.6 CDN 设计

#### 14.6.1 direct play 与 HLS 分开

- **direct play**：通常是完整 MP4/MKV，支持 `Range` 和 `206`。对象不变时最适合 CDN 缓存整对象，再由 CDN 从完整对象切片响应 Range。
- **HLS**：`m3u8` 是短生命周期索引，`.ts`/`.m4s` 分片才是大流量。点播分片应内容寻址并长期缓存；直播或按会话转码的播放列表不能当公共静态文件缓存。

不要把 `/emby/Items/*`、登录接口、播放进度接口放进 CDN 缓存。它们可能包含用户身份和动态状态。

#### 14.6.2 签名 URL 与缓存 key

推荐使用短 TTL 的签名 URL 或签名 Cookie：

```text
https://cdn.example.com/media/<content-id>/<file>?exp=...&sig=...
```

要求：

- token TTL 建议 5-15 分钟；长时间播放由客户端按需续签，不签发永久 URL。
- 签名覆盖路径、过期时间、必要时覆盖用户/设备；不要把 Emby API key、用户密码、真实文件路径放进 URL。
- **CDN 缓存 key 必须去掉 `exp`/`sig` 等签名参数**；否则每个用户、每次刷新都是新对象，命中率会接近零。
- 如果 CDN 支持，优先把签名放在 Cookie 或请求头，路径作为稳定缓存 key；若只能用 query，配置 CDN 在 cache key 中忽略签名字段，但校验仍要在回源前完成。
- 不要把 `Authorization`、Cookie、`X-Emby-Token` 放进缓存 key；这既会碎片化缓存，也可能造成越权命中。

#### 14.6.3 缓存头和 Range

direct play 的源站至少返回：

```text
Accept-Ranges: bytes
ETag: "<strong-etag>"
Last-Modified: <http-date>
Cache-Control: public, max-age=31536000, immutable   # 内容寻址、永不覆盖时
```

如果同一个 URL 会被替换，不能使用一年 `immutable`；应使用较短 TTL，并在替换时主动 purge。更稳妥的做法是把内容哈希放进路径，替换文件就生成新 URL，旧 URL 自然淘汰。

Range 的关键是避免缓存 key 被 `Range` 打散：

- 让 CDN 缓存完整对象，`206` 从完整对象切片；不要按每个 `Range` 生成独立缓存对象。
- 源站必须正确实现 `206`、`Content-Range` 和边界 `416`；否则播放器拖动进度会失败。
- 大文件如果 CDN 只支持分片缓存，要确认分片能被合并和复用，并监控 `206` 命中率与回源字节数。
- 禁止在媒体响应里下发 `Set-Cookie`；不要设置 `Vary: Range` 或 `Vary: Cookie`。

#### 14.6.4 HLS 缓存策略

| 对象 | 建议 |
|---|---|
| 点播 `m3u8` | `max-age=5-60`，内容变化时 purge；或使用版本化路径 |
| 直播/会话 `m3u8` | `no-cache` 或极短 TTL，不共享缓存 |
| `.ts`/`.m4s` 分片 | 内容寻址后 `public, max-age=31536000, immutable` |
| 加密密钥 | 绝不公共缓存；短 TTL、鉴权、独立 origin |

按会话转码的 HLS 分片不能直接放进公共 CDN；要么先打包成共享 VOD 分片，要么给每个会话独立 token 和隔离缓存命名空间。

#### 14.6.5 Origin shield、回源和多 edge

- 每个区域设置一层 origin shield，回源请求先收敛到 shield，避免每个 edge 都回源。
- nginx 开启 `proxy_cache_lock`，合并同一对象的并发回源；配置 `proxy_cache_use_stale error timeout updating`，源站抖动时用旧对象兜底。
- 只允许 CDN 回源地址访问 origin，使用 mTLS、HMAC 或 IP allowlist；不要因为 CDN 在前面就取消源站鉴权。
- 多 edge 用 DNS/GeoDNS 或 L4 LB 分流，健康检查覆盖 TLS、媒体 Range 和 API 两条路径。
- API 和文件服务分开做健康检查：文件 edge 挂了不应把 Emby API 一起判死，反之亦然。

#### 14.6.6 失效与一致性

- 内容寻址路径（`/media/<hash>/...`）不需要逐条 purge，替换文件生成新 hash 即可。
- 固定 URL 必须在文件替换后 purge，并保证索引、CDN 缓存和 Emby 元数据按同一顺序更新；不要先更新索引再让客户端读到旧缓存。
- HLS 播放列表更新要和分片上传完成绑定；分片未就绪时不能提前发布新 `m3u8`。
- 预留 `stale-if-error` 和回源熔断，避免源站故障时 CDN 把错误页缓存成媒体对象。

#### 14.6.7 安全与限流

- 防盗链用签名 URL/Cookie，不依赖可伪造的 `Referer`。
- 每用户限制并发流数、总带宽和单 IP 连接数；转码任务单独限流，避免一个用户占满 CPU。
- token 可绑定用户和设备；绑定 IP 会影响移动网络切换，按业务决定，不要默认强绑。
- 日志脱敏：不记录完整签名、Emby token、Cookie 和真实文件路径。
- 媒体响应不要回显内部路径、堆栈或 Emby 版本信息；错误页统一。
- 对 `/emby/*` 开 WAF/速率限制，对 `/media/*` 重点防批量扫描和盗链。

#### 14.6.8 观测指标

至少采集：

- CDN/edge 的字节命中率、请求命中率、回源字节数和回源 QPS。
- `206` 比例、Range 请求分布、`416` 数量、TTFB p50/p95/p99。
- 每用户并发流、每用户码率、启动时间、卡顿/rebuffer（客户端侧）。
- 源站出口带宽、磁盘读吞吐/IOPS、NFS 延迟、对象存储 4xx/5xx。
- token 校验失败、403/410、盗链拦截、单 IP 限流触发次数。
- 每个 edge 的 TLS 握手失败、连接数、重传和 keepalive 使用情况。

不要只看“平均速度”。多用户场景更重要的是 p95 启动时间、卡顿率和源站出口是否被打满。

### 14.7 分阶段落地

**阶段 A：单 edge，本地盘**

- Emby 迁到 systemd，API 只监听 `127.0.0.1:8096`。
- edge 用内核 nginx 多 worker，`sendfile on`、`aio threads`，本地 NVMe 存热门媒体。
- 实现 `itemId -> object key` 索引和短 TTL 签名 URL。
- 先不做 CDN，验证直连 edge 的 Range、拖动、并发和限速。

**阶段 B：双 edge + L4 LB**

- 两台 edge 共享 NFS/MinIO 或做内容同步，LB 做健康检查和故障摘除。
- API 与媒体分开路由；媒体请求不要求会话粘滞。
- 压测从多台外部客户端发起，不用本机 VCL 客户端冒充公网客户端。

**阶段 C：对象存储 + CDN + origin shield**

- 媒体上传到对象存储，路径带内容哈希；CDN 缓存完整对象，Range 从缓存切片。
- 签名 URL/Cookie 在 CDN 边缘校验，源站只接受 CDN 回源身份。
- 配置缓存 key 忽略签名参数、`proxy_cache_lock`、stale 兜底和 purge 流程。

**阶段 D：独立转码节点 + HLS/CDN**

- 转码节点用 QSV/NVENC/独显，和 edge 文件服务隔离；E5-2690 v2 纯软件转码很快会成为瓶颈。
- 需要转码的请求由 Emby 控制面创建任务，输出 HLS；点播分片内容寻址后交给 CDN。
- 按会话生成的播放列表不共享缓存，避免用户之间串流或越权。

### 14.8 实施清单

- [ ] 把 Emby 从当前 Docker 验证环境迁到 systemd，媒体目录和配置路径先备份。
- [ ] 保留 Emby API 在 loopback，不让 `.NET` 进程直接发送大文件。
- [ ] 建立 `itemId -> object key -> size/mtime/etag` 索引，支持增量更新和删除。
- [ ] 实现播放 token 签发/校验，TTL 5-15 分钟，不暴露 Emby API key。
- [ ] edge nginx 使用 internal location + `X-Accel-Redirect`，或 Go/Rust 文件服务使用 `sendfile`。
- [ ] 配置 `Accept-Ranges`、`ETag`、`Last-Modified`、`206/416` 和媒体缓存头。
- [ ] CDN cache key 去掉签名参数，禁止缓存 `Set-Cookie`、Authorization 和用户 API。
- [ ] 配置 origin shield、回源鉴权、`proxy_cache_lock`、stale 兜底和 purge。
- [ ] 加每用户并发/带宽限制、单 IP 限流、日志脱敏和防盗链。
- [ ] 用多台外部客户端压测；记录 p95 TTFB、命中率、回源字节、卡顿和源站出口。
- [ ] 最后再决定 VPP 保留在 L3/L4、punt 还是 host-stack 终止；不要用调大 fifo 代替架构拆分。

## 15. HLS 分片实现与使用

### 15.1 目标与边界

HLS 分片由外部 ffmpeg 任务预生成，VPP 进程只读文件，不启动转码器，也不在数据面调用 ffmpeg。生成的目录约定为：

```text
/srv/media/hls/<itemId>/master.m3u8
/srv/media/hls/<itemId>/v<timestamp>-<pid>/main/index.m3u8
/srv/media/hls/<itemId>/v<timestamp>-<pid>/main/seg_00000.ts
```

`master.m3u8` 最后原子替换，客户端不会看到生成到一半的播放列表。旧的版本目录可以保留用于回滚，也可以用 `--prune` 清理。

### 15.2 media_server 路由

`media_server` 现在支持以下映射：

| 请求 | 实际文件 |
|---|---|
| `/Videos/<id>/stream`、`/Videos/<id>/original.mp4` | 索引中的原视频，Range/206 |
| `/Videos/<id>/master.m3u8` | `/srv/media/hls/<id>/master.m3u8` |
| `/Videos/<id>/<version>/main/index.m3u8` | `/srv/media/hls/<id>/<version>/main/index.m3u8` |
| `/Videos/<id>/<version>/main/seg_00000.ts` | 对应分片 |
| `/hls/<id>/...` | 直接访问 `/srv/media/hls/<id>/...` |

`/emby/videos/<id>/...` 由 nginx 去掉 `/emby` 前缀后走同一套规则。请求 query 不参与文件路径，Range 仍由 `http_static` transport 统一处理。

`.m3u8` 返回 `application/vnd.apple.mpegurl`，`.ts` 返回 `video/mp2t`。相对 URL 以 master 所在目录为基准，因此 master 中的 `v.../main/index.m3u8` 会自然解析到同一套 HLS 目录。

### 15.3 生成 HLS

脚本：`src/scripts/emby-hls.py`。默认从 `/srv/media/index.tsv` 查 `itemId`，使用 libx264 转码为单码率 HLS，分片 6 秒，最大宽度 1920。

```bash
# 单个条目
src/scripts/emby-hls.py 21082

# 全量生成，完成后删除每个条目的旧版本
src/scripts/emby-hls.py --all --prune

# 源已经是 H.264/AAC，直接 copy，速度快很多
src/scripts/emby-hls.py 21082 --copy

# 自定义分片和码率参数
src/scripts/emby-hls.py 21082 \
  --segment-time 4 --max-width 1280 --crf 22 --preset veryfast
```

环境变量：

```text
MEDIA_ROOT=/srv/media
INDEX_OUT=/srv/media/index.tsv
HLS_ROOT=/srv/media/hls
```

常用选项：

- `--all`：对索引中的文件去重后批量生成；同一路径的数字 ID/GUID 别名不会重复编码。
- `--copy`：只适合源视频编码和 HLS 客户端兼容的情况；否则默认 libx264 转码。
- `--prune`：只保留本次生成的版本。
- `--max-width`：限制输出宽度，避免 4K 源在小带宽链路上产生过大的分片。

生成完成后不需要重启 VPP，`media_server` 每次请求直接打开文件。若索引路径变化，重新运行 `emby-index.py` 并在 VPP 中执行 `media server index reload`。

### 15.4 nginx 路由

如果 VPP 已加载本次修改后的 `media_server`，把 `/Videos/<id>/...` 原样转给 VPP 即可：

```nginx
location ~ ^/(emby/)?Videos/[^/]+/(stream|original) {
    proxy_pass http://vpp_media;
}
location ~ ^/(emby/)?Videos/[^/]+/(.+)$ {
    proxy_pass http://vpp_media;
}
location ~ ^/(emby/)?Items/[^/]+/Download {
    proxy_pass http://vpp_media;
}
location ~ ^/(emby/)?Audio/[^/]+/stream {
    proxy_pass http://vpp_media;
}
location /hls/ {
    proxy_pass http://vpp_media;
}
```

如果暂时不重启 VPP、还在用旧的 `media_server`，把 HLS location rewrite 到已有的 `/hls/` 树即可：

```nginx
location ~ ^/(emby/)?Videos/[^/]+/(.+)$ {
    rewrite ^(?i)/(?:emby/)?Videos/([^/]+)/(.*)$ /hls/$1/$2 break;
    proxy_pass http://vpp_media;
}
```

注意：直接播放的 `stream|original` location 必须放在通用 HLS location 之前，否则 `stream.mp4` 会被当成 HLS 子路径。不要给 HLS location 加 `reuseport`，VCL listener 仍由 nginx master 继承。

启动 nginx 前必须确认 VPP 仍在运行。VPP 不在时启动 VCL nginx，worker 会反复收到空的 `epoll_wait`，日志出现大量：

```text
epoll_wait() returned no events without timeout
```

并可能把 `/var/log/nginx/pornvpp-error.log` 迅速写到几十 GB。仓库里的 `src/scripts/nginx-vcl-start.sh` 会先执行 `vppctl show version`，VPP 不可达时直接拒绝启动；不要绕过这个检查直接 `LD_PRELOAD=... nginx`。

### 15.5 验证

```bash
HOST=pornvpp.linqiankai.top
BASE=https://${HOST}/Videos/21082

# master playlist，应为 200，Content-Type 为 application/vnd.apple.mpegurl
curl -4 -skS --noproxy '*' -D - -o /tmp/master.m3u8 "$BASE/master.m3u8"

# 按 master 中的实际版本路径取 variant
VARIANT=$(awk '/^[^#]/{print; exit}' /tmp/master.m3u8)
curl -4 -skS --noproxy '*' -D - -o /tmp/index.m3u8 "$BASE/$VARIANT"

# 取第一个分片，并验证 Range/206
SEG=$(awk '/^[^#]/{print; exit}' /tmp/index.m3u8)
curl -4 -skS --noproxy '*' -r 0-1023 -D - -o /tmp/seg.part "$BASE/$(dirname "$VARIANT")/$SEG"

# 用 ffprobe 验证播放列表可解析
ffprobe -v error -show_entries format=duration -of default=nw=1 "$BASE/master.m3u8"
```

HLS 只改变分发形态，不会自动解决大陆跨境带宽或内容合规问题。真正上线前仍应按第 14 节把 edge 文件服务、CDN 缓存 key、签名 URL 和源站鉴权拆开；`m3u8` 用短 TTL，`ts`/`m4s` 用内容寻址后的长 TTL。

## 16. VPP 崩溃与 core dump

### 16.1 开启 core

VPP 支持在启动配置里直接调整 RLIMIT_CORE 和 coredump filter：

```text
unix {
  nodaemon
  coredump-size unlimited
  full-coredump
  cli-listen /run/vpp-dpdk/cli.sock
  log /var/log/vpp-dpdk/vpp.log
  runtime-dir /run/vpp-dpdk
}
```

启动脚本在拉起 VPP 前还要执行：

```bash
ulimit -c unlimited
```

内核侧不要依赖 apport 的默认 pipe，直接把 core 落到 `/var/crash`：

```text
# /etc/sysctl.d/99-vpp-core.conf
kernel.core_pattern=/var/crash/core.%e.%p.%t
kernel.core_uses_pid=1
```

```bash
sysctl --system
cat /proc/sys/kernel/core_pattern
```

验证运行中的 VPP 确实拿到了 unlimited：

```bash
cat /proc/$(pgrep -x vpp_main)/limits | grep -i core
```

应看到 `Max core file size unlimited`。core 文件形如 `/var/crash/core.vpp.<pid>.<timestamp>`。

### 16.2 崩溃现场

2026-10-23 12:12:11，VPP 在 HTTP1 路径崩溃：

```text
http1_app_tx_callback:1973: ... invalid tx state: http req state 'wait transport method', session state 'ESTABLISHED'
http1_parse_request_line:264: request line incomplete
received signal SIGSEGV, PC 0x9206, faulting address 0x9206
PC contains invalid memory address
```

随后 watchdog 在 12:12:38 判定 WAN 不可达并回滚 `enp2s0f0` 到内核 `igb`。这说明 **VPP 是先崩溃，watchdog 后回滚**，不是 watchdog 主动杀死了正常的 VPP。崩溃发生在 HTTP1/VCL 连接处理，具体调用栈必须用 core 确认；在拿到 core 前不要把 `0x9206` 解释成业务代码地址。

拿到 core 后：

```bash
gdb -q /root/netsystem-rs/third_party/vpp/build-vpp-lab-install/bin/vpp \
  /var/crash/core.vpp.<pid>.<timestamp>

(gdb) set pagination off
(gdb) thread apply all bt full
(gdb) info registers
(gdb) p vlib_last_signum
(gdb) p vlib_last_faulting_address
```

如果栈里落到 `http1_app_tx_callback` 或 `http1_parse_request_line`，下一步应优先检查 VCL/nginx 关闭连接、短读和半包请求的状态机，而不是调 TCP 参数。

本次同时加了两处防御：`http1_app_tx_callback`/`http1_app_rx_evt_callback` 先检查 request index 是否已释放，避免在连接关闭竞态中解引用 stale request；authority-form 解析在读取 `rx_buf[i + 1]` 前检查边界。它们不能替代 core 分析，但能避免同一类 use-after-free/越界读继续把 VPP 打崩。

