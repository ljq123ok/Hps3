import { setPadButton, setPadStick } from 'libhps3core.so';

@Component
export struct VirtualGamePad {
  @Prop areaWidth: number = 0;
  @Prop areaHeight: number = 0;
  @State private stickOffsetX: number = 0;
  @State private stickOffsetY: number = 0;
  @State private rightStickOffsetX: number = 0;
  @State private rightStickOffsetY: number = 0;
  private leftStickTouchId: number = -1;
  private rightStickTouchId: number = -1;

  private areaW(): number {
    return Math.max(this.areaWidth, this.areaHeight);
  }

  private areaH(): number {
    return Math.min(this.areaWidth, this.areaHeight);
  }

  private buttonSize(): number {
    return Math.max(38, Math.min(58, this.areaH() * 0.13));
  }

  private gap(): number {
    return Math.max(5, this.buttonSize() * 0.16);
  }

  private leftCenterX(): number {
    return Math.max(this.buttonSize() * 1.75, this.areaW() * 0.12);
  }

  private rightCenterX(): number {
    return Math.min(this.areaW() - this.buttonSize() * 1.75, this.areaW() * 0.88);
  }

  private dpadCenterY(): number {
    const buttonGroupHalf = this.buttonSize() * 1.5 + this.gap();
    const minimumCenter = this.shoulderY() + this.buttonSize() * 0.34 + buttonGroupHalf + 6;
    const maximumCenter = this.stickCenterY() - this.stickRadius() - buttonGroupHalf - 8;
    return Math.max(minimumCenter, Math.min(this.areaH() * 0.44, maximumCenter));
  }

  private faceCenterY(): number {
    return this.dpadCenterY();
  }

  private stickRadius(): number {
    return Math.max(38, Math.min(54, this.areaH() * 0.15));
  }

  private stickCenterY(): number {
    return this.areaH() * 0.82;
  }

  private stickClickX(isLeft: boolean): number {
    const inset = this.stickRadius() * 2.35;
    return isLeft ? this.leftCenterX() + inset : this.rightCenterX() - inset;
  }

  private shoulderY(): number { return Math.max(26, this.areaH() * 0.075); }
  private systemY(): number { return Math.max(28, this.areaH() * 0.09); }

  @Builder
  padButton(label: string, index: number, cx: number, cy: number, w: number = this.buttonSize(),
    h: number = this.buttonSize(), round: boolean = true) {
    Button(label)
      .width(w)
      .height(h)
      .fontSize(round ? Math.max(15, this.buttonSize() * 0.34) : 13)
      .fontWeight(FontWeight.Medium)
      .fontColor('#F5FFFFFF')
      .padding(0)
      .backgroundColor('#62FFFFFF')
      .border({ width: 1, color: '#A6FFFFFF' })
      .borderRadius(round ? w / 2 : 12)
      .opacity(0.82)
      .position({ x: cx - w / 2, y: cy - h / 2 })
      .onTouch((event: TouchEvent) => {
        if (event.type === TouchType.Down) {
          setPadButton(index, true);
        } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
          setPadButton(index, false);
        }
      })
  }

  @Builder
  analogStick(isLeft: boolean) {
    Stack() {
      Circle()
        .width(this.stickRadius() * 2)
        .height(this.stickRadius() * 2)
        .fill('#35FFFFFF')
        .stroke('#8FFFFFFF')
        .strokeWidth(2)
      Circle()
        .width(this.stickRadius() * 0.92)
        .height(this.stickRadius() * 0.92)
        .fill('#B3FFFFFF')
        .offset({
          x: isLeft ? this.stickOffsetX : this.rightStickOffsetX,
          y: isLeft ? this.stickOffsetY : this.rightStickOffsetY
        })
    }
    .width(this.stickRadius() * 2)
    .height(this.stickRadius() * 2)
    .position({
      x: (isLeft ? this.leftCenterX() : this.rightCenterX()) - this.stickRadius(),
      y: this.stickCenterY() - this.stickRadius()
    })
    .onTouch((event: TouchEvent) => { this.handleStickTouch(event, isLeft); })
  }

  private handleStickTouch(event: TouchEvent, isLeft: boolean): void {
    event.stopPropagation();
    if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
      const activeId = isLeft ? this.leftStickTouchId : this.rightStickTouchId;
      const ended = event.changedTouches.length > 0 ? event.changedTouches[0].id : activeId;
      if (activeId >= 0 && ended !== activeId) {
        return;
      }
      if (isLeft) {
        this.leftStickTouchId = -1;
        this.stickOffsetX = 0;
        this.stickOffsetY = 0;
      } else {
        this.rightStickTouchId = -1;
        this.rightStickOffsetX = 0;
        this.rightStickOffsetY = 0;
      }
      setPadStick(isLeft ? 0 : 1, 128, 128);
      return;
    }

    if (event.type === TouchType.Down && event.changedTouches.length > 0) {
      if (isLeft) {
        this.leftStickTouchId = event.changedTouches[0].id;
      } else {
        this.rightStickTouchId = event.changedTouches[0].id;
      }
    }
    const activeId = isLeft ? this.leftStickTouchId : this.rightStickTouchId;
    if (activeId < 0) {
      return;
    }
    let touchX = 0;
    let touchY = 0;
    let touchWindowX = 0;
    let touchWindowY = 0;
    let found = false;
    for (let i = 0; i < event.touches.length; i++) {
      if (event.touches[i].id === activeId) {
        touchX = event.touches[i].x;
        touchY = event.touches[i].y;
        touchWindowX = event.touches[i].windowX;
        touchWindowY = event.touches[i].windowY;
        found = true;
        break;
      }
    }
    if (!found && event.changedTouches.length > 0 && event.changedTouches[0].id === activeId) {
      touchX = event.changedTouches[0].x;
      touchY = event.changedTouches[0].y;
      touchWindowX = event.changedTouches[0].windowX;
      touchWindowY = event.changedTouches[0].windowY;
      found = true;
    }
    if (!found) { return; }

    const radius = this.stickRadius();
    let localX = touchX;
    let localY = touchY;
    if (localX < -radius * 2 || localX > radius * 4 || localY < -radius * 2 || localY > radius * 4) {
      const centerX = isLeft ? this.leftCenterX() : this.rightCenterX();
      localX = touchWindowX - (centerX - radius);
      localY = touchWindowY - (this.stickCenterY() - radius);
    }
    let dx = localX - radius;
    let dy = localY - radius;
    const distance = Math.sqrt(dx * dx + dy * dy);
    if (distance > radius) {
      dx = dx * radius / distance;
      dy = dy * radius / distance;
    }
    if (isLeft) {
      this.stickOffsetX = dx;
      this.stickOffsetY = dy;
    } else {
      this.rightStickOffsetX = dx;
      this.rightStickOffsetY = dy;
    }
    setPadStick(isLeft ? 0 : 1,
      Math.round(128 + dx / radius * 127), Math.round(128 + dy / radius * 127));
  }

  build() {
    Stack() {
      this.padButton('↑', 0, this.leftCenterX(), this.dpadCenterY() - this.buttonSize() - this.gap());
      this.padButton('↓', 1, this.leftCenterX(), this.dpadCenterY() + this.buttonSize() + this.gap());
      this.padButton('←', 2, this.leftCenterX() - this.buttonSize() - this.gap(), this.dpadCenterY());
      this.padButton('→', 3, this.leftCenterX() + this.buttonSize() + this.gap(), this.dpadCenterY());

      this.padButton('△', 7, this.rightCenterX(), this.faceCenterY() - this.buttonSize() - this.gap());
      this.padButton('×', 4, this.rightCenterX(), this.faceCenterY() + this.buttonSize() + this.gap());
      this.padButton('□', 5, this.rightCenterX() - this.buttonSize() - this.gap(), this.faceCenterY());
      this.padButton('○', 6, this.rightCenterX() + this.buttonSize() + this.gap(), this.faceCenterY());

      this.padButton('L2', 9, this.areaW() * 0.075, this.shoulderY(), this.buttonSize() * 1.15, this.buttonSize() * 0.68, false);
      this.padButton('L1', 8, this.areaW() * 0.235, this.shoulderY(), this.buttonSize() * 1.15, this.buttonSize() * 0.68, false);
      this.padButton('R1', 11, this.areaW() * 0.765, this.shoulderY(), this.buttonSize() * 1.15, this.buttonSize() * 0.68, false);
      this.padButton('R2', 12, this.areaW() * 0.925, this.shoulderY(), this.buttonSize() * 1.15, this.buttonSize() * 0.68, false);

      this.padButton('SELECT', 15, this.areaW() * 0.425, this.systemY(), this.buttonSize() * 1.35, this.buttonSize() * 0.62, false);
      this.padButton('START', 14, this.areaW() * 0.575, this.systemY(), this.buttonSize() * 1.35, this.buttonSize() * 0.62, false);
      this.analogStick(true);
      this.analogStick(false);
      this.padButton('L3', 10, this.stickClickX(true), this.stickCenterY(),
        this.buttonSize() * 0.78, this.buttonSize() * 0.78);
      this.padButton('R3', 13, this.stickClickX(false), this.stickCenterY(),
        this.buttonSize() * 0.78, this.buttonSize() * 0.78);
    }
    .width('100%')
    .height('100%')
  }
}
