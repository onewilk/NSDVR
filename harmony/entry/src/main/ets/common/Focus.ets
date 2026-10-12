import { ColorMetrics, LengthMetrics } from '@kit.ArkUI';

/**
 * 遥控器/键盘能选中的自定义点击区域（Row、Column 等默认不能获焦）：加 .focusable(true).focusBox(FOCUS_BOX)。
 * 焦点框用亮蓝色、3vp 粗，电视上隔得远也看得清；只在用方向键或 Tab 操作时出现，触屏上看不到。
 * 系统组件（按钮、开关、下拉框、页签）的焦点框颜色在 EntryAbility 里用主题统一设置
 */
export const FOCUS_BOX: FocusBoxStyle = {
  margin: LengthMetrics.vp(2),
  strokeColor: ColorMetrics.rgba(63, 140, 255, 1),
  strokeWidth: LengthMetrics.vp(3)
};
