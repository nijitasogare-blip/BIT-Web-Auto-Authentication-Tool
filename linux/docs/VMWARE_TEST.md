# VMware 实测记录

## 环境

- 宿主机：Windows 11，VMware Workstation 17.5.2
- 虚拟机：Ubuntu 22.04（jammy）cloud image，NAT 网络，4 GB 内存 / 2 vCPU，磁盘扩展至 25 GB
- 初始化：cloud-init（seed ISO），安装 `open-vm-tools`、`xvfb`、`libgtk-3-0`、
  `libayatana-appindicator3-1`、`imagemagick`、`fonts-noto-cjk`
- 运行方式：Xvfb `1280x800x24`，`GDK_BACKEND=x11`（VM 内无物理显示器）

## 验证结果

- `./bitweb crypto-test`：HMAC/info/chksum 向量全部通过；
- `BITWEB_SIMULATE_SSID=BIT-Web ./bitweb --config config.json status`：
  外网 HTTP 200，白名单监听状态正常；
- `self-test`：门户页面识别 `ac_id=43`，`get_challenge` 可用（未配置用户名/密码时
  challenge 按预期报参数缺失）；
- `bitweb-gui` 在 Xvfb 中启动成功：窗口标题
  “BIT-Web 自动登录（Linux GTK 版）”，AppIndicator 状态图标窗口创建成功；
- 截图：[gui-vmware.png](gui-vmware.png)。

## 备注

- 无线 SSID 无法在 VMware NAT 虚拟机中真实读取（无无线网卡），因此 SSID 相关逻辑
  使用 `BITWEB_SIMULATE_SSID` 验证；真实 Ubuntu 桌面由 `nmcli` 读取；
- 托盘图标需要桌面环境的 AppIndicator/StatusNotifier 支持；Xvfb 无桌面面板，
  因此截图只显示主窗口，但 AppIndicator 对象已创建、无缺库。