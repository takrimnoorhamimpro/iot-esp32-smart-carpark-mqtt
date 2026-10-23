# Smart Carpark (v2: real gate flow)

Two ESP32 boards, two web pages.

| Folder | What it is |
| --- | --- |
| `bays/` | Bay controller (the brain): 4 bay sensors, LEDs, LCD, bookings, code check |
| `gate/` | Gate controller: 4x4 keypad, entry/exit sensors, barrier servo, buzzer, LCD |
| `web/index.html` | Simulator: drives the cars, sets sensor distances |
| `web/app.html` | Driver app: book a bay, get a 4-digit code |

