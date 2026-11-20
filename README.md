# таймер-напоминатель

прошивка для Arduino Uno Rev3 с часами DS3231, экраном LCD1602, кнопками и зуммером

`CountdownTimer.ino` связывает устройства, `timer_core.S` содержит логику таймера на AVR-ассемблере

сборка: `arduino-cli compile --fqbn arduino:avr:uno CountdownTimer`
