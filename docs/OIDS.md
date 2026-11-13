# Реализованный SNMP-интерфейс

SNMPv2c, UDP 161. GET, GETNEXT и GETBULK поддерживаются. SET принимается
только с отдельной Write Community и только для объектов управления ИБП
(см. «Управление ИБП по SNMP»); с Read Community SET возвращает
`notWritable`. Максимум UDP-пакета 1400 байт, входных varbind 32, выходных 64.
Большой GETBULK сокращается до помещающегося упорядоченного префикса. Большой
обычный GET/GETNEXT получает `tooBig`.

## Идентификация шлюза

| OID | Имя | Тип / значение |
| --- | --- | --- |
| 1.3.6.1.2.1.1.1.0 | sysDescr | `ESP32 UPS Gateway/0.2.0; driver=...; PowerNet subset` |
| 1.3.6.1.2.1.1.2.0 | sysObjectID | OBJECT IDENTIFIER `0.0`, идентификатор производителя пока не назначен |
| 1.3.6.1.2.1.1.3.0 | sysUpTime | TimeTicks, сотые секунды от запуска |
| 1.3.6.1.2.1.1.4.0 | sysContact | Строка из конфигурации |
| 1.3.6.1.2.1.1.5.0 | sysName | Имя шлюза |
| 1.3.6.1.2.1.1.6.0 | sysLocation | Расположение |
| 1.3.6.1.2.1.1.7.0 | sysServices | INTEGER 72 |

Для Observium используйте приложенное определение по `sysDescr`. Наличие
APC OID не означает, что шлюз является конкретной моделью APC.

## PowerNet

К каждому суффиксу ниже добавляется префикс `1.3.6.1.4.1.318.1.1.1.`.

| Суффикс с .0 | Объект | Тип | Единица / условие |
| --- | --- | --- | --- |
| 1.1.1.0 | upsBasicIdentModel | OCTET STRING | Модель из I; иначе обозначение драйвера |
| 1.1.2.0 | upsBasicIdentName | OCTET STRING | Имя шлюза |
| 1.2.1.0 | upsAdvIdentFirmwareRevision | OCTET STRING | Версия ИБП из I, если получена |
| 1.2.3.0 | upsAdvIdentSerialNumber | OCTET STRING | Только если драйвер предоставит серийный номер |
| 8.1.0 | upsCommStatus | INTEGER | 1 связь есть; 2 связи нет |
| 2.1.1.0 | upsBasicBatteryStatus | INTEGER | 1 неизвестно; 2 нет признака low battery; 3 low battery |
| 4.1.1.0 | upsBasicOutputStatus | INTEGER | 1 неизвестно; 2 online; 3 battery; 9 bypass; 15 тест |
| 3.2.1.0 | upsAdvInputLineVoltage | Gauge32 | Вольты |
| 3.3.1.0 | upsHighPrecInputLineVoltage | Gauge32 | Десятые доли вольта |
| 3.2.4.0 | upsAdvInputFrequency | Gauge32 | Герцы |
| 3.3.4.0 | upsHighPrecInputFrequency | Gauge32 | Десятые доли герца |
| 4.2.1.0 | upsAdvOutputVoltage | Gauge32 | Вольты |
| 4.3.1.0 | upsHighPrecOutputVoltage | Gauge32 | Десятые доли вольта |
| 4.2.3.0 | upsAdvOutputLoad | Gauge32 | Проценты |
| 4.3.3.0 | upsHighPrecOutputLoad | Gauge32 | Десятые доли процента |
| 2.2.2.0 | upsAdvBatteryTemperature | Gauge32 | Внутренняя температура ИБП, °C; ниже 0 → 0 |
| 2.3.2.0 | upsHighPrecBatteryTemperature | Gauge32 | Та же температура, десятые °C |
| 2.2.8.0 | upsAdvBatteryActualVoltage | INTEGER | Вольты батарейной цепи, только с настроенным множителем |
| 2.3.4.0 | upsHighPrecBatteryActualVoltage | INTEGER | Десятые доли вольта батарейной цепи |

Имена температурных объектов исторически содержат Battery, но их описание
в PowerNet говорит о **внутренней температуре UPS**. В веб-интерфейсе это
температура ИБП, а не отдельный датчик батареи. Код bypass=9 — совместимое
представление обнаруженного bypass; Q1 не сообщает причину его включения.

Ниже предусмотрены адаптером для будущих драйверов, но базовый Q1 их не
предоставляет: `2.2.1.0` — заряд (%); `2.2.3.0` — runtime (TimeTicks);
`4.2.2.0` / `4.3.2.0` — измеренная выходная частота (Hz / 0.1 Hz).

Неизвестные/неподдерживаемые измерения отсутствуют: GET получает
`noSuchInstance`, walk пропускает их. При потере RS232 измерения также
перестают выдаваться как текущие; статусы становятся unknown/noComm.
Модель/версия могут сохраняться после потери связи. Последние значения
доступны в веб-интерфейсе с явным признаком устаревания.

## Управление ИБП по SNMP

Выключено по умолчанию. Включается заданием Write Community (страница
Network / SNMP, поле `snmp_write_community`, 1–64 байта, должно отличаться от
Read Community). Кроме того, действуют те же разрешения, что и для
веб-управления: `controls_enabled` для всех команд и `power_controls_enabled`
для команд, меняющих выход. Ограничение `snmp_manager_ip` действует и на SET.
Запрос с Write Community может делать и GET, и SET.

| Суффикс с .0 | Объект | Значение → команда MegaTec |
| --- | --- | --- |
| 6.2.1.0 | upsAdvControlUpsOff | 2 → `output_off` через 12 с (`S.2`); 3 (graceful) → через 1 мин (`S01`) |
| 6.1.1.0 | upsBasicControlConserveBattery | 2 → `output_restart`: отключение через 1 мин, включение через 3 мин (`S01R0003`); только при работе от батареи |
| 6.2.2.0 | upsAdvControlRebootShutdownUps | 2 → `output_restart` через 12 с, включение через 3 мин (`S.2R0003`); 3 → через 1 мин (`S01R0003`) |
| 6.2.5.0 | upsAdvControlFlashAndBeep | 2 → `beeper_toggle` (`Q`, переключает звук) |
| 6.2.6.0 | upsAdvControlTurnOnUPS | 2 → `shutdown_cancel` (`C`) |
| 7.2.2.0 | upsAdvTestDiagnostics | 2 → `test_short` (`T`) |
| 7.2.5.0 | upsAdvTestRuntimeCalibration | 2 → `test_deep` (`TL`); 3 → `test_cancel` (`CT`) |
| 5.2.10.0 | upsAdvConfigShutoffDelay | TimeTicks 6000 (60 с — задержка graceful-отключения), только чтение |
| 5.2.9.0 | upsAdvConfigReturnDelay | TimeTicks 18000 (180 с — задержка включения), только чтение |

Что делает ИБП после `S01` и `SnnRmmmm` при отсутствии и возврате сети
(остаётся ли выключенным, отсчитывает ли `mmmm` от отключения или от возврата
сети), на Sinus EVO не проверено. В частности, NUT `blazer`/`nutdrv_qx` для
`shutdown.stayoff` по RS-232 отправляет `SnnR0000`, а шлюз на
`upsAdvControlUpsOff = 3` — `S01`; что ИБП останется выключенным после
возврата сети, не гарантируется. Проверьте на своём ИБП до использования в
сценариях автоматического отключения.

GET объекта управления возвращает INTEGER 1 («нет действия», как требует
PowerNet-MIB), но объект виден (с любой community) только если задана Write
Community, включены нужные разрешения и драйвер поддерживает команду. Иначе GET получает
`noSuchInstance`, а walk его пропускает. NUT `snmp-ups` (`mibs=apc`) объявляет
команду только при успешном GET, поэтому `upscmd -l` показывает ровно
доступные команды:

| NUT | Объект и значение |
| --- | --- |
| `load.off` | upsAdvControlUpsOff = 2 |
| `shutdown.stayoff` | upsAdvControlUpsOff = 3 |
| `shutdown.return` | upsBasicControlConserveBattery = 2 |
| `load.on` | upsAdvControlTurnOnUPS = 2 |
| `test.panel.start` | upsAdvControlFlashAndBeep = 2 |
| `test.battery.start` | upsAdvTestDiagnostics = 2 |
| `calibrate.start` / `calibrate.stop` | upsAdvTestRuntimeCalibration = 2 / 3 |

Ответы SET (SNMPv2c error-status):

| Ситуация | Ответ |
| --- | --- |
| Значение 1 | `noError`, команда не отправляется |
| Команда принята в очередь | `noError`. Это только приём в очередь: если потом отправка по RS-232 не удастся (статус устарел, запись в UART неполная), SET об этом уже не сообщит — ошибка попадёт в журнал событий и `last_state` на странице Control. Выполнение ИБП проверяйте по следующему Q1 |
| Повтор байт-в-байт последнего SET с того же адреса и порта в течение 10 с | Тот же ответ из кэша; команда повторно не ставится. Кэш на один запрос: если между исходным SET и повтором пришёл другой непустой SET с Write Community (от любого менеджера), повтор выполняется заново |
| Пустой список varbind | `noError` |
| Read Community, неизвестный или только читаемый OID | `notWritable` (17) |
| Не INTEGER (у записываемого объекта) | `wrongType` (7) |
| Другое значение; ConserveBattery не от батареи | `wrongValue` (10) |
| Больше одного действия в запросе | `inconsistentValue` (12) на втором, ничего не отправляется |
| Управление выключено, команда не поддерживается, статус Q1 устарел, ИБП занят другой командой | `inconsistentValue` (12) |

Ошибка возвращается для первого по порядку неверного varbind. Запрос
проверяется целиком до постановки команды в очередь.

SNMPv2c передаёт community открытым текстом. Write Community не может
совпадать ни с Read Community, ни с community включённого получателя traps
(иначе trap раскрыл бы её получателю). Ограничьте SET через
`snmp_manager_ip` и держите шлюз в доверенной сети управления. Задержки 12 с, 1 мин и
3 мин выбраны так, чтобы точно кодироваться в MegaTec без округления;
настраиваемых задержек по SNMP нет. Карта KSTAR на «reboot» отправляет
`S.2R0000`; шлюз не использует `R0000`, потому что смысл нуля не установлен.

## Уведомления

Только SNMPv2c TRAP. INFORM, SNMPv1 и SNMPv3 не реализованы в версии 0.2.0.
В каждой trap первые varbind — sysUpTime.0 и snmpTrapOID.0, затем
`mtrapargsString.0` (`1.3.6.1.4.1.318.2.3.3.0`) и sysName.0.

| trap OID | Событие |
| --- | --- |
| 1.3.6.1.4.1.318.0.1 | communicationLost |
| 1.3.6.1.4.1.318.0.8 | communicationEstablished |
| 1.3.6.1.4.1.318.0.5 | upsOnBattery |
| 1.3.6.1.4.1.318.0.9 | powerRestored |
| 1.3.6.1.4.1.318.0.7 | lowBattery |
| 1.3.6.1.4.1.318.0.11 | returnFromLowBattery |
| 1.3.6.1.6.3.1.1.5.1 | coldStart: запуск шлюза или явно помеченная проверка канала |

Два получателя, у каждого IPv4/порт/community. Очередь на 16 событий.
При отсутствии Ethernet события ждут не более 30 секунд до возобновления
отправки; более старые записи не отправляются как новые. Кольцевой журнал
на 48 записей остаётся в RAM до перезапуска. `sent` означает успешную
передачу UDP-пакета локальному стеку, а не подтверждение приёма.

Общая неисправность Q1, начало/окончание теста и процедура shutdown
попадают в локальный журнал. Они не подменяются чужими APC-событиями
перегрузки, замены батареи или результата самотеста, которых Q1 не сообщает.

