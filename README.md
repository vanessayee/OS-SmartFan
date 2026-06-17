# CSC1107 Operating Systems — Project 10
## GPIO Smart Fan Controller Driver (LED Variant)

## Hardware Required

- Raspberry Pi 4
- Sense HAT (Temperature and Humidity Sensor)
- LED sensor
- Breadboard

---

## Project Structure

```
project-root/
├── led_driver.c          # Linux kernel module (LKM)
├── Makefile              # Compiles led_driver.c → led_driver.ko
├── led_control.c         # User-space C program
├── led_controller.sh     # Bash automation script (runs everything)
├── README.md
│
├── Dashboard/            # Runs on LAPTOP
│   ├── app.py            # Dashboard server (port 8080)
│   ├── templates/
│   │   └── index.html
│   └── static/
│       ├── styles.css
│       └── app.js
│
├── PiServers/            # Runs on PI
│   └── led_server.py     # Flask REST API (port 5000)
│
└── Sensors/              # Runs on PI
    ├── led.py            # Sense HAT sensor + LED control logic
    └── led_controller.py # Controller class
```

---

## Setup

### Raspberry Pi

```bash
# 1. Install dependencies
sudo apt install -y build-essential sense-hat
pip3 install flask --break-system-packages

# 2. Compile kernel module
make clean && make

# 3. Load kernel module
sudo insmod led_driver.ko

# 4. Fix device permissions
sudo chmod 666 /dev/gpioled

# 5. Make permissions permanent (run once)
echo 'KERNEL=="gpioled", MODE="0666"' | sudo tee /etc/udev/rules.d/99-gpioled.rules
sudo udevadm control --reload-rules
```

### Laptop (Dashboard only)

```bash
cd Dashboard
pip3 install flask requests
```

---

## Running the Project

### Option A — Automated (recommended)
```bash
# Runs everything: compile → insmod → gcc → launch controller
sudo ./led_controller.sh
```

### Option B — Manual

**Terminal 1 (Pi) — load kernel module:**
```bash
sudo insmod led_driver.ko
sudo chmod 666 /dev/gpioled
```

**Terminal 2 (Pi) — start Flask API server:**
```bash
python3 PiServers/led_server.py
```

**Terminal 3 (Pi) — run LED controller:**
```bash
gcc -o led_control led_control.c
echo "30.5" > /tmp/sense_temp    # replaced by led.py when Sense HAT is running
sudo ./led_control
```

**Terminal 4 (Laptop) — start dashboard:**
```bash
cd Dashboard
python app.py
# Open http://localhost:8080 in browser
```

> **Note:** If Pi hostname is not `raspberrypi`, set the IP manually before running app.py:
> ```bash
> export PI_URL=http://<pi-ip-address>:5000
> python app.py
> ```

---

## Quick Test Commands

```bash
# Verify kernel module loaded
lsmod | grep led_driver

# Check kernel log
dmesg | tail -20

# Manually test LED via device node
echo "ON"  | sudo tee /dev/gpioled
echo "OFF" | sudo tee /dev/gpioled
sudo cat /dev/gpioled       # returns LED:ON or LED:OFF

# Test Flask API
curl http://localhost:5000/api/status
curl -X POST http://localhost:5000/api/led/on
curl -X POST http://localhost:5000/api/led/off
curl -X POST http://localhost:5000/api/led/auto \
  -H "Content-Type: application/json" -d '{"active": true}'

# Unload kernel module
sudo rmmod led_driver
dmesg | tail -10
```

---

## Team

| Name | Student ID | Role |
|---|---|---|
| Darrius John Chan Tiang Ser | 2500360 | Kernel module (led_driver.c, Makefile) |
| Hoon Chi Peng Shaun  | 2500629 | User-space C + LED hardware (led_control.c) |
| Liris Goh | [ID] | Sense HAT + Python server (led.py, led_server.py) |
| Vanessa Yee | [ID] | Bash script + integration (led_controller.sh) |
| Zechary  | [ID] | Report, slides, documentation |