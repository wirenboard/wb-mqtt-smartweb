# wb-mqtt-smartweb
MQTT to SamrtWeb gateway which follows [Wiren Board MQTT Conventions](https://github.com/wirenboard/homeui/blob/master/conventions.md).
It's designed to be used on [Wiren Board](https://wirenboard.com/en) family of programmable automation controllers (PACs).

Шлюз предназначен для трансляции сообщений между MQTT брокером и системами с поддержкой протокола [SmartWeb](http://www.smartweb.su).
Шлюз предназначен для устройств [Wiren Board](https://wirenboard.com/ru) и соответствует [Конвенции Wiren Board MQTT](https://github.com/wirenboard/homeui/blob/master/conventions.md).

Запускается командой `systemctl start wb-mqtt-smartweb` или `service wb-mqtt-smartweb start`

Шлюз транслирует данные датчиков, доступных в MQTT на Wiren Board, в виде сенсоров виртуального контроллера SmartWeb X1 на CAN-шине. Также шлюз осуществляет опрос программ на шине SmartWeb и создаёт для них контролы в MQTT брокере.

Возможен запуск шлюза вручную, что может быть полезно для работы в отладочном режиме:
```
# service wb-mqtt-smartweb stop
# wb-mqtt-smartweb -d 3
```

<div style="page-break-after: always;"></div>

### Настройка CAN

Переключить порт RS-485/CAN в режим CAN на вкладке Hardware Modules Configuration.

![Включение интерфейса CAN](doc/can_enable.png)

Настроить в веб-интерфейсе (Settings - Configs - Hardware interfaces configuration) параметры CAN.

![Настройка интерфейса CAN](doc/can_setup.png)

Либо вручную добавить в файл `/etc/network/interfaces` следующие строки:

```
auto can0
iface can0 inet manual
pre-up ip link set can0 type can bitrate 20000
up ifconfig can0 up
```

### Структура конфигурационного файла

```javascript
{
  // Включает/выключает выдачу отладочной информации во время работы шлюза
  "debug" : false,

  // Интевал опроса параметров программ в сети SmartWeb, мс
  "poll_interval_ms": 1000,

  // Идентификатор виртуального контроллера, от имени которого шлюз транслирует данные из MQTT
  "controller_id": 204,

  // Список контролов MQTT и соответствующих им параметров виртуального контроллера SmartWeb
  "mappings": [
    {
      "mqtt": {
        // Имя устройства и канала в терминах MQTT конвенции,
        // разделённые символом "/". В конкретном случае,
        // соответствующая MQTT-тема /devices/wb-adc/controls/Vin
        "channel": "wb-adc/Vin"
      },

      // Соответствующий контролу параметр виртуального контроллера SmartWeb
      "parameter": {
        // Идентификатор параметра согласно протоколу SmartWeb
        "parameter_id": 1,
        // Тип программы, предоставляющей этот параметр
        "program_type": 11,
        // Индекс параметра
        "parameter_index": 0
      }
    },
    {
      "mqtt": {
        "channel": "wb-adc/R1",

        // Время в минутах, по истечение которого, если не обновилось значение канала, 
        // соответствующим параметрам SmartWeb будет присвоен признак ошибки
        "value_timeout_min": 60
      },

      // Соответствующий контролу датчик виртуального контроллера SmartWeb
      "sensor": {
        // Индекс датчика
        "index": 1
      },

      // Соответствующий контролу выход виртуального контроллера SmartWeb
      "output": {
        // Индекс выхода
        "channel_id": 0
      }
    }
  ]
}
```

<div style="page-break-after: always;"></div>

### Опрос программ SmartWeb

Шлюз автоматически определяет доступные программы в сети SmartWeb и создаёт для их датчиков, входов и параметров MQTT контролы. Типы программ должны быть описаны в отдельных json файлах. [Схема структуры файлов](wb-mqtt-smartweb-class.schema.json). Файлы с описанием типов программ сохраняются в каталоге `/etc/wb-mqtt-smartweb.conf.d/classes`.
