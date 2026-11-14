# obs-multistream — Windows 构建与安装

把 OBS 的**一路推流**同时分发到多个 RTMP/RTMPS 平台（B 站 / 抖音 / 快手 /
YouTube / Twitch…）。本目录是**面向 Windows 的构建工程**，由官方
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) 改造而来。

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
