# OS-SmartFan — PowerPoint Slide Content

---

## Slide 1: Kernel Module

**Title:** Linux Kernel Module — `/dev/gpioled`

**What it does:**
- Creates a character device at `/dev/gpioled` that user-space programs interact with via standard `read()` and `write()` system calls
- Controls GPIO 24 (physical pin 18) to turn the fan/LED on and off
- Directly manipulates BCM2711 hardware registers using `ioremap()` — required because standard GPIO APIs were unavailable on the custom kernel build

**Key design points:**
- `dev_write()` — receives `"ON"` or `"OFF"` from user space, drives GPIO 24 HIGH/LOW by writing to `GPSET0`/`GPCLR0` registers
- `dev_read()` — returns `"LED:ON\n"` or `"LED:OFF\n"` so user space can verify the actual hardware state
- **Simulation mode** — if `ioremap()` fails, the module still loads and the device node works; no hardware is driven but the interface remains fully functional for testing

**GPIO register map (BCM2711, base: `0xFE200000`):**

| Register | Offset | Purpose            |
|----------|--------|--------------------|
| GPFSEL2  | +0x08  | Set GPIO 24 as output |
| GPSET0   | +0x1C  | Drive pin HIGH (ON) |
| GPCLR0   | +0x28  | Drive pin LOW (OFF) |

---

## Slide 2: Sense HAT + Python Server

**Title:** Sense HAT Sensor Layer & Flask REST API

**`led.py` — sensor + device bridge:**
- Polls the Sense HAT temperature sensor every **2 seconds** in a background daemon thread
- Applies a **CPU heat correction** because the sensor sits near the Pi's processor and reads 5–15 °C too high:
  > `corrected = raw − (cpu_temp − raw) / 5.4`
  > CPU temperature sourced from `/sys/class/thermal/thermal_zone0/temp`
- Writes corrected temperature to `/tmp/sense_temp` (shared with the C controller)
- **Auto mode:** if temperature exceeds **29 °C**, writes `"ON"` to `/dev/gpioled`; drops below → writes `"OFF"`
- Always reads LED state **directly from the kernel device** (not an in-memory copy) to prevent stale state bugs

**`led_server.py` — REST API (port 5000):**

| Endpoint       | Method | Action                                    |
|----------------|--------|-------------------------------------------|
| `/api/status`  | GET    | Returns current temperature + LED state   |
| `/api/led/on`  | POST   | Turns LED on, disables auto mode          |
| `/api/led/off` | POST   | Turns LED off, disables auto mode         |
| `/api/led/auto`| POST   | Enables/disables automatic temperature control |

- Manual `on`/`off` commands automatically disable auto mode to prevent the monitor thread from immediately overriding them

---

## Slide 3: Bash Script + Integration

**Title:** Automation Script & Full System Integration

**`led_controller.sh` — one-command project launcher:**

The script automates the entire setup sequence in order:

1. **Compile kernel module** — `make clean && make` → produces `led_driver.ko`
2. **Load module** — `sudo insmod led_driver.ko`, fix permissions (`chmod 666 /dev/gpioled`), verify with `lsmod` and `dmesg`
3. **Compile C controller** — `gcc -Wall -o led_control led_control.c`
4. **Demonstrate system calls** — directly echoes `"ON"` / `"OFF"` to `/dev/gpioled` and reads back the kernel response, showing user-space ↔ kernel communication working
5. **Launch controller** — runs `sudo ./led_control` continuously until Ctrl-C
6. **Cleanup trap** — on any exit, `rmmod led_driver` safely unloads the module and prints final `dmesg` to confirm clean teardown

**Full data flow:**

```
Sense HAT ──► led.py ──► /tmp/sense_temp ──► led_control.c
                │                                    │
                └──► write() ──► /dev/gpioled ◄──────┘
                                      │
                               led_driver.ko
                                      │
                               GPIO 24 register ──► Fan/LED

led.py ──► led_server.py (Flask :5000) ──► Dashboard proxy (:8080) ──► Browser
```

- The C controller and Python server both control the same `/dev/gpioled` device — the kernel is the single source of truth for hardware state
