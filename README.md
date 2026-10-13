# REF-IME v1.0.0
给 REFramework 的原生 ImGui 输入框补充 Windows 输入法桥接。

## 安装

将 `dist/REF-IME.dll`（发布包内为 `reframework/plugins/REF-IME.dll`）复制到该游戏目录下：

```text
<游戏目录>\reframework\plugins\REF-IME.dll
```

## 标签构建与发布

推送 `vN.N.N` 格式标签（例如 `v1.0.0`）自动构建并将 ZIP 上传到该标签的 GitHub Release。也可在 **Actions → Build tag → Run workflow** 输入已有标签，构建对应代码并补发 Release；已有同名资产会更新。

产物 `REF-IME-vN.N.N-windows-x64.zip` 只包含 `reframework/plugins/REF-IME.dll`，同时保留在 Actions 构建产物中。工作流更新须先提交并推送到默认分支；补发旧标签时使用手动运行，重跑旧记录不会采用新的工作流。

## 源码

| 文件 | 职责 |
| --- | --- |
| `src/plugin.cpp` | REF UI/IME 回调、Windows 消息转接、活动输入会话、字符注入与组合预览 |
| `src/ime_proxy.cpp` | 专用线程透明 EDIT、IMM 组合/确认、系统焦点和候选窗定位 |
| `src/bridge_state.hpp` | 会话事件队列、去重、UTF-16 解码和编辑顺序 |
| `src/host_api.cpp` | 跨游戏宿主能力/ABI 验证、cimgui 接口绑定和宿主分配器转接 |

第三方许可见 `licenses/`，本项目许可见 `LICENSE`。

