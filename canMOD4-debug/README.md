# canMOD4 / HT42B416 — репро bus-off (минимум)

Воспроизведение бага `ht42b416 canMOD4: TX timeout waiting for Z` → синтетический `bus-off` →
`Restart failed` на WBE2-I-CAN (чип Holtek HT42B416, слот mod4). Подтверждённые факты и разбор —
[`../canMOD4-busoff-investigation.md`](../canMOD4-busoff-investigation.md).

## Окружение
- Контроллер **WB8.5**, ядро `6.8.0-wb158`; слот **mod4** в норме = `wbe2-i-can`.
- Запускать **на контроллере, под root**. UART чипа = `/dev/ttyS5` (115200 8N1), чип = controller
  **204** (0xCC).
- Стенд **с подключёнными устройствами SmartWeb** на шине (конфигурации «пустая шина» у нас нет —
  вклад их трафика в баг не проверялся).

## Файлы
| Файл | Назначение |
|---|---|
| `canmod4_applike_repro.py` | **главный репро:** мастер как `wb-mqtt-smartweb` (запрос → ждать ответ → 500 мс → следующий) по сырому UART; ловит залип (нет `Z`), печатает весь трафик и пробует восстановление |
| `raw-uart-enable.sh` | переключить mod4 → `wbe2x-generic-uart` (через `/etc/wb-hardware.conf` + `wb-hwconf-helper config-apply`), проверить ответ `V` |
| `raw-uart-restore.sh` | вернуть оригинальный conf (CAN-драйвер, `canMOD4`) |
| `HT42B416-xv100.pdf` | даташит чипа (Rev. V1.00) |
| `log-soft-recoverable.txt` | пример вывода `canmod4_applike_repro.py`: залип на поллинге + восстановление (на шаге `C+O`), sustained 5/5 — класс «софт-восстановимый» |

> Лог класса **«только power-cycle»** пока не снят: дип-дед по требованию не вызывается (~12 мин флуда
> передачами — ~920k кадров — его не вызвали), снимем, когда случится естественно.

## Запуск
```sh
# на контроллере, под root, из этого каталога (желательно сперва reboot — оживляет чип)
./raw-uart-enable.sh                 # -> "OK: raw UART up ... VID 0x04D9 answers"
python3 canmod4_applike_repro.py     # снимает с шины реальный запрос и поллит как приложение
./raw-uart-restore.sh                # вернуть canMOD4 (CAN-драйвер)
systemctl enable --now wb-mqtt-smartweb   # вернуть приложение, если выключали
```
Скрипт: health-gate (чип должен ACK'ать `I_AM_HERE`) → снять с шины реальный `GET_PARAMETER_VALUE` →
поллить по протоколу → при залипе печать всего трафика с разбором каждого кадра (по
`../src/smart_web_conventions.h`) и эскалация восстановления (`RCY` → `C+O` → `RST`+реинит).

## Что наблюдать (через штатный драйвер)
`dmesg -w | grep ht42b416`; `ip -details -statistics link show canMOD4` — синтетический bus-off виден
как счётчик `bus-off`=0 при `carrier`=0 (state `ERROR-ACTIVE`). Восстановление: «софт-восстановимый»
залип лечит `RST`+реинит (драйверный restart); класс «только power-cycle» — лишь полный reboot (WBEC).

## Оговорки
- Видна только UART-сторона чипа, не шина CAN: «гонка передача ⊗ входящий кадр» — гипотеза H1
  (см. investigation).
- Переключение в raw иногда поднимается не с первого раза — reboot и повтор.
- После работы — `raw-uart-restore.sh` и при необходимости вернуть сервис в автозапуск.
