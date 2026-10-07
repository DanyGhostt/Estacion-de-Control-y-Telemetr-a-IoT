/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include <stdbool.h>
#include <stdint.h>

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
#define CMD_MOTOR_DC       0x01
#define CMD_ULTRASONIC     0x02
#define CMD_SERVO          0x03
#define CMD_TEMP           0x04
#define CMD_EMERGENCY      0xEE

//Driver TB6612FNG
#define MOTOR_STOP         0x00
#define MOTOR_CW_MAX       0x01
#define MOTOR_CW_MIN       0x02
#define MOTOR_CCW_MAX      0x03
#define MOTOR_CCW_MIN      0x04
#define MOTOR_BRAKE        0x05

typedef struct __attribute__((packed)) {
    uint8_t startMarker;   // 0xAA
    uint8_t commandCode;   // Identificador de acción
    uint8_t payloadLength; // 0x01
    uint8_t actionData;    // Parámetro (ángulo, velocidad, etc.)
    uint8_t checksum;      // Validación matemática
    uint8_t endMarker;     // 0x55
} RemoteInteractionFrame_t;


/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define TIMEOUT_TICKS      60000U  //Ticks para la activacion de el sensor ultrasonico
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart3;

/* Definitions for vSensorTask */
osThreadId_t vSensorTaskHandle;
const osThreadAttr_t vSensorTask_attributes = {
  .name = "vSensorTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for vMotorTask */
osThreadId_t vMotorTaskHandle;
const osThreadAttr_t vMotorTask_attributes = {
  .name = "vMotorTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for QueueCommand */
osMessageQueueId_t QueueCommandHandle;
const osMessageQueueAttr_t QueueCommand_attributes = {
  .name = "QueueCommand"
};
/* USER CODE BEGIN PV */

volatile int8_t g_temperature_c = 25;
RemoteInteractionFrame_t esp32_frame = {0xAA, 0x01, 0x01, 0x01, 0x03, 0x55};
RemoteInteractionFrame_t esp32_frame_rx; // Buffer de recepción DMA

volatile bool g_emergency_state = false;
volatile uint32_t g_distance_cm = 0;
volatile uint8_t g_current_speed = 50;  // Velocidad activa (se va a 0 en STOP y OFF)
volatile uint8_t g_saved_speed = 50;    // Memoria para reanudar al presionar ON
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART3_UART_Init(void);
void StartSensorTask(void *argument);
void StartMotorTask(void *argument);

/* USER CODE BEGIN PFP */
void Hardware_Actuators_Init(void);
void Hardware_Ultrasonic_Init(void);
void Hardware_Emergency_Button_Init(void);
void DMA_USART3_TX_Init(void);
void DMA_USART3_RX_Init(void);

void Set_Servo_Angle(uint8_t angle_deg);
void Control_DC_Motor(uint8_t mode);
void Emergency_Stop_All(void);
uint32_t Ultrasonic_Read_Distance_cm(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

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
  //MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
    // 1. Inicialización física de periféricos mediante CMSIS puro
      Hardware_Actuators_Init();
      Hardware_Ultrasonic_Init();
      Hardware_Emergency_Button_Init();

      // 2. ACTIVAR RELOJES (INDISPENSABLE PARA USART3, GPIOS Y DMA)
      RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_DMA1EN;
      RCC->APB1ENR |= RCC_APB1ENR_USART3EN;

      // 3. Configurar PB10 (TX) y PC5 (RX) en Función Alterna AF7
      GPIOB->MODER &= ~(0x3U << (10 * 2));
      GPIOB->MODER |=  (0x2U << (10 * 2)); // PB10 en AF
      GPIOC->MODER &= ~(0x3U << (5 * 2));
      GPIOC->MODER |=  (0x2U << (5 * 2));  // PC5 en AF

      GPIOB->AFR[1] &= ~(0xFU << ((10 - 8) * 4));
      GPIOB->AFR[1] |=  (7U << ((10 - 8) * 4)); // AF7 para USART3_TX en PB10

      GPIOC->AFR[0] &= ~(0xFU << (5 * 4));
      GPIOC->AFR[0] |=  (7U << (5 * 4));        // AF7 para USART3_RX en PC5

      // 4. Baudrate exacto a 115200 bps con bus APB1 a 42 MHz
      USART3->BRR = 0x16D;
      USART3->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
      USART3->CR3 = USART_CR3_DMAT | USART_CR3_DMAR;

      DMA_USART3_TX_Init();
      DMA_USART3_RX_Init();

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

  /* Create the queue(s) */
  /* creation of QueueCommand */
  QueueCommandHandle = osMessageQueueNew (4, 6, &QueueCommand_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of vSensorTask */
  vSensorTaskHandle = osThreadNew(StartSensorTask, NULL, &vSensorTask_attributes);

  /* creation of vMotorTask */
  vMotorTaskHandle = osThreadNew(StartMotorTask, NULL, &vMotorTask_attributes);

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
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

/* USER CODE BEGIN MX_GPIO_Init_2 */
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
// ====================================================================================================
// 1. CONTROLADORES DE ACTUADORES (PA6 Servo 50Hz, PB6 PWM Motor 10kHz, PB3/4/5 TB6612)
// ====================================================================================================
void Hardware_Actuators_Init(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN  | RCC_APB1ENR_TIM4EN;

    __NOP();
    __NOP();

    // Servomotor: PA6 en AF2 (TIM3_CH1) a 50 Hz exactos
    GPIOA->MODER &= ~(3U << (6 * 2));
    GPIOA->MODER |=  (2U << (6 * 2));
    GPIOA->AFR[0] &= ~(0xFU << (6 * 4));
    GPIOA->AFR[0] |=  (2U << (6 * 4)); // AF2

    TIM3->PSC = 84 - 1;                // 84 MHz / 84 = 1 MHz (1 tick = 1 us)
    TIM3->ARR = 20000 - 1;             // 20000 us = 20 ms de periodo (50 Hz)
    TIM3->CCMR1 &= ~(TIM_CCMR1_OC1M | TIM_CCMR1_CC1S);
    TIM3->CCMR1 |=  (6U << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;
    TIM3->CCER  |= TIM_CCER_CC1E;
    TIM3->CCR1   = 1500;               // Posición inicial neutra (90 grados)
    TIM3->CR1   |= TIM_CR1_CEN;

    // Motor Amarillo: PB6 en AF2 (TIM4_CH1) a 10 kHz
    GPIOB->MODER &= ~(3U << (6 * 2));
    GPIOB->MODER |=  (2U << (6 * 2));
    GPIOB->AFR[0] &= ~(0xFU << (6 * 4));
    GPIOB->AFR[0] |=  (2U << (6 * 4)); // AF2

    TIM4->PSC = 84 - 1;                // 1 MHz
    TIM4->ARR = 100 - 1;               // 100 us = 10 kHz (Rango de 0 a 100% de ciclo de trabajo)
    TIM4->CCMR1 &= ~(TIM_CCMR1_OC1M | TIM_CCMR1_CC1S);
    TIM4->CCMR1 |=  (6U << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;
    TIM4->CCER  |= TIM_CCER_CC1E;
    TIM4->CCR1   = 0;                  // Apagado inicial
    TIM4->CR1   |= TIM_CR1_CEN;

    // Pines de dirección TB6612: PB3 (STBY), PB4 (AIN1), PB5 (AIN2) como Salidas Push-Pull
    GPIOB->MODER &= ~((3U << (3 * 2)) | (3U << (4 * 2)) | (3U << (5 * 2)));
    GPIOB->MODER |=  ((1U << (3 * 2)) | (1U << (4 * 2)) | (1U << (5 * 2)));
    GPIOB->OTYPER &= ~((1U << 3) | (1U << 4) | (1U << 5));

    // STBY = 1 (Activo), AIN1 = 0, AIN2 = 0 (Reposo inicial)
    GPIOB->BSRR = (1U << 3) | (1U << (4 + 16)) | (1U << (5 + 16));


    // Dejar PB0 en estado de entrada para que la resistencia externa levante la línea a 3.3V
    GPIOB->MODER &= ~(3U << (0 * 2));
    GPIOB->PUPDR &= ~(3U << (0 * 2));
}

// ====================================================================================================
// 2. SENSOR ULTRASONICO (PA9 Trigger, PA8 Echo en TIM1_CH1 - Entrada FT a 5V)
// ====================================================================================================
void Hardware_Ultrasonic_Init(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;

    __NOP();
    __NOP();

    // PA9 Trigger Salida
    GPIOA->MODER &= ~(3U << (9 * 2));
    GPIOA->MODER |=  (1U << (9 * 2));
    GPIOA->OTYPER &= ~(1U << 9);

    // PA8 Echo Entrada Función Alterna AF1 (Tolerante a 5V)
    GPIOA->MODER &= ~(3U << (8 * 2));
    GPIOA->MODER |=  (2U << (8 * 2));
    GPIOA->PUPDR &= ~(3U << (8 * 2)); // No-Pull obligatorio
    GPIOA->AFR[1] &= ~(0xFU << ((8 - 8) * 4));
    GPIOA->AFR[1] |=  (1U << ((8 - 8) * 4)); // AF1 = TIM1

    TIM1->PSC = 84 - 1;               // 1 tick = 1 us
    TIM1->ARR = 0xFFFF;
    TIM1->CCMR1 &= ~TIM_CCMR1_CC1S;
    TIM1->CCMR1 |= (1U << TIM_CCMR1_CC1S_Pos); // Mapeado a TI1
    TIM1->CCER &= ~(TIM_CCER_CC1P | TIM_CCER_CC1NP);
    TIM1->CCER |= TIM_CCER_CC1E;
    TIM1->CR1  |= TIM_CR1_CEN;
}

// ====================================================================================================
// 3. PARO DE EMERGENCIA EN BOTON AZUL (PC13 / EXTI13)
// ====================================================================================================
void Hardware_Emergency_Button_Init(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;

    __NOP();
    __NOP();

    GPIOC->MODER &= ~(3U << (13 * 2)); // Entrada
    GPIOC->PUPDR &= ~(3U << (13 * 2));

    SYSCFG->EXTICR[3] &= ~(0xFU << (1 * 4));
    SYSCFG->EXTICR[3] |=  (0x2U << (1 * 4)); // Conectar EXTI13 al Puerto C

    EXTI->FTSR |= EXTI_FTSR_TR13;            // Flanco de bajada (al presionar cae a 0V)
    EXTI->IMR  |= EXTI_IMR_MR13;             // Habilitar máscara

    NVIC_SetPriority(EXTI15_10_IRQn, 5);     // Prioridad compatible con FreeRTOS
    NVIC_EnableIRQ(EXTI15_10_IRQn);
}

// ====================================================================================================
// 4. FUNCIONES DE CONTROL DE MOVIMIENTO Y SEGURIDAD
// ====================================================================================================
void Set_Servo_Angle(uint8_t angle_deg) {
//    if (g_emergency_state) return;
		if (angle_deg > 180) angle_deg = 180;
		TIM3->CCR1 = 1000 + ((uint32_t)angle_deg * 1000) / 180;
}

void Control_DC_Motor(uint8_t mode) {
    if (g_emergency_state && mode != MOTOR_STOP && mode != MOTOR_BRAKE) return;

    switch (mode) {
        case MOTOR_CW_MAX: // ON (Resume: reanuda a la velocidad previa o 50%)
            if (g_saved_speed == 0) g_saved_speed = 50;
            g_current_speed = g_saved_speed;

            GPIOB->BSRR = (1U << 4) | (1U << (5 + 16)); // AIN1=1, AIN2=0 (Giro horario)
            TIM4->CCR1 = g_current_speed;
            break;

        case MOTOR_CW_MIN:
            GPIOB->BSRR = (1U << 4) | (1U << (5 + 16));
            TIM4->CCR1 = 35;
            break;

        case MOTOR_CCW_MAX:
            if (g_saved_speed == 0) g_saved_speed = 50;
            g_current_speed = g_saved_speed;

            GPIOB->BSRR = (1U << (4 + 16)) | (1U << 5); // AIN1=0, AIN2=1
            TIM4->CCR1 = g_current_speed;
            break;

        case MOTOR_CCW_MIN:
            GPIOB->BSRR = (1U << (4 + 16)) | (1U << 5);
            TIM4->CCR1 = 35;
            break;

        case MOTOR_BRAKE: // STOP: Bloqueo activo y apagado total sin residuales
            if (g_current_speed > 0) {
                g_saved_speed = g_current_speed; // Memoriza la velocidad activa
            }
            g_current_speed = 0;

            // Freno activo instantáneo en TB6612 (cortocircuito de bornes)
            GPIOB->BSRR = (1U << 4) | (1U << 5); // AIN1=1, AIN2=1
            TIM4->CCR1 = 100;

            // Pulso breve para absorber la inercia del rotor
            for (volatile int i = 0; i < 4000; i++) { __NOP(); }

            // Corte total: desconecta el puente H y apaga el timer
            GPIOB->BSRR = (1U << (4 + 16)) | (1U << (5 + 16)); // AIN1=0, AIN2=0
            TIM4->CCR1 = 0;
            break;

        case MOTOR_STOP:  // OFF: Desconexión suave conservando memoria
        default:
            if (g_current_speed > 0) {
                g_saved_speed = g_current_speed;
            }
            g_current_speed = 0;

            GPIOB->BSRR = (1U << (4 + 16)) | (1U << (5 + 16)); // AIN1=0, AIN2=0
            TIM4->CCR1 = 0;
            break;
    }
}
void Emergency_Stop_All(void) {
    g_emergency_state = true;
    TIM4->CCR1 = 0;
    GPIOB->BSRR = (1U << 4) | (1U << 5); // Freno activo inmediato en TB6612
    TIM3->CCR1 = 1500;                  // Servo centrado en posición neutral
}

void EXTI15_10_IRQHandler(void) {
    if (EXTI->PR & EXTI_PR_PR13) {
        EXTI->PR = EXTI_PR_PR13; // Limpieza de bandera

        // 1. Corte inmediato en el silicio
        Emergency_Stop_All();

        // 2. Despachar trama de emergencia a la cola del RTOS
        RemoteInteractionFrame_t emergency_frame = {
            .startMarker = 0xAA,
            .commandCode = CMD_EMERGENCY,
            .payloadLength = 0x01,
            .actionData = 0x01,
            .checksum = (uint8_t)(CMD_EMERGENCY + 0x01 + 0x01),
            .endMarker = 0x55
        };

        if (QueueCommandHandle != NULL) {
            osMessageQueuePut(QueueCommandHandle, &emergency_frame, 0U, 0U);
        }
    }
}

uint32_t Ultrasonic_Read_Distance_cm(void) {
    uint32_t t_subida = 0, t_bajada = 0, timeout = TIMEOUT_TICKS;

    TIM1->SR &= ~TIM_SR_CC1IF;

    // Disparo Trigger de 10 us en PA9
    GPIOA->BSRR = (1U << 9);
    uint16_t start = (uint16_t)TIM1->CNT;
    while ((uint16_t)(TIM1->CNT - start) < 10);
    GPIOA->BSRR = (1U << (9 + 16));

    // Esperar flanco de subida
    while (!(TIM1->SR & TIM_SR_CC1IF)) {
        if (--timeout == 0) return 0;
    }
    t_subida = TIM1->CCR1;

    // Conmutar a flanco de bajada
    TIM1->CCER |= TIM_CCER_CC1P;
    TIM1->SR &= ~TIM_SR_CC1IF;
    timeout = TIMEOUT_TICKS;

    while (!(TIM1->SR & TIM_SR_CC1IF)) {
        if (--timeout == 0) {
            TIM1->CCER &= ~TIM_CCER_CC1P;
            return 0;
        }
    }
    t_bajada = TIM1->CCR1;

    // Restaurar a subida
    TIM1->CCER &= ~TIM_CCER_CC1P;
    TIM1->SR &= ~TIM_SR_CC1IF;

    uint32_t duracion_us = (t_bajada >= t_subida) ? (t_bajada - t_subida) : ((0xFFFF - t_subida) + t_bajada + 1);
    return (uint32_t)(duracion_us * 0.01715f);
}

// ====================================================================================================
// 5. DRIVERS DMA USART3 (Stream 3 TX, Stream 1 RX)
// ====================================================================================================
void DMA_USART3_TX_Init(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    DMA1_Stream3->CR = 0;
    DMA1_Stream3->PAR = (uint32_t)&(USART3->DR);
    DMA1_Stream3->M0AR = (uint32_t)&esp32_frame;
    DMA1_Stream3->NDTR = sizeof(RemoteInteractionFrame_t);
    DMA1_Stream3->CR = (4 << 25) | DMA_SxCR_MINC | DMA_SxCR_DIR_0;
}

void DMA_USART3_RX_Init(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    DMA1_Stream1->CR = 0;
    DMA1_Stream1->PAR = (uint32_t)&(USART3->DR);
    DMA1_Stream1->M0AR = (uint32_t)&esp32_frame_rx;
    DMA1_Stream1->NDTR = sizeof(RemoteInteractionFrame_t);
    DMA1_Stream1->CR = (4 << 25) | DMA_SxCR_MINC | DMA_SxCR_TCIE;

    NVIC_SetPriority(DMA1_Stream1_IRQn, 5);
    NVIC_EnableIRQ(DMA1_Stream1_IRQn);
    DMA1_Stream1->CR |= DMA_SxCR_EN;
}

void DMA1_Stream1_IRQHandler(void) {
    if (DMA1->LISR & DMA_LISR_TCIF1) {
        DMA1->LIFCR |= DMA_LIFCR_CTCIF1;

        uint8_t calc_sum1 = (uint8_t)(esp32_frame_rx.commandCode + esp32_frame_rx.payloadLength + esp32_frame_rx.actionData);
        uint8_t calc_sum2 = (uint8_t)(esp32_frame_rx.commandCode + esp32_frame_rx.actionData);

        if (esp32_frame_rx.startMarker == 0xAA && esp32_frame_rx.endMarker == 0x55) {
            if (esp32_frame_rx.checksum == calc_sum1 || esp32_frame_rx.checksum == calc_sum2) {
                // Ejecución directa de servo
                if (esp32_frame_rx.commandCode == CMD_SERVO) {
                    Set_Servo_Angle(esp32_frame_rx.actionData);
                }

                if (QueueCommandHandle != NULL) {
                    osMessageQueuePut(QueueCommandHandle, &esp32_frame_rx, 0U, 0U);
                }
            }
        }

        // Rearmar canal DMA RX
        DMA1_Stream1->CR &= ~DMA_SxCR_EN;
        while(DMA1_Stream1->CR & DMA_SxCR_EN);
        DMA1_Stream1->NDTR = sizeof(RemoteInteractionFrame_t);
        DMA1_Stream1->CR |= DMA_SxCR_EN;
    }
}
// ====================================================================================================
// 6. CONTROLADOR 1-WIRE PARA DS18B20 EN PB0 (BLINDADO PARA RTOS)
// ====================================================================================================
static inline void DS18B20_Delay_us(uint32_t us) {
    // Calibración a 84 MHz para bucle en Cortex-M4 (~7 ciclos por iteración)
    uint32_t count = us * 12;
    while (count--) {
        __NOP();
    }
}

void DS18B20_Set_Pin_Output(void) {
    GPIOB->MODER &= ~(3U << (0 * 2));
    GPIOB->MODER |=  (1U << (0 * 2)); // Salida Push-pull
}

void DS18B20_Set_Pin_Input(void) {
    GPIOB->MODER &= ~(3U << (0 * 2)); // Entrada
    GPIOB->PUPDR &= ~(3U << (0 * 2)); // Sin pull-up interno (usa las externas)
}

uint8_t DS18B20_Reset(void) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq(); // Pausar RTOS para que no deforme el pulso

    DS18B20_Set_Pin_Output();
    GPIOB->BSRR = (1U << (0 + 16)); // PB0 a 0V
    DS18B20_Delay_us(480);

    DS18B20_Set_Pin_Input();
    DS18B20_Delay_us(70);

    uint8_t presence = !(GPIOB->IDR & (1U << 0)); // 1 si el DS18B20 responde en bajo
    DS18B20_Delay_us(410);

    __set_PRIMASK(primask); // Restaurar RTOS
    return presence;
}

void DS18B20_WriteBit(uint8_t bit) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    DS18B20_Set_Pin_Output();
    GPIOB->BSRR = (1U << (0 + 16)); // PB0 a 0V
    if (bit) {
        DS18B20_Delay_us(6);
        DS18B20_Set_Pin_Input();
        DS18B20_Delay_us(64);
    } else {
        DS18B20_Delay_us(60);
        DS18B20_Set_Pin_Input();
        DS18B20_Delay_us(10);
    }

    __set_PRIMASK(primask);
}

uint8_t DS18B20_ReadBit(void) {
    uint8_t bit = 0;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    DS18B20_Set_Pin_Output();
    GPIOB->BSRR = (1U << (0 + 16)); // Inicio de ranura de lectura
    DS18B20_Delay_us(3);
    DS18B20_Set_Pin_Input();
    DS18B20_Delay_us(10);           // Ventana de muestreo

    if (GPIOB->IDR & (1U << 0)) {
        bit = 1;
    }
    DS18B20_Delay_us(55);

    __set_PRIMASK(primask);
    return bit;
}

void DS18B20_WriteByte(uint8_t data) {
    for (uint8_t i = 0; i < 8; i++) {
        DS18B20_WriteBit(data & (1 << i));
    }
}

uint8_t DS18B20_ReadByte(void) {
    uint8_t data = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (DS18B20_ReadBit()) {
            data |= (1 << i);
        }
    }
    return data;
}

int8_t DS18B20_Read_Temperature(void) {
    if (!DS18B20_Reset()) return -127;

    DS18B20_WriteByte(0xCC); // Skip ROM
    DS18B20_WriteByte(0x44); // Convert T
    return 0;
}

int8_t DS18B20_Get_Result(void) {
    if (!DS18B20_Reset()) return -127;

    DS18B20_WriteByte(0xCC); // Skip ROM
    DS18B20_WriteByte(0xBE); // Read Scratchpad

    uint8_t temp_lsb = DS18B20_ReadByte();
    uint8_t temp_msb = DS18B20_ReadByte();

    int16_t raw_temp = (int16_t)((temp_msb << 8) | temp_lsb);
    return (int8_t)(raw_temp >> 4); // Grados Celsius
}
/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartSensorTask */
/**
  * @brief  Function implementing the vSensorTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartSensorTask */
void StartSensorTask(void *argument)
{
  /* USER CODE BEGIN 5 */
	uint32_t temp_timer = 0;
  /* Infinite loop */
  for(;;)
  {
	  g_distance_cm = Ultrasonic_Read_Distance_cm();



	        // Cada 840 ms (14 ciclos de 60 ms) actualiza la temperatura
	        temp_timer++;
	        if (temp_timer == 1) {
	            DS18B20_Read_Temperature(); // Lanza orden de conversión (requiere ~750ms)
	        } else if (temp_timer >= 14) {
	            int8_t t = DS18B20_Get_Result();
	            if (t != -127) {
	                g_temperature_c = t;
	            }
	            temp_timer = 0;
	        }

	        osDelay(60);
    }
  /* USER CODE END 5 */
}

/* USER CODE BEGIN Header_StartMotorTask */
/**
* @brief Function implementing the vMotorTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartMotorTask */
void StartMotorTask(void *argument)
{
  /* USER CODE BEGIN StartMotorTask */

  // 1. Barrido de prueba físico al arrancar (para descartar timer y cableado)
  Set_Servo_Angle(0);
  osDelay(1000);
  Set_Servo_Angle(180);
  osDelay(1000);
  Set_Servo_Angle(90);

  /* Infinite loop */
  RemoteInteractionFrame_t received_cmd;
  for(;;)
  {
      if (osMessageQueueGet(QueueCommandHandle, &received_cmd, NULL, osWaitForever) == osOK)
      {
          switch (received_cmd.commandCode)
          {
              case CMD_SERVO:
                  Set_Servo_Angle(received_cmd.actionData);
                  break;

              case CMD_MOTOR_DC:
                                if (received_cmd.actionData <= 5) {
                                    // Comandos directos: STOP (5), OFF (0), ON (1)
                                    Control_DC_Motor(received_cmd.actionData);
                                } else {
                                    // Valor del slider (6% a 100%)
                                    if (!g_emergency_state) {
                                        uint8_t new_speed = received_cmd.actionData;
                                        if (new_speed > 100) new_speed = 100;

                                        // Guarda la nueva velocidad elegida en el slider
                                        g_saved_speed = new_speed;

                                        // Solo si el motor está girando en ON (AIN1=1 y AIN2=0) actualiza el PWM de inmediato
                                        if ((GPIOB->ODR & (1U << 4)) && !(GPIOB->ODR & (1U << 5))) {
                                            g_current_speed = new_speed;
                                            TIM4->CCR1 = g_current_speed;
                                        }
                                    }
                                }
                                break;

              case CMD_ULTRASONIC:
                  // Responder telemetría de distancia al ESP32
                  esp32_frame.startMarker   = 0xAA;
                  esp32_frame.commandCode   = CMD_ULTRASONIC;
                  esp32_frame.payloadLength = 0x01;
                  esp32_frame.actionData    = (uint8_t)(g_distance_cm > 255 ? 255 : g_distance_cm);
                  esp32_frame.checksum      = esp32_frame.commandCode + esp32_frame.payloadLength + esp32_frame.actionData;
                  esp32_frame.endMarker     = 0x55;

                  // Limpieza de banderas obligatoria para rearmar TX
                  DMA1_Stream3->CR &= ~DMA_SxCR_EN;
                  while (DMA1_Stream3->CR & DMA_SxCR_EN);
                  DMA1->LIFCR = DMA_LIFCR_CTCIF3 | DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTEIF3; // Borra banderas previas
                  DMA1_Stream3->NDTR = sizeof(RemoteInteractionFrame_t);
                  DMA1_Stream3->CR |= DMA_SxCR_EN;
                  break;

              case CMD_EMERGENCY:
                  // Notificar paro de emergencia confirmado hacia ESP32
                  esp32_frame.startMarker   = 0xAA;
                  esp32_frame.commandCode   = CMD_EMERGENCY;
                  esp32_frame.payloadLength = 0x01;
                  esp32_frame.actionData    = 0x01;
                  esp32_frame.checksum      = (uint8_t)(CMD_EMERGENCY + 0x01 + 0x01);
                  esp32_frame.endMarker     = 0x55;

                  DMA1_Stream3->CR &= ~DMA_SxCR_EN;
                  DMA1_Stream3->NDTR = sizeof(RemoteInteractionFrame_t);
                  DMA1_Stream3->CR |= DMA_SxCR_EN;
                  break;

              case CMD_TEMP:
                                // Responder telemetría de temperatura al ESP32
                                esp32_frame.startMarker   = 0xAA;
                                esp32_frame.commandCode   = CMD_TEMP;
                                esp32_frame.payloadLength = 0x01;
                                esp32_frame.actionData    = (uint8_t)g_temperature_c;
                                esp32_frame.checksum      = esp32_frame.commandCode + esp32_frame.payloadLength + esp32_frame.actionData;
                                esp32_frame.endMarker     = 0x55;

                                // 1. Apagar stream de forma segura
                                DMA1_Stream3->CR &= ~DMA_SxCR_EN;
                                while (DMA1_Stream3->CR & DMA_SxCR_EN);

                                // 2. OBLIGATORIO: Limpiar banderas de fin de transferencia y error
                                DMA1->LIFCR = DMA_LIFCR_CTCIF3 | DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTEIF3;

                                // 3. Recargar tamaño y volver a disparar transmisión
                                DMA1_Stream3->NDTR = sizeof(RemoteInteractionFrame_t);
                                DMA1_Stream3->CR |= DMA_SxCR_EN;
                                break;
          }
      }
  }
  /* USER CODE END StartMotorTask */
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
  if (htim->Instance == TIM6) {
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
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
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
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
