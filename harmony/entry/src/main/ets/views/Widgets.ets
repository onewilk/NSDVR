// 各页面共用的小组件：卡片、设置行、提示条、标签
@Builder
function emptyBuilder() {
}

/** 圆角卡片：可选标题和说明，内容由尾随闭包提供 */
@Component
export struct Card {
  @Prop title: string = '';
  @Prop subtitle: string = '';
  @BuilderParam content: () => void = emptyBuilder;

  build() {
    Column({ space: 12 }) {
      if (this.title.length > 0) {
        Column({ space: 4 }) {
          Text(this.title)
            .fontSize(16)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
          if (this.subtitle.length > 0) {
            Text(this.subtitle)
              .fontSize(12)
              .fontColor($r('app.color.text_secondary'))
          }
        }
        .alignItems(HorizontalAlign.Start)
        .width('100%')
      }
      this.content()
    }
    .width('100%')
    .padding(16)
    .backgroundColor($r('app.color.card_bg'))
    .borderRadius(16)
    .alignItems(HorizontalAlign.Start)
  }
}

/** 设置行：左边图标 + 标题 + 说明，右边控件由尾随闭包提供（开关、下拉框等） */
@Component
export struct SettingRow {
  @Prop title: string = '';
  @Prop desc: string = '';
  @Prop disabledNote: string = '';
  /** 适用范围标签，如“仅 TCP Bridge”“RTSP 不可用” */
  @Prop scope: string = '';
  icon: Resource | null = null;
  @BuilderParam trailing: () => void = emptyBuilder;

  build() {
    Row({ space: 12 }) {
      if (this.icon !== null) {
        SymbolGlyph(this.icon)
          .fontSize(22)
          .fontColor([$r('app.color.accent')])
          .opacity(this.disabledNote.length > 0 ? 0.4 : 1)
      }
      Column({ space: 2 }) {
        Row({ space: 6 }) {
          Text(this.title)
            .fontSize(15)
            .fontColor($r('app.color.text_primary'))
          if (this.scope.length > 0) {
            Chip({ text: this.scope })
          }
        }
        Text(this.disabledNote.length > 0 ? this.disabledNote : this.desc)
          .fontSize(12)
          .fontColor(this.disabledNote.length > 0 ? $r('app.color.warn') : $r('app.color.text_secondary'))
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)
      .opacity(this.disabledNote.length > 0 ? 0.6 : 1)

      if (this.disabledNote.length === 0) {
        this.trailing()
      }
    }
    .width('100%')
    .constraintSize({ minHeight: 48 })
  }
}

/** 提示条：蓝色建议 / 红色警告 */
@Component
export struct TipBar {
  @Prop text: string = '';
  @Prop warn: boolean = false;
  /** 可选的操作按钮，如“去配置” */
  @Prop actionText: string = '';
  onAction: () => void = () => {
  };

  build() {
    Row({ space: 8 }) {
      SymbolGlyph(this.warn ? $r('sys.symbol.exclamationmark_circle') : $r('sys.symbol.lightbulb'))
        .fontSize(18)
        .fontColor([this.warn ? $r('app.color.warn') : $r('app.color.tip_text')])
      Column({ space: 4 }) {
        Text(this.text)
          .fontSize(13)
          .lineHeight(20)
          .fontColor(this.warn ? $r('app.color.warn') : $r('app.color.tip_text'))
        if (this.actionText.length > 0) {
          Text(this.actionText)
            .fontSize(13)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.accent'))
            .onClick(() => {
              this.onAction();
            })
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)
    }
    .alignItems(VerticalAlign.Top)
    .width('100%')
    .padding(12)
    .borderRadius(12)
    .backgroundColor(this.warn ? $r('app.color.warn_bg') : $r('app.color.tip_bg'))
  }
}

/** 小标签（如“推荐”“已连接”） */
@Component
export struct Chip {
  @Prop text: string = '';
  @Prop accent: boolean = false;

  build() {
    Text(this.text)
      .fontSize(11)
      .fontColor(this.accent ? $r('app.color.on_accent') : $r('app.color.text_secondary'))
      .backgroundColor(this.accent ? $r('app.color.accent') : $r('app.color.chip_bg'))
      .padding({ left: 8, right: 8, top: 2, bottom: 2 })
      .borderRadius(10)
  }
}

/** 编号步骤（使用说明页） */
@Component
export struct Step {
  @Prop index: number = 1;
  @Prop text: string = '';

  build() {
    Row({ space: 10 }) {
      Text(`${this.index}`)
        .fontSize(12)
        .fontColor($r('app.color.on_accent'))
        .backgroundColor($r('app.color.accent'))
        .width(20)
        .height(20)
        .borderRadius(10)
        .textAlign(TextAlign.Center)
        .margin({ top: 1 })
      Text(this.text)
        .fontSize(14)
        .lineHeight(22)
        .fontColor($r('app.color.text_primary'))
        .layoutWeight(1)
    }
    .alignItems(VerticalAlign.Top)
    .width('100%')
  }
}

/** 可点击的跳转行：图标 + 标题 + 说明 + 右箭头（或外链箭头） */
@Component
export struct NavRow {
  @Prop title: string = '';
  @Prop desc: string = '';
  @Prop external: boolean = false;
  icon: Resource | null = null;
  onTap: () => void = () => {
  };

  build() {
    Row({ space: 12 }) {
      if (this.icon !== null) {
        SymbolGlyph(this.icon).fontSize(22).fontColor([$r('app.color.accent')])
      }
      Column({ space: 2 }) {
        Text(this.title).fontSize(15).fontColor($r('app.color.text_primary'))
        if (this.desc.length > 0) {
          Text(this.desc)
            .fontSize(12)
            .fontColor($r('app.color.text_secondary'))
            .maxLines(2)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      SymbolGlyph(this.external ? $r('sys.symbol.arrow_up_right') : $r('sys.symbol.chevron_right'))
        .fontSize(18)
        .fontColor([$r('app.color.text_tertiary')])
    }
    .width('100%')
    .constraintSize({ minHeight: 48 })
    .onClick(() => {
      this.onTap();
    })
  }
}

/** 页面大标题 */
@Component
export struct PageTitle {
  @Prop title: string = '';
  @Prop subtitle: string = '';
  @Prop large: boolean = false;

  build() {
    Column({ space: 4 }) {
      Text(this.title)
        .fontSize(this.large ? 30 : 26)
        .fontWeight(FontWeight.Bold)
        .fontColor($r('app.color.text_primary'))
      if (this.subtitle.length > 0) {
        Text(this.subtitle)
          .fontSize(13)
          .fontColor($r('app.color.text_secondary'))
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
  }
}
