# Third-party code

The firmware project contains code from other sources. It keeps the license of its origin, and the license of this repository (see [LICENSE](LICENSE)) does not apply to it.

| Component | Where | License |
|---|---|---|
| STM32 HAL driver and CMSIS device files (STMicroelectronics) | `CubeIDE_Workspace/STM32_H7Firmware/Drivers/`, the generated files in `Core/` | see `Drivers/STM32H7xx_HAL_Driver/LICENSE.txt` and `Drivers/CMSIS/Device/ST/STM32H7xx/LICENSE.txt` |
| CMSIS core (Arm) | `Drivers/CMSIS/` | Apache-2.0, see `Drivers/CMSIS/LICENSE.txt` |
| FreeRTOS Kernel V10.6.2 | `Middlewares/Third_Party/FreeRTOS/` | MIT, see `Middlewares/Third_Party/FreeRTOS/Source/LICENSE` |
| X-CUBE-AI runtime and generated network files (STMicroelectronics) | `Middlewares/ST/AI/`, `X-CUBE-AI/` | see `Middlewares/ST/AI/LICENSE.txt` and `LICENSE_X-CUBE-AI.txt` (SLA0044) |
| OV2640 driver and register lists | `Core/Src/ov2640_Drive.c`, `Core/Src/ov2640_regs.c` and their headers | adapted from code found on the Internet and modified for this project; the original source and its license are not recorded yet |

The ST Model Zoo is used for training but is not part of this repository.
