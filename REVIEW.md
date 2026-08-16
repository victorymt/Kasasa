# Kasasa 代码审查报告

> 审查对象：`victorymt/Kasasa` fork（Hyprland 截图/贴纸工具），基于 `main` 分支 + 工作区未提交改动
> 代码规模：`src/` 约 14,700 行 C，`tests/` 约 5,200 行，Meson 构建，GTK 4.14+ / Libadwaita / GStreamer / Wayland
> 审查方式：主代理通读 + 5 个深度子代理逐行审查（Wayland 捕获层 / screencast 流水线 / UI 核心 / CLI 与子进程 / 测试·构建·i18n），实际编译并运行测试套件（unit 7/7 通过；4 个 GTK 测试因本机无 X 服务中止，CI 以 Xvfb 覆盖），并对工作区未提交的捕获层重构与 `git show HEAD:` 逐项比对
> 审查日期：2026-08-12

---

## 总体评价

代码质量明显高于同类 GTK C 项目平均水平：**线程纪律、引用计数、溢出校验、析构清理、行为测试**都做得很扎实，未发现命令注入、严重内存安全或生命周期缺陷；但存在 **1 个真实协议隐患（低概率）、2 个可靠性缺陷**，以及一批**流程性缺口**（CI 未提交、i18n 过期、无版本约束、发布未打 tag）。

> **复核更正（2026-08-12）**：原报告的"帧池 UAF"经 GStreamer 官方文档核实为**误报**，已降级为性能问题（见下文 #1）。

| 级别 | 数量 | 摘要 |
|---|---|---|
| 🔴 高 | 7 | 协议隐患 ×1、应用冻结 ×1、资源泄漏 ×1、CI/i18n/版本约束/覆盖缺口 ×4 |
| 🟡 中 | ~15 | 协议健壮性、颜色正确性、线程/状态、CLI 解析、打包 |
| 🟢 细节 | ~10 | 测试竞态、死代码、文档、命名 |
| ✅ 亮点 | 8 | 无 shell 注入、DMA-BUF 租约池、join-before-teardown、16 组变换测试等 |

---

## 一、高风险问题（🔴）

### 1. ~~帧池 use-after-free~~（复核后：**误报**，降级为性能问题）
- **原始结论**：`screencast.c:176-209` 的 `clear_frame_pool` 在 caps 变化时 unref 池，display queue 仍持有旧池 buffer → UAF 崩溃
- **复核结论**：**不成立**。GStreamer 官方文档明确（[GstBufferPool](https://www.manpagez.com/html/gstreamer-1.0/gstreamer-1.0-1.14.3/GstBufferPool.php)）：buffer 从池 acquire 时持有池的引用（`GstBuffer.pool`，"returned to the pool when refcount drops to 0"），`gst_object_unref(pool)` 只有在 **refcount 归零**时才会释放池；`gst_buffer_pool_set_active(FALSE)` 的语义是 "Deactivating will free the resources **when there are no outstanding buffers**; when there are outstanding buffers, they will be freed as soon as they are all returned"。因此 unref 不会让池提前析构，outstanding buffer 归还时安全 free
- **残留的真实问题（minor）**：`configure_frame_pool` 每次 caps 变化都**销毁并重建池**（性能浪费 + worker 线程上的 caps 重协商时序），且 `update_hypr_stream_caps` 在 worker 线程调用 `gst_app_src_set_caps`/池操作。建议改为固定尺寸池或复用

### 2. DMA-BUF fd 在 flush 前关闭（唯一真正的协议隐患）
- **位置**：`src/kasasa-hyprland-stream.c:765-772`
- **问题**：`zwp_linux_buffer_params_v1_add()` 后立即 `close(fd)`，但请求要到下一次 `wl_display_flush` 才发送；libwayland 在 sendmsg 时才解析 fd 号。若窗口期内 fd 号被其他线程（如 GTK 主线程文件 I/O）复用，合成器拿到**错误的 fd** 或 flush 报 EBADF → 流中断
- **修复**：把 `close(fd)` 推迟到 flush 之后

### 3. 临时截图 PNG 永久泄漏到 /tmp
- **位置**：`src/kasasa-region-capture.c:136-170`、`src/kasasa-hyprland-capture.c:434-452`、`src/kasasa-screenshot.c:117-132`
- **问题**：成功路径不 unlink；pin 关闭时仅当 **auto-trash 开启**（默认关闭，`gschema.xml:65-66`）才 `g_file_trash`。默认配置下**每次截图在 /tmp 永久累积一个 0600 的 `kasasa-*.png`**
- **修复**：关闭 pin 时删除临时文件；或默认开启清理；至少文档化文件生命周期

### 4. hyprctl 同步调用无超时且阻塞主线程
- **位置**：`src/kasasa-window-query.c:445`（`g_spawn_sync`），调用方 `kasasa-application.c:91,115,190`、`kasasa-window-picker.c:305,346`
- **问题**：hyprctl 挂起（陈旧 socket、合成器无响应、PATH 慢）时整个应用（CLI 或 GUI）冻结且无法退出。对比：区域捕获在 worker 线程（`region-capture.c:191`）、窗口捕获有 5s 首帧超时（`hyprland-capture.c:344-378`），唯独 hyprctl 路径裸奔
- **修复**：GTask worker 线程或 `g_subprocess` + cancellable + 超时（建议 5s）

### 5. CI 根本没有提交
- **位置**：`.github/workflows/ci.yml`（整个 `.github/` 目录 **untracked**；提交历史还有 "chore: remove GitHub Actions workflow"）
- **问题**：文件本身写得不错（Xvfb 测试、valgrind、ASan+UBSan、staged install 验证），但**当前提交的仓库没有任何 CI**
- **修复**：连同工作区未提交的捕获层重构（已核实是忠实重构、无回归）一起提交

### 6. i18n 全面过期
- `po/kasasa.pot`（193 msgid）落后源码 6+ 个 msgid（缺 "Apply crop"/"Cancel crop"/"Timed out waiting for the window frame" 等，重新生成得 199）
- 除 zh_CN（192/193）外全部滞后：de/fr/es/pt/ru ≈ 27%（53/193），**ar 仅 7%**（14/193）
- `src/kasasa-region-capture.c` 用户可见错误串**硬编码英文**（"Region selection cancelled" 等）且不在 `po/POTFILES`
- 语言选择器只暴露 system/en/zh_CN 三选一，实际打包 12 种翻译
- 无 CI 检查（pot 新鲜度、msgfmt 校验均缺）
- ✅ 亮点：POTFILES 覆盖完整、`_()` 用法一致、desktop/metainfo 走 `i18n.merge_file`

### 7. wayland-protocols 无版本约束
- **位置**：`src/meson.build:3,25-31` 硬依赖 staging XML（ext-image-capture/copy-capture ≥1.33、ext-foreign-toplevel-list ≥1.35）却无版本检查
- **问题**：老发行版（Ubuntu 24.04 = 1.34）以难以理解的 "file not found" 失败
- **修复**：`dependency('wayland-protocols', version: '>= 1.33')`

### 8. 三大合成器相关模块几乎无测试覆盖
- `kasasa-hyprland-stream.c`（~2,400 行）：仅 `handle_from_address` / `available` / 连接失败路径被覆盖（`test-window-query.c:388-473`）
- `kasasa-screencast.c`（~1,850 行）：layout、fallback 信号、不可用后端；GStreamer 流水线构建/启动/错误路径与 crop API **从未运行**
- `kasasa-window-picker.c`（430 行）：编译进测试但**从未执行**（fixture 注入了 fake picker）
- `integration_tests` 套件只断言环境变量（`test-wayland-integration.c:24-48`），**从不真正跑一次捕获**
- 覆盖度估计：window ~45%、screencast 20-30%、hyprland-stream <10%、window-picker <5%

---

## 二、中等问题（🟡）

### Wayland 协议健壮性（`src/kasasa-hyprland-stream.c`）
| 位置 | 问题 |
|---|---|
| `773-781` | `create_immed` 失败无回退：合成器可抛致命协议错误（INVALID_FORMAT/MODIFIER）杀死整个连接，用户只见 "Wayland event dispatch failed"，且不降级 wl_shm |
| `813-838` + `505-577` | shm stride 未校验 `stride >= width*4` / `stride % 4 == 0`，坏 stride 触发合成器协议错误（连接死亡）而非干净报错 |
| `1233-1264` | format-table `mmap(size)` 无界（未 fstat 校验 vs fd 实际大小），截断/恶意 fd → SIGBUS |
| `1413-1425` | `wl_output` 绑 `MIN(version,4)` 但名字匹配需 v4；老合成器报误导性的 "output unavailable" |
| `1550-1580` | roundtrip 用栈上 `RoundtripState`，超时/停止路径销毁 callback 后迟到 `done` 会写栈地址（当前不可达，防御性建议堆分配） |
| `1615-1618` | `g_new0` 未检查，OOM 泄漏 fd + 池槽位（用 `g_try_new0`） |

### 颜色正确性：straight vs premultiplied alpha
- **位置**：`kasasa-hyprland-capture.c:428-432`、`kasasa-screencast.c:822, 1047-1068`
- **问题**：Hyprland 的 ARGB8888 是直通（straight）alpha，但三处（内存纹理、dmabuf builder、GStreamer caps 映射）都标成 `PREMULTIPLIED`；半透明窗口会出现**变暗毛边**（预览和保存的 PNG 均受影响）。窗口内容通常不透明，属潜伏问题
- **修复**：核实合成器约定后改标 `FALSE`/非 premultiplied 格式

### 线程与状态
| 位置 | 问题 |
|---|---|
| `screencast.c:1112-1256` | worker 写 / 主线程读 `stream_width/height/format` 数据竞争（对齐 32 位实际良性，属 C11 UB/TSan 命中点）；建议走已有的 `StreamSizeUpdate` 队列或原子量 |
| `screencast.c:764-896` | 预览更新调度依赖 `g_main_context_invoke_full` 的**未文档化**返回值语义（已对 glib 源码核实正确，但脆弱；且帧间常驻 HIGH_IDLE 轮询源） |
| `screencast.c:979-997, 305-310` | EOS/错误路径清理完全依赖容器的 EOS handler；`error_cb` 不展示 GStreamer `debug_info` |
| `screencast.c:137-163, 1249-1252` | 会话中改 FPS 静默不生效（`update_hypr_stream_caps` 尺寸不变时早退）；无任何提示 |
| `window.c:1355-1356` | `has_modal()` 每次迷你化计时器触发都对检测到的 modal **重复 connect** `close-request` 且不 disconnect（闭包累积）；同时 AdwDialog 式偏好设置/关于对话框**检测不到**（迷你化不被阻断） |
| `window.c:981-986` | initial-reveal tick 每帧重试 pending resize；显示器永久异常时窗口卡在 opacity 0 隐形 + 每帧 g_warning |
| `window.c:2015` | crop 模式下滚动仍触发缩放，裁切面在拖拽中移位 |
| `window.c:2033-2043` | CAPTURE 阶段左右方向键被劫持，破坏控件键盘导航 |
| `window.c:575-585` vs `content-container.c:404-422` | 窗口 resize 锁与容器引用计数 carousel 锁互相独立，可提前解锁，破坏"resize/capture 期间禁交互"不变量 |

### UI 状态一致性
- **`content-container.c:2030-2034, 1382-1385`（唯一真正的 UI 状态 bug）**：删除当前查看的**第一页**后 `page-changed` 不触发、也无 `request_window_resize`，窗口保持已删除内容的尺寸；screencast EOS 时同理

### CLI / 解析
| 位置 | 问题 |
|---|---|
| `window-query.c:839-844, 917-921` | 表格/候选输出把窗口标题（攻击者可控）原样打印到终端——**终端转义注入**（JSON 路径安全） |
| `application.c:193+` | 转发到已运行实例的调用丢失错误消息（只有退出码）；应用 `g_application_command_line_printerr` |
| `application.c:322-332` | activate 的 listing 分支丢弃退出码（近乎死代码，但脚本若触发会假成功） |
| `window-query.c:215-254, 412-420` | json-glib 成员类型不符走 `g_critical` + 静默默认值而非干净解析错误 |
| `region-capture.c:111-118` | 任何 `G_IO_ERROR_FAILED` 都映射成 "Region selection cancelled"（slurp 真实失败被误标）；`geometry == NULL` 极端路径会流进 critical |
| `window-query.c:78-124` | 未知前缀（`--window=classx:foo`）静默回退到 BARE 匹配，用户看不出拼错；前导冒号永不被识别为前缀 |
| `application.c:34-38, 65-82` | 退出码方案（0/1/2/3）未在 `--help` 文档化；`KASASA_EXIT_MATCH` 对 "no match" 情形命名误导 |
| `screencast.c`（fps） | 会话中 FPS 修改静默不生效 |
| `application.c:141-157` | `--screencast --list-windows` 通过校验但静默忽略 `--screencast` |

### 构建 / 打包 / 发布
| 位置 | 问题 |
|---|---|
| `meson.build` | 版本仍 1.1.6，30+ 功能提交未发版；metainfo release 记录停留在 1.1.6（2026-02-24） |
| `data/…metainfo.xml.in` | URL 全部指向 **upstream**（KelvinNovais），而 about 对话框指向 fork；描述残留已删除的 GNOME/portal 文案，与 fork 实际行为（仅 Hyprland）不符 |
| `src/kasasa.gresource.xml:8` | `help-overlay.ui` 编译进资源但**无任何引用**（无 `help_overlay` 模板子对象、无 action）→ 快捷键总览（F1/Ctrl+,）不可用 |
| `data/icons/meson.build` | 仅 scalable SVG，无 PNG 尺寸；`.Devel.svg`/`.Source.svg` 跟踪但未安装 |
| `data/` | 过期的跟踪文件 `io.github.kelvinnovais.Kasasa.desktop`（缺 `Actions=screencast`，与 `.desktop.in` 不同步） |
| `gschema.xml:17-79` | 仅 `language` 有 summary/description，其余 10 个 key 裸奔 |
| `src/kasasa-content-container.h:36-76` | 测试注入 API（capture ops setter）无守卫地暴露在生产头文件（应与 `KASASA_ENABLE_TESTS` 守卫一致） |
| `meson.build:19-73` | `test_c_args` 6 个重复的 `-Werror` 标志；命名误导——实际作用于全项目（含 `-Wno-*` 抑制与 `-Werror=shadow`） |
| `src/kasasa-wayland-stubs.c` | 经核实**不是死代码**（wayland-scanner 生成代码 `extern` 引用该符号），但可加注释说明 |
| `.github/workflows/ci.yml:13,58` | `container: fedora:latest` 未固定（moving target） |
| `src/kasasa-language.c:120-121` | `g_settings_new` 未守卫：从 build 目录直接运行（未安装 schema）产生 critical 链；`G_DEBUG=fatal-warnings` 下 abort。对比 `screencast.c` 有 schema 缺失回退 |
| `main.c:49` | `gst_init` 先于 GApplication，非法 `--gst-*` 值直接 abort，`--help` 来不及展示（GStreamer 标准行为） |

### 测试
| 位置 | 问题 |
|---|---|
| `test-hyprland-capture.c:233-274` | 取消竞态测试本身有竞态（生产代码正确，测试可能 flake） |
| `test-gtk-window.c:709-720` | 动画收缩断言边界较紧（Xvfb 负载下 CI flake 风险） |
| `test-gtk-application.c:109-113` | `kasasa_preferences_new` 被 stub 成 NULL（该二进制若激活 preferences action 会崩溃，当前不会） |
| `test-source.c:115-121` | 真实 1s sleep |

---

## 三、细节问题（🟢）

| 位置 | 问题 |
|---|---|
| `window.c:384` | `compute_size` 每次调用新建 GSettings 对象，应复用 `self->settings` |
| `window.c:1367-1382` | 迷你化计时器不检查 `mouse_over_window`：指针仍在 pin 内时 3s 后仍迷你化 |
| `content-container.c:1300, 1334, 1910, 1922, 2089` | 多个 handler 把可能 NULL 的 window 引用传入，靠 callee `g_return_*` 兜底 → warning 刷屏 |
| `content-container.c:2059-2061` | 复制图片时主线程同步解码 PNG（大图 UI 冻结） |
| `screencast.c:543-556` | 裁剪角拖过对边不重锚定 x/y，矩形不跟随指针 |
| `window-picker.c:154` | 无选中行按 Up 从第 0 行开始（应到最后一行） |
| `zoom.c:37-39` | 对角线触摸板手势判为缩放而非滑动（平局规则） |
| `crop-paintable.c:243-246` | `set_rect` 允许零尺寸裁切（UI 到不了，未来调用方得静默空白） |
| `window-query.c:949-952` | `%s` 传可能 NULL 的 monitor name/description |
| `window-query.c:775` | `g_assert_not_reached()`（仅损坏 spec 可达，低风险） |
| `main.c:49` | `gst_init` 先于 GApplication（见上） |
| `.gitignore` | 未列 `build*`（靠 Meson 内部 `.gitignore` 兜底） |
| `metainfo.xml.in:75` | 拼写错误 "Added **Frech** translation" |
| `desktop` | `Categories=Utility;Graphics;GTK;GNOME;` 两个主类别触发验证 hint |
| `hyprland-capture.c:139-218` | `copy_frame_rgba` 内层 `source_y*stride` 32 位平台可能溢出（公开测试钩子，真实流已约束） |

---

## 四、做得好的地方（✅）

1. **零 shell 注入**：全部子进程（hyprctl/slurp/grim）都是 argv 数组 spawn（`g_spawn_sync` / `g_subprocess_newv`），窗口标题/规格永不到达 shell；无 `system()/popen` 调用
2. **DMA-BUF 租约池设计**（`grefcount` + `busy[]` + `stopped` 广播）：fd 生命周期、槽位归还、worker 拆除 vs 主线程 GTK 释放的防 UAF 都正确；`ENSURE_DMABUF_BUSY` 干净丢帧；释放通知所有权转移单次释放保证
3. **线程纪律**：`finish()` 先 join 流线程再清 pipeline（杜绝 worker vs dispose UAF）；所有 worker→UI 回调走 `g_idle` + `g_object_ref`；跨线程状态原子/互斥
4. **溢出校验**：合成器提供的 width/height/stride 全部有 `G_MAXSIZE`/int32 联合校验；memfd+mmap 生命周期正确
5. **变换数学**：`kasasa-frame-transform.h` 8 种变换 + y-invert 与删除的内联代码及 GPU 矩阵路径逐项一致，无 off-by-one/溢出/除零；16 组组合有单元测试钉住
6. **析构清理**：window/screencast 的 dispose 把所有 timer/tick/bus watch 移除；weak-ref 异步请求结构；cancellable 取消 + `disconnect_by_data` 到位
7. **CLI 设计**：退出码 2（模糊匹配）与 README 一致；选项校验全面；geometry 正则校验；0600 symlink 安全临时文件；`--json` 用 JsonBuilder 正确转义、错误走 stderr 保持 stdout 可机读
8. **测试质量**：行为测试有真实断言与干净 fixtures（fake slurp/grim 脚本、受守卫的 fake 捕获后端、GTask 异步 fakes）；unit 7/7 <1s 通过；desktop/appstream/schema 均通过验证工具

---

## 五、修复优先级建议

| 批次 | 内容 | 理由 |
|---|---|---|
| **第一批（可靠性）** | ① fd close-before-flush（`stream.c:765-772`，低概率协议隐患）② hyprctl 超时（`window-query.c:445`）③ 直播失败路径对称清理（`screencast.c:1432-1606`，防御性）④ /tmp PNG 清理（`screenshot.c`/capture）⑤ 帧池重建优化（`screencast.c:176-209`，性能，非 UAF） | 协议隐患 + 冻结 + 泄漏 |
| **第二批（提交与流程）** | ⑥ 提交 `.github/` CI 与工作区重构 ⑦ `wayland-protocols >= 1.33` 约束 ⑧ 重新生成 pot、补齐 .po ⑨ bump 版本 + 更新 metainfo（URL 改 fork、补 release、删 portal 文案、修 "Frech"） | 仓库立即可见性问题 |
| **第三批（UI 一致性）** | 删页 resize（`content-container.c:2030-2034`）、`has_modal` 重构（去重 + AdwDialog 检测）、crop 中禁缩放、initial-reveal 重试上限、carousel 锁统一 | 用户可见状态不一致 |
| **第四批（加固）** | premultiplied alpha 修正、stride/format-table 校验、终端转义过滤、`g_settings_new` 兜底、slurp 误报取消、未知前缀报错、`--help` 文档化退出码 | 健壮性与正确性 |
| **第五批（测试）** | 为 window-picker/stream/screencast 补单测；integration 套件真正跑一次捕获；修取消竞态测试 | 覆盖缺口 |
| **收尾** | 删除过期 `.desktop`、补 PNG 图标、schema key 文档、`test_c_args` 去重拆分、死代码清理 | 卫生 |

---

*报告完。所有发现均有 `file:line` 依据，并经主代理与子代理双重交叉验证。*
