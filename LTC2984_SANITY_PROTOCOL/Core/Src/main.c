/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body - Prueba de radiación LTC2984
  *                   Chip 0 = referencia, Chip 1 = DUT (bajo haz).
  *                   El DUT se "limpia" (scrubbing) después de cada lectura:
  *                   volcado -> comparación con copia maestra -> reescritura
  *                   -> verificación. Incluye registros de canal (0x200-0x24F)
  *                   y máscara de conversión múltiple (0x0F4-0x0F7).
  ******************************************************************************
  * @attention
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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <stdarg.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define LTC2984_CMD_WRITE 0x02
#define LTC2984_CMD_READ  0x03

#define CHIP_REF 0   /* Placa de referencia */
#define CHIP_DUT 1   /* Placa bajo prueba   */

#define LTC_NUM_CH           20
#define LTC_ADDR_CH_CFG(ch)  ((uint16_t)(0x200 + ((ch) - 1) * 4))
#define LTC_ADDR_RESULT(ch)  ((uint16_t)(0x010 + ((ch) - 1) * 4))
#define LTC_ADDR_MASK        0x0F4   /* 0x0F4..0x0F7: 4 bytes consecutivos */

/* Copia maestra (en flash, no en RAM) */
/* Rsense en CH2: bits 31:27 = 29 (resistencia de referencia),
   bits 26:0 = R * 1024 -> 0x00FA000 = 1024000 / 1024 = 1000,000 Ohm */
#define CFG_RSENSE   0xE80FA000UL
#define CFG_PT1000   0x78860000UL    /* PT-1000, Rsense en CH2            */
/* 0x0F4=0x00, 0x0F5=0x0A, 0x0F6=0xAA, 0x0F7=0xA8 -> CH4,6,...,20        */
#define MASK_MULTI   0x000AAAA8UL

#define CONV_TIMEOUT_MS  1000U
#define TEMP_INVALID     (-999.0f)
#define FAULT_TIMEOUT    0xFF       /* Marca de "no hubo conversión"      */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
SPI_HandleTypeDef hspi1;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
volatile int vit[2] = {0, 0};
volatile int sanity_ok[2] = {0, 0};
volatile uint8_t status_reg[2] = {0, 0};
volatile uint8_t byte_estado = 0;

volatile float temp_ch[2][22];
volatile uint8_t fault_ch[2][22];
volatile uint32_t raw_raw_ch[2][22];
volatile uint32_t raw_ch[2][22] = {0};

// ---- SANIDAD / SCRUBBING DEL DUT ----
volatile uint32_t ciclo = 0;
volatile uint32_t config_leida_ch[22] = {0}; // Valor leído ANTES de reescribir
volatile uint32_t mask_leida = 0;            // Máscara leída ANTES de reescribir
volatile uint32_t seu_flags = 0;             // Ciclo actual: bit (ch-1)=canal, bit 20=máscara
volatile uint32_t seu_cnt_ch[22] = {0};      // Nº de ciclos con el registro alterado
volatile uint32_t seu_cnt_mask = 0;
volatile uint32_t rewrite_fail_cnt = 0;      // Reescrituras que no verificaron
volatile int      rewrite_status = 0;        // 1 = OK, 0 = falló verificación, -1 = omitida (timeout)
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */
void LTC_Write_Reg(uint8_t chip, uint16_t addr, uint32_t data);
void LTC_Write_Byte(uint8_t chip, uint16_t addr, uint8_t data);
uint32_t LTC_Read_Reg(uint8_t chip, uint16_t addr);
uint8_t LTC_Read_Byte(uint8_t chip, uint16_t addr);
void ProcessTemperature(uint8_t chip, uint32_t raw_data, uint8_t channel);
void LTC_Write_Golden(uint8_t chip);
int  LTC_Scrub(uint8_t chip, uint8_t do_rewrite);
uint8_t LTC_Wait_Done(uint8_t chip);
void SendDataUART_T(void);
void SendDataUART_S(void);
void SendDataUART_E(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* Valor maestro de configuración por canal (código en flash, inmune a SEU en RAM) */
static inline uint32_t Golden_Cfg(uint8_t ch)
{
    return (ch == 2) ? CFG_RSENSE : CFG_PT1000;
}

/* snprintf acumulativo que nunca se sale del buffer */
static int buf_append(char *buf, int size, int off, const char *fmt, ...)
{
    if (off >= size - 1) return off;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, (size_t)(size - off), fmt, ap);
    va_end(ap);
    if (n < 0) return off;
    off += n;
    return (off > size - 1) ? (size - 1) : off;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* Configure the system clock */
  SystemClock_Config();

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_SPI1_Init();
  MX_USART2_UART_Init();

  /* USER CODE BEGIN 2 */

  // Reinicio por hardware de ambos chips (línea RST compartida)
  HAL_GPIO_WritePin(LTC_RST_GPIO_Port, LTC_RST_Pin, GPIO_PIN_RESET);
  HAL_Delay(10);
  HAL_GPIO_WritePin(LTC_RST_GPIO_Port, LTC_RST_Pin, GPIO_PIN_SET);
  HAL_Delay(1000);

  // Inicialización de ambos chips
  for (uint8_t chip = 0; chip < 2; chip++) {

      // Chequeo de vitalidad inicial (tras reset: Done=1, Start=0 -> 0x40)
      byte_estado = LTC_Read_Byte(chip, 0x000);
      vit[chip] = (byte_estado == 0x40) ? 1 : -1;
      HAL_Delay(500);

      // Chequeo de comunicación SPI (se sobrescribe después con la config real)
      uint32_t test_val = 0xDEADBEEF;
      LTC_Write_Reg(chip, LTC_ADDR_CH_CFG(2), test_val);
      sanity_ok[chip] = (LTC_Read_Reg(chip, LTC_ADDR_CH_CFG(2)) == test_val) ? 1 : -1;
      HAL_Delay(500);

      // Configuración de canales (1 al 20) + máscara de conversión múltiple
      LTC_Write_Golden(chip);
  }

  // Verificación inicial del DUT (no cuenta como SEU: se limpian contadores)
  rewrite_status = LTC_Scrub(CHIP_DUT, 1);
  for (uint8_t ch = 0; ch < 22; ch++) seu_cnt_ch[ch] = 0;
  seu_cnt_mask = 0;
  rewrite_fail_cnt = 0;

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
      ciclo++;

      // ==========================================
      // 1. LECTURA DEL CHIP 0 (REFERENCIA, SOLO CANAL 4)
      // ==========================================
      LTC_Write_Byte(CHIP_REF, 0x000, 0x80 | 4);   // Conversión directa CH4
      uint8_t chip0_done = LTC_Wait_Done(CHIP_REF);

      status_reg[0] = LTC_Read_Byte(CHIP_REF, 0x000);
      vit[0] = chip0_done ? 1 : -1;

      if (chip0_done) {
          raw_ch[0][4] = LTC_Read_Reg(CHIP_REF, LTC_ADDR_RESULT(4));
          ProcessTemperature(CHIP_REF, raw_ch[0][4], 4);
      } else {
          temp_ch[0][4]  = TEMP_INVALID;   // no reportar datos viejos
          fault_ch[0][4] = FAULT_TIMEOUT;
      }

      // ==========================================
      // 2. LECTURA DEL CHIP 1 (DUT, CANALES PARES POR MÁSCARA)
      // ==========================================
      LTC_Write_Byte(CHIP_DUT, 0x000, 0x80);        // Conversión múltiple
      uint8_t chip1_done = LTC_Wait_Done(CHIP_DUT);

      status_reg[1] = LTC_Read_Byte(CHIP_DUT, 0x000);
      vit[1] = chip1_done ? 1 : -1;

      for (uint8_t ch = 4; ch <= LTC_NUM_CH; ch += 2) {
          if (chip1_done) {
              raw_ch[1][ch] = LTC_Read_Reg(CHIP_DUT, LTC_ADDR_RESULT(ch));
              ProcessTemperature(CHIP_DUT, raw_ch[1][ch], ch);
          } else {
              temp_ch[1][ch]  = TEMP_INVALID;
              fault_ch[1][ch] = FAULT_TIMEOUT;
          }
      }

      // ==========================================
      // 3. SANIDAD + REESCRITURA DEL DUT (DESPUÉS DE LA LECTURA)
      //    Volcado -> comparación -> reescritura -> verificación.
      //    Si la conversión no terminó, solo se vuelca: no se escribe
      //    la RAM del LTC2984 con una conversión en curso.
      // ==========================================
      rewrite_status = LTC_Scrub(CHIP_DUT, chip1_done);

      // ==========================================
      // 4. ENVÍO DE DATOS
      // ==========================================
      SendDataUART_T();
      SendDataUART_S();
      SendDataUART_E();

      HAL_Delay(500);
  }
  /* USER CODE END WHILE */

  /* USER CODE BEGIN 3 */
}
/* USER CODE END 3 */

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 10;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  HAL_GPIO_WritePin(LTC_RST_GPIO_Port, LTC_RST_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOA, LTC_CS_Pin|LTC2_CS_Pin, GPIO_PIN_SET);

  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = LTC_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LTC_RST_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = LTC_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(LTC_CS_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = LTC_INT_Pin|LTC2_INT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = LTC2_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LTC2_CS_GPIO_Port, &GPIO_InitStruct);
}

/* USER CODE BEGIN 4 */

static inline void LTC_CS_Low(uint8_t chip) {
    if (chip == 0) {
        HAL_GPIO_WritePin(LTC_CS_GPIO_Port, LTC_CS_Pin, GPIO_PIN_RESET);
    } else {
        HAL_GPIO_WritePin(LTC2_CS_GPIO_Port, LTC2_CS_Pin, GPIO_PIN_RESET);
    }
}

static inline void LTC_CS_High(uint8_t chip) {
    if (chip == 0) {
        HAL_GPIO_WritePin(LTC_CS_GPIO_Port, LTC_CS_Pin, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(LTC2_CS_GPIO_Port, LTC2_CS_Pin, GPIO_PIN_SET);
    }
}

void LTC_Write_Byte(uint8_t chip, uint16_t address, uint8_t data) {
    uint8_t tx_data[4];

    tx_data[0] = LTC2984_CMD_WRITE;
    tx_data[1] = (uint8_t)(address >> 8);
    tx_data[2] = (uint8_t)(address & 0xFF);
    tx_data[3] = data;

    LTC_CS_Low(chip);
    HAL_SPI_Transmit(&hspi1, tx_data, 4, HAL_MAX_DELAY);
    LTC_CS_High(chip);
}

uint8_t LTC_Read_Byte(uint8_t chip, uint16_t address) {
    uint8_t tx_data[4];
    uint8_t rx_data[4] = {0};

    tx_data[0] = LTC2984_CMD_READ;
    tx_data[1] = (uint8_t)(address >> 8);
    tx_data[2] = (uint8_t)(address & 0xFF);
    tx_data[3] = 0x00;

    LTC_CS_Low(chip);
    HAL_SPI_TransmitReceive(&hspi1, tx_data, rx_data, 4, HAL_MAX_DELAY);
    LTC_CS_High(chip);

    return rx_data[3];
}

/* Escritura de 4 bytes consecutivos (MSB primero, dirección auto-incremental) */
void LTC_Write_Reg(uint8_t chip, uint16_t address, uint32_t data) {
    uint8_t tx_data[7];

    tx_data[0] = LTC2984_CMD_WRITE;
    tx_data[1] = (uint8_t)(address >> 8);
    tx_data[2] = (uint8_t)(address & 0xFF);

    tx_data[3] = (uint8_t)(data >> 24);
    tx_data[4] = (uint8_t)(data >> 16);
    tx_data[5] = (uint8_t)(data >> 8);
    tx_data[6] = (uint8_t)(data & 0xFF);

    LTC_CS_Low(chip);
    HAL_SPI_Transmit(&hspi1, tx_data, 7, HAL_MAX_DELAY);
    LTC_CS_High(chip);
}

/* Lectura de 4 bytes consecutivos (MSB primero, dirección auto-incremental) */
uint32_t LTC_Read_Reg(uint8_t chip, uint16_t address) {
    uint8_t tx_data[7] = {0};
    uint8_t rx_data[7] = {0};

    tx_data[0] = LTC2984_CMD_READ;
    tx_data[1] = (uint8_t)(address >> 8);
    tx_data[2] = (uint8_t)(address & 0xFF);

    LTC_CS_Low(chip);
    HAL_SPI_TransmitReceive(&hspi1, tx_data, rx_data, 7, HAL_MAX_DELAY);
    LTC_CS_High(chip);

    return ((uint32_t)rx_data[3] << 24) |
           ((uint32_t)rx_data[4] << 16) |
           ((uint32_t)rx_data[5] << 8)  |
           ((uint32_t)rx_data[6]);
}

/* Espera a que INT suba (fin de conversión). Seguro ante desborde de HAL_GetTick */
uint8_t LTC_Wait_Done(uint8_t chip) {
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < CONV_TIMEOUT_MS) {
        GPIO_PinState s = (chip == 0)
            ? HAL_GPIO_ReadPin(LTC_INT_GPIO_Port,  LTC_INT_Pin)
            : HAL_GPIO_ReadPin(LTC2_INT_GPIO_Port, LTC2_INT_Pin);
        if (s == GPIO_PIN_SET) return 1;
        HAL_Delay(1);
    }
    return 0;
}

/* Escribe toda la configuración desde la copia maestra:
   20 registros de canal + máscara de conversión múltiple (0x0F4..0x0F7). */
void LTC_Write_Golden(uint8_t chip) {
    for (uint8_t ch = 1; ch <= LTC_NUM_CH; ch++) {
        LTC_Write_Reg(chip, LTC_ADDR_CH_CFG(ch), Golden_Cfg(ch));
    }
    LTC_Write_Reg(chip, LTC_ADDR_MASK, MASK_MULTI);
}

/* Scrubbing del DUT:
   1) Volcado de todos los registros (valor ANTES de reescribir -> detecta SEU)
   2) Comparación con la copia maestra y conteo por registro
   3) Reescritura incondicional (si do_rewrite)
   4) Verificación de la reescritura
   Retorna 1 = verificado OK, 0 = la verificación falló, -1 = reescritura omitida */
int LTC_Scrub(uint8_t chip, uint8_t do_rewrite) {
    uint32_t flags = 0;

    // 1-2. Volcado y comparación
    for (uint8_t ch = 1; ch <= LTC_NUM_CH; ch++) {
        uint32_t v = LTC_Read_Reg(chip, LTC_ADDR_CH_CFG(ch));
        config_leida_ch[ch] = v;
        if (v != Golden_Cfg(ch)) {
            seu_cnt_ch[ch]++;
            flags |= (1UL << (ch - 1));
        }
    }
    mask_leida = LTC_Read_Reg(chip, LTC_ADDR_MASK);
    if (mask_leida != MASK_MULTI) {
        seu_cnt_mask++;
        flags |= (1UL << 20);
    }
    seu_flags = flags;

    if (!do_rewrite) {
        return -1;
    }

    // 3. Reescritura incondicional de todo
    LTC_Write_Golden(chip);

    // 4. Verificación
    uint8_t ok = 1;
    for (uint8_t ch = 1; ch <= LTC_NUM_CH; ch++) {
        if (LTC_Read_Reg(chip, LTC_ADDR_CH_CFG(ch)) != Golden_Cfg(ch)) {
            ok = 0;
        }
    }
    if (LTC_Read_Reg(chip, LTC_ADDR_MASK) != MASK_MULTI) {
        ok = 0;
    }
    if (!ok) {
        rewrite_fail_cnt++;
    }
    return ok;
}

void ProcessTemperature(uint8_t chip, uint32_t raw_data, uint8_t channel){
    uint8_t status_byte = (uint8_t)(raw_data >> 24);
    fault_ch[chip][channel] = status_byte;

    uint8_t valid = status_byte & 0x01;

    if (!valid || (status_byte & 0xCE)) {
        temp_ch[chip][channel] = TEMP_INVALID;
        return;
    }

    uint32_t temp_data_24b = raw_data & 0x00FFFFFF;
    raw_raw_ch[chip][channel] = temp_data_24b;

    int32_t signed_temp = (int32_t)temp_data_24b;
    if (signed_temp & 0x00800000) {
        signed_temp |= 0xFF000000;
    }

    temp_ch[chip][channel] = (float)signed_temp / 1024.0f;
}

/* TEMP|CYC:n|C0_CH4:t:0xFF|C1_CH4:t:0xFF|...|C1_CH20:t:0xFF */
void SendDataUART_T(void) {
    char buf[320];
    int off = 0;

    off = buf_append(buf, sizeof(buf), off, "TEMP|CYC:%lu|C0_CH4:%.2f:0x%02X",
                     (unsigned long)ciclo, temp_ch[0][4], fault_ch[0][4]);
    for (uint8_t ch = 4; ch <= LTC_NUM_CH; ch += 2) {
        off = buf_append(buf, sizeof(buf), off, "|C1_CH%u:%.2f:0x%02X",
                         ch, temp_ch[1][ch], fault_ch[1][ch]);
    }
    off = buf_append(buf, sizeof(buf), off, "\r\n");

    HAL_UART_Transmit(&huart2, (uint8_t*)buf, (uint16_t)off, HAL_MAX_DELAY);
}

/* SAN|CYC:n|RW:1|FLG:0x000000|CH1:0x........|...|CH20:0x........|MASK:0x........
   Valores leídos ANTES de reescribir. FLG: bit(ch-1)=canal alterado, bit20=máscara */
void SendDataUART_S(void) {
    char buf[512];
    int off = 0;

    off = buf_append(buf, sizeof(buf), off, "SAN|CYC:%lu|RW:%d|FLG:0x%06lX",
                     (unsigned long)ciclo, rewrite_status, (unsigned long)seu_flags);
    for (uint8_t ch = 1; ch <= LTC_NUM_CH; ch++) {
        off = buf_append(buf, sizeof(buf), off, "|CH%u:0x%08lX",
                         ch, (unsigned long)config_leida_ch[ch]);
    }
    off = buf_append(buf, sizeof(buf), off, "|MASK:0x%08lX\r\n", (unsigned long)mask_leida);

    HAL_UART_Transmit(&huart2, (uint8_t*)buf, (uint16_t)off, HAL_MAX_DELAY);
}

/* SEU|CYC:n|CH1:n|...|CH20:n|MASK:n|RWFAIL:n  (contadores acumulados) */
void SendDataUART_E(void) {
    char buf[384];
    int off = 0;

    off = buf_append(buf, sizeof(buf), off, "SEU|CYC:%lu", (unsigned long)ciclo);
    for (uint8_t ch = 1; ch <= LTC_NUM_CH; ch++) {
        off = buf_append(buf, sizeof(buf), off, "|CH%u:%lu", ch, (unsigned long)seu_cnt_ch[ch]);
    }
    off = buf_append(buf, sizeof(buf), off, "|MASK:%lu|RWFAIL:%lu\r\n",
                     (unsigned long)seu_cnt_mask, (unsigned long)rewrite_fail_cnt);

    HAL_UART_Transmit(&huart2, (uint8_t*)buf, (uint16_t)off, HAL_MAX_DELAY);
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
}
#endif /* USE_FULL_ASSERT */
