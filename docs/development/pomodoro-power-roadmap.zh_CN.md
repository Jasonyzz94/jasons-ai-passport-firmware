<p align="right">
  <strong>简体中文</strong> · <a href="pomodoro-power-roadmap.md">English</a>
</p>

# 番茄时钟省电路线（规划）

状态：**规划，未实施**。当前固件只做到多档背光熄灭（30s/60s/120s/5min/常亮），
CPU、LVGL、codec 与 I2C 外设保持工作。本文确定后续 light/deep sleep 的分阶段方案与
计时状态恢复设计，实施前逐阶段确认真机授权。

## 现状与目标

- 已具备：多档自动息屏、OK 长按手动息屏、唤醒首键只点亮不触发、NVS 保存时长/次数/
  主题/显示档位。
- 未具备：系统级休眠。背光熄灭后主控与外设仍在耗电。
- 目标：在不丢计时、不误触、不损坏外设的前提下降低空闲功耗，并用实测验证收益。

## 阶段划分

| 阶段 | 内容 | 计时状态恢复 | 风险/工作量 |
| --- | --- | --- | --- |
| B1 light sleep | 空闲自动浅睡，RAM 保持；音频用 `bsp_audio_sleep()`/`bsp_audio_wake()` 挂起恢复 | 基本不需要：`esp_timer` 在浅睡唤醒后由 RTC 补偿，现有截止时间戳继续有效 | 中：需验证 ADC 按键、I2C、USB 日志在浅睡前后的行为 |
| B2 deep sleep（仅非 RUNNING） | 空闲超时后终端关闭外设并深睡，GPIO0 按键唤醒；RTC 内存保存冻结的剩余时间与界面状态 | 唤醒即重启应用，从 RTC 内存恢复剩余时间、阶段、页面；唤醒键用现有 key_guard 防误触 | 中高：外设关闭顺序、唤醒后重初始化、显示闪烁需逐项真机验收 |
| B3（可选，暂缓）RUNNING 也深睡 | 睡到下一阶段截止点，RTC 定时唤醒 | 按睡眠时长推进状态机，跨多个阶段循环结算，提示音唤醒后补播 | 高：睡眠中无法刷新屏幕，体验更接近"定时器"而非番茄钟 |

执行顺序：B1 → 实测评估 → B2 → 再评估；B3 默认不做。

## B1 light sleep 设计

1. 入口：仅在背光已熄灭且无提示音播放时进入；有按键事件、蜂鸣任务在工作、或处于
   设置/帮助页交互中时不睡。
2. 方式：优先评估 ESP-IDF 自动浅睡（`CONFIG_PM_ENABLE` + FreeRTOS tickless idle），
   避免手工在 app 循环里塞 `esp_light_sleep_start()` 造成时序碎片。
3. 约束：light sleep **不得**调用任何终端引脚释放或 LCD hold 接口；音频恢复只能走
   `bsp_audio_wake()`。ADC 按键若使用 oneshot 驱动，需确认浅睡后采样与事件仍正常。
4. 计时：`pomo_tick()` 的 `now_ms` 来自 `esp_timer_get_time()`，浅睡后时间补偿由
   IDF 完成；验收时对比浅睡前后剩余时间误差。

## B2 deep sleep 设计（仅非 RUNNING）

进入条件：READY 或 PAUSED，且超过设定空闲时长（建议独立于息屏档位，如 5 分钟）。
RUNNING 不进入 B2。

睡前顺序（依仓库深睡档案）：

1. 配置唤醒源：`esp_sleep_enable_gpio_wakeup()`（GPIO0 低电平）+ 可选 RTC 定时器。
2. `bsp_battery_sleep()` → `bsp_audio_sleep()` → `bsp_audio_prepare_deep_sleep()` →
   `bsp_i2c_prepare_deep_sleep()` → `bsp_display_prepare_deep_sleep()`。
3. `bsp_button_prepare_deep_sleep()` 停止按键设备、释放共享 ADC unit，并把 GPIO0 交回
   数字输入开上拉。**ADC 接管时数字电平恒读 0，不做这一步会入睡即醒**。
4. `esp_deep_sleep_start()`；若意外返回，按终端路径处理（重启），不恢复已释放总线。

状态保存与恢复：

- 用 `RTC_DATA_ATTR` 结构体保存：魔数/版本、`phase`、`state`（仅 READY/PAUSED）、
  冻结的 `remaining_ms`、`work_min`、`break_min`、`done`、`theme`、`display_mode`、
  校验和。RTC 内存随 deep sleep 保留，避免每次入睡写 NVS 造成 flash 磨损。
- 时长/次数/主题/显示档位仍在变化时写 NVS（现状不变），作为 RTC 数据校验失败或
  整机上电后的回退。
- 唤醒后：`esp_sleep_get_wakeup_causes()` 命中 GPIO 时先 arm key_guard；正常 BSP
  初始化解除 LCD hold 并重建显示/音频/I2C；RTC 数据校验通过则恢复冻结的剩余时间和
  页面，否则按 NVS 时长回到 READY。
- PAUSED 恢复后仍是 PAUSED，剩余时间与入睡一致；READY 恢复后显示设定时长。

## 验收与测量

- Build：现有 `tools/validate.sh --all` 全绿；新增纯逻辑（状态序列化/恢复/跨阶段结算）
  先过主机测试。
- Device 矩阵：浅睡/深睡各覆盖：唤醒后剩余时间误差、完成次数、提示音不丢、按键无
  鬼触发、屏幕无异常闪烁、设置页数据一致、连续 10 次睡眠唤醒无重启循环。
- 功耗：同场景对比（固定 30 分钟，记录电量百分比变化；有条件用 USB 功耗仪测待机
  电流）。无实测数据不宣称省电比例。
- 风险：软件无法消除功放待机、稳压器静态电流、外部上拉等硬件负载，收益上限需在
  目标板测量后确认。
