#ifndef RAW_INPUT_H
#define RAW_INPUT_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <limits.h>
#include <errno.h>
#include <atomic>

// Global state
static std::atomic<int> current_pressure(0);
static std::atomic<int> max_pressure(4096); // Default, will be updated
static std::atomic<bool> is_running(false);
static pthread_t input_thread;
static int device_fd = -1;

// Helper macros for bit manipulation
#define BITS_PER_LONG (sizeof(long) * 8)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bit, array) ((array[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1)

// Helper to find the correct device by capabilities
inline char* find_tablet_device() {
    char path[64];
    char name[256] = "Unknown";
    unsigned long absbit[NLONGS(ABS_MAX)];
    struct input_absinfo absinfo;
    
    // Scan event0 to event32
    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        // Get device name
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
            strcpy(name, "Unknown");
        }

        // Check if device supports Absolute events
        memset(absbit, 0, sizeof(absbit));
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) >= 0) {
            // Check specifically for Pressure capability
            if (TEST_BIT(ABS_PRESSURE, absbit)) {
                // Get pressure range
                if (ioctl(fd, EVIOCGABS(ABS_PRESSURE), &absinfo) >= 0) {
                    max_pressure = absinfo.maximum;
                    printf("[RawInput] >>> FOUND PRESSURE DEVICE: %s (%s) Max Pressure: %d\n", path, name, absinfo.maximum);
                } else {
                    printf("[RawInput] >>> FOUND PRESSURE DEVICE: %s (%s) (Could not read max pressure, using default 4096)\n", path, name);
                }
                close(fd);
                return strdup(path);
            }
        }
        
        close(fd);
    }
    
    return NULL;
}

// The thread loop
inline void* input_thread_func(void* arg) {
    struct input_event ev;
    
    printf("[RawInput] Thread started. Reading from fd %d...\n", device_fd);

    while (is_running) {
        // Blocking read
        ssize_t bytes = read(device_fd, &ev, sizeof(ev));
        
        if (bytes < (ssize_t)sizeof(ev)) {
            if (bytes < 0 && errno != EINTR) {
                perror("[RawInput] Read error");
                break; // Exit on fatal error
            }
            continue;
        }

        if (ev.type == EV_ABS) {
            if (ev.code == ABS_PRESSURE) {
                current_pressure = ev.value;
                // printf("[RawInput] Pressure Update: %d\n", ev.value); // Debug enabled
            }
        }
    }
    
    printf("[RawInput] Thread stopping.\n");
    return NULL;
}

inline bool RawInput_Start(void) {
    if (is_running) return true;

    char* dev_path = find_tablet_device();
    if (!dev_path) {
        printf("[RawInput] No tablet device found in /dev/input/by-id/\n");
        return false;
    }

    device_fd = open(dev_path, O_RDONLY);
    if (device_fd < 0) {
        printf("[RawInput] Failed to open %s: %s\n", dev_path, strerror(errno));
        printf("[RawInput] Try running with sudo?\n");
        free(dev_path);
        return false;
    }

    free(dev_path);
    is_running = true;
    current_pressure = 0;

    if (pthread_create(&input_thread, NULL, input_thread_func, NULL) != 0) {
        perror("[RawInput] Failed to create thread");
        close(device_fd);
        is_running = false;
        return false;
    }

    return true;
}

inline void RawInput_Stop(void) {
    if (!is_running) return;

    is_running = false;
    // Cancel thread or wait for it? 
    // Since read() is blocking, we might need to cancel or just close the fd to wake it up.
    // Closing fd usually causes read to return error.
    close(device_fd);
    pthread_join(input_thread, NULL);
    device_fd = -1;
    printf("[RawInput] Stopped.\n");
}

inline float RawInput_GetPressure(void) {
    if (!is_running) return -1.0f;
    
    int p = current_pressure;
    if (p <= 0) return 0.0f;
    
    int max_p = max_pressure;
    if (max_p <= 0) max_p = 4096; // Safety fallback
    
    float norm = (float)p / max_p;
    if (norm > 1.0f) norm = 1.0f;
    
    return norm;
}

#endif
