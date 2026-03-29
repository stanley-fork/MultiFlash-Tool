# MISC 厂商镜像（可选嵌入）

将 **`misc_wipedata_*.img`** 放入本目录后重新 **CMake 配置并编译**，会打进 `:/misc_vendor/`，运行时释放到 `%LOCALAPPDATA%\…\misc_vendor\`。也可直接放在 **exe 同目录**（优先于嵌入）。

主界面 **「恢复出厂 ▾」** 与「重启设备」相同：先点开按钮，再在菜单里选厂商；自动查找并写入对应镜像，然后 **MISC 写入 + 重启**。

| 文件名 |
|--------|
| `misc_wipedata_common1.img` |
| `misc_wipedata_huawei_lowlevel.img` |
| `misc_wipedata_lenovo.img` |
| `misc_wipedata_lg.img` |
| `misc_wipedata_meizu.img` |
| `misc_wipedata_meizu2.img` |
| `misc_wipedata_mi.img` |
| `misc_wipedata_oneplus.img` |
| `misc_wipedata_oppo.img` |
| `misc_wipedata_zte.img` |
