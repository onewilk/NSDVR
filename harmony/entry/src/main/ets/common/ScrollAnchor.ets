interface BlockPos {
  top: number;
  height: number;
  width: number;
}

/**
 * 折叠屏展开/折叠、窗口改变宽度时，让滚动页面停在原来看到的内容上。
 *
 * Scroll 只记住像素偏移：屏幕变宽后文字换行变少、整页变短，同样的偏移会落到后面的内容，甚至被截到页尾
 * （应用市场审核指出的“折叠屏切换后页面内容不连续”）。这里记下顶部是哪一块、滚到了这块的几分之几，
 * 宽度变化、各块按新宽度排好后，再滚回同一块的同一比例处。
 *
 * 用法：Scroll(anchor.scroller)，加 .onDidScroll(() => anchor.onScrolled())、
 * .onAreaChange((_o, n) => anchor.onResize(n))、.onAppear(() => anchor.onAppear())；
 * 内容 Column 的每个直接子组件加 .onAreaChange((_o, n) => anchor.report('键', n))。
 * 首页切换底部页签/侧边导航时页面会整个重建，所以各页的锚点放在组件外（见 PAGE_ANCHORS），重建后接着用。
 */
export class ScrollAnchor {
  readonly scroller: Scroller = new Scroller();
  private blocks: Map<string, BlockPos> = new Map();
  private anchorKey: string = '';
  private anchorFrac: number = 0;
  private anchorWidth: number = 0;
  private width: number = -1;
  private restoreTimer: number = -1;

  report(key: string, area: Area): void {
    this.blocks.set(key, { top: Number(area.position.y), height: Number(area.height), width: Number(area.width) });
  }

  // 条件显示的块消失时调用（onDisAppear），免得拿过期位置当锚点
  forget(key: string): void {
    this.blocks.delete(key);
  }

  onScrolled(): void {
    if (this.restoreTimer >= 0) {
      return; // 正在等新排版，这时的偏移还是旧排版下的
    }
    // 锚点那块的宽度变了：是重新排版带来的偏移变化（例如内容变短被截到页尾），不是用户在滚动，不覆盖锚点
    const anchor = this.blocks.get(this.anchorKey);
    if (anchor !== undefined && this.anchorWidth > 0 && Math.abs(anchor.width - this.anchorWidth) >= 1) {
      this.scheduleRestore();
      return;
    }
    const y = this.scroller.currentOffset().yOffset;
    let key = '';
    let top = -1;
    let height = 0;
    let width = 0;
    this.blocks.forEach((b: BlockPos, k: string) => {
      if (b.top <= y + 0.5 && b.top > top) {
        key = k;
        top = b.top;
        height = b.height;
        width = b.width;
      }
    });
    // 还在第一块上面（页面顶部的留白里）就不记，恢复时保持在顶部
    this.anchorKey = key;
    this.anchorFrac = height > 0 ? Math.min(1, Math.max(0, (y - top) / height)) : 0;
    this.anchorWidth = width;
  }

  onResize(area: Area): void {
    const w = Number(area.width);
    if (this.width >= 0 && Math.abs(w - this.width) >= 1) {
      this.scheduleRestore();
    }
    this.width = w;
  }

  // 页面重建（首页换导航布局、切语言）后新 Scroll 从顶部开始，回到重建前的位置
  onAppear(): void {
    this.scheduleRestore();
  }

  private scheduleRestore(): void {
    if (this.anchorKey.length === 0) {
      return;
    }
    if (this.restoreTimer >= 0) {
      clearTimeout(this.restoreTimer);
    }
    // 等各块按新宽度排完、报告新位置后再滚动
    this.restoreTimer = setTimeout(() => {
      this.restoreTimer = -1;
      const b = this.blocks.get(this.anchorKey);
      if (b !== undefined) {
        this.anchorWidth = b.width;
        this.scroller.scrollTo({ xOffset: 0, yOffset: b.top + this.anchorFrac * b.height, animation: false });
      }
    }, 120);
  }
}

/** 首页三个标签页的锚点：页面重建后沿用 */
export const PAGE_ANCHORS: ScrollAnchor[] = [new ScrollAnchor(), new ScrollAnchor(), new ScrollAnchor()];
