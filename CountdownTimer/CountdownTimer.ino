#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// конечный автомат таймера и 32-разрядная арифметика находятся в AVR-ассемблере
extern "C" {
  extern volatile uint16_t g_days;
  extern volatile uint8_t g_hours;
  extern volatile uint8_t g_selected;
  extern volatile uint8_t g_state;
  extern volatile uint8_t g_rtc_ok;
  extern volatile uint32_t g_deadline;

  void timer_setup(void);
  void timer_loop(void);
  uint8_t timer_start(void);
  uint32_t timer_add_u32(uint32_t left, uint32_t right);
  void platform_save_deadline(void);
}

static const uint8_t RTC_ADDRESS = 0x68;
static const uint8_t LCD_ADDRESS = 0x27;
static const uint32_t EPOCH_LIMIT = 3155760000UL; // предел эпохи: 1 января 2100 года от 1 января 2000 года
static const uint8_t EEPROM_MAGIC = 0xA5;
static const uint8_t EEPROM_XOR = 0x5A;

static const uint8_t BUTTON_MODE = 2;
static const uint8_t BUTTON_UP = 3;
static const uint8_t BUTTON_DOWN = 4;
static const uint8_t BUTTON_START = 5;
static const uint8_t BUZZER_PIN = 9;

struct DateTimeParts {
  uint8_t second;
  uint8_t minute;
  uint8_t hour;
  uint8_t day;
  uint8_t month;
  uint8_t year;
};

static char lastLine1[17] = "";
static char lastLine2[17] = "";
static uint8_t previousButton = 0;
static uint32_t previousButtonAt = 0;

static uint8_t bcdToBinary(uint8_t value) {
  return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static uint8_t binaryToBcd(uint8_t value) {
  return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static bool isLeapYear(uint8_t yearSince2000) {
  return (yearSince2000 % 4) == 0;
}

static uint8_t daysInMonth(uint8_t month, uint8_t yearSince2000) {
  static const uint8_t monthLengths[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeapYear(yearSince2000)) return 29;
  return monthLengths[month - 1];
}

static uint32_t toEpoch2000(const DateTimeParts &parts) {
  uint32_t days = 0;
  for (uint8_t year = 0; year < parts.year; ++year) {
    days += isLeapYear(year) ? 366UL : 365UL;
  }
  for (uint8_t month = 1; month < parts.month; ++month) {
    days += daysInMonth(month, parts.year);
  }
  days += (uint32_t)(parts.day - 1);
  return (((days * 24UL + parts.hour) * 60UL + parts.minute) * 60UL) + parts.second;
}

static bool readRtcRegisters(uint8_t firstRegister, uint8_t *destination, uint8_t count) {
  Wire.beginTransmission(RTC_ADDRESS);
  Wire.write(firstRegister);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t received = Wire.requestFrom(RTC_ADDRESS, count);
  if (received != count) return false;
  for (uint8_t i = 0; i < count; ++i) destination[i] = (uint8_t)Wire.read();
  return true;
}

static bool writeRtcRegisters(uint8_t firstRegister, const uint8_t *values, uint8_t count) {
  Wire.beginTransmission(RTC_ADDRESS);
  Wire.write(firstRegister);
  for (uint8_t i = 0; i < count; ++i) Wire.write(values[i]);
  return Wire.endTransmission() == 0;
}

static bool readRtcParts(DateTimeParts &parts) {
  uint8_t raw[7];
  if (!readRtcRegisters(0, raw, 7)) return false;
  if (raw[5] & 0x80) return false; // рабочий диапазон проекта: 2000-2099
  parts.second = bcdToBinary(raw[0] & 0x7F);
  parts.minute = bcdToBinary(raw[1] & 0x7F);
  parts.hour = bcdToBinary(raw[2] & 0x3F);
  parts.day = bcdToBinary(raw[4] & 0x3F);
  parts.month = bcdToBinary(raw[5] & 0x1F);
  parts.year = bcdToBinary(raw[6]);
  if (parts.second > 59 || parts.minute > 59 || parts.hour > 23) return false;
  if (parts.month < 1 || parts.month > 12) return false;
  if (parts.day < 1 || parts.day > daysInMonth(parts.month, parts.year)) return false;
  return true;
}

static uint8_t monthFromBuildDate(const char *dateText) {
  static const char monthNames[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  for (uint8_t month = 0; month < 12; ++month) {
    if (strncmp(dateText, monthNames + month * 3, 3) == 0) return month + 1;
  }
  return 1;
}

static bool setRtcFromBuildTime(void) {
  const char *dateText = __DATE__;
  const char *timeText = __TIME__;
  int fullYear = atoi(dateText + 7);
  if (fullYear < 2000 || fullYear > 2099) return false;
  uint8_t parts[7];
  parts[0] = binaryToBcd((uint8_t)atoi(timeText + 6));
  parts[1] = binaryToBcd((uint8_t)atoi(timeText + 3));
  parts[2] = binaryToBcd((uint8_t)atoi(timeText));
  parts[3] = 1;
  parts[4] = binaryToBcd((uint8_t)atoi(dateText + 4));
  parts[5] = binaryToBcd(monthFromBuildDate(dateText));
  parts[6] = binaryToBcd((uint8_t)(fullYear - 2000));
  if (!writeRtcRegisters(0, parts, 7)) return false;
  uint8_t clearOscillatorStop = 0;
  return writeRtcRegisters(0x0F, &clearOscillatorStop, 1);
}

static bool rtcEpoch(uint32_t &epoch) {
  DateTimeParts parts;
  if (!readRtcParts(parts)) return false;
  epoch = toEpoch2000(parts);
  return epoch < EPOCH_LIMIT;
}

static void lcdExpander(uint8_t value) {
  Wire.beginTransmission(LCD_ADDRESS);
  Wire.write(value | 0x08); // включить подсветку
  Wire.endTransmission();
}

static void lcdPulse(uint8_t value) {
  lcdExpander(value | 0x04);
  delayMicroseconds(1);
  lcdExpander(value & (uint8_t)~0x04);
  delayMicroseconds(50);
}

static void lcdNibble(uint8_t value, bool dataMode) {
  uint8_t output = (uint8_t)(value & 0xF0);
  if (dataMode) output |= 0x01;
  lcdPulse(output);
}

static void lcdByte(uint8_t value, bool dataMode) {
  lcdNibble(value & 0xF0, dataMode);
  lcdNibble((uint8_t)(value << 4), dataMode);
}

static void lcdCommand(uint8_t value) {
  lcdByte(value, false);
  if (value == 0x01 || value == 0x02) delay(2);
}

static void lcdSetCursor(uint8_t column, uint8_t row) {
  lcdCommand((uint8_t)(0x80 | (row ? 0x40 : 0x00) | column));
}

static void lcdPrint(const char *text) {
  while (*text) lcdByte((uint8_t)*text++, true);
}

static void lcdWriteLines(const char *line1, const char *line2) {
  char padded1[17];
  char padded2[17];
  memset(padded1, ' ', 16);
  memset(padded2, ' ', 16);
  padded1[16] = '\0';
  padded2[16] = '\0';
  strncpy(padded1, line1, 16);
  strncpy(padded2, line2, 16);
  if (strncmp(padded1, lastLine1, 17) != 0) {
    lcdSetCursor(0, 0);
    lcdPrint(padded1);
    memcpy(lastLine1, padded1, 17);
  }
  if (strncmp(padded2, lastLine2, 17) != 0) {
    lcdSetCursor(0, 1);
    lcdPrint(padded2);
    memcpy(lastLine2, padded2, 17);
  }
}

static void lcdInitialize(void) {
  delay(50);
  lcdNibble(0x30, false);
  delay(5);
  lcdNibble(0x30, false);
  delayMicroseconds(150);
  lcdNibble(0x30, false);
  lcdNibble(0x20, false);
  lcdCommand(0x28); // включить 4-разрядный режим, 2 строки, шрифт 5x8
  lcdCommand(0x0C); // включить экран, скрыть курсор
  lcdCommand(0x06); // перемещать курсор вправо
  lcdCommand(0x01); // очистить экран
}

static uint8_t readButtonCode(void) {
  uint8_t code = 0;
  if (digitalRead(BUTTON_MODE) == LOW) code = 1;
  else if (digitalRead(BUTTON_UP) == LOW) code = 2;
  else if (digitalRead(BUTTON_DOWN) == LOW) code = 3;
  else if (digitalRead(BUTTON_START) == LOW) code = 4;
  return code;
}

extern "C" void platform_init(void) {
  pinMode(BUTTON_MODE, INPUT_PULLUP);
  pinMode(BUTTON_UP, INPUT_PULLUP);
  pinMode(BUTTON_DOWN, INPUT_PULLUP);
  pinMode(BUTTON_START, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  Wire.begin();
  Wire.setClock(100000UL);
  lcdInitialize();

  uint8_t status = 0;
  if (readRtcRegisters(0x0F, &status, 1) && (status & 0x80)) {
    setRtcFromBuildTime(); // при первом запуске используется время сборки прошивки
  }
  uint32_t now;
  g_rtc_ok = rtcEpoch(now) ? 1 : 0;
}

extern "C" uint8_t platform_event(void) {
  uint8_t code = readButtonCode();
  if (code == 0) {
    previousButton = 0;
    return 0;
  }
  uint32_t now = millis();
  if (code == previousButton && (code == 1 || code == 4)) return 0;
  if (now - previousButtonAt < 130) return 0;
  delay(20);
  if (readButtonCode() != code) return 0;
  previousButton = code;
  previousButtonAt = now;
  return code;
}

extern "C" uint32_t platform_now(void) {
  uint32_t epoch = 0;
  g_rtc_ok = rtcEpoch(epoch) ? 1 : 0;
  return epoch;
}

extern "C" void platform_save_deadline(void) {
  uint32_t target = g_deadline;
  EEPROM.update(0, 0); // сначала пометить старую запись недействительной
  uint8_t checksum = EEPROM_XOR;
  for (uint8_t i = 0; i < 4; ++i) {
    uint8_t value = (uint8_t)(target >> (8 * i));
    EEPROM.update(i + 1, value);
    checksum ^= value;
  }
  EEPROM.update(5, checksum);
  EEPROM.update(0, EEPROM_MAGIC); // признак завершения записывается последним
}

extern "C" uint8_t platform_load_deadline(void) {
  if (EEPROM.read(0) != EEPROM_MAGIC) return 0;
  uint32_t target = 0;
  uint8_t checksum = EEPROM_XOR;
  for (uint8_t i = 0; i < 4; ++i) {
    uint8_t value = EEPROM.read(i + 1);
    target |= ((uint32_t)value << (8 * i));
    checksum ^= value;
  }
  if (checksum != EEPROM.read(5) || target >= EPOCH_LIMIT) return 0;
  g_deadline = target;
  return 1;
}

extern "C" void platform_clear_deadline(void) {
  EEPROM.update(0, 0);
  g_deadline = 0;
}

extern "C" void platform_show_setup(void) {
  char line2[17];
  snprintf(line2, sizeof(line2), "D:%05u H:%02u", (unsigned)g_days, (unsigned)g_hours);
  lcdWriteLines(g_selected ? "SET HOURS / MODE" : "SET DAYS / MODE", line2);
}

extern "C" void platform_show_running(uint32_t remaining) {
  char line1[17];
  char line2[17];
  uint32_t days = remaining / 86400UL;
  uint32_t rest = remaining % 86400UL;
  uint8_t hours = (uint8_t)(rest / 3600UL);
  rest %= 3600UL;
  uint8_t minutes = (uint8_t)(rest / 60UL);
  uint8_t seconds = (uint8_t)(rest % 60UL);
  snprintf(line1, sizeof(line1), "LEFT %5lu days", (unsigned long)days);
  snprintf(line2, sizeof(line2), "%02u:%02u:%02u START",
           (unsigned)hours, (unsigned)minutes, (unsigned)seconds);
  lcdWriteLines(line1, line2);
}

extern "C" void platform_show_invalid(void) {
  lcdWriteLines("Invalid duration", "Date limit: 2099");
  delay(800);
}

extern "C" void platform_show_rtc_error(void) {
  lcdWriteLines("RTC unavailable", "Check DS3231 time");
  delay(800);
}

extern "C" void platform_show_alarm(void) {
  lcdWriteLines("COUNTDOWN DONE", "START to clear");
}

extern "C" void platform_alarm(uint8_t enabled) {
  if (!enabled) digitalWrite(BUZZER_PIN, LOW);
  else digitalWrite(BUZZER_PIN, ((millis() / 250UL) & 1UL) ? HIGH : LOW);
}

void setup(void) {
  platform_init();
  timer_setup();
}

void loop(void) {
  timer_loop();
}
