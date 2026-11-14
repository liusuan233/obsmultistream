# obs-multistream — Windows 构建与安装

把 OBS 的**一路推流**同时分发到多个 RTMP/RTMPS 平台（B 站 / 抖音 / 快手 /
YouTube / Twitch…）。本目录是**面向 Windows 的构建工程**，由官方
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) 改造而来；
Linux 那边的原生构建仍在本仓库的 `../obs-multistream`。

> 功能说明、实现细节、已知限制见上一级目录的 `README.md`，这里只讲**怎么在
> Windows 上把它编出来、装上去**。

---

## 为什么必须用 MSVC 重新编

插件里有一个 Qt 停靠面板（`MultiStreamDock : public QWidget`，通过
`obs_frontend_add_dock_by_id` 挂进 OBS）。Windows 版 OBS 是用 **MSVC** 编译的，
并且带着它**自己那份 MSVC 版 Qt6**。如果插件用别的工具链（比如 Linux 上的
MinGW）编出来，Qt 的 C++ ABI 和符号修饰都对不上——链接不过，或者一加载就崩。

所以：**Windows 的 `.dll` 只能用 MSVC + OBS 官方 libobs + 与之配套的 Qt6 编出来。**
本工程把这一步全自动化了，两种做法任选其一。

---

## 方式一：GitHub Actions 自动出包（推荐，无需 Windows 机器）

1. 把本目录作为一个独立仓库推到 GitHub（`git init && git add -A && git commit &&
   git remote add origin … && git push`）。
2. push 到 `main` / `master`，或手动触发 **Actions → Build Windows Plugin →
   Run workflow**。
3. 跑完后在该次运行页面底部下载 artifact **`obs-multistream-windows-x64`**，
   里面就是 `obs-multistream-0.1.0-windows-x64.zip`。
4. 打一个 `0.1.0` 这样的 tag 推上去，会自动创建一个 draft Release 并附上 zip。

CI 做的事（和官方模板一致）：下载 prebuilt obs-deps + Qt6 + OBS 31.1.1 源码 →
只用 MSVC 编 `libobs` / `obs-frontend-api` → 再用它编插件 → 按“免安装布局”打包。
全程 MSVC，产物肯定和 OBS 自带的 Qt ABI 一致。

> 依赖版本**和哈希**都写在 `buildspec.json`（`obs-studio` / `prebuilt` / `qt6` 各自的
> `hashes.windows-x64`）里，没有单独的 `hashes.windows-x64` 文件；升级时两者一起改，
> 哈希可从 obs-deps 对应 release 说明的 “Checksums” 段复制。
>
> CI 首次运行约 20–40 分钟（下载 ~300 MB 依赖 + MSVC 编译两遍 `libobs`），工作流已缓存
> `.deps` 里的压缩包，后续会快很多。若构建因 MSVC 警告中断，是
> `cmake/common/compiler_common.cmake` 默认把 `CMAKE_COMPILE_WARNING_AS_ERROR` 设为 ON
> 导致的，先加 `-DCMAKE_COMPILE_WARNING_AS_ERROR=OFF` 跑通即可。

---

## 方式二：在 Windows 本机一键构建

前提：**Windows 10/11 x64** + **Visual Studio 2022**（含 “使用 C++ 的桌面开发”
和 Windows 10 SDK ≥ 10.0.20348）+ **CMake ≥ 3.28** + **PowerShell 7**（`pwsh`）。

在仓库根目录打开 PowerShell 7，执行：

```powershell
pwsh -File .\build-windows.ps1
```

或完全手动：

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo --parallel
cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo
```

产物：

```
release/RelWithDebInfo/
└── obs-multistream/
    ├── bin/64bit/obs-multistream.dll      ← 插件本体
    └── data/locale/{en-US,zh-CN}.ini      ← 语言文件
```

`build-windows.ps1` 还会顺手把它压成
`release/obs-multistream-<版本>-windows-x64.zip`。

> 首次配置会自动下载 OBS 依赖（几百 MB）并编译 `libobs` / `obs-frontend-api`，
> 需要几分钟，仅第一次。之后增量很快。

---

## 安装到 OBS

**用户级安装（推荐，免管理员）**——把 zip 解压到：

```
%APPDATA%\obs-studio\plugins\
```

解压后应形成：

```
%APPDATA%\obs-studio\plugins\obs-multistream\bin\64bit\obs-multistream.dll
%APPDATA%\obs-studio\plugins\obs-multistream\data\locale\*.ini
```

重启 OBS → 菜单 **视图 → 停靠窗口 → 多平台推流** 即可看到面板。

**系统级安装**——把 `bin/64bit/obs-multistream.dll` 放到
`C:\Program Files\obs-studio\obs-plugins\64bit\`，语言文件放到
`C:\Program Files\obs-studio\data\obs-plugins\obs-multistream\locale\`（需管理员）。

---

## 兼容性

- 目标 **OBS Studio 28 及以上（Qt6）**，构建基线为 OBS 31.1.1。
- 运行时依赖 OBS 自带的 `obs-outputs.dll`（提供 `rtmp_output`）与
  `rtmp-services.dll`（提供 `rtmp_custom`），官方安装包默认自带。
- 配置保存在 `%APPDATA%\obs-studio\plugin_config\obs-multistream\targets.json`。

## 目录结构

```
CMakeLists.txt          # 插件目标定义（已开启 frontend-api + Qt）
CMakePresets.json       # windows-x64 预设
buildspec.json          # 名字/版本/依赖版本与哈希
build-windows.ps1       # 本机一键构建 + 打包
src/                    # 插件源码（与 Linux 版一致）
data/locale/            # 中英文语言文件
cmake/                  # 官方模板的 CMake 辅助模块（windows/ 为主）
.github/workflows/build-windows.yaml   # CI：MSVC 出包
```
