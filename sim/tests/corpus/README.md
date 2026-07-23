# pi_card v2 golden corpus

Golden JSON corpus for the grid-only v2 card schema (see `docs/CARD_V2.md`), translated
from the v1 demo cards in `sim/main.cc` (`kCard*`). Used as input for solver golden tests,
streaming-prefix tests, and headless screenshot smoke.

## Positive examples (`sim/tests/corpus/*.json`)

| 文件 | v1 来源 | 迁移要点 |
|---|---|---|
| `00_device_ctl.json` | idx0 `kCard0` 设备控制 | 官方 v2 示例照抄；`spacer`→`switch side:end` |
| `01_confirm.json` | idx1 确认框 | 头部 cells + 长正文单独 cells(SPAN_ALL WRAP) + 两 button 均分 |
| `02_menu.json` | idx2 菜单 | 官方 v2 示例照抄；`rows` 每行一个 button（竖排菜单） |
| `03_status.json` | idx3 状态卡 | `rows`(icon,label,value 三列) 键值表；SIGNAL 行无 icon，缺列少写（2 cell） |
| `04_wrap_stress.json` | idx4 换行压力 | title+长正文各一个 SPAN_ALL cells + 6 button 一个 cells 交给 solver 折行 |
| `05_degenerate.json` | idx5 退化兜底 | slider min>max / 空 button / 未知 icon 均保留；v1 空 column 容器直接删除节点 |
| `07_arc.json` | idx7 音量旋钮 | `cells`([title]) + `cells`([arc,value-label]) |
| `08_qrcode.json` | idx8 扫码 | 两侧 spacer 删除，qrcode 独占行自动居中 |
| `09_choice.json` | idx9 难度选择 | `cells`([title]) + `cells`([choice]) + `cells`([button]) |
| `10_patch.json` | idx10 本地 patch | slider 去掉 `grow:1`（天然 FILL），value-label 保留 id |
| `11_telemetry.json` | idx11 P4a 遥测 | 两组 `rows` 键值表(温度/健康/续航/循环 + 运营商/IP/SD剩余) 用 divider 分块 |
| `12_charts.json` | idx12 P4a 图表 | section/value 行 + `chart` 块(`h:120` 删除)，电压/CPU 各一组 |
| `13_sensors.json` | idx13 P4b 传感器 | `rows` 键值表 + divider + 单 button(invoke device.vibrate) |
| `14_gps.json` | idx14 P4c GPS | `rows` 键值表 + divider + 两 button 均分(disable/enable) |
| `15_multicol.json` | idx15 多列对比 | A/B 两种写法合并为单一 `rows`(城市/温度num/天气)，对比说明 caption 删除 |
| `16_stock_bind.json` | idx16 股票绑定 | 头部 + `rows`(标的/现价/涨跌) + divider + `rows`(PE/PB/市值/换手等辅助字段) + 底部 caption(side:end) |
| `17_media_ctl.json` | idx17 媒体控制 | state/position 两 caption(mono,side:end) + bar + 3 button + `bind_rows`(tracks) 替代 v1 list |
| `18_stock_chart.json` | idx18 个股图表 | `w:520` 删除，SPAN_ALL 自动满宽 |
| `19_style_family.json` | idx19 样式家福 | 4 variant button 一行 + slider/bar/switch/choice/divider + `fill:"card2"` 容器块 |
| `22_table.json` | idx22 `kCardGridBasic` | 官方 v2 示例照抄；`cols` 对象表头(num 标数值列) |
| `23_grid_auto.json` | idx23 auto 轨道 | v1 `cols:["auto",1]` → `rows`(cols:[{title:"名称"},{title:"值"}])，value 列为纯文本未标 num（见拿不准点） |
| `24_grid_ctl.json` | idx24 控件混排 | v1 `cols:[1,2]` 数字权重 → `rows`（cols 省略，2 列非 num 内容自身推断） |
| `25_grid_tall.json` | idx25 长表格 | 22 行 `rows`(KEY/VAL num) 键值表原样保留 |
| `f0_form.json` | 文档 §1.5③ 官方 form | 无 v1 来源，直接照抄文档示例（亮度 slider / 静音 switch / 模式 choice + 保存 button）；rows 省略 `cols`（form 不该有表头，ncol 自动推断为 2） |
| `n1_cells_wrap.json` | 新增（文档 §5.2 建议） | 一个 `cells` 块含 8 个长度不等的 button，验证贪心折行+行内均分 |
| `n2_rows_align.json` | 新增（文档 §5.2 建议） | `rows` 验数值列右对齐/mono+文本列截断DOT；`cells`([label,switch side:end])；`cells`(两 arc 验同 grid 等径) |

## 负例 (`sim/tests/corpus/negative/*.json`)

| 文件 | 触发规则 | 预期拒绝理由 |
|---|---|---|
| `neg_choice_single_option.json` | `kCardChoiceBad` | choice options 只有 1 项，需 2-6 项 |
| `neg_qrcode_too_long.json` | qrcode text 300 字节 | qrcode text 须 ≤256 字节 |
| `neg_unknown_bind.json` | label bind 未注册路径 | bind 路径必须已在 DataHub 注册（`DataHub::Has`） |
| `neg_numeric_control_string_bind.json` | slider 绑到 `net.ssid`(String) | 数值控件不得 bind String 路径 |
| `neg_nested_container.json` | cells 里塞 `type:"column"` | v2 树深恒 2，禁止嵌套容器；`column/row` 类型不在自动扁平化白名单内 |
| `neg_fmt_percent_s_numeric_bind.json` | `kCardBadFmt`，数值路径 fmt 用 `%s` | 真机 newlib-nano 下 `%s` 配数值参数会解引用崩溃，`FmtSafeForType` 拒绝 |
| `neg_root_not_array.json` | root 非数组且带 `children` 键 | root 必须是数组；出现 `children` 说明是旧深层结构，无法安全自动展开修复 |
| `neg_too_many_nodes.json` | 4 grid × 17 label = 68 节点（grid 数 4 ≤8，不撞 grid 上限） | 节点数需 ≤64 |
| `neg_too_many_grids.json` | root 数组含 9 个 grid 块 | grid 数需 ≤8 |
| `neg_bind_rows_missing_item.json` | `bind_rows` 缺 `item` 字段 | bind_rows 需要 `item`（叶子或叶子数组） |
| `neg_grid_multiple_forms.json` | 同一 grid 块同时给 `cells` 和 `rows` | grid 必须恰含 `cells`/`rows`/`bind_rows` 三者之一 |
