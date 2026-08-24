# 冷却按钮实现与验证方案

仓库当前没有既有前端框架，因此按钮以 `ui/cooldown_button.js` 的无框架控制器
和 `ui/cooldown_button.html` 的可运行演示提供，后续可嵌入 React/Vue 或原生页面。

## 性能方案

- 使用 `readyAt` 绝对单调时间点，不递减计数器，检测为 O(1)。
- 所有按钮共享 `SharedFrameScheduler`，只在存在活动冷却时驱动帧；避免每按钮
  一个 timer 和重复 wakeup。
- 动画只更新进度变换和文字，状态对象很小，不分配大对象或启动后台线程。
- 冷却完成即取消订阅；组件销毁取消订阅并使旧响应失效。

## 安全可靠性

- 客户端禁用只用于体验，服务端仍必须执行授权、幂等 token、重放防护和限流。
- 服务端返回的冷却时长/截止时间是权威值；拒绝响应不会进入冷却。
- generation token 防止迟到的旧请求重新启用新一轮按钮。
- 动画附带 `aria-live` 状态，键盘和指针都走同一 click guard，颜色不是唯一提示。

## TDD 覆盖

`ui/test_cooldown_button.js` 使用无时间依赖的 FakeScheduler 覆盖：

1. 成功动作启动冷却和动画帧。
2. 冷却期间重复激活被拒绝。
3. 到期后按钮恢复可用并停止订阅。
4. 并发/迟到请求不会覆盖当前状态。
5. 服务端拒绝不会启动冷却。

运行：

```sh
node ui/test_cooldown_button.js
ctest --test-dir build --output-on-failure -R test_cooldown_button
```
