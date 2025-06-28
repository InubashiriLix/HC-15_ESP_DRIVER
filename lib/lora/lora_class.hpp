#pragma once
#include <Arduino.h>

enum class HC15_ERROR_TYPE
{
    NONE = 0,
    SERIAL_ERROR = 1,
    TIMEOUT_ERROR = 2,
};

class HC15
{
public:
    HC15(HardwareSerial *serial, uint32_t baud_rate, uint8_t rx_pin, uint8_t tx_pin, uint16_t timeout, uint8_t sta_pin, uint8_t key_pin) : serial_(serial), baud_rate_(baud_rate), rx_pin_(rx_pin), tx_pin_(tx_pin), timeout_(timeout), sta_pin_(sta_pin), key_pin_(key_pin)
    {
        hc15_buzy_semaphore_ = xSemaphoreCreateBinary();
    }

    bool begin()
    {
        if (serial_)
        {
            serial_->begin(baud_rate_, SERIAL_8N1, rx_pin_, tx_pin_);
        }
        else
        {
            return false;
        }

        pinMode(sta_pin_, INPUT_PULLDOWN);
        pinMode(key_pin_, OUTPUT);
        digitalWrite(key_pin_, HIGH); // Set key pin to HIGH to ensure HC-15 is in command mode

        Serial.println("STA_PIN:" + String(sta_pin_) + ", KEY_PIN:" + String(key_pin_));

        serial_->flush();                     // clear the serial
        xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore to indicate that the HC-15 is ready
        return true;
    }

    /*
     * @brief Check for errors in the HC-15 module.
     * @return The error type if an error is detected, or NONE if no error is found.
     */
    HC15_ERROR_TYPE errorCheck()
    {
        if (!serial_)
        {
            Serial.println("HC-15 serial port is not initialized.");
            return HC15_ERROR_TYPE::SERIAL_ERROR;
        }
        return HC15_ERROR_TYPE::NONE;
    }

    /*
     * @brief get the read buffer length of the HC-15 module.
     * @return The length of the read buffer.
     */
    int available()
    {
        return readBuffer.length();
    }

    /*
     * @brief Read data from the HC-15 module, use rtos task please
     */
    void monitorTask(void *pvParameters)
    {
        // ① 取出延时：我们用 uintptr_t 而不是直接 uint32_t，
        //    这样在 32/64 位 MCU 都安全
        uint32_t delay_ms = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pvParameters));
        if (delay_ms == 0)
            delay_ms = 200; // 默认 10 ms 兜底

        if (errorCheck() != HC15_ERROR_TYPE::NONE)
        {
            Serial.println("HC-15 error detected, task will not start.");
            vTaskDelete(nullptr);
        }

        for (;;)
        {
            /*── 2.1 尝试拿锁：给 5000 ms 超时，避免命令模式被饿死 ──*/
            if (xSemaphoreTake(hc15_buzy_semaphore_, pdMS_TO_TICKS(5000)) == pdTRUE)
            {
                // 2.2 只有模块空闲 & 串口有数据才读
                if (!isBuzy() && serial_->available() > 0)
                {
                    // 读到本地缓冲，减少锁占用时间
                    String chunk = serial_->readString();
                    readBuffer.reserve(readBuffer.length() + chunk.length());
                    readBuffer += chunk;
                }
                xSemaphoreGive(hc15_buzy_semaphore_); // 2.3 立刻放锁
            }

            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    String readLine()
    {
        int idx = readBuffer.indexOf('\n');
        if (idx == -1)
            idx = readBuffer.indexOf('\r');
        if (idx != -1)
        {
            String line = readBuffer.substring(0, idx);
            // Remove the line (and the newline character) from the buffer
            readBuffer = readBuffer.substring(idx + 1);
            return line;
        }
        else if (readBuffer.length() > 0)
        {
            // No newline found, return all and clear buffer
            String line = readBuffer;
            readBuffer = "";
            return line;
        }
        return String();
    }

    /*
     * @brief Check if the HC-15 module is busy.
     * @return true if the module is busy, false otherwise.
     */
    bool isBuzy()
    {
        return digitalRead(sta_pin_) == LOW; // ggLOW means busy
    }

    /*
     * simple test to check if the HC-15 module is working.
     */
    bool test()
    {
        return (writeCommand("AT\r\n") > 0 && _expectOK("OK"));
    }

    /*
     * @brief Reset the HC-15 module to its default settings.
     */
    bool resetDefault()
    {
        if (xSemaphoreTake(hc15_buzy_semaphore_, pdMS_TO_TICKS(10000)) != pdTRUE)
            return false; // wait for the semaphore to be available, timeout after 10 seconds
        auto rtn = (writeCommand("AT+DEFAULT\r\n") > 0 && _expectOK("OK+DEFAULT"));
        xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after writing
        return rtn;
    }

    /*
     * @brief Get the current baud rate of the HC-15 module.
     * @return The baud rate as a string.
     */
    String getBaudRate(uint32_t timeout_ms = 5000)
    {

        if (xSemaphoreTake(hc15_buzy_semaphore_, pdMS_TO_TICKS(10000)) != pdTRUE)
            return "ERROR SEMAPHORE: TIMEOUT"; // wait for the semaphore to be available, timeout after 10 seconds
        if (writeCommand("AT+B?\r\n") > 0)
        {
            String line = _expectLine(timeout_ms);
            // parse the baud rate from the response
            if (line.startsWith("OK+B:"))
            {
                String baudRateStr = line.substring(5); // Extract the baud rate part
                xSemaphoreGive(hc15_buzy_semaphore_);   // release the semaphore after reading
                return baudRateStr;
            }
            else
            {
                Serial.println("getBaudRate failed, response: " + line);
                xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                return "ERROR RESPONSE";
            }
        }
        xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
        return "WRITE COMMAND FAILED";
    }

    /*
     * @brief Get the current parity bit of the HC-15 module.
     * @return The parity bit as a string (1, 0, or 2).
     */
    String getParityBit()
    {
        if (xSemaphoreTake(hc15_buzy_semaphore_, pdMS_TO_TICKS(10000)) != pdTRUE)
            return "ERROR SEMAPHORE: TIMEOUT"; // wait for the semaphore to be available, timeout after 10 seconds
        if (writeCommand("AT+PARITYBIT?\r\n") > 0)
        {
            String line = _expectLine();
            if (line.startsWith("OK+PARITYBIT"))
            {
                xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                return line.substring(15);
            }
            else
            {
                xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                Serial.println("getParityBit failed, response: " + line);
                return "ERROR RESPONSE";
            }
        }
        xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
        return "WRITE COMMAND FAILED";
    }

    /*
     * @brief Set the parity bit for the HC-15 module.
     * @param parity_bit The parity bit to set (1, 0, or 2).
     * @param timeout_ms The maximum time to wait for the command to complete in milliseconds.
     */
    String setParityBit(const String &parity_bit, uint32_t timeout_ms = 5000)
    {
        if (parity_bit == "1" || parity_bit == "0" || parity_bit == "2")
        {
            if (xSemaphoreTake(hc15_buzy_semaphore_, pdMS_TO_TICKS(10000)) != pdTRUE)
                return "ERROR SEMAPHORE: TIMEOUT"; // wait for the semaphore to be available, timeout after 10 seconds
            String cmd = "AT+PARITYBIT" + parity_bit + "\r\n";
            if (writeCommand(cmd.c_str()) > 0)
            {
                String line = _expectLine(timeout_ms);
                if (line.startsWith("OK+PARITYBIT"))
                {
                    xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                    return line.substring(1);
                }
                else
                {
                    xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                    Serial.println("setParityBit failed, response: " + line);
                    return "ERROR RESPONSE";
                }
            }
            xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
            return "WRITE COMMAND FAILED";
        }
        return "INVALID PARITY BIT";
    }

    /*
     * @brief Get the current stop bit of the HC-15 module.
     * @return The stop bit as a string (1, 2, or 3).
     * 1 -> 1 stop bit, 2 -> 1.5 stop bits, 3 -> 2 stop bits.
     */
    String getStopBit()
    {
        if (xSemaphoreTake(hc15_buzy_semaphore_, pdMS_TO_TICKS(10000)) != pdTRUE)
            return "ERROR SEMAPHORE: TIMEOUT"; // wait for the semaphore to be available, timeout after 10 seconds
        if (writeCommand("AT+STOPBIT?\r\n") > 0)
        {
            String line = _expectLine();
            if (line.startsWith("OK+STOPBIT"))
            {
                xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                return line.substring(10);
            }
            else
            {
                xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                Serial.println("getStopBit failed, response: " + line);
                return "ERROR RESPONSE";
            }
        }
        xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
        return "WRITE COMMAND FAILED";
    }

    /*
     * @brief Set the stop bit for the HC-15 module.
     * @param stop_bit The stop bit to set (1, 2, or 3).
     * @param timeout_ms The maximum time to wait for the command to complete in milliseconds.
     */
    String setStopBit(const String &stop_bit, uint32_t timeout_ms = 5000)
    {
        if (stop_bit == "1" || stop_bit == "2" || stop_bit == "3")
        // 1 -> 1, 2 -> 1.5, 3 -> 2
        {
            if (xSemaphoreTake(hc15_buzy_semaphore_, pdMS_TO_TICKS(10000)) != pdTRUE)
                return "ERROR SEMAPHORE: TIMEOUT"; // wait for the semaphore to be available, timeout after 10 seconds
            String cmd = "AT+STOPBIT" + stop_bit + "\r\n";
            if (writeCommand(cmd.c_str()) > 0)
            {
                String line = _expectLine(timeout_ms);
                if (line.startsWith("OK+STOPBIT"))
                {
                    xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                    return line.substring(10);
                }
                else
                {
                    xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                    Serial.println("setStopBit failed, response: " + line);
                    return "ERROR RESPONSE";
                }
            }
            xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
            return "WRITE COMMAND FAILED";
        }
        return "INVALID STOP BIT";
    }

    /*
     * @brief Get the current channel of the HC-15 module.
     */
    String getChannel()
    {
        if (xSemaphoreTake(hc15_buzy_semaphore_, pdMS_TO_TICKS(10000)) != pdTRUE)
            return "ERROR SEMAPHORE: TIMEOUT"; // wait for the semaphore to be available, timeout after 10 seconds
        if (writeCommand("AT+C?\r\n") > 0)
        {
            String line = _expectLine();
            if (line.startsWith("OK+C:"))
            {
                xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                return line.substring(5);
            }
            else
            {
                xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                Serial.println("getChannel failed, response: " + line);
                return "ERROR RESPONSE";
            }
        }
        xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
        return "WRITE COMMAND FAILED";
    }

    /*
     * @brief Set the channel for the HC-15 module.
     * @param channel The channel number to set (1-50).
     * @param timeout_ms The maximum time to wait for the command to complete in milliseconds.
     */
    String setChannel(uint8_t channel, uint32_t timeout_ms = 5000)
    {
        if (channel >= 1 && channel <= 50)
        {
            if (xSemaphoreTake(hc15_buzy_semaphore_, pdMS_TO_TICKS(10000)) != pdTRUE)
                return "ERROR SEMAPHORE: TIMEOUT"; // wait for the semaphore to be available, timeout after 10 seconds
            String cmd = "AT+C" + channelConvertString(channel) + "\r\n";
            if (writeCommand(cmd.c_str()) > 0)
            {
                String line = _expectLine(timeout_ms);
                if (line.startsWith("OK+C:"))
                {
                    xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                    return line.substring(5);
                }
                else
                {
                    xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                    Serial.println("GetChannel failed, response: " + line);
                    return "ERROR RESPONSE";
                }
            }
            xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
            return "WRITE COMMAND FAILED";
        }
        return "INVALID CHANNEL";
    }

    /*
     * @brief Get the current speed of the HC-15 module.
     * @return The speed as a string (1-8).
     */
    String getSpeed()
    {
        if (xSemaphoreTake(hc15_buzy_semaphore_, pdMS_TO_TICKS(10000)) != pdTRUE)
            return "ERROR SEMAPHORE: TIMEOUT"; // wait for the semaphore to be available, timeout after 10 seconds
        if (writeCommand("AT+S?\r\n") > 0)
        {
            String line = _expectLine();
            if (line.startsWith("OK+S:"))
            {
                xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                return line.substring(5);
            }
            else
            {
                xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                Serial.println("getSpeed failed, response: " + line);
                return "ERROR RESPONSE";
            }
        }
        xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
        return "WRITE COMMAND FAILED";
    }

    /*
     * @brief Set the speed for the HC-15 module.
     * @param speed The speed to set (1-8).
     * @param timeout_ms The maximum time to wait for the command to complete in milliseconds.
     */
    String setSpeed(uint8_t speed, uint32_t timeout_ms = 5000)
    {
        if (speed >= 1 && speed <= 8)
        {
            if (xSemaphoreTake(hc15_buzy_semaphore_, pdMS_TO_TICKS(10000)) != pdTRUE)
                return "ERROR SEMAPHORE: TIMEOUT"; // wait for the semaphore to be available, timeout after 10 seconds
            String cmd = "AT+S" + channelConvertString(speed) + "\r\n";
            if (writeCommand(cmd.c_str()) > 0)
            {
                String line = _expectLine(timeout_ms);
                if (line.startsWith("OK+S:"))
                {
                    xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                    return line.substring(5);
                }
                else
                {
                    xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
                    Serial.println("setSpeed failed, response: " + line);
                    return "ERROR RESPONSE";
                }
            }
            xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading
            return "WRITE COMMAND FAILED";
        }
        return "INVALID CHANNEL";
    }

    struct HC15BasicParams
    {
        uint32_t baud;  // 串口波特率 1200~115200
        uint8_t chan;   // 无线信道 1~50
        uint8_t airSpd; // 无线空速 0~7（典型 3 = 9600）
        int8_t txPwr;   // 发射功率 dBm，可正可负
    };

    HC15BasicParams getBasicParams(uint32_t timeout_ms = 3000)
    {
        HC15BasicParams info{0, 0, 0, 0};

        if (xSemaphoreTake(hc15_buzy_semaphore_, pdMS_TO_TICKS(10000)) != pdTRUE)
        {
            Serial.println("[HC15] getBasicParams: SEMAPHORE TIMEOUT");
            return info;
        }

        /* 1. 发命令 */
        if (writeCommand("AT+RX\r\n") <= 0)
        {
            Serial.println("[HC15] write AT+RX failed");
            return info;
        }

        /* 2. 逐行拿 4 行，或直到超时/空行 */
        unsigned long t0 = millis();
        uint8_t linesGot = 0;
        while (millis() - t0 < timeout_ms && linesGot < 4)
        {
            String line = _expectLine(500 /*每行局部超时*/);
            if (line.length() == 0)
                continue; // 超时或空行
            linesGot++;

            // OK+B:9600
            if (line.startsWith("OK+B:"))
            {
                info.baud = line.substring(5).toInt();
            }
            // OK+C:28
            else if (line.startsWith("OK+C:"))
            {
                info.chan = static_cast<uint8_t>(line.substring(5).toInt());
            }
            // OK+S:3
            else if (line.startsWith("OK+S:"))
            {
                info.airSpd = static_cast<uint8_t>(line.substring(5).toInt());
            }
            // OK+P:22dBm  / OK+P:-1dBm
            else if (line.startsWith("OK+P:"))
            {
                String val = line.substring(5);
                val.replace("dBm", "");
                info.txPwr = static_cast<int8_t>(val.toInt());
            }
            // 其余忽略
        }

        xSemaphoreGive(hc15_buzy_semaphore_); // release the semaphore after reading

        if (linesGot < 4)
        {
            Serial.println("[HC15] getBasicParams timeout/incomplete");
        }
        return info; // 失败时字段有可能为 0，自行判
    }

    SemaphoreHandle_t hc15_buzy_semaphore_;
    String readBuffer; // the buffer to store the read data

private:
    /*
     * @brief Write a string to the HC-15 module.
     * @param str The string to write.
     * @param timeout_ms The maximum time to wait for the write operation to complete in milliseconds.
     * @return The number of bytes written, or 0 if the write operation failed.
     */
    int write(const char *str, uint32_t timeout_ms = 0)
    {
        // Ensure the key pin is set high before sending commands
        if (timeout_ms == 0) // if timeout is not set, use the default timeout
            timeout_ms = timeout_;
        auto time1 = millis();
        while (millis() - time1 < timeout_ms && !isBuzy())
            ;
        if (isBuzy())
            return 0;
        if (serial_ && str)
        {
            delay(100);
            return serial_->write((const uint8_t *)str, strlen(str));
        }
        else
        {
            return 0;
        }
    }

    /*
     * @brief Write a command to the HC-15 module. will automatically pull the key pin high.
     * @param command The command to write.
     * @param timeout_ms The maximum time to wait for the write operation to complete in milliseconds.
     * @return The number of bytes written, or 0 if the write operation failed.
     */
    int writeCommand(const char *command, uint32_t timeout_ms = 0)
    {
        digitalWrite(key_pin_, LOW); // Set key pin high to send commands
        int result = write(command, timeout_ms);
        digitalWrite(key_pin_, HIGH);
        return result;
    }

    /**
     * @brief Wait for a specific reply line.
     * @param expect_word    the expected reply line, default is "OK"
     * @param timeout_ms     the maximum time to wait for the reply in milliseconds; 0 means use the class member timeout_
     * @param spill_to_buf   true → spill unmatched lines / remaining bytes to readBuffer
     *                       false → discard directly, suitable for "command mode"
     * @return true  : received matching line
     *         false : timeout or unmatched
     */
    bool _expectOK(const String &expect_word = "OK",
                   uint32_t timeout_ms = 0,
                   bool spill_to_buf = false)
    {
        if (timeout_ms == 0)
            timeout_ms = timeout_;

        unsigned long start = millis();
        String line;

        while (millis() - start < timeout_ms)
        {
            while (serial_->available() > 0)
            {
                char c = serial_->read();
                if (c == '\r' || c == '\n')
                {
                    if (line.length())
                    {
                        if (line == expect_word) // 命中
                            return true;

                        // 未命中：按需回流 / 丢弃
                        if (spill_to_buf)
                        {
                            readBuffer += line + '\n';
                        }
                        line.clear();
                    }
                    /* 连续 CR/LF 直接忽略 */
                }
                else
                {
                    line += c;
                }
            }
            vTaskDelay(1); // 喂狗 + 让出 CPU
        }

        // 超时：把半行残余也按需回流
        if (spill_to_buf && line.length())
        {
            readBuffer += line;
        }
        return false;
    }

    /*
     * @brief Expect a line of text from the HC-15 module.
     * @param timeout_ms The maximum time to wait for the line in milliseconds.
     * @return The line read from the HC-15 module, or an empty string if the timeout is reached.
     */
    String _expectLine(uint32_t timeout_ms = 5000)
    {
        auto time1 = millis();
        String line = "";

        while (millis() - time1 < timeout_ms)
        {
            if (serial_->available() > 0)
            {
                char c = serial_->read();
                if (c == '\n' || c == '\r')
                {
                    break;
                }
                else
                {
                    line += c;
                }
            }
        }
        return line;
    }

    String channelConvertString(uint8_t channel)
    {
        if (channel >= 1 && channel < 10)
        {
            return "00" + String(channel); // Convert to three-digit format
        }
        else if (channel >= 10 && channel <= 50)
        {
            return "0" + String(channel); // Convert to three-digit format
        }
        return "028";
    }

    // Add private members or methods if needed
    HardwareSerial *serial_ = nullptr;
    uint32_t baud_rate_ = 9600; // Default baud rate
    uint8_t rx_pin_ = 1;        // Default RX pin
    uint8_t tx_pin_ = 0;        // Default TX pin
    uint8_t sta_pin_ = 12;      // Default status pin
    uint8_t key_pin_ = 18;      // Default key pin need to set high when send commands
    uint32_t timeout_ = 5000;
};