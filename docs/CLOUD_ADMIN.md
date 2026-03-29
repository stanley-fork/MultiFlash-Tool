# EDL 与 edl-admin 云端对接

Qt 客户端通过 **设置 → EDL 云端服务** 填写管理端根地址，与 `edl-admin` 后端对齐。

### 本地构建 / 联调默认

- **`EdlApi::defaultLocalAdminBaseUrl()`** 为 **`http://127.0.0.1:8088`**（与 edl-admin 默认监听一致）。
- 若注册表/配置中 **尚未保存过** `cloud/edlBaseUrl` 键，则「选择机型」、启动时更新检查、设置里展示均 **自动使用该地址**，便于本机先起后端再测。
- 若在设置中 **留空并保存**，则写入空字符串，表示 **不使用** 云端地址（不再套用默认）。

## 已对接接口

| 用途 | HTTP | 说明 |
|------|------|------|
| 机型列表 | `GET /api/v1/device-models` | 「选择机型」弹窗仅从此拉取；失败则列表为空并提示 |
| 更新策略 | `GET /api/v1/update-info` | 启动约 2s 后静默检查；`latest_version` 与客户端 `0.1.0` 比较 |
| 文件缓存 | `GET /api/v1/files/<path>` | 仅允许 `uploads/` 前缀；选中机型后下载 Firehose / digest / sign 等到本机 `%AppData%` 下 `cloud_cache/<机型id>/` |

实现见 `edl_api_client.cpp`、`mainwindow.cpp`（`applyCloudDeviceEntry` / `checkCloudUpdateIfConfigured`）。

## 管理端配置

- 在 **机型** 中为设备填写 `firehose`、`auth_params` 中文件类字段时，路径应为服务端返回的 **`uploads/...`**，客户端才能通过 `/api/v1/files/` 拉取。
- 若启用 **`EDL_ADMIN_PASSWORD`**，未带 Bearer 的客户端会得到 **掩码后的 Realme 数据**（无路径），需另行在管理端导出或由管理员在本机解锁后同步。

## 版本号

当前客户端用于更新比较的版本字符串为 **`0.1.0`**（与 `CMakeLists.txt` 中 `project(EDL VERSION 0.1.0)` 一致）。修改版本时请同时更新 `MainWindow::checkCloudUpdateIfConfigured` 中的 `appVer`。
