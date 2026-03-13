/**
 * @file rtthread_wrapper.c
 * @brief RT-Thread API Wrapper Implementation
 *
 * Maps RT-Thread API calls to FreeRTOS on ESP32-S3.
 */

#include "rtthread_wrapper.h"
#include <stdlib.h>
#include <string.h>

/* ==================== Thread API ==================== */

/**
 * FreeRTOS task entry wrapper: calls RT-Thread style entry then cleans up.
 */
static void thread_entry_wrapper(void *param)
{
    rt_thread_t thread = (rt_thread_t)param;
    if (thread && thread->entry) {
        thread->entry(thread->parameter);
    }
    vTaskDelete(NULL);
}

rt_thread_t rt_thread_create(const char *name,
                             void (*entry)(void *parameter),
                             void *parameter,
                             rt_uint32_t stack_size,
                             rt_uint8_t priority,
                             rt_uint32_t tick)
{
    (void)tick; /* RT-Thread time slice, not used in FreeRTOS */

    rt_thread_t thread = malloc(sizeof(struct rt_thread));
    if (!thread) return NULL;

    thread->name = name;
    thread->entry = entry;
    thread->parameter = parameter;
    thread->stack_size = stack_size;
    thread->priority = priority;
    thread->task_handle = NULL;

    return thread;
}

rt_err_t rt_thread_startup(rt_thread_t thread)
{
    if (!thread) return RT_ERROR;

    /* FreeRTOS: higher number = higher priority (opposite of RT-Thread)
     * RT-Thread: lower number = higher priority
     * Map: FreeRTOS priority = (configMAX_PRIORITIES - 1) - rt_priority */
    UBaseType_t freertos_prio = thread->priority;
    if (freertos_prio >= configMAX_PRIORITIES) {
        freertos_prio = configMAX_PRIORITIES - 1;
    }

    BaseType_t ret = xTaskCreate(thread_entry_wrapper,
                                 thread->name,
                                 thread->stack_size / sizeof(StackType_t),
                                 thread,
                                 freertos_prio,
                                 &thread->task_handle);

    return (ret == pdPASS) ? RT_EOK : RT_ENOMEM;
}

rt_err_t rt_thread_delete(rt_thread_t thread)
{
    if (!thread) return RT_ERROR;

    if (thread->task_handle) {
        vTaskDelete(thread->task_handle);
    }
    free(thread);
    return RT_EOK;
}

void rt_thread_mdelay(rt_int32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ==================== Event API ==================== */

rt_event_t rt_event_create(const char *name, rt_uint8_t flag)
{
    (void)flag; /* RT-Thread IPC flag, not applicable to FreeRTOS */

    rt_event_t event = malloc(sizeof(struct rt_event));
    if (!event) return NULL;

    event->name = name;
    event->event_group = xEventGroupCreate();
    if (!event->event_group) {
        free(event);
        return NULL;
    }

    return event;
}

rt_err_t rt_event_delete(rt_event_t event)
{
    if (!event) return RT_ERROR;

    if (event->event_group) {
        vEventGroupDelete(event->event_group);
    }
    free(event);
    return RT_EOK;
}

rt_err_t rt_event_send(rt_event_t event, rt_uint32_t set)
{
    if (!event || !event->event_group) return RT_ERROR;

    /* FreeRTOS EventGroup uses 24 bits max (bits 0-23) */
    xEventGroupSetBits(event->event_group, (EventBits_t)(set & 0x00FFFFFF));
    return RT_EOK;
}

rt_err_t rt_event_recv(rt_event_t event,
                       rt_uint32_t set,
                       rt_uint8_t option,
                       rt_int32_t timeout,
                       rt_uint32_t *recved)
{
    if (!event || !event->event_group) return RT_ERROR;

    BaseType_t wait_all = (option & RT_EVENT_FLAG_AND) ? pdTRUE : pdFALSE;
    BaseType_t clear = (option & RT_EVENT_FLAG_CLEAR) ? pdTRUE : pdFALSE;

    TickType_t ticks;
    if (timeout == RT_WAITING_FOREVER) {
        ticks = portMAX_DELAY;
    } else if (timeout == 0) {
        ticks = 0;
    } else {
        ticks = pdMS_TO_TICKS(timeout);
    }

    EventBits_t bits = xEventGroupWaitBits(event->event_group,
                                           (EventBits_t)(set & 0x00FFFFFF),
                                           clear,
                                           wait_all,
                                           ticks);

    if (recved) {
        *recved = (rt_uint32_t)bits;
    }

    /* Check if any requested bits were actually set */
    if (wait_all) {
        return ((bits & set) == set) ? RT_EOK : RT_ETIMEOUT;
    } else {
        return (bits & set) ? RT_EOK : RT_ETIMEOUT;
    }
}

/* ==================== Tick API ==================== */

rt_tick_t rt_tick_from_millisecond(rt_int32_t ms)
{
    return (rt_tick_t)pdMS_TO_TICKS(ms);
}
