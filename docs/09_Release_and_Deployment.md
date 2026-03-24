# 版本、打包与发布（Release/Deployment）

## 1. 企业里“发布”通常包含什么

- 版本号（SemVer 或企业内规则）
- 打 tag（例如 `v1.2.3`）
- 变更记录（Release Notes / ChangeLog）
- 构建产物（exe + 依赖 + 配置模板）

---

## 2. Windows 常见打包方式（Qt）

Qt Widgets 程序发布时常见问题：用户机器没有 Qt DLL。

典型做法：

- Release 构建后，使用 `windeployqt` 把依赖 DLL 拷贝到 exe 同目录
- 把 `config.json` 作为“模板配置”随包提供

建议产物结构（示例）：

```
Tenco/
  Tenco.exe
  config.json
  Qt6Core.dll ...
  platforms/qwindows.dll
  image/...
```

---

## 3. 发布前 checklist（建议）

- [ ] Release 构建通过
- [ ] 冒烟测试通过（见 `docs/02_Build_and_Run.md`）
- [ ] 版本号更新（如果你采用版本号策略）
- [ ] 打包产物能在“无 Qt 环境”的机器运行（用干净 VM 最好）

---

## 4. 当前仓库可直接执行的发布命令

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

生成 ZIP 包（基于 CPack）：

```powershell
cd build\preset-release
cpack -G ZIP
```

也可以直接运行脚本（根目录）：

```powershell
.\scripts\package_windows.ps1
```
