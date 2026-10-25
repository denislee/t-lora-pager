/**
 * @file      LilyGoKeyboard.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-04
 *
 */
#include "LilyGoKeyboard.h"

#ifdef USING_INPUT_DEV_KEYBOARD

#ifndef LEDC_BACKLIGHT_CHANNEL
#define LEDC_BACKLIGHT_CHANNEL      4
#endif

#ifndef LEDC_BACKLIGHT_BIT_WIDTH
#define LEDC_BACKLIGHT_BIT_WIDTH    8
#endif

#ifndef LEDC_BACKLIGHT_FREQ
#define LEDC_BACKLIGHT_FREQ         1000 //HZ
#endif


static bool keyboard_interrupted = false;

// [LOCAL PATCH P4.12] Task handle registered via setNotifyTask(). When set,
// keyboard_isr notifies it with vTaskNotifyGiveFromISR() so the keyboard task
// can block on ulTaskNotifyTake() instead of polling at 50 Hz.  nullptr = off.
static TaskHandle_t s_notify_task = nullptr;

static void keyboard_isr()
{
    keyboard_interrupted = true;
    // [LOCAL PATCH P4.12] Wake the registered task immediately on every TCA8418
    // interrupt edge. The task still has a fallback timeout so no event is lost
    // if this runs before the task blocks on ulTaskNotifyTake.
    if (s_notify_task) {
        BaseType_t higher_prio_woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_notify_task, &higher_prio_woken);
        portYIELD_FROM_ISR(higher_prio_woken);
    }
}

LilyGoKeyboard::LilyGoKeyboard()
    : _backlight(-1), _brightness(0), _irq(0), cb(nullptr),
      repeat_function(false), symbol_key_pressed(false),
      cap_key_pressed(false), alt_key_pressed(false),
      lastState(false),  lastKeyVal('\0'), lastPressedTime(0)
{
}

LilyGoKeyboard::~LilyGoKeyboard()
{
}

void LilyGoKeyboard::setPins(int backlight)
{
    _backlight = backlight;
}

void LilyGoKeyboard::setBrightness(uint8_t level)
{
    if (this->bl_cb) {
        this->bl_cb(level);
        return;
    }
    if (_backlight == -1) {
        return;
    }
    _brightness = level;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)
    ledcWrite(_backlight, _brightness);
#else
    ledcWrite(LEDC_BACKLIGHT_CHANNEL, _brightness);
#endif
}

uint8_t LilyGoKeyboard::getBrightness()
{
    return _brightness;
}

bool LilyGoKeyboard::begin(const LilyGoKeyboardConfigure_t &config, TwoWire &w,  uint8_t irq, uint8_t sda, uint8_t scl)
{
    if (_backlight != -1) {
        ::pinMode(_backlight, OUTPUT);
        ::digitalWrite(_backlight, LOW);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)
        ledcAttach(_backlight, LEDC_BACKLIGHT_FREQ, LEDC_BACKLIGHT_BIT_WIDTH);
#else
        ledcSetup(LEDC_BACKLIGHT_CHANNEL, LEDC_BACKLIGHT_FREQ, LEDC_BACKLIGHT_BIT_WIDTH);
        ledcAttachPin(_backlight, LEDC_BACKLIGHT_CHANNEL);
#endif
        setBrightness(127);
    }


    bool res = Adafruit_TCA8418::begin(TCA8418_DEFAULT_ADDR, &w);
    if (!res) {
        log_e("Failed to find Keyboard");
        return false;
    }

    symbol_key_pressed = false;
    cap_key_pressed = false;
    alt_key_pressed = false;
    lastState = false;
    lastKeyVal = '\0';
    lastPressedTime = 0;

    log_d("Initializing Keyboard succeeded");

    _config = &config;

    // Configure the matrix size (using the number of rows and columns currently mapped)
    log_d("set matrix : rows: %d  cols: %d\n", _config->kb_rows, _config->kb_cols);
    this->matrix(_config->kb_rows, _config->kb_cols);
    this->flush();

    if (irq > 0) {
        _irq = irq;
        ::pinMode(_irq, INPUT_PULLUP);
        attachInterrupt(_irq, keyboard_isr, CHANGE);
        log_d("Set keyboard input pull. pin %d", _irq);
        this->enableInterrupts();
    }
    return true;
}

void LilyGoKeyboard::end()
{
    setBrightness(0);
    
    if (_irq > 0) {
        this->disableInterrupts();
        detachInterrupt(_irq);
        ::pinMode(_irq, OPEN_DRAIN);
    }
    if (_backlight != -1) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)
        ledcDetach(_backlight);
#else
        ledcDetachPin(_backlight);
#endif
        ::pinMode(_backlight, OPEN_DRAIN);
    }
    for (int pin = 0; pin < 18; pin++) {
        this->pinMode(pin, INPUT);
    }
}

void LilyGoKeyboard::setCallback(KeyboardReadCallback cb)
{
    this->cb = cb;
}

void LilyGoKeyboard::setGpioEventCallback(GpioEventCallback cb)
{
    this->gpio_cb = cb;
}

void LilyGoKeyboard::setBacklightChangeCallback(BacklightCallback cb)
{
    this->bl_cb = cb;
}

void LilyGoKeyboard::setRawCallback(KeyboardRawCallback cb)
{
    this->raw_cb = cb;
}

void LilyGoKeyboard::setRepeat(bool enable)
{
    repeat_function = enable;
}

// [LOCAL PATCH P4.12] Register (or clear) the task handle that keyboard_isr
// will notify via vTaskNotifyGiveFromISR on every TCA8418 interrupt edge.
// Safe to call from any task context; ISRs read s_notify_task atomically on
// Xtensa (aligned pointer write is single-cycle, no torn read possible).
void LilyGoKeyboard::setNotifyTask(TaskHandle_t h)
{
    s_notify_task = h;
}

int LilyGoKeyboard::getKey(char *c)
{
    const uint32_t REPEAT_INTERVAL = 300;
    static char output;
    static uint32_t interval = 0;
    int val = -1;

    if (millis() - interval > 100) {
        // Polling detects whether there is an ignored state in the interrupt status that has not been processed.
        // The polling speed affects the response speed of the keyboard.
        interval = millis();
        this->readRegister(TCA8418_REG_INT_STAT);
        if (this->available() != 0 && !keyboard_interrupted) {
            keyboard_interrupted = true;
        }
    }

    if (repeat_function) {
        if (lastState) {
            if (millis() - lastPressedTime > REPEAT_INTERVAL) {
                // The space key conflicts with the symbol function,
                // so the space key is not processed as a continuous key.
                if (lastKeyVal == 0 || lastKeyVal == ' ') {
                    lastState = false;
                    return -1;
                }
                log_d("Pressed repeat %c\n", output);
                lastPressedTime = millis();
                if (c) {
                    *c = output;
                }
                if (cb) {
                    cb(KB_PRESSED, output);
                }
                return KB_PRESSED;
            }
        }
    }

    if (!keyboard_interrupted) {
        return val;
    }

    int intStat = this->readRegister(TCA8418_REG_INT_STAT);
    if (intStat & 0x02) {
        //  reading the registers is mandatory to clear IRQ flag
        //  can also be used to find the GPIO changed
        //  as these registers are a bitmap of the gpio pins.
        this->readRegister(TCA8418_REG_GPIO_INT_STAT_1);
        this->readRegister(TCA8418_REG_GPIO_INT_STAT_2);
        this->readRegister(TCA8418_REG_GPIO_INT_STAT_3);
        //  clear GPIO IRQ flag
        this->writeRegister(TCA8418_REG_INT_STAT, 2);
    }


    // Clear IRQ flag
    this->writeRegister(TCA8418_REG_INT_STAT, 1);
    
    int ret = update(&output);

    uint8_t intstat = this->readRegister(TCA8418_REG_INT_STAT);
    if ((intstat & 0x01) == 0 && this->available() == 0) {
        keyboard_interrupted = false;
    } else {
        keyboard_interrupted = true;
    }

    if (cb) {
        cb(ret, output);
    }
    if (c) {
        *c = output;
    }
    return ret;
}


int LilyGoKeyboard::handleSpecialKeys(uint8_t k, bool pressed, char *c)
{
    int ret = 0;
    bool isSpecial = false;

    if (k == _config->symbol_key_value) {
        symbol_key_pressed = pressed; // Momentary symbol mode
        if (_config->has_symbol_key) ret = -1;
        isSpecial = true;
    }
    if (k == _config->caps_key_value || k == _config->caps_b_key_value) {
        cap_key_pressed = pressed; // Momentary shift
        ret = -1;
        isSpecial = true;
    }
    if (k == _config->alt_key_value) {
        alt_key_pressed = pressed; // Momentary alt
        ret = -1;
        isSpecial = true;
    }

    if (isSpecial) {
        return ret;
    }

    if (k == _config->backspace_value) {
        if (pressed) {
            if (alt_key_pressed) {
                *c = 0x17; // Alt+Backspace
            } else {
                *c = '\b'; // Backspace character
            }
            lastKeyVal = *c;
            lastState = true;
            lastPressedTime = millis();
            return KB_PRESSED;
        } else {
            lastState = false;
            lastPressedTime = 0;
        }
        return -1;
    }
    return 0;
}

bool LilyGoKeyboard::handleBrightnessAdjustment(uint8_t k, bool pressed)
{
    static bool adjust_brightness_pressed = false;
    if (pressed) {
        if (alt_key_pressed && k == _config->char_b_value) {
            if (_backlight != -1 || (this->bl_cb != NULL)) {
                // ALT+B toggle brightness
                _brightness = (_brightness > 0) ? 0 : 127;
                if (this->bl_cb) {
                    this->bl_cb(_brightness);
                } else if (_backlight != -1) {
                    setBrightness(_brightness);
                }
                adjust_brightness_pressed = true;
                return true;
            }
        }
    } else if (adjust_brightness_pressed) {
        adjust_brightness_pressed = false;
        return true;
    }
    return false;
}

char LilyGoKeyboard::getKeyChar(uint8_t k)
{
    uint8_t row = k / 10;
    uint8_t col = k % 10;

    if (row >= _config->kb_rows || col >= _config->kb_cols) {
        log_e("Returns a null character if out of bounds");
        return '\0'; // Return empty character if out of bounds
    }

    char keyVal;
    // If alt_key is same as caps_key, don't trigger symbol map with it.
    bool useSymbols = symbol_key_pressed;
    if (alt_key_pressed && (_config->alt_key_value != _config->caps_key_value)) {
        useSymbols = true;
    }

    if (useSymbols) {
        // Symbol mode: access the current symbol map (first address + offset)
        keyVal = *(_config->current_symbol_map + row * _config->kb_cols + col);
    } else {
        // Character mode: access the current character map
        keyVal = *(_config->current_keymap + row * _config->kb_cols + col);
        // Uppercase conversion (skipping null characters)
        if (cap_key_pressed && keyVal != '\0') {
            keyVal = toupper(keyVal);
        }
    }
    return keyVal;
}

char LilyGoKeyboard::handleSpaceAndNullChar(char keyVal, char &lastKeyVal, bool &pressed)
{

    // 符号模式下空格无效，统一转换为'\0'
    if ((symbol_key_pressed || alt_key_pressed) && keyVal == ' ') {
        keyVal = '\0';
    }
    // 非符号模式下处理空格逻辑
    else if (!(symbol_key_pressed || alt_key_pressed)) {
        // 有符号键的配置：连续空字符视为空格
        if (_config->has_symbol_key) {
            if (keyVal == '\0' && lastKeyVal == '\0' && pressed) {
                keyVal = ' ';
            }
        }
        // 无符号键的配置：如果是空字符，尝试转换为空格（修复之前错误拦截所有字符的bug）
        else if (keyVal == '\0' && lastKeyVal == '\0' && pressed) {
            keyVal = ' ';
        }
    }
    return keyVal;
}

void LilyGoKeyboard::printDebugInfo(bool pressed, uint8_t k, char keyVal)
{
    Serial.printf("Debug: symbol=%d, caps=%d, alt=%d\n",
                  symbol_key_pressed, cap_key_pressed, alt_key_pressed);
    Serial.print(pressed ? "Pressed" : "Released");
    Serial.printf(" - Key:0x%X, Row:%d, Col:%d\n",
                  k, k / 10, k % 10);
    Serial.printf("Char:'%c' (0x%X)\n", keyVal, keyVal);
}

int LilyGoKeyboard::update(char *c)
{
    char keyVal = '\0';
    uint8_t k = this->getEvent();
    if (k == 0) {
        return -1; // No event
    }

    bool pressed = (k & 0x80) != 0; //The highest bit indicates the pressed state
    k &= 0x7F; // Clear the status bit and keep the original key value

    // When this callback is set, the program only returns the original key
    // value and does not continue with subsequent processing. The user needs to handle it by himself.
    if (this->raw_cb) {
        this->raw_cb(pressed, k);
        return -1;
    }

    if (k > 96) {
        uint8_t idx = k - 97;
        if (this->gpio_cb) {
            this->gpio_cb(pressed, idx);
        }
        return -1;
    }

    k--; // Adjust key value index
    // Serial.printf("Value:%X\n", k);

    // Check if the key value is within the current mapping range
    uint8_t row = k / 10;
    if (row >= _config->kb_rows) {
        log_e("Key values out of range are ignored,current  row:%d k:%d , _config->kb_cols:%d\n", row, k, _config->kb_cols);
        return -1;
    }

    // Handling special keys
    int specialKeyResult = handleSpecialKeys(k, pressed, c);
    if (specialKeyResult != 0) {
        return specialKeyResult;
    }

    // Handling brightness adjustments
    if (handleBrightnessAdjustment(k, pressed)) {
        return -1;
    }

    // Get the character corresponding to the current key
    keyVal = getKeyChar(k);
    // Handling spaces and null characters
    keyVal = handleSpaceAndNullChar(keyVal, lastKeyVal, pressed);

    // Print debug information
    // printDebugInfo(pressed, k, keyVal);

    // Update state variables
    lastKeyVal = keyVal;
    if (c) {
        *c = keyVal;
    }

    lastState = pressed;
    lastPressedTime = pressed ? millis() : 0;

    return pressed ? KB_PRESSED : KB_RELEASED;
}


#endif // USING_INPUT_DEV_KEYBOARD
