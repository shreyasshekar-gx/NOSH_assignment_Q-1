#include "main.h"
#define LED_PIN       GPIO_PIN_5
#define LED_PORT      GPIOA
#define BTN_PIN       GPIO_PIN_13
#define BTN_PORT      GPIOC
static const uint32_t HALF_PERIOD_MS[4] = {
    0,
    1000,
    500,
    250
};
volatile uint8_t  g_btn_pressed  = 0;
static   uint8_t  g_state        = 0;
static   uint32_t g_last_toggle  = 0;
static   uint8_t  g_led_on       = 0;
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void enter_stop_mode(void);
static void handle_button_press(void);
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
    g_state = 0;
    while (1)
    {
        if (g_btn_pressed)
        {
            g_btn_pressed = 0;
            handle_button_press();
        }
        if (g_state == 0)
        {
            HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
            g_led_on = 0;
            enter_stop_mode();
        }
        else
        {
            uint32_t now = HAL_GetTick();
            if ((now - g_last_toggle) >= HALF_PERIOD_MS[g_state])
            {
                g_led_on = !g_led_on;
                HAL_GPIO_WritePin(LED_PORT, LED_PIN,
                                  g_led_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
                g_last_toggle = now;
            }
            HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON,
                                    PWR_SLEEPENTRY_WFI);
        }
    }
}
static void handle_button_press(void)
{
    static uint32_t last_press_time = 0;
    uint32_t now = HAL_GetTick();
    if ((now - last_press_time) < 50U) return;
    last_press_time = now;
    g_state = (g_state >= 3) ? 0 : g_state + 1;
    g_last_toggle = HAL_GetTick();
    g_led_on      = 0;
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
}
static void enter_stop_mode(void)
{
    HAL_PWR_EnterSTOPMode(PWR_MAINREGULATOR_ON, PWR_STOPENTRY_WFI);
    SystemClock_Config();
}
void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == BTN_PIN)
    {
        g_btn_pressed = 1;
    }
}
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin   = LED_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &GPIO_InitStruct);
    GPIO_InitStruct.Pin   = BTN_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull  = GPIO_PULLUP;
    HAL_GPIO_Init(BTN_PORT, &GPIO_InitStruct);
    HAL_NVIC_SetPriority(EXTI4_15_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(EXTI4_15_IRQn);
}
