/*
 * конечный автомат таймера и 32-разрядная арифметика времени для ATmega328P
 * доступ к устройствам вынесен в функции platform_* из скетча
 *
 * события platform_event:
 *   0 нет события, 1 MODE, 2 UP, 3 DOWN, 4 START/отмена/подтверждение
 * состояния:
 *   0 настройка, 1 отсчёт, 2 сигнал
 */

.section .bss
.global g_days
.type g_days, @object
g_days:
  .skip 2
.size g_days, 2

.global g_hours
.type g_hours, @object
g_hours:
  .skip 1
.size g_hours, 1

.global g_selected
.type g_selected, @object
g_selected:
  .skip 1
.size g_selected, 1

.global g_state
.type g_state, @object
g_state:
  .skip 1
.size g_state, 1

.global g_rtc_ok
.type g_rtc_ok, @object
g_rtc_ok:
  .skip 1
.size g_rtc_ok, 1

.global g_deadline
.type g_deadline, @object
g_deadline:
  .skip 4
.size g_deadline, 4

.local event_value
event_value:
  .skip 1

.local now_value
now_value:
  .skip 4

.section .text

.global timer_setup
.type timer_setup, @function
timer_setup:
  /* по умолчанию задать 1 час и выбор поля дней */
  clr r24
  sts g_days, r24
  sts g_days+1, r24
  sts g_selected, r24
  sts g_state, r24
  ldi r24, 1
  sts g_hours, r24

  /* восстановить срок после перезапуска контроллера */
  call platform_load_deadline
  tst r24
  breq .Lsetup_done
  ldi r24, 1
  sts g_state, r24
.Lsetup_done:
  ret
.size timer_setup, .-timer_setup

.global timer_start
.type timer_start, @function
timer_start:
  /* прочитать время RTC и прибавить дни и часы на AVR-ассемблере */
  call platform_now
  lds r18, g_rtc_ok
  tst r18
  brne .Lrtc_available
  rjmp .Ltimer_start_invalid
.Lrtc_available:
  sts now_value, r22
  sts now_value+1, r23
  sts now_value+2, r24
  sts now_value+3, r25

  clr r22
  clr r23
  clr r24
  clr r25

  /* в сутках 86400 секунд, длительность хранится в 32 разрядах */
  lds r18, g_days
  lds r19, g_days+1
  tst r18
  brne .Ladd_days
  tst r19
  breq .Ladd_hours_start
.Ladd_days:
  ldi r20, 0x80
  ldi r21, 0x51
  ldi r26, 0x01
  clr r27
.Ladd_day_again:
  add r22, r20
  adc r23, r21
  adc r24, r26
  adc r25, r27
  subi r18, 1
  sbci r19, 0
  brne .Ladd_day_again

.Ladd_hours_start:
  lds r18, g_hours
  tst r18
  breq .Lduration_ready
  ldi r20, 0x10
  ldi r21, 0x0E
  clr r26
  clr r27
.Ladd_hour_again:
  add r22, r20
  adc r23, r21
  adc r24, r26
  adc r25, r27
  dec r18
  brne .Ladd_hour_again

.Lduration_ready:
  /* отклонить нулевую длительность */
  mov r18, r22
  or r18, r23
  or r18, r24
  or r18, r25
  breq .Ltimer_start_invalid

  /* прибавить длительность к текущему времени RTC */
  lds r18, now_value
  lds r19, now_value+1
  lds r20, now_value+2
  lds r21, now_value+3
  call timer_add_u32
  brcs .Ltimer_start_invalid

  /* диапазон эпохи проекта: 2000-01-01 - 2099-12-31 */
  ldi r18, 0x80
  cp r22, r18
  ldi r18, 0x13
  cpc r23, r18
  ldi r18, 0x19
  cpc r24, r18
  ldi r18, 0xBC
  cpc r25, r18
  brsh .Ltimer_start_invalid

  sts g_deadline, r22
  sts g_deadline+1, r23
  sts g_deadline+2, r24
  sts g_deadline+3, r25
  call platform_save_deadline
  ldi r24, 1
  ret

.Ltimer_start_invalid:
  clr r24
  ret
.size timer_start, .-timer_start

.global timer_loop
.type timer_loop, @function
timer_loop:
  call platform_event
  sts event_value, r24

  lds r18, g_state
  cpi r18, 0
  breq .Lsetup_state
  cpi r18, 1
  brne .Ldispatch_alarm
  rjmp .Lrunning_state
.Ldispatch_alarm:
  rjmp .Lalarm_state

.Lsetup_state:
  lds r24, event_value
  cpi r24, 1
  breq .Lmode_event
  cpi r24, 2
  breq .Lincrease_event
  cpi r24, 3
  breq .Ldecrease_event
  cpi r24, 4
  breq .Lstart_event
  rjmp .Lshow_setup

.Lmode_event:
  lds r24, g_selected
  ldi r18, 1
  eor r24, r18
  sts g_selected, r24
  rjmp .Lshow_setup

.Lincrease_event:
  lds r24, g_selected
  tst r24
  brne .Lincrease_hours
  lds r24, g_days
  lds r25, g_days+1
  adiw r24, 1
  sts g_days, r24
  sts g_days+1, r25
  rjmp .Lshow_setup
.Lincrease_hours:
  lds r24, g_hours
  inc r24
  cpi r24, 24
  brlo .Lstore_hours_up
  clr r24
.Lstore_hours_up:
  sts g_hours, r24
  rjmp .Lshow_setup

.Ldecrease_event:
  lds r24, g_selected
  tst r24
  brne .Ldecrease_hours
  lds r24, g_days
  lds r25, g_days+1
  mov r18, r24
  or r18, r25
  breq .Lshow_setup
  sbiw r24, 1
  sts g_days, r24
  sts g_days+1, r25
  rjmp .Lshow_setup
.Ldecrease_hours:
  lds r24, g_hours
  tst r24
  brne .Lhours_down
  ldi r24, 24
.Lhours_down:
  dec r24
  sts g_hours, r24
  rjmp .Lshow_setup

.Lstart_event:
  call timer_start
  tst r24
  breq .Linvalid_setup
  ldi r24, 1
  sts g_state, r24
  ret

.Linvalid_setup:
  lds r24, g_rtc_ok
  tst r24
  breq .Lshow_rtc_error
  call platform_show_invalid
  ret
.Lshow_rtc_error:
  call platform_show_rtc_error
  ret

.Lshow_setup:
  call platform_show_setup
  ret

.Lrunning_state:
  lds r24, event_value
  cpi r24, 4
  brne .Lcontinue_running
  rjmp .Lcancel_running
.Lcontinue_running:

  call platform_now
  lds r18, g_rtc_ok
  tst r18
  breq .Lrunning_rtc_error
  sts now_value, r22
  sts now_value+1, r23
  sts now_value+2, r24
  sts now_value+3, r25

  /* timer_due(now, deadline) возвращает 1 при наступлении срока */
  lds r22, now_value
  lds r23, now_value+1
  lds r24, now_value+2
  lds r25, now_value+3
  lds r18, g_deadline
  lds r19, g_deadline+1
  lds r20, g_deadline+2
  lds r21, g_deadline+3
  call timer_due
  tst r24
  brne .Ldeadline_reached

  /* timer_remaining(deadline, now) возвращает секунды для экрана */
  lds r22, g_deadline
  lds r23, g_deadline+1
  lds r24, g_deadline+2
  lds r25, g_deadline+3
  lds r18, now_value
  lds r19, now_value+1
  lds r20, now_value+2
  lds r21, now_value+3
  call timer_remaining
  call platform_show_running
  ret

.Lrunning_rtc_error:
  call platform_show_rtc_error
  ret

.Ldeadline_reached:
  ldi r24, 2
  sts g_state, r24
  ldi r24, 1
  call platform_alarm
  call platform_show_alarm
  ret

.Lcancel_running:
  call platform_clear_deadline
  clr r24
  sts g_state, r24
  call platform_alarm
  call platform_show_setup
  ret

.Lalarm_state:
  lds r24, event_value
  cpi r24, 4
  breq .Lacknowledge_alarm
  ldi r24, 1
  call platform_alarm
  call platform_show_alarm
  ret

.Lacknowledge_alarm:
  call platform_clear_deadline
  clr r24
  sts g_state, r24
  call platform_alarm
  call platform_show_setup
  ret
.size timer_loop, .-timer_loop

.global timer_add_u32
.type timer_add_u32, @function
timer_add_u32:
  /* сложить два числа uint32_t: timer_add_u32(uint32_t left, uint32_t right) */
  add r22, r18
  adc r23, r19
  adc r24, r20
  adc r25, r21
  ret
.size timer_add_u32, .-timer_add_u32

.global timer_due
.type timer_due, @function
timer_due:
  /* проверить, наступил ли срок: uint8_t timer_due(uint32_t now, uint32_t deadline) */
  cp r22, r18
  cpc r23, r19
  cpc r24, r20
  cpc r25, r21
  brlo .Lnot_due
  ldi r24, 1
  ret
.Lnot_due:
  clr r24
  ret
.size timer_due, .-timer_due

.global timer_remaining
.type timer_remaining, @function
timer_remaining:
  /* найти остаток в секундах: uint32_t timer_remaining(uint32_t deadline, uint32_t now) */
  sub r22, r18
  sbc r23, r19
  sbc r24, r20
  sbc r25, r21
  ret
.size timer_remaining, .-timer_remaining
