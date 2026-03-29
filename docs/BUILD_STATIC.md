# EDL 静态编译说明（单文件 / 少 DLL）

`edl_core` 已是 **静态库**；臃肿主要来自 **Qt Widgets / Svg / Network** 等 **动态 DLL**。要得到「几乎无 Qt DLL」的 `EDL.exe`，必须使用 **静态编译的 Qt**，再打开工程的 **`EDL_STATIC_QT`** 选项。

---

## 0. 一键完成（本机需有 Git + CMake + Ninja + MSVC）

**默认在【本仓库根目录】下拉取 vcpkg**：`.\vcpkg\`，构建输出：`.\build-vcpkg-static\`（已加入 `.gitignore`，勿提交 `vcpkg\`）。

在仓库根目录执行（**x64 Native Tools** 或已配置好编译器的终端），任选其一：

- **PowerShell**（当前目录运行批处理必须加 `.\`）：

```powershell
.\build-static-vcpkg.bat
```

或直接跑脚本：

```powershell
.\scripts\complete-vcpkg-static-build.ps1
```

- **cmd.exe**：可直接 `build-static-vcpkg.bat`（或 `.\build-static-vcpkg.bat` 亦可）。

- **不传参数**时：在 **`.\vcpkg\`** 克隆官方 vcpkg → bootstrap → CMake 配置 → 编译（可用 **`-SkipBuild`** 只配置）。
- 若 vcpkg 已在其它路径：  
  `.\scripts\complete-vcpkg-static-build.ps1 -VcpkgRoot D:\其它\vcpkg`
- **首次**会根据 `vcpkg.json` 编译 **qtbase + qtsvg**，耗时可很长，请保持网络与磁盘空间。
- **二进制缓存**默认写到 **仓库上一级 `\vcpkg-binary-cache`**（例如 **`D:\EDL\vcpkg-binary-cache`**），避免占 **C:** 的 `%LOCALAPPDATA%\vcpkg`；详见 **§4.1**。

**本机缺少工具时**：请先安装 [Git](https://git-scm.com/download/win)、[CMake](https://cmake.org/download)、Ninja，并安装 **Visual Studio** 的「使用 C++ 的桌面开发」工作负载（提供 MSVC）。

**若出现乱码或「`[...]` 不是命令」**：请用 **UTF-8** 保存脚本；根目录 **`build-static-vcpkg.bat`** 的注释已改为 **英文**，避免 cmd 默认代码页误解析。PowerShell 中双引号字符串里的 **`[`** 有特殊含义，提示行已改为单引号/拼接写法。

### Sahara（9008）握手选项

- 默认 **`EDL_SAHARA_COMMAND_MODE=ON`**：在 Sahara 阶段尝试**命令模式**（读序列号/PK/HWID 等），行为与常见工具一致。
- 若某机型在命令模式下握手失败、仅需刷 Loader，可配置 CMake 关闭：  
  `-DEDL_SAHARA_COMMAND_MODE=OFF`

---

## 1. 你需要什么

| 项目 | 说明 |
|------|------|
| **静态 Qt** | 官方安装器默认多为 **动态** 套件；静态需 **自行用源码配置 `-static`**，或使用已提供的 **静态 Kit**（若有）。 |
| **编译器与 Qt 一致** | 例如 MinGW 静态 Qt → 用 **同一套 MinGW** 编 EDL；MSVC 静态 Qt → 用 **对应 MSVC** 与 **/MT** 运行库（与 Qt 静态包一致）。 |
| **许可证** | Qt 动态/静态与 LGPL/商业条款请自行合规；本文仅技术说明。 |

---

## 2. 自行构建 Qt 6（MinGW 示例，思路）

在 Qt 源码目录（示例，路径请按本机修改）：

```bat
configure.bat -static -release -prefix C:\Qt\6.x.x\mingw_static ^
  -opensource -confirm-license ^
  -nomake examples -nomake tests ^
  -platform win32-g++ ^
  -qt-zlib -qt-libpng -qt-libjpeg ^
  -skip webengine
cmake --build . --parallel
cmake --install .
```

实际参数请按 [Qt 官方文档](https://doc.qt.io/qt-6/build-sources.html) 与磁盘空间调整；**WebEngine 体积大**，GUI 工具常 `-skip webengine`。

装好后，将 **`CMAKE_PREFIX_PATH`** 指到该 **`prefix`**（其下应有 `lib/cmake/Qt6`）。

---

## 2.1 推荐：用 vcpkg 自动编「静态 Qt」（Windows / MSVC）

不必先手工编译整棵 Qt 源码：用 **vcpkg** 的 **静态三元组** 即可把 **Qt 以静态库形式** 装进 `EDL.exe`（仍可能依赖 **VC 运行库** `vcruntime*.dll` 等，取决于三元组）。

### 准备

1. 推荐用 **§0** 的 **`build-static-vcpkg.bat`**，在 **`.\vcpkg\`** 自动克隆并引导 vcpkg（无需设置环境变量）。
2. 工程根目录已有 **`vcpkg.json`**，声明依赖 **`qtbase`**、**`qtsvg`**。
3. 建议使用三元组 **`x64-windows-static-md`**：静态链 Qt，运行库为 **`/MD`**。**不要**在 CMake 里打开 **`EDL_MSVC_STATIC_RUNTIME`**（除非你的静态 Qt 是 **`/MT`** 编的）。

### 仅重新配置 / 编译（已有 .\vcpkg\ 时）

```bat
scripts\build-vcpkg-static.bat
```

- **若仓库根目录下已有 `.\vcpkg\`**，脚本会 **优先使用该路径**，避免系统环境变量里错误的 `VCPKG_ROOT`（例如指向不存在的 `C:\vcpkg`）导致失败。
- `build-vcpkg-static.bat` 会调用 **`build-vcpkg-static.ps1`**：自动加载 **vcvars64**（若当前无 `cl.exe`）、并在 PATH 中查找 **CMake / Ninja**。

### 手动 CMake（vcpkg 已在根目录）

```bat
cmake -B build-vcpkg-static -G Ninja ^
  -DCMAKE_TOOLCHAIN_FILE=%CD%\vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DEDL_STATIC_QT=ON
cmake --build build-vcpkg-static
```

输出：`build-vcpkg-static\EDL.exe`。

---

## 3. 配置并编译 EDL（已有静态 Qt 前缀时）

```bat
cd 你的\EDL\工程根目录
cmake -B build-static -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DEDL_STATIC_QT=ON ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.x.x/mingw_static
cmake --build build-static
```

生成物一般在 `build-static/EDL.exe`（或 `Release/EDL.exe`，视生成器而定）。

- **`EDL_STATIC_QT=ON`**：打开 `QT_STATIC`、优先静态包；Qt6 下会 **`qt_import_plugins` 嵌入 SVG** 等必要插件。
- **MinGW**：CMake 会为 `EDL` 增加 `-static -static-libgcc -static-libstdc++`，减少运行时 DLL。
- **MSVC**：默认 **不**改运行库；若你的静态 Qt 是 **`/MT`** 编出来的，再打开 CMake 选项 **`EDL_MSVC_STATIC_RUNTIME=ON`**。**vcpkg `*-static-md`** 使用 **`/MD`**，**不要**开此项。

若 **`find_package(Qt6)` 仍拉到动态库**，说明 `CMAKE_PREFIX_PATH` 指错或该前缀下没有静态 Qt，请检查 `Qt6Config.cmake` 所在路径。

---

## 4. 与动态 Qt 的对比

| 方式 | 优点 | 缺点 |
|------|------|------|
| **动态 Qt + windeployqt** | 配置简单 | 大量 `Qt6*.dll`、platforms、imageformats 等 |
| **静态 Qt + EDL_STATIC_QT** | 单 exe 为主、易分发 | Qt 编译时间长、exe 较大、需匹配工具链 |

### 4.1 磁盘与 vcpkg 缓存（避免占满 C 盘）

- **vcpkg 默认**会把**二进制缓存**写在 **`%LOCALAPPDATA%\vcpkg`**（一般在 **C:**），体积可很大。
- **`complete-vcpkg-static-build.ps1`** 已默认设置 **`VCPKG_DEFAULT_BINARY_CACHE`** 为 **「仓库上一级目录」下的 `vcpkg-binary-cache`**。例如仓库在 **`D:\EDL\EDL`** 时，缓存目录为 **`D:\EDL\vcpkg-binary-cache`**（可按需改 **`-BinaryCacheDir`**）。
- **删掉 C 盘旧缓存**（释放空间；**不影响** 已装在工程里的 **`build-vcpkg-static\vcpkg_installed`**）：

```powershell
.\scripts\clean-vcpkg-user-cache-c-drive.ps1 -Force
```

- **省 D 盘空间**：可删除 **`.\vcpkg\buildtrees`**（中间编译目录，体积最大；以后若 vcpkg 要重编某包会再生成）。**不要**随意删 **`.\vcpkg\packages`** 除非你清楚后果。

---

## 5. 常见问题

- **`BUG (fork bomb): ...\git.exe` 或克隆后仍报找不到 `vcpkg.cmake`**  
  多为 **PATH 里优先使用了其它 Git**（例如 **Anaconda/Miniconda**），或 **Git 安装不完整**（例如 `cmd\git.exe` 缺失、只剩 `bin\git.exe`）。处理：先 **`conda deactivate`**；或 **修复/重装 Git for Windows**。脚本在 **git clone 失败** 时会自动尝试从 GitHub **下载 vcpkg 的 ZIP**（无需 git）。仍失败可手动解压 [vcpkg master.zip](https://github.com/microsoft/vcpkg/archive/refs/heads/master.zip) 到 **`.\vcpkg\`**（顶层目录须为 `scripts\` 等），再执行 **`.\build-static-vcpkg.bat`**。已有 vcpkg 时可用 **`-VcpkgRoot`**。

- **`vcpkg install` 已成功，但随后 CMake 报找不到 Ninja / `CMAKE_CXX_COMPILER not set`**  
  说明 **未安装 Ninja** 或未加入 **PATH**，或 **未在 MSVC 环境**里跑。处理：安装 Ninja（例如 **`winget install Ninja-build.Ninja`**），在 **x64 Native Tools** 或已 **`vcvars64.bat`** 的终端里重跑脚本；脚本会传入 **`-DCMAKE_MAKE_PROGRAM`** 并在能检测到 **`cl.exe`** 时固定 **`CMAKE_*_COMPILER`**。依赖已装完时，重跑 **CMake 会很快**（无需再编几小时 Qt）。

- **链接报错：找不到 Qt6::xxx 或符号未解析**  
  多为 **Release/Debug** 或 **/MD 与 /MT** 与 Qt 不一致；或混用了 **动态 Qt 头文件** 与 **静态库**。

- **MSVC 链接 `LNK2019`：`__std_rotate`、`__std_unique_*`、`__std_find_last_not_ch_pos_*`（来自 Qt/ICU 静态库）**  
  vcpkg **`x64-windows-static-md`** 为 **`/MD`**，需与工程 **`MSVC_RUNTIME_LIBRARY`** 一致。另：Ninja 下 **`msvcprt.lib` 若在链接表中间**，后面才出现的 Qt `.obj` 无法向前解析 STL 内含符号。本仓库在 **`qt_finalize_executable` 之后** 用 **`target_link_options`** 追加 **`…/VC/Tools/MSVC/<ver>/lib/x64/msvcprt.lib`**（落在 **LINK_FLAGS**，在 **`@` rsp 之后**），避免顺序问题。

- **运行时报缺 SVG、图标**  
  静态 Qt6 需正确 **`qt_import_plugins`**；本仓库在 `EDL_STATIC_QT` 且 Qt6 下已加入 `Qt6::QSvgPlugin`。若仍缺插件，按 Qt 文档补其它 `INCLUDE` 插件目标。

- **只想缩小体积、不一定要静态**  
  可用 **Release**、**strip**、**UPX**（注意杀软误报），以及 **windeployqt** 只带必要插件。

---

## 6. CMake Preset（可选）

- **`static-mingw`**：本机已有 **MinGW 静态 Qt** 时，把 **`CMAKE_PREFIX_PATH`** 改成你的安装前缀。
- **`vcpkg-static-md`**：在工程根目录存在 **`.\vcpkg\`**（已 bootstrap）时，`cmake --preset vcpkg-static-md` 使用 **`${sourceDir}/vcpkg/.../vcpkg.cmake`**，无需再设 **`VCPKG_ROOT`**。
