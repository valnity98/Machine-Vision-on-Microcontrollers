# UART protocol

Back to the [README](../README.md).

## UART protocol

**Connection:** USART3, **2 000 000 Baud, 8N1**, no flow control

### Frame transfer headers (raw bytes follow immediately after `\r\n`)

```
JPG: <bytes>
RGB565: <width> <height> <bytes>
```

### Status / log lines

```
INFO:  <message>
WARN:  <message>
ERR:   <message>
DEBUG: <message>
STAT:  FPS=X.X SIZE=XB HEAP=XKB FB=XKB LAT=Xms
```

### Classical Vision (CV)

```
CVCFG: EN=1 PRESET=0 THR=128 THRMODE=0 INV=0
       BLUR=0 FILTER=0 MORPH=0 MORPHMODE=0 CON=8
       MIN=50 MAX=0 ARMIN=0 ARMAX=0 CIRCMIN=0
       BORDFILT=1 BGSUB=0 BGCAP=0
       ROIEN=0 ROIX=0 ROIY=0 ROIW=0 ROIH=0

CVSTAT: COUNT=N MEAN=X MAX=X MIN=X BRIGHT=X TIME=X
        REJSMALL=X REJLARGE=X REJBORDER=X REJSHAPE=X
        FGPIX=X RAWCOMP=X BOXES=N
CVBOX:  ID=N AREA=X X=X Y=X W=X H=X PERI=X CIRC=X
CVDONE
```

### Camera commands

| Command | Effect |
|---------|--------|
| `MODE JPEG\|RGB` | Pixel format; `RGB` is RGB565. Resets `ZOOM` |
| `RES QQVGA\|QVGA\|WQVGA\|VGA\|SVGA\|XGA\|SXGA` | Resolution, see the table below. Resets `ZOOM` |
| `SNAP` | One frame (`JPG:` or `RGB565:` header, then the bytes). Not allowed while streaming |
| `STREAM 0\|1` | Continuous JPEG frames (JPEG mode only) |
| `GET_STATUS` | `STAT:` line |
| `BRI` / `CON` / `SAT` `-2\|-1\|0\|+1\|+2` | Brightness, contrast, saturation |
| `EFFECT NORMAL\|BW\|NEGATIVE\|NEGATIVE_BW\|BLUISH\|GREENISH\|REDDISH\|ANTIQUE` | Colour effect |
| `LIGHT AUTO\|SUNNY\|CLOUDY\|OFFICE\|HOME` | White-balance preset (all except `AUTO` use fixed gains) |
| `AWB 0\|1` | See the note below |
| `FLASH 0\|1` | Flash LED on for the following `SNAP`s; the LED is lit for 1 s before the frame is taken |
| `ZOOM 1\|2\|4` | RGB mode only, see the note below |

Frame size that the board delivers (checked on a Nucleo-144 board):

| `RES` | JPEG mode | RGB mode |
|-------|-----------|----------|
| `QQVGA` | **176×144** (not 160×120) | 160×120 |
| `QVGA` | 320×240 | 320×240 |
| `WQVGA` | **320×240** (not 480×272) | 480×272 |
| `VGA` | 640×480 | refused (`ERR: buffer too small for RGB frame`: 614 KB do not fit into the 255 KB frame buffer) |
| `SVGA` | 800×600 | 320×240 (no RGB setting for it) |

> `AWB 1` (the default) selects the sensor's simple white balance, `AWB 0` its other automatic mode. Neither switches the white balance off; fixed gains come from `LIGHT SUNNY`, `CLOUDY`, `OFFICE` and `HOME`. In a test scene `AWB 1` gave a clear green cast (R/G 0.76, B/G 0.79) and `AWB 0` a neutral balance (R/G 0.95, B/G 0.94). The OV2640 datasheet text for register 0xC7 was not available; the mapping (0x10 simple, 0x00 the other automatic mode, 0x40 off) follows a [Linux driver commit](https://patchwork.linuxtv.org/project/linux-media/patch/20170416173546.4317-4-fschaefer.oss@googlemail.com) and the colours measured on the board.
>
> `ZOOM 2` and `ZOOM 4` reduce the output size to ½ and ¼ (for example 160×120 from 320×240). The field of view stays the same: in a test the zoomed frame correlated with the down-scaled full frame (0.996) and hardly with the centre crop (0.34). It is a smaller image, not a magnified detail.

### TinyML

```
TMCFG:  EN=1 INPUT=96x96x3 CLASSES=5 MODEL=count_model
TMINFO: STATUS=XCUBEAI_OK RAM=41KB FLASH=215KB
TMRES:  CLASS=COUNT_N IDX=N CONF=XXX TIME=X UNCERTAIN=0
TMPROB: IDX=N NAME=COUNT_N SCORE=XXX
TMDONE
```

> `TIME` (in `CVSTAT` and `TMRES`) is the run time in **milliseconds**, printed as a plain integer without a unit suffix (`HAL_GetTick()` difference, 1 ms resolution). Only the `STAT` line has a unit suffix (`LAT=Xms`).
> In `TMINFO`, `RAM` is the activation buffer and `FLASH` the network weights, each rounded up to full KiB (41 KiB / 215 KiB for this model); they are not the totals including runtime.
> `CONF` and `SCORE` are in **permille (0…1000)**. The GUI normalises to 0.0–1.0.  
> When `UNCERTAIN=1`, `CLASS=UNCERTAIN` and `IDX=-1`; the GUI marks it as uncertain, not a real class.

**TinyML commands**

| Command | Effect |
|---------|--------|
| `TM RUN` | Run inference on last RGB565 frame |
| `TM GET` | Request TMCFG + TMINFO |
| `TM EN 0\|1` | Enable / disable TinyML |

