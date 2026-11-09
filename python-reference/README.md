# python-reference（仅作原理验证）

这是本项目的 **Python 参考实现**，用于：

1. 从门户真实前端 JS 逆向认证算法时的可读原型；
2. 生成 `tests/js_reference_expected.json` 测试向量，供原生 C 版 `bitweb-cli crypto-test` 交叉验证；
3. 39 项离线单元测试（协议、SSID 状态机、DPAPI、本地假门户集成）。

生产版本是仓库根目录的 **原生 C 版**（见 `src/`、`bitweb.exe`、`bitweb-cli.exe`）。
本目录的脚本只用于开发/验证，不参与安装部署；其配置格式与 DPAPI 密文与 C 版完全兼容。