#include <builtin_led.hpp>
#include <pins_arduino.h>
#include <Arduino.h>

void builtin_led_setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(LED_BUILTIN_AUX, OUTPUT);
    digitalWrite(LED_BUILTIN, 1);
    digitalWrite(LED_BUILTIN_AUX, 1);
}

void builtin_led_task(void *args)
{
    static bool led_1 = 0;
    static bool led_2 = 1;
    for (;;)
    {
        led_1 = !led_1;
        led_2 = !led_2;
        digitalWrite(LED_BUILTIN, led_1);
        digitalWrite(LED_BUILTIN_AUX, led_2);
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial1.println("shit!");
    }
}