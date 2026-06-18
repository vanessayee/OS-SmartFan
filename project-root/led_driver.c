/*
 * led_driver.c — CSC1107 Operating Systems, Project 10
 * GPIO LED Indicator — Linux Loadable Kernel Module (LKM)
 *
 * WHAT THIS MODULE DOES
 * ─────────────────────
 * Creates a character device at /dev/gpioled. User-space programs write "ON"
 * or "OFF" to control a physical LED, and read back the current LED state.
 *
 *   User space                   Kernel space              Hardware
 *   ─────────                    ────────────              ────────
 *   write(fd, "ON",  2)  ──►  dev_write() ──► gpiod_set_value(desc, 1) ──► LED ON
 *   write(fd, "OFF", 3)  ──►  dev_write() ──► gpiod_set_value(desc, 0) ──► LED OFF
 *   read (fd, buf,   8)  ──►  dev_read()  ──► returns "LED:ON\n" or "LED:OFF\n"
 *
 * GPIO API
 * ────────
 * Uses the modern gpiod (GPIO descriptor) API via gpio_to_desc() + gpiod_*
 * functions. This replaces the legacy gpio_request() / gpio_set_value() API
 * which returns -EPROBE_DEFER (-517) on kernel 6.x custom builds.
 *
 * SIMULATION MODE
 * ───────────────
 * If the gpiod API also cannot claim the pin, the module loads in simulation
 * mode: /dev/gpioled is fully functional for read/write but no physical pin
 * is driven. gpio_available tracks which mode is active.
 *
 * HARDWARE SETUP
 * ──────────────
 *   Raspberry Pi GPIO 24 (BCM, physical pin 18) ──[330 Ω]──  LED anode (+)
 *   Raspberry Pi GND     (physical pin 20)       ────────────  LED cathode (-)
 *
 * USAGE
 * ─────
 *   make                        (compile — produces led_driver.ko)
 *   sudo insmod led_driver.ko   (load module into the kernel)
 *   dmesg | tail -20            (verify init messages)
 *   sudo rmmod led_driver       (unload and release all resources)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/gpio/consumer.h>   /* gpiod API — gpio_to_desc, gpiod_* */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CSC1107 Group — Project 10");
MODULE_DESCRIPTION("GPIO LED indicator driver: ON/OFF via /dev/gpioled, "
                   "controlled by Sense HAT temperature threshold");
MODULE_VERSION("1.0");

/* ── Configuration ───────────────────────────────────────────────────────── */
#define DEVICE_NAME  "gpioled"    /* Creates /dev/gpioled                      */
#define CLASS_NAME   "gpio_led"   /* Sysfs class at /sys/class/gpio_led/       */
#define GPIO_LED_PIN  24          /* BCM GPIO 24 = physical header pin 18      */

/* ── Module-level state ──────────────────────────────────────────────────── */
static dev_t          dev_number;
static struct class  *led_class     = NULL;
static struct device *led_device    = NULL;
static struct cdev    led_cdev;

/* GPIO descriptor — replaces bare integer pin number used in legacy API */
static struct gpio_desc *led_gpio_desc = NULL;

/* Current LED state: 0 = off, 1 = on */
static int led_state = 0;

/*
 * gpio_available — 1 if gpiod successfully claimed the pin, 0 if simulation.
 * Checked before every gpio_set_value call to avoid null pointer dereference.
 */
static int gpio_available = 0;

/* ── Forward declarations ────────────────────────────────────────────────── */
static int     dev_open    (struct inode *inode, struct file *filp);
static int     dev_release (struct inode *inode, struct file *filp);
static ssize_t dev_read    (struct file *filp, char __user *buf,
                            size_t len, loff_t *offset);
static ssize_t dev_write   (struct file *filp, const char __user *buf,
                            size_t len, loff_t *offset);

/*
 * file_operations — maps system calls on /dev/gpioled to our handlers.
 */
static const struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = dev_open,
    .release = dev_release,
    .read    = dev_read,
    .write   = dev_write,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * DEVICE OPERATION HANDLERS
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * dev_open — called when user space opens /dev/gpioled.
 */
static int dev_open(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO "gpioled: device opened by user-space process\n");
    return 0;
}

/*
 * dev_release — called when user space closes the file descriptor.
 */
static int dev_release(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO "gpioled: device closed by user-space process\n");
    return 0;
}

/*
 * dev_read — returns current LED state to user space.
 *
 * Returns "LED:ON\n" or "LED:OFF\n" on the first read per open().
 * Returns 0 (EOF) on subsequent reads so `cat /dev/gpioled` terminates.
 */
static ssize_t dev_read(struct file *filp, char __user *buf,
                        size_t len, loff_t *offset)
{
    char response[16];
    int  response_len;

    if (*offset > 0)
        return 0;

    if (led_state)
        snprintf(response, sizeof(response), "LED:ON\n");
    else
        snprintf(response, sizeof(response), "LED:OFF\n");

    response_len = strlen(response);

    if (len < response_len) {
        printk(KERN_WARNING "gpioled: read() buffer too small (%zu < %d)\n",
               len, response_len);
        return -EINVAL;
    }

    if (copy_to_user(buf, response, response_len)) {
        printk(KERN_ERR "gpioled: copy_to_user failed in dev_read\n");
        return -EFAULT;
    }

    *offset += response_len;
    printk(KERN_INFO "gpioled: state sent to user space → %s", response);
    return response_len;
}

/*
 * dev_write — receives LED command from user space and drives GPIO.
 *
 * Accepted commands:
 *   "ON"  → gpiod_set_value(led_gpio_desc, 1) → LED on
 *   "OFF" → gpiod_set_value(led_gpio_desc, 0) → LED off
 *
 * Uses gpiod_set_value() (modern API) instead of gpio_set_value() (legacy).
 * If gpio_available is 0 (simulation mode), state is updated in memory only.
 */
static ssize_t dev_write(struct file *filp, const char __user *buf,
                         size_t len, loff_t *offset)
{
    char   command[8];
    size_t copy_len = min(len, sizeof(command) - 1);

    if (copy_from_user(command, buf, copy_len)) {
        printk(KERN_ERR "gpioled: copy_from_user failed in dev_write\n");
        return -EFAULT;
    }

    command[copy_len] = '\0';

    if (strncmp(command, "ON", 2) == 0) {
        if (gpio_available && led_gpio_desc)
            gpiod_set_value(led_gpio_desc, 1);   /* drive pin HIGH → LED on */
        led_state = 1;
        printk(KERN_INFO "gpioled: command 'ON'  received — GPIO %d %s\n",
               GPIO_LED_PIN, gpio_available ? "HIGH" : "(simulated)");

    } else if (strncmp(command, "OFF", 3) == 0) {
        if (gpio_available && led_gpio_desc)
            gpiod_set_value(led_gpio_desc, 0);   /* drive pin LOW  → LED off */
        led_state = 0;
        printk(KERN_INFO "gpioled: command 'OFF' received — GPIO %d %s\n",
               GPIO_LED_PIN, gpio_available ? "LOW"  : "(simulated)");

    } else {
        printk(KERN_WARNING "gpioled: unknown command '%s' "
               "(expected 'ON' or 'OFF')\n", command);
        return -EINVAL;
    }

    return len;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MODULE INIT AND EXIT
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * led_driver_init — runs on `sudo insmod led_driver.ko`.
 *
 * Steps 1–4: character device setup (fatal if they fail).
 * Step  5:   GPIO setup via modern gpiod API (non-fatal, falls back to
 *            simulation mode if the pin cannot be claimed).
 *
 * gpiod API sequence:
 *   gpio_to_desc()          — convert BCM number to GPIO descriptor
 *   gpiod_request()         — claim exclusive ownership of the pin
 *   gpiod_direction_output()— configure as output, set initial LOW value
 */
static int __init led_driver_init(void)
{
    int ret;

    printk(KERN_INFO "gpioled: ── loading module ──────────────────────────\n");

    /* Step 1: Allocate major + minor device number */
    ret = alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR "gpioled: Step 1 FAILED — alloc_chrdev_region (%d)\n", ret);
        return ret;
    }
    printk(KERN_INFO "gpioled: Step 1 OK — major number %d allocated\n",
           MAJOR(dev_number));

    /* Step 2: Initialise character device and register file operations */
    cdev_init(&led_cdev, &fops);
    led_cdev.owner = THIS_MODULE;
    ret = cdev_add(&led_cdev, dev_number, 1);
    if (ret < 0) {
        printk(KERN_ERR "gpioled: Step 2 FAILED — cdev_add (%d)\n", ret);
        goto err_unreg;
    }
    printk(KERN_INFO "gpioled: Step 2 OK — character device registered\n");

    /* Step 3: Create device class at /sys/class/gpio_led/ */
    led_class = class_create(CLASS_NAME);
    if (IS_ERR(led_class)) {
        ret = PTR_ERR(led_class);
        printk(KERN_ERR "gpioled: Step 3 FAILED — class_create (%d)\n", ret);
        goto err_cdev;
    }
    printk(KERN_INFO "gpioled: Step 3 OK — class '%s' created\n", CLASS_NAME);

    /* Step 4: Create /dev/gpioled device node via udev */
    led_device = device_create(led_class, NULL, dev_number, NULL, DEVICE_NAME);
    if (IS_ERR(led_device)) {
        ret = PTR_ERR(led_device);
        printk(KERN_ERR "gpioled: Step 4 FAILED — device_create (%d)\n", ret);
        goto err_class;
    }
    printk(KERN_INFO "gpioled: Step 4 OK — /dev/%s created\n", DEVICE_NAME);

    /* Step 5: Claim GPIO pin using the modern gpiod descriptor API.
     *
     * This replaces the legacy gpio_request() which returns -EPROBE_DEFER
     * (-517) on kernel 6.x custom builds. The gpiod API accesses the GPIO
     * through the pinctrl subsystem correctly.
     *
     * gpio_to_desc() — converts BCM GPIO number to a descriptor pointer.
     *                  Does not claim the pin, just gets a reference.
     * gpiod_request() — claims exclusive ownership (equivalent to old
     *                   gpio_request but works with the descriptor).
     * gpiod_direction_output() — sets pin as output and writes initial LOW.
     */
    led_gpio_desc = gpio_to_desc(GPIO_LED_PIN);

    if (!led_gpio_desc) {
        printk(KERN_WARNING "gpioled: Step 5 WARN — gpio_to_desc(GPIO %d) "
               "returned NULL — running in simulation mode\n", GPIO_LED_PIN);
        gpio_available = 0;
    } else {
        ret = gpiod_request(led_gpio_desc, "led_gpio");
        if (ret < 0) {
            printk(KERN_WARNING "gpioled: Step 5 WARN — gpiod_request "
                   "returned %d — running in simulation mode\n", ret);
            led_gpio_desc = NULL;
            gpio_available = 0;
        } else {
            ret = gpiod_direction_output(led_gpio_desc, 0);
            if (ret < 0) {
                printk(KERN_WARNING "gpioled: Step 5 WARN — "
                       "gpiod_direction_output returned %d — simulation mode\n",
                       ret);
                gpiod_free(led_gpio_desc);
                led_gpio_desc = NULL;
                gpio_available = 0;
            } else {
                gpio_available = 1;
                printk(KERN_INFO "gpioled: Step 5 OK — GPIO %d claimed "
                       "via gpiod API\n", GPIO_LED_PIN);
                printk(KERN_INFO "gpioled: Step 6 OK — GPIO %d set as "
                       "output (LOW)\n", GPIO_LED_PIN);
            }
        }
    }

    if (gpio_available)
        printk(KERN_INFO "gpioled: ── module ready. /dev/%s open for "
               "commands (GPIO mode — LED active). ──\n", DEVICE_NAME);
    else
        printk(KERN_INFO "gpioled: ── module ready. /dev/%s open for "
               "commands (simulation mode — no physical LED). ──\n",
               DEVICE_NAME);

    return 0;

err_class:
    class_destroy(led_class);
err_cdev:
    cdev_del(&led_cdev);
err_unreg:
    unregister_chrdev_region(dev_number, 1);
    return ret;
}

/*
 * led_driver_exit — runs on `sudo rmmod led_driver`.
 *
 * Releases every resource in reverse order.
 * GPIO cleanup guarded by gpio_available to avoid freeing an unclaimed pin.
 */
static void __exit led_driver_exit(void)
{
    printk(KERN_INFO "gpioled: ── unloading module ─────────────────────────\n");

    if (gpio_available && led_gpio_desc) {
        gpiod_set_value(led_gpio_desc, 0);   /* drive LOW before releasing */
        gpiod_free(led_gpio_desc);
        led_gpio_desc = NULL;
        printk(KERN_INFO "gpioled: GPIO %d driven LOW and released\n",
               GPIO_LED_PIN);
    }

    device_destroy(led_class, dev_number);
    printk(KERN_INFO "gpioled: /dev/%s removed\n", DEVICE_NAME);

    class_destroy(led_class);
    printk(KERN_INFO "gpioled: class '%s' destroyed\n", CLASS_NAME);

    cdev_del(&led_cdev);
    unregister_chrdev_region(dev_number, 1);
    printk(KERN_INFO "gpioled: ── module unloaded cleanly ──────────────────\n");
}

module_init(led_driver_init);
module_exit(led_driver_exit);