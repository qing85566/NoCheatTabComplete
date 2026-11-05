# NoCheatTabComplete

A Minecraft Bedrock client mod that fixes first-level command Tab completion when cheats are disabled in the world.

一个用于《我的世界》基岩版客户端模组，解决在世界未开启作弊状态下一级指令无法按 Tab 键自动补全的问题。

## Features / 功能特性

- **无作弊补全**：在单人本地世界或远程服务器未开启作弊（`allow-cheats=false`）时，支持按 Tab 键在所有匹配指令候选之间循环补全。
- **纯客户端生效**：仅需客户端安装本模组，无需依赖服务端。

## Build / 构建

```bash
xmake f -m release -p windows -a x64 -y
xmake
```

构建生成的模组文件将输出到 `bin/NoCheatTabComplete/` 目录。

## License

[LICENSE](https://github.com/qing85566/NoCheatTabComplete/blob/main/LICENSE)
