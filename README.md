# Switch-Controlled LED Blinker — NUCLEO-G070RB


---

## 1. The Problem, restated simply

One button. One LED. Every time you press the button, the LED's blink speed
changes, cycling through 4 states:

| Press number | What happens | Blink frequency | Toggle every... |
|---|---|---|---|
| 1st press | LED starts blinking slow | 0.5 Hz (1 blink every 2 sec) | 1000 ms |
| 2nd press | LED blinks normal | 1 Hz (1 blink every 1 sec) | 500 ms |
| 3rd press | LED blinks fast | 2 Hz (2 blinks every 1 sec) | 250 ms |
| 4th press | LED turns OFF | — | — |
| 5th press | back to row 1 | 0.5 Hz | 1000 ms |

> Why "toggle every X ms" matters: an LED that blinks at 1 Hz means it
> completes **one full ON+OFF cycle per second** — so it must flip state
> (toggle) **twice** a second → every 500 ms. This "toggle = half the period"
> math shows up directly in the timer code later.

The twist: this has to be **power-optimized using the STM32's low-power
modes**, not just "blink with delay()" like you'd do on Arduino.

---

## 2. Hardware / Circuit

Good news: the NUCLEO-G070RB already has **both parts onboard**, so for the
interview demo you don't need to solder anything:

| Component | Nucleo pin | Notes |
|---|---|---|
| User push-button **B1** | **PC13** | Already wired to GND with debounce-friendly pull-up. Pressing pulls PC13 LOW. |
| User LED **LD2** | **PA5** | Green LED, already wired through a resistor to PA5. |

If you (or the interviewer) want an **external** switch + LED instead of the
onboard ones (closer to "design a circuit" as asked), here's the simple
schematic:

```
        3V3
         |
         |        +---[ Tactile Switch ]---+
         |         |                       |
        (not       PC13 -------------------+----> GND
       needed,                              (internal pull-up enabled
       internal                              in software, so no external
       pull-up                               resistor is required)
       used)


   PA5 ----[ 330 Ω resistor ]----[ LED Anode | Cathode ]---- GND
```

- **Switch**: one leg to a GPIO pin (PC13), other leg to GND. We turn on the
  microcontroller's **internal pull-up resistor** in software (`GPIO_PULLUP`),
  exactly like `pinMode(pin, INPUT_PULLUP)` on Arduino. No external resistor
  needed.
- **LED**: GPIO pin (PA5) → 330 Ω series resistor (limits current, protects
  the LED and the pin) → LED → GND. Same idea as wiring an LED to an ESP32
  GPIO.

### Block diagram

```mermaid
flowchart LR
    SW[Tactile Switch] -- pulls LOW on press --> PC13[PC13 / EXTI13]
    PC13 --> MCU((STM32G070RB))
    MCU -- drives --> PA5[PA5 GPIO]
    PA5 --> R[330 Ω Resistor] --> LED[LED] --> GND1[GND]
```

---

## 3. The "brain" of the project — State Machine

Think of this exactly like a 4-position rotary switch that the button cycles
through. Every press moves you one step forward, and step 5 wraps back to
step 1.

```mermaid
stateDiagram-v2
    [*] --> OFF
    OFF --> SLOW : button press (1st)
    SLOW --> NORMAL : button press (2nd)
    NORMAL --> FAST : button press (3rd)
    FAST --> OFF : button press (4th)
    OFF --> SLOW : button press (5th = back to 1st)

    OFF : LED off, MCU in deep sleep (STOP mode)
    SLOW : Blinking 0.5 Hz
    NORMAL : Blinking 1 Hz
    FAST : Blinking 2 Hz
```

That's it. Four states, one button, wraps around. In code this is just one
counter variable that goes `0 → 1 → 2 → 3 → 0 → 1...` and a `switch/case` (or
lookup table) that decides what the LED should do in each state.

---

## 4. Why this isn't "just delay() and digitalWrite()"

On Arduino you'd write:

```cpp
digitalWrite(LED, HIGH); delay(500); digitalWrite(LED, LOW); delay(500);
```

`delay()` **burns CPU cycles doing nothing** — the chip is fully awake,
clocked at full speed, just spinning in a loop. That's terrible for battery
life. STM32 (and the task itself) wants us to do better:

1. **Never poll. Always interrupt.** The button is read using an
   **EXTI (External Interrupt)** — the CPU isn't checking the pin in a loop;
   the hardware itself wakes the CPU up the instant the pin changes. This is
   like `attachInterrupt()` on Arduino, but on STM32 it can also **wake the
   chip from sleep**, which a plain Arduino interrupt can't always do.

2. **Don't use a CPU timer for blinking — use the LPTIM (Low-Power Timer).**
   STM32G0 has a special timer peripheral that can keep ticking and
   generating interrupts **even while the CPU is in STOP mode** (a deep sleep
   state), as long as it's clocked from the **LSE** (a slow 32.768 kHz
   crystal, the same type used in wristwatches — draws almost no current).
   This means: the CPU can go to sleep, the LPTIM silently counts in the
   background, and only wakes the CPU up for the few microseconds it takes to
   flip the LED pin — then goes straight back to sleep.

3. **Two flavors of "sleep" used here:**
   - **STOP1 mode** — used whenever the LED is OFF (state 4) and we're just
     waiting for the next button press. Deepest sleep we can use while still
     waking up instantly on a button press (EXTI). Power draw here is in the
     **microamps**.
   - Also **STOP1 mode** *during blinking* — between LED toggles, the CPU
     has literally nothing to do for hundreds of milliseconds, so it goes
     back to STOP1 every single time, and the LPTIM interrupt wakes it only
     to toggle the pin.

   So actually the CPU is **asleep almost 100% of the time**, in every state.
   It only "blinks awake" for microseconds either to toggle the LED or to
   register a button press.

### Power flow diagram

```mermaid
flowchart TD
    A[MCU wakes up] --> B{Why did it wake up?}
    B -- "EXTI: Button pressed" --> C[Debounce check]
    C --> D[Advance state: OFF→SLOW→NORMAL→FAST→OFF]
    D --> E[Reconfigure LPTIM period for new blink speed]
    E --> F[Enter STOP1 mode]
    B -- "LPTIM: Toggle time reached" --> G[Toggle LED pin]
    G --> F
    F --> A
```



```
main()
 ├─ System_Clock_Setup()        // configures MCU + enables LSE crystal
 ├─ LED_Pin_Setup()              // PA5 as output
 ├─ Button_Interrupt_Setup()     // PC13 as EXTI input, pull-up, falling edge
 ├─ LowPowerTimer_Setup()        // LPTIM1 clocked from LSE
 ├─ Enter_Sleep_Forever()        // infinite loop that just goes back to STOP1
 │
 ├─ EXTI15_10_IRQHandler()  → Button_Pressed_Callback()
 │     - software debounce
 │     - advance blinkState (0,1,2,3,0,1,...)
 │     - reprogram LPTIM period for new frequency (or stop it if OFF)
 │
 └─ LPTIM1_IRQHandler()     → Blink_Timer_Tick_Callback()
       - toggles the LED pin
```

---

## 9. Files in this project

- `README.md` — this file
- `switch_led_blink.c` — the full, heavily commented source file
