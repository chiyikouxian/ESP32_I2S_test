/**
 * @file rtthread_wrapper.h
 * @brief RT-Thread API Wrapper over FreeRTOS
 *
 * Provides RT-Thread compatible API that maps to FreeRTOS calls
 * on ESP32-S3. Application code uses RT-Thread style APIs while
 * the underlying RTOS remains FreeRTOS (required by ESP-IDF).
 */

#ifndef __RTTHREAD_WRAPPER_H__
#define __RTTHREAD_WRAPPER_H__

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== RT-Thread Type Definitions ==================== */

typedef int32_t  rt_err_t;
typedef int32_t  rt_int32_t;
typedef uint32_t rt_uint32_t;
typedef uint8_t  rt_uint8_t;
typedef size_t   rt_size_t;
typedef int32_t  rt_tick_t;
typedef uint32_t rt_base_t;

/* RT-Thread error codes */
#define RT_EOK       0
#define RT_ERROR     (-1)
#define RT_ETIMEOUT  (-2)
#define RT_ENOMEM    (-3)
#define RT_ENOSYS    (-4)
#define RT_EBUSY     (-5)

/* RT-Thread waiting */
#define RT_WAITING_FOREVER  (-1)

/* ==================== Thread API ==================== */

/**
 * RT-Thread thread control block (wraps FreeRTOS TaskHandle_t)
 */
struct rt_thread {
    TaskHandle_t task_handle;
    const char  *name;
    void       (*entry)(void *parameter);
    void        *parameter;
    uint32_t     stack_size;
    uint8_t      priority;
};
typedef struct rt_thread *rt_thread_t;

/**
 * @brief Create a new thread (does not start it)
 */
rt_thread_t rt_thread_create(const char *name,
                             void (*entry)(void *parameter),
                             void *parameter,
                             rt_uint32_t stack_size,
                             rt_uint8_t priority,
                             rt_uint32_t tick);

/**
 * @brief Start a created thread
 */
rt_err_t rt_thread_startup(rt_thread_t thread);

/**
 * @brief Delete a thread
 */
rt_err_t rt_thread_delete(rt_thread_t thread);

/**
 * @brief Delay current thread (milliseconds)
 */
void rt_thread_mdelay(rt_int32_t ms);

/* ==================== Event API ==================== */

/* Event flags */
#define RT_EVENT_FLAG_OR   0x01
#define RT_EVENT_FLAG_AND  0x02
#define RT_EVENT_FLAG_CLEAR 0x04

/**
 * RT-Thread event object (wraps FreeRTOS EventGroupHandle_t)
 */
struct rt_event {
    EventGroupHandle_t event_group;
    const char *name;
};
typedef struct rt_event *rt_event_t;

/**
 * @brief Create a dynamic event object
 */
rt_event_t rt_event_create(const char *name, rt_uint8_t flag);

/**
 * @brief Delete an event object
 */
rt_err_t rt_event_delete(rt_event_t event);

/**
 * @brief Send event bits
 */
rt_err_t rt_event_send(rt_event_t event, rt_uint32_t set);

/**
 * @brief Receive (wait for) event bits
 *
 * @param event     Event object
 * @param set       Bits to wait for
 * @param option    RT_EVENT_FLAG_OR | RT_EVENT_FLAG_AND, optionally | RT_EVENT_FLAG_CLEAR
 * @param timeout   Timeout in ms, or RT_WAITING_FOREVER
 * @param recved    Pointer to store received bits (can be NULL)
 * @return RT_EOK on success, RT_ETIMEOUT on timeout
 */
rt_err_t rt_event_recv(rt_event_t event,
                       rt_uint32_t set,
                       rt_uint8_t option,
                       rt_int32_t timeout,
                       rt_uint32_t *recved);

/* ==================== Tick API ==================== */

/**
 * @brief Convert milliseconds to ticks
 */
rt_tick_t rt_tick_from_millisecond(rt_int32_t ms);

/* ==================== Console Output ==================== */

/**
 * @brief RT-Thread kernel printf (maps to printf)
 */
#define rt_kprintf(fmt, ...)  printf(fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __RTTHREAD_WRAPPER_H__ */
