# 内置资源（可选）

将以下三个文件放入本目录后重新 **CMake 配置并编译**，会嵌入到可执行文件中；首次使用时会自动释放到 `%LOCALAPPDATA%\…\bundled\`，无需手动解压。

| 文件 | 用途 |
|------|------|
| `dwebp.exe` | WebP 壁纸/API 主题解码（Google libwebp 工具，需与程序同架构，一般为 x64） |
| `misc_tofastbootd.img` | 写入 MISC 后重启到 **fastbootd** |
| `misc_torecovery.img` | 写入 MISC 后重启到 **recovery** |

若缺少任一文件，工程仍可编译，但不会嵌入对应资源；程序会继续使用 **exe 同目录** 下的同名文件（或「设置」里自定义路径）。

**许可**：请自行确认 `dwebp.exe` 的许可证与分发条款。

---

另见 **`misc_vendor/README.md`**：各厂商 `misc_wipedata_*.img` 可放入 `bundled/misc_vendor/` 一并嵌入。
