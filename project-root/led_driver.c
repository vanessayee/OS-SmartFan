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
 *   write(fd, "ON",  2)  ──►  dev_write() ──► gpio24_set(1) ──► LED ON
 *   write(fd, "OFF", 3)  ──►  dev_write() ──► gpio24_set(0) ──► LED OFF
 *   read (fd, buf,   8)  ──►  dev_read()  ──► returns "LED:ON\n" or "LED:OFF\n"
 *
 * GPIO APPROACH — DIRECT REGISTER ACCESS
 * ───────────────────────────────────────
 * Both the legacy gpio_request() API and the modern gpiod_request() API are
 * unavailable on this custom kernel (6.18 CSC1107_CUSTOM_KERNEL). Instead
 * we use ioremap() to map the BCM2711 GPIO peripheral registers directly into
 * kernel virtual address space, then use readl()/writel() to control the pin.
 *
 * This is the same approach used internally by RPi.GPIO (Python) and pigpio.
 * It works on any kernel version because it talks directly to hardware.
 *
 * BCM2711 (Raspberry Pi 4) GPIO register map:
 *   Physical base : 0xFE200000
 *   GPFSEL2       : +0x08  — function select for pins 20–29
 *   GPSET0        : +0x1C  — write 1 to a bit to drive that pin HIGH
 *   GPCLR0        : +0x28  — write 1 to a bit to drive that pin LOW
 *
 * GPIO 24 sits in GPFSEL2, bits 12–14 (each pin uses 3 bits):
 *   000 = input, 001 = output
 *
 * SIMULATION MODE
 * ───────────────
 * If ioremap() fails (e.g. memory protection), the module loads in simulation
 * mode: /dev/gpioled works for read/write but no physical pin is driven.
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
#include <linux/io.h>       /* ioremap, iounmap, readl, writel */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CSC1107 Group — Project 10");
MODULE_DESCRIPTION("GPIO LED indicator driver using direct BCM2711 register "
                   "access via ioremap — controlled by temperature threshold");
MODULE_VERSION("1.0");

/* ── Configuration ───────────────────────────────────────────────────────── */
#define DEVICE_NAME  "gpioled"          /* Creates /dev/gpioled               */
#define CLASS_NAME   "gpio_led"         /* /sys/class/gpio_led/               */
#define GPIO_LED_PIN  24                /* BCM GPIO 24 = physical pin 18      */

/*
 * BCM2711 (Raspberry Pi 4) GPIO peripheral registers
 * Physical base address and register offsets from the BCM2711 datasheet.
 */
#define BCM2711_GPIO_BASE   0xFE200000UL    /* GPIO peripheral physical base  */
#define GPIO_REG_SIZE       0x000000B4UL    /* Size of GPIO register block    */

#define GPFSEL2_OFFSET  0x08    /* Function select register 2 (pins 20–29)   */
#define GPSET0_OFFSET   0x1C    /* Output set register (pins 0–31)           */
#define GPCLR0_OFFSET   0x28    /* Output clear register (pins 0–31)         */

/*
 * GPIO 24 function select bits within GPFSEL2.
 * Each pin uses 3 consecutive bits. Pin 24 offset within FSEL2: (24-20)*3=12
 *   000 = input
 *   001 = output  ← we want this
 */
#define GPIO24_FSEL_SHIFT   12
#define GPIO24_FSEL_MASK    (0x7U << GPIO24_FSEL_SHIFT)
#define GPIO24_FSEL_OUTPUT  (0x1U << GPIO24_FSEL_SHIFT)

/* ── Module-level state ──────────────────────────────────────────────────── */
static dev_t          dev_number;
static struct class  *led_class   = NULL;
static struct device *led_device  = NULL;
static struct cdev    led_cdev;

/* Pointer to the ioremap'd GPIO register block */
static void __iomem  *gpio_regs   = NULL;

/* Current LED state: 0 = off, 1 = on */
static int led_state = 0;

/*
 * gpio_available — 1 if ioremap succeeded and GPIO is ready to use.
 * 0 means simulation mode: driver works but no physical pin is driven.
 */
static int gpio_available = 0;

/* ── GPIO helper ─────────────────────────────────────────────────────────── */

/*
 * gpio24_set — drive GPIO 24 HIGH (1) or LOW (0) via direct register write.
 *
 * GPSET0: writing a 1 to bit N drives pin N HIGH (other bits unaffected).
 * GPCLR0: writing a 1 to bit N drives pin N LOW  (other bits unaffected).
 *
 * This never reads GPSET0/GPCLR0 — they are write-only registers.
 */
static void gpio24_set(int value)
{
    if (!gpio_regs)
        return;

    if (value)
        writel(1U << GPIO_LED_PIN, gpio_regs + GPSET0_OFFSET);
    else
        writel(1U << GPIO_LED_PIN, gpio_regs + GPCLR0_OFFSET);
}

/* ── Forward declarations ────────────────────────────────────────────────── */
static int     dev_open    (struct inode *inode, struct file *filp);
static int     dev_release (struct inode *inode, struct file *filp);
static ssize_t dev_read    (struct file *filp, char __user *buf,
                            size_t len, loff_t *offset);
static ssize_t dev_write   (struct file *filp, const char __user *buf,
                            size_t len, loff_t *offset);

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

static int dev_open(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO "gpioled: device opened by user-space process\n");
    return 0;
}

static int dev_release(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO "gpioled: device closed by user-space process\n");
    return 0;
}

/*
 * dev_read — returns current LED state to user space.
 * Returns "LED:ON\n" or "LED:OFF\n" once per open(), then EOF.
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
 * dev_write — receives "ON" or "OFF" from user space and drives GPIO 24.
 *
 * Uses gpio24_set() which writes directly to the BCM2711 GPSET0/GPCLR0
 * registers via the ioremap'd pointer. No kernel GPIO subsystem involved.
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
        gpio24_set(1);
        led_state = 1;
        printk(KERN_INFO "gpioled: command 'ON'  received — GPIO %d %s\n",
               GPIO_LED_PIN, gpio_available ? "HIGH" : "(simulated)");

    } else if (strncmp(command, "OFF", 3) == 0) {
        gpio24_set(0);
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
 * Steps 1–4: character device setup (fatal on failure).
 * Step  5:   GPIO setup via direct BCM2711 register access (non-fatal,
 *            falls back to simulation mode if ioremap fails).
 *
 * Direct register access steps for GPIO 24:
 *   ioremap()  — map physical GPIO registers into kernel virtual address space
 *   readl()    — read GPFSEL2 to get current function settings
 *   writel()   — write GPFSEL2 with bits 12–14 set to 001 (output)
 *   writel()   — write GPCLR0 bit 24 to drive pin LOW (LED off at startup)
 */
static int __init led_driver_init(void)
{
    int ret;
    u32 fsel;

    printk(KERN_INFO "gpioled: ── loading module ──────────────────────────\n");
    printk(KERN_INFO "gpioled: GPIO method: direct BCM2711 register access "
           "(ioremap @ 0x%08lX)\n", BCM2711_GPIO_BASE);

    /* Step 1: Allocate major + minor device number */
    ret = alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR "gpioled: Step 1 FAILED — alloc_chrdev_region (%d)\n",
               ret);
        return ret;
    }
    printk(KERN_INFO "gpioled: Step 1 OK — major number %d allocated\n",
           MAJOR(dev_number));

    /* Step 2: Initialise character device */
    cdev_init(&led_cdev, &fops);
    led_cdev.owner = THIS_MODULE;
    ret = cdev_add(&led_cdev, dev_number, 1);
    if (ret < 0) {
        printk(KERN_ERR "gpioled: Step 2 FAILED — cdev_add (%d)\n", ret);
        goto err_unreg;
    }
    printk(KERN_INFO "gpioled: Step 2 OK — character device registered\n");

    /* Step 3: Create device class */
    led_class = class_create(CLASS_NAME);
    if (IS_ERR(led_class)) {
        ret = PTR_ERR(led_class);
        printk(KERN_ERR "gpioled: Step 3 FAILED — class_create (%d)\n", ret);
        goto err_cdev;
    }
    printk(KERN_INFO "gpioled: Step 3 OK — class '%s' created\n", CLASS_NAME);

    /* Step 4: Create /dev/gpioled device node */
    led_device = device_create(led_class, NULL, dev_number, NULL, DEVICE_NAME);
    if (IS_ERR(led_device)) {
        ret = PTR_ERR(led_device);
        printk(KERN_ERR "gpioled: Step 4 FAILED — device_create (%d)\n", ret);
        goto err_class;
    }
    printk(KERN_INFO "gpioled: Step 4 OK — /dev/%s created\n", DEVICE_NAME);

    /* Step 5: Map BCM2711 GPIO registers into kernel virtual address space.
     *
     * ioremap() requests the kernel's MMU to create a virtual → physical
     * mapping for the GPIO peripheral registers at BCM2711_GPIO_BASE.
     * After this call, gpio_regs is a pointer we can read/write safely.
     *
     * We then configure GPIO 24 as an output by setting its 3-bit function
     * field in GPFSEL2 to 001 (output), then drive it LOW (LED off).
     */
    gpio_regs = ioremap(BCM2711_GPIO_BASE, GPIO_REG_SIZE);
    if (!gpio_regs) {
        printk(KERN_WARNING "gpioled: Step 5 WARN — ioremap(0x%08lX) failed "
               "— running in simulation mode\n", BCM2711_GPIO_BASE);
        gpio_available = 0;
    } else {
        /* Read current GPFSEL2 value, clear GPIO 24 bits, set as output */
        fsel  = readl(gpio_regs + GPFSEL2_OFFSET);
        fsel &= ~GPIO24_FSEL_MASK;      /* clear bits 12–14                   */
        fsel |=  GPIO24_FSEL_OUTPUT;    /* set bits 12–14 to 001 (output)     */
        writel(fsel, gpio_regs + GPFSEL2_OFFSET);

        /* Drive GPIO 24 LOW — LED starts off */
        writel(1U << GPIO_LED_PIN, gpio_regs + GPCLR0_OFFSET);

        gpio_available = 1;
        printk(KERN_INFO "gpioled: Step 5 OK — GPIO %d mapped at "
               "virtual addr %p, configured as output (LOW)\n",
               GPIO_LED_PIN, gpio_regs);
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
 * Drives LED LOW, unmaps the GPIO registers, then tears down the
 * character device in reverse order of init.
 */
static void __exit led_driver_exit(void)
{
    printk(KERN_INFO "gpioled: ── unloading module ─────────────────────────\n");

    if (gpio_available && gpio_regs) {
        /* Drive LOW before releasing — leave LED off */
        writel(1U << GPIO_LED_PIN, gpio_regs + GPCLR0_OFFSET);
        printk(KERN_INFO "gpioled: GPIO %d driven LOW\n", GPIO_LED_PIN);

        /* Unmap the GPIO registers from kernel virtual address space */
        iounmap(gpio_regs);
        gpio_regs = NULL;
        printk(KERN_INFO "gpioled: GPIO registers unmapped\n");
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