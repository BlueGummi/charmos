#include <thread/workqueue.h>
#include <time/timer.h>

static void delayed_work_timer_cb(struct timer *t) {
    struct delayed_work *dwork = t->data;
    if (dwork->wq)
        workqueue_enqueue(dwork->wq, &dwork->work);
    else
        (void) workqueue_add(&dwork->work);
}

void delayed_work_init(struct delayed_work *dwork, work_function fn,
                       struct work_args args) {
    work_init(&dwork->work, fn, args);
    dwork->wq = NULL;
    timer_init(&dwork->timer, delayed_work_timer_cb, dwork);
}

bool delayed_work_schedule_on(struct workqueue *wq, struct delayed_work *dwork,
                              time_ms_t delay_ms) {
    dwork->wq = wq;
    if (delay_ms == 0) {
        if (wq)
            return workqueue_enqueue(wq, &dwork->work) == WORKQUEUE_ERROR_OK;
        return workqueue_add(&dwork->work) == WORKQUEUE_ERROR_OK;
    }

    return timer_modify(&dwork->timer, timer_delta_us(MS_TO_US(delay_ms)));
}

bool delayed_work_schedule(struct delayed_work *dwork, time_ms_t delay_ms) {
    return delayed_work_schedule_on(NULL, dwork, delay_ms);
}

bool delayed_work_cancel(struct delayed_work *dwork) {
    return timer_delete(&dwork->timer);
}

bool delayed_work_cancel_sync(struct delayed_work *dwork) {
    return timer_delete_sync(&dwork->timer);
}
