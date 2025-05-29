#include <errno.h>
#include <printf.h>
#include <uacpi/event.h>
#include <uacpi/sleep.h>

int system_shutdown(void) {
    uacpi_status ret = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
    if (uacpi_unlikely_error(ret)) {
        k_printf("failed to prepare for sleep: %s",
                 uacpi_status_to_string(ret));
        return -EIO;
    }

    asm volatile("cli");

    ret = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    if (uacpi_unlikely_error(ret)) {
        k_printf("failed to enter sleep: %s", uacpi_status_to_string(ret));
        return -EIO;
    }

    return 0;
}

static uacpi_interrupt_ret handle_power_button(uacpi_handle ctx) {
    system_shutdown();
    return UACPI_INTERRUPT_HANDLED;
}

int power_button_init(void) {
    uacpi_status ret = uacpi_install_fixed_event_handler(
        UACPI_FIXED_EVENT_POWER_BUTTON, handle_power_button, UACPI_NULL);
    if (uacpi_unlikely_error(ret)) {
        k_printf("failed to install power button event callback: %s",
                 uacpi_status_to_string(ret));
        return -ENODEV;
    }

    return 0;
}
