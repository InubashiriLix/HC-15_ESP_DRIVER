#include <Arduino.h>
#include <builtin_led.hpp>
#include <lora_class.hpp>

/* ---------- 全局 / 静态 HC15 实例 ---------- */
static HC15 hc15(&Serial1, // 注意取地址 &
                 115200,   // baud
                 1, 0,     // RX, TX (顺序要和 Serial1.begin 一致)
                 5000,     // 默认超时
                 12, 18);  // STA, KEY

void setup()
{
  Serial.begin(115200);
  Serial.println("lora test begin");

  /* LED 任务 */
  builtin_led_setup();
  xTaskCreate(builtin_led_task,
              "builtin led task",
              1024,
              nullptr,
              4,
              nullptr);

  /* 串口先启动 */
  Serial1.begin(115200, SERIAL_8N1, 1, 0);

  /* 初始化 HC-15 */
  if (!hc15.begin())
  {
    Serial.println("HC15 initialization failed!");
    return;
  }
  Serial.println("test begin");
  Serial.println(hc15.getChannel());
  Serial.println("done");

  /* 监控任务 */
  xTaskCreate(
      [](void *pv) {                                      // pv = &hc15
        static_cast<HC15 *>(pv)->monitorTask((void *)20); // 20 ms 轮询
      },
      "HC15 monitoring task",
      4096,
      &hc15, // 把对象地址传进去
      1,
      nullptr);

  xTaskCreate(
      [](void * /*pv*/)
      {
        for (;;)
        {
          /* ① 先试着拿锁，给 50 ms 超时即可 */
          if (xSemaphoreTake(hc15.hc15_buzy_semaphore_, pdMS_TO_TICKS(50)) == pdTRUE)
          {
            /* ② 有数据就一行一行吐出来 */
            while (hc15.available())
            {
              String line = hc15.readLine();
              Serial.println(line);
            }
            xSemaphoreGive(hc15.hc15_buzy_semaphore_);
          }

          /* ③ 适当休息，避免占满 CPU */
          vTaskDelay(pdMS_TO_TICKS(100));
        }
      },
      "HC15 read task",
      2048,
      nullptr,
      1,
      nullptr);
}

void loop() {} // 主循环留空
