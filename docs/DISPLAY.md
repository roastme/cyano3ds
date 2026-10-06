# The Nintendo 3DS display, and why this port needs a custom framebuffer driver

This is the single most surprising part of the port, so it gets its own
document. Everything here is either quoted from 3dbrew/libctru or derived from
the two drivers we can actually read (`firm_linux_loader`, `libctru`).

## 1. The claim

**The 3DS LCD panels are wired portrait, so the framebuffer the LCD controller
scans is portrait even though the visible screen is landscape. Any image written
row-major by the CPU must be rotated 90° in memory to appear upright.**

Bottom screen: panel 320x240 (landscape, normal holding),
framebuffer **240x320**.
Top screen: panel 400x240 per eye, framebuffer **240x400**.

## 2. The evidence

### 2.1 libctru (the canonical 3DS homebrew library)

`libctru/include/3ds/services/gspgpu.h`:

```c
#define GSP_SCREEN_WIDTH         240 ///< Width of the top/bottom screens.
#define GSP_SCREEN_HEIGHT_TOP    400 ///< Height of the top screen.
#define GSP_SCREEN_HEIGHT_TOP_2X 800 ///< Height of the top screen (2x).
#define GSP_SCREEN_HEIGHT_BOTTOM 320 ///< Height of the bottom screen.
```

and `gfxGetFramebuffer()` returns exactly those pairs. So the *framebuffer* for
the bottom screen is 240 wide and 320 tall — a portrait buffer for a landscape
panel. (`gspGetBytesPerPixel()` / `gfxFlushBuffers()` size their cache flushes
with the same numbers.)

### 2.2 3dbrew, "GPU/External Registers" → "LCD Source Framebuffer Setup"

> "The naming of these parameters reflects the physical characteristics of the
> displays, and not the way the 3DS is normally held.
>
> To make sense of these values, the 3DS must be held in a way, so that the
> bottom screen is in the left hand, and the top screen is in the right hand,
> and that way the first pixel will be in the top-left corner, as it should be.
> **If the 3DS is held normally, the first pixel is in the bottom-left
> corner.**"

That single sentence pins the geometry: the framebuffer's origin is the panel's
bottom-left when held normally, i.e. the framebuffer rows run *up the left
column* of the visible image.

The same page documents the registers we need:

| Offset | Name | Notes |
|---|---|---|
| 0x64 | `??` / framebuffer total height | "high u16: framebuffer total height (amount of scanlines blitted regardless of framebuffer height)" |
| 0x68 | Framebuffer A first address | (double-buffer pair with 0x6C) |
| 0x6C | Framebuffer A second address | |
| 0x70 | Format | bits 2-0 colour format, 5-4 interlace, 9-8 DMA size |
| 0x74 | Control | bit 0 = enable display controller |
| 0x78 | Select | "Bit 0: Next framebuffer to display (after VBlank). Bit 4: Currently displaying framebuffer?" |
| 0x90 | Stride | "Distance in bytes between the start of two framebuffer rows (must be a multiple of 8)" |
| 0x94/0x98 | Framebuffer B | right eye / unused on the bottom screen |

Colour formats: `0 = RGBA8, 1 = RGB8, 2 = RGB565, 3 = RGB5A1, 4 = RGBA4`, with
the note *"Color components are laid out in reverse byte order"* — so format 1
is stored BGR (libctru calls it `GSP_BGR8_OES`) and **format 2 is plain
little-endian RGB565**, bit-for-bit identical to Android's
`HAL_PIXEL_FORMAT_RGB_565`.

### 2.3 `firm_linux_loader` (the FIRM payload that boots Linux)

`arm11/source/start.S` + `common/linux_config.h`:

```c
#define FB_TOP_SIZE           (400 * 240 * 3)
#define FB_BOT_SIZE           (320 * 240 * 3)
#define FB_BASE_PA            (VRAM_BASE)          /* 0x18000000 */
#define FB_TOP_LEFT1          (FB_BASE_PA)
#define FB_TOP_LEFT2          (FB_TOP_LEFT1  + FB_TOP_SIZE)
#define FB_TOP_RIGHT1         (FB_TOP_LEFT2  + FB_TOP_SIZE)
#define FB_TOP_RIGHT2         (FB_TOP_RIGHT1 + FB_TOP_SIZE)
#define FB_BOT_1              (FB_TOP_RIGHT2 + FB_TOP_SIZE)   /* 0x18119400 */
#define FB_BOT_2              (FB_BOT_1      + FB_BOT_SIZE)   /* 0x18151800 */
...
#define LCD_FB_PDC0_FORMAT    (0x80341)
#define LCD_FB_PDC0_STRIDE    (0x2D0)      /* 720 = 240 * 3 ! */
```

`0x2D0 = 720 = 240 px x 3 bytes`: the LCD's stride is 240 pixels, not 400 — even
though the buffer holds 400x240 pixels of data. 400 scanlines x 720 bytes =
288000 = `FB_TOP_SIZE` exactly. **The panel's "row" is 240 pixels long.**

`0x80341` decodes with the table above: colour format 1 (RGB8, stored BGR),
interlace bits 5-4 = 0 (A mode, no interlacing), bit 6 = 1 (the top screen's
"alternative pixel output mode", i.e. 2D mode), DMA size (bits 9-8) = 3.

### 2.4 linux-3ds' own device tree

`arch/arm/boot/dts/nintendo3ds.dtsi`:

```dts
display: framebuffer@18000000 {
        compatible = "simple-framebuffer";
        reg = <0x18000000 (400*240*3)>;
        width = <240>;      /* ! */
        height = <400>;     /* ! */
        stride = <(240*3)>;
        format = "r8g8b8";
};
```

…and `nintendo3ds_ktr.dts` boots with **`fbcon=rotate:1`**, i.e. the console
rendering is rotated to compensate for the transposed buffer. Two independent
confirmations in the upstream port itself.

### 2.5 Luma3DS - the authoritative "what works on real hardware"

Luma3DS is the CFW that boots and displays correctly on every 3DS, so its
screen init is the reference to mirror.  `arm11/source/main.c`,
`initScreens()`, for the **bottom** screen (PDC1 at `0x10400500`):

```
0x10400500 = 0x000001C2   HTotal  = 450
0x10400510 = 0x000000CD   HSync
0x10400560 = 0x01C100D1   HDisp: image 209..449  =>  240 px per scanline
0x10400568 = fbs[0].bottom      <- framebuffer A, first buffer
0x1040056C = fbs[1].bottom      <- framebuffer A, second buffer
0x10400570 = 0x00080301   Format: colour format 1 = 24bpp *stored BGR*,
                                 no interlacing, no alternate pixel mode,
                                 128-byte DMA bursts
0x10400574 = 0x00010501   Control (bit 0 = enable)
0x10400578 = 0             Select (bit 0 = next buffer after VBlank)
0x10400590 = 0x000002D0   Stride = 720 = 240 px * 3 bytes
```

and `arm11/source/types.h`: `SCREEN_BOTTOM_FBSIZE = 3 * 320 * 240` = 230400
bytes, i.e. **320 scanlines of 720 bytes** - a 240x320 portrait buffer for a
320x240 screen.

Three consequences for this port:

1. The portrait/transposed conclusion above is confirmed for the *bottom*
screen specifically (px/line = 240), not just inferred from the top screen.
2. The default scanout format on real hardware is **24bpp BGR**, not RGB565.
Both are self-consistent (stride = px/line x bytes/px), but a driver that
changes the format mid-flight is changing something the bootloader owns; the
port's driver therefore reads the format/stride/timing and adapts to *them*.
3. It also explains why the first build produced garbage on the bottom screen:
it wrote a 16bpp format and a 320-scanline value into registers that the
bootloader had set up as 24bpp/402, i.e. it re-timed a live panel.

## 3. The mapping, derived

Hold the console normally and look at the bottom screen. Name the visible
coordinates `(X, Y)`: `X = 0..319` left→right, `Y = 0..239` top→bottom.

Rotate the console 90° clockwise so that (per 3dbrew) the bottom screen is in
the left hand. In that orientation the visible image is upright and the
framebuffer is a plain row-major 240x320 image: framebuffer `x` runs along the
rotated view's "right", framebuffer `y` down the rotated view's "down".

The rotated view's "right" direction is the normal view's *up*, and its "down"
is the normal view's *right*. Therefore

```
fb(x, y)  is displayed at   X = y,  Y = 239 - x
        =>  fb(x = 239 - Y, y = X)  =  visible(X, Y)
```

so the framebuffer is the visible image **rotated 90° counter-clockwise**, and
conversely, to draw a landscape 320x240 image you must store it rotated 90°
clockwise. Check the corners: `fb(0,0)` → `(X,Y) = (0,239)` = bottom-left ✔
(exactly what 3dbrew says).

## 4. Why the kernel driver hides it

Android's software-GLES path mmaps the framebuffer and draws straight into it.
The generic renderer (`libGLES_android` / `framebuffer.cpp`) does this:

```c
    info.yres_virtual = info.yres * 2;
    info.bits_per_pixel = 16;
    info.red.offset = 11; ... /* RGB565 */
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &info) == -1) { /* no PAGE_FLIP */ }
    ...
    void* buffer = mmap(0, finfo.smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    offscreen[0] = buffer;
    offscreen[1] = buffer + finfo.line_length * info.yres;   /* the second buffer */
    ...
    mFb[i].width = info.xres; mFb[i].height = info.yres;
    mFb[i].stride = finfo.line_length / 2;
    mFb[i].format = GGL_PIXEL_FORMAT_RGB_565;
```

* The software GLES renderer draws straight into the mmap'ed framebuffer
  (`mFb[1 - mIndex]`), i.e. `buffer + line_length*yres`.
* `swapBuffers()` then writes `FBIOPUT_VSCREENINFO` with `yoffset = yres` to
  flip. There is no HAL call in that path — so a `copybit` HAL alone cannot
  rotate the finished frame.
* It also computes the refresh rate as
  `1e15 / ((upper+lower+yres) * (left+right+xres) * pixclock)` and divides by
  `pixclock`, so `pixclock` **must not be zero** (that would be a SIGFPE in the
  compositor).

Given that, there are only three ways to get an upright landscape UI:

1. **Patch libagl to rotate** — rotates every composited layer, 3-4x the
   software-rendering cost, and requires building the framework from source.
2. **Run Android in portrait and hold the console sideways** — zero cost, but
   bad UX and it forces a rotated touch mapping.
3. **Transpose once per flip inside the framebuffer driver** ← what we do.

`ctr_lcd_fb` therefore presents:

| `/dev/fb0` says | reality |
|---|---|
| `xres = 320`, `yres = 240`, bpp 16, RGB565 | the Android side of the story |
| `xres_virtual = 320`, `yres_virtual = 480` | two buffers of 153600 bytes |
| `line_length = 640` | so `offscreen[1] = buffer + 640*240` = exactly the second buffer |
| `fix.smem_len = 307200` | both render buffers, in **cached FCRAM** |
| `fb_pan_display(yoffset = 0 | 240)` | transposes the finished frame into the inactive **VRAM** buffer and flips the PDC select bit |

Consequences:

* Android needs **no source patches** to be correct — it can run from a
  prebuilt system image.
* Rendering happens in cached DRAM at full speed; the LCD never scans those
  buffers, so no cache maintenance is needed on the render side.
* The transpose is one pass over 76800 pixels per frame, tiled 16x16 to keep
  destination writes in 32-byte write-combining bursts: ~1 ms per flip on an
  804 MHz ARM11, versus 3-4 full-screen rotated blends if the framework did it.
* Double buffering is real: 3dbrew says bit 0 of the select register takes
  effect *after VBlank*, so the flip is tear-free.

## 5. Register values the driver programs

**Update (after the first hardware boot and after reading Luma3DS):** the
driver no longer *writes* a chosen format/stride.  Luma3DS - the reference
implementation that works on every 3DS - configures the bottom screen as
**24bpp stored BGR, stride 720 (240 px/line), 320 scanlines**
(`arm11/source/main.c`, `initScreens()`: `0x10400570 = 0x80301`,
`0x10400590 = 0x2D0`, and `SCREEN_BOTTOM_FBSIZE = 3 * 320 * 240`), so the
port's driver now *reads* those registers, derives the scanout geometry from
them (`px/line` from HDisp, bytes/pixel from stride/px-per-line) and writes
**only** the two buffer addresses and the select bit.  The panel timing is
owned by the bootloader and must never be touched - rewriting the pixel
format (e.g. to 16bpp) or the scanline-count fields instead garbles the
panel into a strip of colour noise.

What the driver writes on every flip:

| Register | Value | Meaning |
|---|---|---|
| 0x68 | `vram + 0` | framebuffer A, first buffer |
| 0x6C | `vram + scanout_size` | framebuffer A, second buffer |
| 0x74 | read/modify/write, bit 0 set | keep the controller enabled |
| 0x78 | `0` / `1` | which buffer the LCD takes at the next VBlank |

Read-only (logged at probe, never written unless the DT sets
`nintendo,lcd-bpp`): 0x60 HDisp, 0x64, 0x70 format, 0x90 stride.

The advertised fbdev geometry stays 320x240/16bpp/RGB565 in all cases; only
the internal conversion changes (BGR888 or RGB565 scanout, transposed or
direct).

## 6. Hardware verification checklist

The display was verified on a real New 3DS.  The driver paints a self-test
pattern directly into scanout memory at probe (no userspace involved), designed
so that every axis of the mapping is visible in one glance.  In the transposed
(real hardware) layout it must look like:

```
+--------+--------+--------+--------+
|  dim   |  dim   |  dim   |  dim   |   <- upper half: same 4 bands,
|  red   | green  |  blue  | white  |      half brightness
+--------+--------+--------+--------+
| bright | bright | bright | bright |
| yellow | green  |  blue  | white  |   <- lower half full brightness
| red    |        |        |        |
+--------+--------+--------+--------+
  ^ yellow marker lives in the bottom-left corner
```

* four **vertical** bands, red leftmost, white rightmost;
* the lower half of each band at full brightness, the upper half dimmed
  (this is the test for the sign of the second axis);
* a **yellow** square in the **bottom-left** corner.

On diagnostic boots (a `diag` file in `CYANO3DS/`), `fbtest` (see
`port/userland/tests/fbtest.c`) additionally replaces it with 8 colour bars, a
staircase, an arrow and an animated bar, which check the fbdev ABI and double
buffering.

Register-level checks, all logged by the driver at probe:

1. `px/line` should be 240 (portrait) - if it is 320 the driver picks the
   direct path and says so.
2. `stride / px/line` (bytes per pixel) should be 3 on a stock console.
3. The flip pair (`fbA`) should be our own VRAM addresses, not the bootloader's
   old ones, after `ctr_fb_set_par()` runs.
4. `select` bit 4 tells which buffer the panel is currently scanning; the
   driver waits for that bit to clear before writing the other buffer.

## 7. Sources

* 3dbrew, *GPU/External Registers* — "LCD Source Framebuffer Setup",
  "Framebuffer format", "Framebuffer color formats"
  <https://www.3dbrew.org/wiki/GPU/External_Registers>
* 3dbrew, *Hardware* (screen resolutions)
  <https://www.3dbrew.org/wiki/Hardware>
* libctru `include/3ds/services/gspgpu.h`, `source/gfx.c`
  <https://github.com/devkitPro/libctru>
* linux-3ds `firm_linux_loader` — `arm11/source/start.S`,
  `common/linux_config.h`
  <https://github.com/linux-3ds/firm_linux_loader>
* linux-3ds kernel — `arch/arm/boot/dts/nintendo3ds.dtsi`,
  `arch/arm/boot/dts/nintendo3ds_ktr.dts`
  <https://github.com/linux-3ds/linux>
* AOSP — the libagl software renderer (`frameworks/base/libs/ui/`)
  <https://android.googlesource.com/platform/frameworks/base/+/gingerbread>
