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

