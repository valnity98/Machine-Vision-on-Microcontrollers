/* USER CODE BEGIN Header */
/**
******************************************************************************
* @file           : main.c
* @brief          : Main program body
******************************************************************************
* @attention
*
* STM32H743ZI Edge Vision — application entry point.
*
* ── Memory layout (see STM32H743ZITX_FLASH.ld for full details) ─────────────
*
*  Region   Base         Size    Section       Contents
*  ──────────────────────────────────────────────────────────────────────────
*  RAM_D1   0x24000000  512 KB  .dcmi_buf     frameBuffer  (255.0 KB, DMA)
*                                             cvBinBuffer  (127.5 KB)
*  RAM_D2   0x30000000  288 KB  .cvtmb_buf    cvTmpBuffer  (127.5 KB)
*                               .cvbg_buf     cvBgBuffer   (127.5 KB)
*  RAM_D3   0x38000000   64 KB  .tinyml_buf   g_activations (~57 KB)
*
* ── Buffer sizing rationale ──────────────────────────────────────────────────
*
*  APP_RGB_MAX_WIDTH  = 480   capture resolution width
*  APP_RGB_MAX_HEIGHT = 272   capture resolution height
*
*  frameBuffer  = 480 × 272 × 2 = 261 120 B (255 KB) — RGB565, DMA target
*  cvBinBuffer  = 480 × 272     = 130 560 B (128 KB) — CV work_bin
*  cvTmpBuffer  = 480 × 272     = 130 560 B (128 KB) — CV tmp_buf (RAM_D2)
*  cvBgBuffer   = 480 × 272     = 130 560 B (128 KB) — bgsub reference (RAM_D2)
*
*  cvBinBuffer and cvTmpBuffer must both be proc_w × proc_h bytes because the
*  CV pipeline writes the full image into each buffer across every stage.
*  Neither can be reduced without splitting the pipeline.  See cv_engine.c.
*
* ── MPU configuration ────────────────────────────────────────────────────────
*  Region 0: 0x00000000 / 4 GB     — No-access background region
*  Region 1: RAM_D1 0x24000000 / 512 KB — Non-Cacheable (TEX=1 C=0 B=0)
*  Region 2: RAM_D2 0x30000000 / 256 KB — Non-Cacheable (same attributes)
*  Region 3: RAM_D3 0x38000000 /  64 KB — Non-Cacheable (same attributes)
*
* ── ROI note ─────────────────────────────────────────────────────────────────
*  If a fixed ROI is always active the CV buffers could be sized for that ROI
*  only, saving ~54 KB per buffer.  Set APP_CV_MAX_WIDTH/HEIGHT to the ROI
*  dimensions to enable this.  Default is full-capture size so ROI can be
*  toggled at runtime.
*
* Copyright (c) 2026 STMicroelectronics.
* All rights reserved.
*
* This software is licensed under terms that can be found in the LICENSE file
* in the root directory of this software component.
* If no LICENSE file comes with this software, it is provided AS-IS.
*
******************************************************************************
*/
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "app_x-cube-ai.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "uart_tx.h"
#include "camera_app.h"
#include "camera_capture.h"
#include "camera_parse.h"
#include "camera_proto.h"
#include "cv_engine.h"
#include "task.h"
#include "queue.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* ── Capture resolution ───────────────────────────────────────────────────────
 * Maximum supported: 480×272.
 * VGA (640×480) is NOT possible: frameBuffer alone = 600 KB > 512 KB RAM_D1. */

#ifndef APP_RGB_MAX_WIDTH
#define APP_RGB_MAX_WIDTH   480u
#endif
#ifndef APP_RGB_MAX_HEIGHT
#define APP_RGB_MAX_HEIGHT  272u
#endif

/* ── CV buffer maximum region size ───────────────────────────────────────────
 * Must be >= the largest proc_w * proc_h that will ever be requested.
 * Default = full capture size so ROI can be toggled at runtime.
 * Reduce to a fixed ROI size only if ROI is always active. */

#ifndef APP_CV_MAX_WIDTH
#define APP_CV_MAX_WIDTH    APP_RGB_MAX_WIDTH
#endif
#ifndef APP_CV_MAX_HEIGHT
#define APP_CV_MAX_HEIGHT   APP_RGB_MAX_HEIGHT
#endif

/* ── Task stack sizes (words = 4 bytes each) ─────────────────────────────── */

#ifndef APP_UART_TX_QUEUE_LEN
#define APP_UART_TX_QUEUE_LEN        8u
#endif
#ifndef APP_CAMERA_TASK_STACK_WORDS
#define APP_CAMERA_TASK_STACK_WORDS  4096u
#endif
#ifndef APP_STREAM_TASK_STACK_WORDS
#define APP_STREAM_TASK_STACK_WORDS  1024u
#endif
#ifndef APP_UART_TASK_STACK_WORDS
#define APP_UART_TASK_STACK_WORDS    768u
#endif
#ifndef APP_HEARTBEAT_STACK_WORDS
#define APP_HEARTBEAT_STACK_WORDS    128u
#endif

/* ── Derived buffer sizes ────────────────────────────────────────────────── */

/* frameBuffer: full RGB565 capture frame, DMA target (2 bytes/pixel). */
#define FRAME_BUF_SIZE   ((uint32_t)APP_RGB_MAX_WIDTH  * APP_RGB_MAX_HEIGHT  * 2u)

/* cvBinBuffer (work_bin): 1 byte/pixel, sized for maximum processing region. */
#define CV_BIN_BUF_SIZE  ((uint32_t)APP_CV_MAX_WIDTH * APP_CV_MAX_HEIGHT)

/* cvTmpBuffer (tmp_buf): same size as cvBinBuffer; used for grayscale staging,
 * binary staging, and morphology intermediate.  Resides in RAM_D2. */
#define CV_TMP_BUF_SIZE  ((uint32_t)APP_CV_MAX_WIDTH * APP_CV_MAX_HEIGHT)

/* cvBgBuffer: grayscale background reference for subtraction.
 * Same region size as cvBinBuffer.  Resides in RAM_D2. */
#define CV_BG_BUF_SIZE   ((uint32_t)APP_CV_MAX_WIDTH * APP_CV_MAX_HEIGHT)

/* ── Compile-time memory budget checks ──────────────────────────────────────
 *
 * RAM_D1 holds frameBuffer + cvBinBuffer only (.dcmi_buf section).
 *   At 480×272: 261 120 + 130 560 = 391 680 B (383 KB) ≤ 512 KB ✅
 *
 * RAM_D2 holds cvTmpBuffer + cvBgBuffer (.cvtmb_buf + .cvbg_buf sections).
 *   At 480×272: 130 560 + 130 560 = 261 120 B (255 KB) ≤ 288 KB ✅
 *
 * RAM_D3 holds g_activations (.tinyml_buf); the linker enforces the 64 KB
 * limit directly — no compile-time approximation needed here. */

#define _RAM_D1_BUF_SIZE  (FRAME_BUF_SIZE + CV_BIN_BUF_SIZE)
_Static_assert(_RAM_D1_BUF_SIZE <= (512u * 1024u),
    "RAM_D1 overflow: frameBuffer + cvBinBuffer exceed 512 KB. "
    "Reduce APP_RGB_MAX_WIDTH/HEIGHT or APP_CV_MAX_WIDTH/HEIGHT.");

#define _RAM_D2_BUF_SIZE  (CV_TMP_BUF_SIZE + CV_BG_BUF_SIZE)
_Static_assert(_RAM_D2_BUF_SIZE <= (288u * 1024u),
    "RAM_D2 overflow: cvTmpBuffer + cvBgBuffer exceed 288 KB. "
    "Reduce APP_CV_MAX_WIDTH/HEIGHT.");

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

CRC_HandleTypeDef hcrc;

DCMI_HandleTypeDef hdcmi;
DMA_HandleTypeDef hdma_dcmi;

I2C_HandleTypeDef hi2c2;

UART_HandleTypeDef huart3;
DMA_HandleTypeDef hdma_usart3_tx;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE BEGIN PV */
/* ── Application state ───────────────────────────────────────────────────── */
static uart_tx_t    uart_tx;
static camera_app_t cam_app;
static uint8_t      rx_byte;   /* Single-byte ISR receive staging register. */

/* ── RAM_D1 (AXI SRAM, 0x24000000, Non-Cacheable via MPU Region 1) ──────────
 *
 * frameBuffer must be first (largest buffer, DMA target — keep at section start).
 * Both buffers are 32-byte aligned for cache-line granularity. */
__attribute__((section(".dcmi_buf"), aligned(32)))
static uint8_t frameBuffer[FRAME_BUF_SIZE];

__attribute__((section(".dcmi_buf"), aligned(32)))
static uint8_t cvBinBuffer[CV_BIN_BUF_SIZE];

/* ── RAM_D2 (AHB SRAM, 0x30000000, Non-Cacheable via MPU Region 2) ──────────
 *
 * cvTmpBuffer: CV pipeline scratch (filter, threshold, morphology).
 * cvBgBuffer:  grayscale background reference for background subtraction.
 * Neither buffer requires DMA access; CPU-only CV pipeline. */
__attribute__((section(".cvtmb_buf"), aligned(32)))
static uint8_t cvTmpBuffer[CV_TMP_BUF_SIZE];

__attribute__((section(".cvbg_buf"), aligned(32)))
static uint8_t cvBgBuffer[CV_BG_BUF_SIZE];

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_CRC_Init(void);
static void MX_DCMI_Init(void);
void MX_USART3_UART_Init(void);
static void MX_I2C2_Init(void);
void StartDefaultTask(void *argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */


/* ── FreeRTOS idle hook ──────────────────────────────────────────────────── */

/* Enter WFI in the idle task to reduce power consumption. */
void vApplicationIdleHook(void) { __WFI(); }

/* ── Heartbeat task ──────────────────────────────────────────────────────── */

/* Toggle red LED at 1 Hz to indicate the scheduler is running. */
static void heartbeat_task(void *arg)
{
    (void)arg;
    for (;;) {
        HAL_GPIO_TogglePin(LED_RED_GPIO_Port, LED_RED_Pin);
        vTaskDelay(pdMS_TO_TICKS(500u));
    }
}

/* ── ISR callbacks ───────────────────────────────────────────────────────── */

/* Push-button: notify the camera command task to take a snapshot. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == Push_Button_Pin) {
        if (!cam_app.cmd_task) { return; }
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(cam_app.cmd_task, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}

/* UART RX complete: forward received byte to the application RX queue. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        if (!cam_app.rx_q) { return; }
        BaseType_t hpw = pdFALSE;
        (void)xQueueSendFromISR(cam_app.rx_q, &rx_byte, &hpw);
        portYIELD_FROM_ISR(hpw);
        (void)HAL_UART_Receive_IT(huart, &rx_byte, 1u);
    }
}

/* UART TX DMA complete: notify the UART TX task to send the next chunk. */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if ((uart_tx.worker_task == NULL) || (huart != uart_tx.huart)) { return; }
    BaseType_t hpw = pdFALSE;
    vTaskNotifyGiveFromISR(uart_tx.worker_task, &hpw);
    portYIELD_FROM_ISR(hpw);
}


/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_CRC_Init();
  MX_DCMI_Init();
  MX_USART3_UART_Init();
  MX_I2C2_Init();
  MX_X_CUBE_AI_Init();
  /* USER CODE BEGIN 2 */

  /* UART TX service. */
      if (uart_tx_init(&uart_tx, &huart3, APP_UART_TX_QUEUE_LEN) != pdTRUE) {
          Error_Handler();
      }

      /* Application init — binds hardware handles and buffer pointers. */
      {
          QueueHandle_t rx_q = xQueueCreate(512u, sizeof(uint8_t));
          if (rx_q == NULL) { Error_Handler(); }

          camera_app_init(&cam_app,
                          &uart_tx,
                          &hi2c2,
                          &hdcmi,
                          RESET_GPIO_Port, RESET_Pin,
						  CTR_FLASH_GPIO_Port,CTR_FLASH_Pin,
                          frameBuffer,  FRAME_BUF_SIZE,
                          cvBinBuffer,  CV_BIN_BUF_SIZE,
                          cvTmpBuffer,  CV_TMP_BUF_SIZE,
                          cvBgBuffer,   CV_BG_BUF_SIZE,
                          &huart3,
                          rx_q);
      }

      /* Start UART interrupt reception. */
      (void)HAL_UART_Receive_IT(&huart3, &rx_byte, 1u);

      /* Create application tasks. */
      xTaskCreate(heartbeat_task,     "HB",
                  APP_HEARTBEAT_STACK_WORDS,   NULL,      1u, NULL);
      xTaskCreate(uart_tx_task,       "UART_TX",
                  APP_UART_TASK_STACK_WORDS,   &uart_tx,  2u, NULL);
      xTaskCreate(camera_app_task,    "CAM_APP",
                  APP_CAMERA_TASK_STACK_WORDS, &cam_app,  3u, NULL);
      xTaskCreate(camera_stream_task, "CAM_STREAM",
                  APP_STREAM_TASK_STACK_WORDS, &cam_app,  4u, NULL);

      /* Hand control to the FreeRTOS scheduler — does not return. */
      vTaskStartScheduler();
      Error_Handler();

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 120;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief CRC Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC_Init(void)
{

  /* USER CODE BEGIN CRC_Init 0 */

  /* USER CODE END CRC_Init 0 */

  /* USER CODE BEGIN CRC_Init 1 */

  /* USER CODE END CRC_Init 1 */
  hcrc.Instance = CRC;
  hcrc.Init.DefaultPolynomialUse = DEFAULT_POLYNOMIAL_ENABLE;
  hcrc.Init.DefaultInitValueUse = DEFAULT_INIT_VALUE_ENABLE;
  hcrc.Init.InputDataInversionMode = CRC_INPUTDATA_INVERSION_NONE;
  hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
  hcrc.InputDataFormat = CRC_INPUTDATA_FORMAT_BYTES;
  if (HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRC_Init 2 */

  /* USER CODE END CRC_Init 2 */

}

/**
  * @brief DCMI Initialization Function
  * @param None
  * @retval None
  */
static void MX_DCMI_Init(void)
{

  /* USER CODE BEGIN DCMI_Init 0 */

  /* USER CODE END DCMI_Init 0 */

  /* USER CODE BEGIN DCMI_Init 1 */

  /* USER CODE END DCMI_Init 1 */
  hdcmi.Instance = DCMI;
  hdcmi.Init.SynchroMode = DCMI_SYNCHRO_HARDWARE;
  hdcmi.Init.PCKPolarity = DCMI_PCKPOLARITY_RISING;
  hdcmi.Init.VSPolarity = DCMI_VSPOLARITY_LOW;
  hdcmi.Init.HSPolarity = DCMI_HSPOLARITY_LOW;
  hdcmi.Init.CaptureRate = DCMI_CR_ALL_FRAME;
  hdcmi.Init.ExtendedDataMode = DCMI_EXTEND_DATA_8B;
  hdcmi.Init.JPEGMode = DCMI_JPEG_DISABLE;
  hdcmi.Init.ByteSelectMode = DCMI_BSM_ALL;
  hdcmi.Init.ByteSelectStart = DCMI_OEBS_ODD;
  hdcmi.Init.LineSelectMode = DCMI_LSM_ALL;
  hdcmi.Init.LineSelectStart = DCMI_OELS_ODD;
  if (HAL_DCMI_Init(&hdcmi) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DCMI_Init 2 */

  /* USER CODE END DCMI_Init 2 */

}

/**
  * @brief I2C2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C2_Init(void)
{

  /* USER CODE BEGIN I2C2_Init 0 */

  /* USER CODE END I2C2_Init 0 */

  /* USER CODE BEGIN I2C2_Init 1 */

  /* USER CODE END I2C2_Init 1 */
  hi2c2.Instance = I2C2;
  hi2c2.Init.Timing = 0x307075B1;
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c2, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c2, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C2_Init 2 */

  /* USER CODE END I2C2_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 2000000;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);
  /* DMA2_Stream3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(CTR_FLASH_GPIO_Port, CTR_FLASH_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LED_GREEN_Pin|LED_RED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(RESET_GPIO_Port, RESET_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(PWDN_GPIO_Port, PWDN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : Push_Button_Pin */
  GPIO_InitStruct.Pin = Push_Button_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(Push_Button_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : CTR_FLASH_Pin */
  GPIO_InitStruct.Pin = CTR_FLASH_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(CTR_FLASH_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : LED_GREEN_Pin LED_RED_Pin */
  GPIO_InitStruct.Pin = LED_GREEN_Pin|LED_RED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : MCO1_Pin */
  GPIO_InitStruct.Pin = MCO1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(MCO1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : RESET_Pin */
  GPIO_InitStruct.Pin = RESET_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(RESET_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : PWDN_Pin */
  GPIO_InitStruct.Pin = PWDN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(PWDN_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(Push_Button_EXTI_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(Push_Button_EXTI_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN 5 */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END 5 */
}

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x24000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_512KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x30000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_256KB;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER3;
  MPU_InitStruct.BaseAddress = 0x38000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_64KB;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* Blink the red LED at 5 Hz so a debugger-less board shows a visible fault. */
  __disable_irq();
  while (1)
  {
      HAL_GPIO_TogglePin(GPIOB, LED_RED_Pin);
      for (volatile uint32_t i = 0u; i < 480000u; i++) { __NOP(); }
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */

    (void)file;
    (void)line;
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
