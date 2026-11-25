# 第三方依赖与架构参考

本仓库发布自研源码。第三方依赖来源于系统包、开发者本地提供的 API JAR，或 CMake 下载的固定版本源码，各自保持上游许可。

| 依赖 | 上游许可或来源 |
| --- | --- |
| cpp-httplib | [MIT](https://github.com/yhirose/cpp-httplib/blob/v0.67.0/LICENSE) |
| nlohmann/json | [MIT](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT) |
| SQLite | [公有领域声明](https://sqlite.org/copyright.html) |
| OpenSSL | [OpenSSL 3.x Apache-2.0](https://www.openssl.org/source/license.html) |
| libarchive | [上游 COPYING](https://github.com/libarchive/libarchive/blob/master/COPYING) |
| zlib | [zlib 许可](https://zlib.net/zlib_license.html) |
| BungeeCord | [上游 LICENSE](https://github.com/SpigotMC/BungeeCord/blob/master/LICENSE)，供 Java 桥接编译或隔离冒烟 |
| aiohttp | [Apache-2.0](https://github.com/aio-libs/aiohttp/blob/master/LICENSE.txt)，供可选 QQ Bot 适配器使用 |

网易开服工具的公开教程用于参考架构分层。README 链接到公开概述与原 SpigotMaster API 文档。这里的控制协议与 SDK 为独立实现。
