/* =====================================================================
 *  switch_led_blink.c
 *  Project   : Switch-Controlled LED Blinker (Power-Optimized)
 *  Board     : NUCLEO-G070RB  (STM32G070RB microcontroller)
 *  Framework : STM32 HAL (the "Arduino library" equivalent for STM32)
 *
 *  WHAT THIS FILE DOES, IN PLAIN ENGLISH:
 *  - Every time the button is pressed, the LED blink speed changes:
 *        press 1 -> blink slow   (0.5 Hz)
 *        press 2 -> blink normal (1 Hz)
 *        press 3 -> blink fast   (2 Hz)
 *        press 4 -> LED off
 *        press 5 -> same as press 1 (it wraps around)
 *  - The chip spends almost all of its time ASLEEP (STOP1 low-power mode).
 *    It only wakes up for a few microseconds to either:
 *        (a) toggle the LED, or
 *        (b) register a button press
 *    and then goes straight back to sleep. This is the opposite of an
 *    Arduino-style "while(1) { delay(); }" loop, which keeps the CPU
 *    awake and burning power the whole time.
 *
 *  HOW THE LOW-POWER PART WORKS (read this before the code, it helps):
 *  - We use a peripheral called LPTIM1 ("Low-Power Timer"). Unlike a
 *    normal timer, this one is allowed to KEEP RUNNING even while the
 *    main CPU is in STOP1 sleep, as long as it's clocked from the LSE
 *    (a slow, low-power 32.768 kHz crystal -- the same type used inside
 *    wristwatches).
 *  - So our plan is: load LPTIM1 with however many "ticks" correspond to
 *    half the blink period (because toggling twice = one full blink
 *    cycle), then tell the CPU to go to sleep. LPTIM1 silently counts in
 *    the background. When it reaches the target count, it fires an
 *    interrupt, which (a) wakes the CPU up and (b) runs our toggle code.
 *    Then we go straight back to sleep. Repeat forever.
 *  - The button is wired to an EXTI (External Interrupt) line, which can
 *    ALSO wake the CPU from STOP1 sleep instantly. So even though the
 *    CPU is "asleep," a button press is never missed.
 *
 *  HUMANIZED NAMING:
 *  Every variable/function below has a descriptive name instead of the
 *  usual terse STM32 style (e.g. "currentBlinkState" instead of "st").
 * =====================================================================
 */

#include "stm32g0xx_hal.h"

/* ---------------------------------------------------------------------
 * SECTION 1: Pin and hardware definitions
 *   Think of these like "#define LED_PIN 5" on an Arduino sketch --
 *   just giving friendly names to the physical wiring so the rest of
 *   the code reads like English instead of raw pin numbers.
 * ------------------------------------------------------------------- */
#define LED_GPIO_PORT            GPIOA
#define LED_GPIO_PIN             GPIO_PIN_5     /* Onboard LED LD2 = PA5 */

#define BUTTON_GPIO_PORT         GPIOC
#define BUTTON_GPIO_PIN          GPIO_PIN_13    /* Onboard button B1 = PC13 */
#define BUTTON_EXTI_IRQ          EXTI4_15_IRQn  /* PC13 lives on the EXTI4-15 interrupt line on G0 */

/* The four possible "modes" the LED can be in.
 * Using an enum instead of plain numbers (0,1,2,3) makes the code
 * self-documenting -- you can read "LED_MODE_FAST_BLINK" and instantly
 * know what it means, instead of having to remember "state == 3 means
 * fast". */
typedef enum
{
    LED_MODE_OFF        = 0,   /* 4th press, and the initial power-on state */
    LED_MODE_SLOW_BLINK  = 1,   /* 1st press -> 0.5 Hz */
    LED_MODE_NORMAL_BLINK = 2,  /* 2nd press -> 1 Hz   */
    LED_MODE_FAST_BLINK  = 3    /* 3rd press -> 2 Hz   */
} LedBlinkMode_t;

/* This is THE state-machine variable. Everything in this project
 * revolves around this one number. It starts at OFF, and every button
 * press moves it forward by one, wrapping back to OFF after FAST. */
static volatile LedBlinkMode_t currentBlinkMode = LED_MODE_OFF;

/* Software debounce bookkeeping.
 * A mechanical switch doesn't cleanly go from "open" to "closed" -- the
 * metal contacts physically bounce for a few milliseconds, which can
 * cause the EXTI interrupt to fire 2-5 times for a SINGLE physical
 * press. We fix this the simple way: remember the time of the last
 * accepted press, and ignore anything that happens within 200 ms of it. */
static volatile uint32_t lastAcceptedPressTimestamp_ms = 0;
#define DEBOUNCE_GUARD_TIME_MS   200

/* The LPTIM1 peripheral handle -- this is just a "remote control" struct
 * that HAL uses internally to talk to the actual LPTIM1 hardware block.
 * You always create one of these for whichever peripheral you're using,
 * the same idea as creating a "Servo myServo;" object in Arduino. */
static LPTIM_HandleTypeDef lowPowerTimerHandle;

/* ---------------------------------------------------------------------
 * SECTION 2: Function prototypes (just declarations, bodies are below)
 * ------------------------------------------------------------------- */
static void System_Clock_Setup(void);
static void LED_Pin_Setup(void);
static void Button_Interrupt_Setup(void);
static void LowPowerTimer_Setup(void);
static void LowPowerTimer_ReprogramForCurrentMode(void);
static void Enter_Low_Power_Sleep(void);

/* =====================================================================
 *  MAIN -- the entry point, equivalent to Arduino's setup()+loop()
 * =====================================================================
 */
int main(void)
{
    /* HAL_Init() sets up the basic systick + flash + interrupt priority
     * grouping that every STM32 HAL project needs before doing anything
     * else. Equivalent to the hidden boilerplate Arduino's core does for
     * you automatically before your setup() even runs. */
    HAL_Init();

    /* Configure the main system clock. We deliberately keep this modest
     * (no need to run at max speed of 64 MHz for a blinking LED -- a
     * slower core clock also sips less power while we ARE awake). */
    System_Clock_Setup();

    /* ---- "setup()" equivalent: configure each piece of hardware once ---- */
    LED_Pin_Setup();              /* PA5 as a digital output, LED starts OFF */
    Button_Interrupt_Setup();     /* PC13 as an interrupt input, pull-up enabled */
    LowPowerTimer_Setup();        /* LPTIM1 ready to go, but not started yet (LED is OFF initially) */

    /* ---- "loop()" equivalent ----
     * Notice how empty this looks! That's intentional and is the whole
     * point of the power-optimization requirement: there is NO polling,
     * NO delay(), NO busy-waiting. The CPU's only job from here on is to
     * go to sleep and let interrupts (button press / timer tick) do all
     * the actual work, each time briefly waking it up. */
    while (1)
    {
        Enter_Low_Power_Sleep();
    }
}

/* =====================================================================
 *  SECTION 3: Setup functions
 * =====================================================================
 */

/* Configures the main CPU clock.
 * We turn on the LSE (Low Speed External) 32.768 kHz crystal here too --
 * this is the slow "watch crystal" that lets LPTIM1 keep ticking even
 * while the CPU is fully asleep in STOP1 mode. This is the single most
 * important clock-setup detail in the whole project. */
static void System_Clock_Setup(void)
{
    RCC_OscInitTypeDef oscillatorConfig = {0};
    RCC_ClkInitTypeDef clockConfig = {0};

    /* Turn on the internal high-speed oscillator (HSI) to run the CPU
     * itself -- simplest, no external crystal needed for the main clock. */
    oscillatorConfig.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSE;
    oscillatorConfig.HSIState       = RCC_HSI_ON;
    oscillatorConfig.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;

    /* Try to start the LSE crystal (the watch-crystal for LPTIM1).
     * NOTE: on some Nucleo-G070RB boards this crystal isn't physically
     * populated (see README "Hardware note"). If HAL_RCC_OscConfig()
     * below returns an error because LSE never starts, a real project
     * would catch that and call HAL_RCCEx_GetPeriphCLKConfig() to fall
     * back to LSI instead -- both work for this project, LSE is simply
     * more accurate. */
    oscillatorConfig.LSEState = RCC_LSE_ON;

    HAL_RCC_OscConfig(&oscillatorConfig);

    /* Pick a modest CPU speed (16 MHz from HSI) -- plenty fast for
     * toggling a pin, and keeps "awake" power draw low too. */
    clockConfig.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1;
    clockConfig.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    clockConfig.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clockConfig.APB1CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&clockConfig, FLASH_LATENCY_0);

    /* Tell LPTIM1 specifically to use the LSE crystal as its clock
     * source. This is the line that makes "ticking while asleep"
     * possible -- without it, the timer would stop the moment the CPU
     * goes to sleep, same as a normal Arduino timer would. */
    RCC_PeriphCLKInitTypeDef periphClockConfig = {0};
    periphClockConfig.PeriphClockSelection = RCC_PERIPHCLK_LPTIM1;
    periphClockConfig.LptimClockSelection  = RCC_LPTIM1CLKSOURCE_LSE;
    HAL_RCCEx_PeriphCLKConfig(&periphClockConfig);
}

/* Configures PA5 (the LED pin) as a simple digital output.
 * Equivalent to Arduino's pinMode(LED_BUILTIN, OUTPUT). */
static void LED_Pin_Setup(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();   /* STM32 peripherals are "off" until you
                                       explicitly enable their clock -- think
                                       of it like flipping a breaker switch
                                       before a wall socket will work. */

    GPIO_InitTypeDef ledPinConfig = {0};
    ledPinConfig.Pin   = LED_GPIO_PIN;
    ledPinConfig.Mode  = GPIO_MODE_OUTPUT_PP;   /* PP = "push-pull", the normal
                                                    output mode, same idea as a
                                                    normal Arduino OUTPUT pin */
    ledPinConfig.Pull  = GPIO_NOPULL;
    ledPinConfig.Speed = GPIO_SPEED_FREQ_LOW;   /* low slew rate = lower power,
                                                    we don't need fast edges for
                                                    a visible LED */
    HAL_GPIO_Init(LED_GPIO_PORT, &ledPinConfig);

    /* Requirement says "LED is off initially" -- make that explicit
     * rather than just hoping the pin resets to 0. */
    HAL_GPIO_WritePin(LED_GPIO_PORT, LED_GPIO_PIN, GPIO_PIN_RESET);
}

/* Configures PC13 (the button pin) as an interrupt input.
 * Equivalent to:
 *     pinMode(BUTTON_PIN, INPUT_PULLUP);
 *     attachInterrupt(BUTTON_PIN, myHandler, FALLING);
 * but on STM32 this interrupt is ALSO capable of waking the chip up
 * from STOP1 sleep -- a regular Arduino interrupt usually can't do that
 * while the chip is in its deepest sleep modes. */
static void Button_Interrupt_Setup(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef buttonPinConfig = {0};
    buttonPinConfig.Pin  = BUTTON_GPIO_PIN;
    buttonPinConfig.Mode = GPIO_MODE_IT_FALLING; /* "Interrupt on Falling edge" --
                                                     fires the moment the pin goes
                                                     from HIGH to LOW, i.e. the
                                                     instant the button is pressed */
    buttonPinConfig.Pull = GPIO_PULLUP;          /* internal pull-up, same as
                                                     INPUT_PULLUP on Arduino --
                                                     keeps the pin HIGH until the
                                                     button physically pulls it
                                                     LOW to GND */
    HAL_GPIO_Init(BUTTON_GPIO_PORT, &buttonPinConfig);

    /* Turn the interrupt "on" in the NVIC (Nested Vector Interrupt
     * Controller) -- the chip's master interrupt switchboard.
     * Priority 1 just means "fairly important, handle it promptly." */
    HAL_NVIC_SetPriority(BUTTON_EXTI_IRQ, 1, 0);
    HAL_NVIC_EnableIRQ(BUTTON_EXTI_IRQ);
}

/* Configures LPTIM1 (the Low-Power Timer) but doesn't start counting
 * yet, since the LED begins in the OFF state and there's nothing to
 * blink at power-on. */
static void LowPowerTimer_Setup(void)
{
    __HAL_RCC_LPTIM1_CLK_ENABLE();

    lowPowerTimerHandle.Instance = LPTIM1;
    lowPowerTimerHandle.Init.Clock.Source    = LPTIM_CLOCKSOURCE_APBCLOCK_LPOSC;
    lowPowerTimerHandle.Init.Clock.Prescaler = LPTIM_PRESCALER_DIV1; /* no extra
                                                     division -- we'll size the
                                                     compare value directly off
                                                     the raw 32.768 kHz LSE tick */
    lowPowerTimerHandle.Init.Trigger.Source  = LPTIM_TRIGSOURCE_SOFTWARE;
    lowPowerTimerHandle.Init.OutputPolarity  = LPTIM_OUTPUTPOLARITY_HIGH;
    lowPowerTimerHandle.Init.UpdateMode      = LPTIM_UPDATE_IMMEDIATE;
    lowPowerTimerHandle.Init.CounterSource   = LPTIM_COUNTERSOURCE_INTERNAL;

    HAL_LPTIM_Init(&lowPowerTimerHandle);
}

/* =====================================================================
 *  SECTION 4: The two interrupt "callbacks" -- this is where the real
 *  logic of the project lives. Everything above was just one-time setup.
 * =====================================================================
 */

/* HAL automatically calls this function whenever ANY GPIO EXTI interrupt
 * fires. We check WHICH pin caused it, in case other interrupts share
 * this same handler in a bigger project. */
void HAL_GPIO_EXTI_Callback(uint16_t triggeredPin)
{
    if (triggeredPin != BUTTON_GPIO_PIN)
    {
        return;   /* some other pin's interrupt -- not our button, ignore */
    }

    uint32_t timeNow_ms = HAL_GetTick();

    /* ---- Software debounce ----
     * If this press happened less than 200 ms after the last ACCEPTED
     * press, treat it as switch-bounce noise and throw it away. */
    if ((timeNow_ms - lastAcceptedPressTimestamp_ms) < DEBOUNCE_GUARD_TIME_MS)
    {
        return;
    }
    lastAcceptedPressTimestamp_ms = timeNow_ms;

    /* ---- Advance the state machine ----
     * This single line IS the "1st press / 2nd press / .../ 5th press
     * wraps to 1st" behavior from the requirements. Modulo 4 (%4) is
     * what makes it wrap back to OFF->SLOW automatically after FAST. */
    currentBlinkMode = (LedBlinkMode_t)((currentBlinkMode + 1) % 4);

    /* If we just moved INTO the OFF state, make sure the LED pin is
     * physically driven low and the timer is stopped -- no point letting
     * LPTIM1 keep ticking in the background if there's nothing to blink,
     * that would waste power for nothing. */
    if (currentBlinkMode == LED_MODE_OFF)
    {
        HAL_GPIO_WritePin(LED_GPIO_PORT, LED_GPIO_PIN, GPIO_PIN_RESET);
        HAL_LPTIM_Counter_Stop_IT(&lowPowerTimerHandle);
    }
    else
    {
        /* Moving into SLOW / NORMAL / FAST: (re)load LPTIM1 with the
         * correct toggle interval for the new mode and make sure it's
         * running. */
        LowPowerTimer_ReprogramForCurrentMode();
    }
}

/* HAL automatically calls this whenever LPTIM1's counter reaches the
 * "autoreload" value we programmed -- i.e. exactly when it's time to
 * flip the LED. */
void HAL_LPTIM_AutoReloadMatchCallback(LPTIM_HandleTypeDef *timerHandle)
{
    /* TogglePin = "if it's currently ON, turn it OFF, and vice versa" --
     * exactly the same as Arduino's digitalWrite(pin, !digitalRead(pin)),
     * just done atomically in one call. */
    HAL_GPIO_TogglePin(LED_GPIO_PORT, LED_GPIO_PIN);
}

/* Works out how many LPTIM "ticks" correspond to half a blink period for
 * the CURRENT mode, and (re)starts LPTIM1 with that value.
 *
 * Reminder of the math (see README section 1):
 *   0.5 Hz -> full period 2000 ms -> toggle every 1000 ms
 *   1 Hz   -> full period 1000 ms -> toggle every  500 ms
 *   2 Hz   -> full period  500 ms -> toggle every  250 ms
 *
 * LPTIM1 is clocked at 32768 Hz (the LSE crystal), so:
 *   ticks_needed = desired_milliseconds * 32768 / 1000
 */
static void LowPowerTimer_ReprogramForCurrentMode(void)
{
    uint32_t toggleInterval_ms;

    switch (currentBlinkMode)
    {
        case LED_MODE_SLOW_BLINK:   toggleInterval_ms = 1000; break; /* 0.5 Hz */
        case LED_MODE_NORMAL_BLINK: toggleInterval_ms =  500; break; /* 1 Hz   */
        case LED_MODE_FAST_BLINK:   toggleInterval_ms =  250; break; /* 2 Hz   */
        default:                    toggleInterval_ms =    0; break; /* OFF, shouldn't reach here */
    }

    uint32_t lptimAutoReloadTicks = (uint32_t)(((uint64_t)toggleInterval_ms * 32768u) / 1000u);

    /* Stop any previous run before reprogramming -- avoids a stale
     * interrupt firing with the OLD timing right as we switch speeds. */
    HAL_LPTIM_Counter_Stop_IT(&lowPowerTimerHandle);

    /* Start LPTIM1 in interrupt mode: "Period" register = our autoreload
     * value, "Pulse" doesn't matter much here since we only care about
     * the autoreload-match interrupt, not the PWM output itself. */
    HAL_LPTIM_TimeOut_Start_IT(&lowPowerTimerHandle, lptimAutoReloadTicks, lptimAutoReloadTicks);
}

/* =====================================================================
 *  SECTION 5: The actual "go to sleep" call
 * =====================================================================
 */

/* Puts the CPU into STOP1 mode. This is the "secret sauce" of the whole
 * power-optimization requirement:
 *   - CPU clock, most peripherals, and most RAM retention logic are
 *     powered down.
 *   - LPTIM1 (clocked from LSE) and the EXTI lines keep working and are
 *     able to wake the CPU back up.
 *   - The very next line of code that runs after this call is whichever
 *     interrupt handler caused the wake-up (button press or LED-toggle
 *     time), and once that handler finishes, we land right back at the
 *     top of the while(1) loop and call this function again.
 *
 * This single call is the STM32 equivalent of "low power sleep" library
 * calls you might have seen for AVR/ESP32, except here it's built
 * straight into HAL, no extra library needed. */
static void Enter_Low_Power_Sleep(void)
{
    HAL_PWR_EnterSTOPMode(PWR_LOWPOWERMODE_STOP1, PWR_STOPENTRY_WFI);
    /* WFI = "Wait For Interrupt" -- the actual CPU instruction that
     * physically halts the processor core until something wakes it. */
}

/* =====================================================================
 *  END OF FILE
 *
 *  Quick summary for the interview if asked "where's the power saving?":
 *    1. No delay()/polling anywhere -- 100% interrupt-driven.
 *    2. CPU sleeps in STOP1 between every single event.
 *    3. Blink timing comes from LPTIM1, clocked by the low-power LSE
 *       crystal, which is specifically designed to keep running while
 *       the CPU itself is powered down.
 *    4. When the LED is OFF, the timer is stopped entirely too -- so in
 *       that state literally only the EXTI button line is "alive,"
 *       drawing the absolute minimum current the chip is capable of.
 * =====================================================================
 */
