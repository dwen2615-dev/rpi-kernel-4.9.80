#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <math.h>

// Global calibration variable (volatile to avoid over-optimization)
static volatile int dummy_load_calib = 1;

/* -------- timing helpers -------- */
static inline long long now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + (long long)tv.tv_usec;
}

/* burn CPU cycles; bound computed in 64-bit to avoid overflow */
void dummy_load(int execution_time_ms) {
    long long bound = (long long)dummy_load_calib * (long long)execution_time_ms;
    for (long long j = 0; j < bound; j++) {
        for (int i = 0; i < 1000; i++) {
            __asm__ volatile ("nop");
        }
    }
}

/* median-of-5 measurement to reduce noise even more */
static long long run_for_ms_and_measure(int ms) {
    long long a[5];
    
    // Warmup
    dummy_load(1);
    
    // Take 5 measurements
    for (int k = 0; k < 5; k++) {
        long long s = now_us();
        dummy_load(ms);
        long long e = now_us();
        a[k] = e - s;
    }
    
    // Bubble sort to find median (a[2])
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            if (a[i] > a[j]) {
                long long t = a[i];
                a[i] = a[j];
                a[j] = t;
            }
        }
    }
    
    return a[2];  // Return median (middle value)
}

/* -------- calibration -------- */
static void calibrate_dummy_load(void) {
    // Use 50ms as target - better mid-range for extrapolation
    const int target_ms  = 50;
    const long long tgt_us = target_ms * 1000LL;
    const int tol_us     = 500;

    //printf("Calibrating dummy_load...\n");

    // 1) Exponential search to find an upper bound
    int lo = 1, hi = 1;
    while (1) {
        dummy_load_calib = hi;
        long long us = run_for_ms_and_measure(target_ms);
        if (us >= tgt_us || hi > 100000000) break;
        lo = hi + 1;
        hi <<= 1;
    }

    // 2) Binary search between lo..hi
    int best = hi;
    long long best_err = (1LL<<62);
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        dummy_load_calib = mid;
        long long us = run_for_ms_and_measure(target_ms);
        long long err = us - tgt_us;

        if (llabs(err) < llabs(best_err)) { best_err = err; best = mid; }
        if (llabs(err) <= tol_us) { best = mid; break; }
        if (us < tgt_us) lo = mid + 1; else hi = mid - 1;
    }
    dummy_load_calib = best;

    // 3) Three-point calibration for better curve fitting
    // Measure at 10ms, 50ms, and 100ms
    long long t10  = run_for_ms_and_measure(10);
    long long t50  = run_for_ms_and_measure(50);
    long long t100 = run_for_ms_and_measure(100);
    
    // Calculate average slope across the range
    double slope1 = (double)(t50 - t10) / 40.0;    // 10-50ms range
    double slope2 = (double)(t100 - t50) / 50.0;   // 50-100ms range
    double avg_slope = (slope1 + slope2) / 2.0;
    
    // Target slope is 1000 µs/ms
    if (avg_slope > 700.0 && avg_slope < 1500.0) {
        double adjust = 1000.0 / avg_slope;
        
        // Apply adjustment with bounds
        if (adjust >= 0.85 && adjust <= 1.15) {
            int new_calib = (int)((double)dummy_load_calib * adjust + 0.5);
            if (new_calib < 1) new_calib = 1;
            dummy_load_calib = new_calib;
        }
    }

    // 4) Final verification at target
    //long long final_us = run_for_ms_and_measure(target_ms);
    //printf("Calibration complete: dummy_load_calib = %d\n", dummy_load_calib);
    //printf("Target: %d ms, Actual: %.3f ms, Error: %lld us\n", target_ms, final_us / 1000.0, final_us - tgt_us);
}

/* -------- main / test -------- */
static void test_dummy_load(int ms_req) {
    //printf("\nTesting dummy_load with %d ms:\n", ms_req);
    
    // Take median of 3 measurements for the test too
    long long measurements[3];
    for (int i = 0; i < 3; i++) {
        long long s = now_us();
        dummy_load(ms_req);
        long long e = now_us();
        measurements[i] = e - s;
    }
    
    // Sort and get median
    if (measurements[0] > measurements[1]) {
        long long t = measurements[0];
        measurements[0] = measurements[1];
        measurements[1] = t;
    }
    if (measurements[1] > measurements[2]) {
        long long t = measurements[1];
        measurements[1] = measurements[2];
        measurements[2] = t;
    }
    if (measurements[0] > measurements[1]) {
        long long t = measurements[0];
        measurements[0] = measurements[1];
        measurements[1] = t;
    }
    
    long long actual_us = measurements[1];  // Median
    double actual_ms = actual_us / 1000.0;
    long long err_us = actual_us - (long long)ms_req * 1000LL;

    printf("Requested execution time: %d ms\n", ms_req);
    printf("Actual execution time: %.3f ms\n", actual_ms);
    
    if (llabs(err_us) <= 500) {
        printf("✓ Within tolerance (±0.5 ms)\n");
    } else {
        printf("✗ Outside tolerance: error = %.3f ms\n", err_us / 1000.0);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <execution_time_ms>\n", argv[0]);
        fprintf(stderr, "Example: %s 10\n", argv[0]);
        return 1;
    }
    
    int ms_req = atoi(argv[1]);
    if (ms_req <= 0 || ms_req > 10000) {
        fprintf(stderr, "Error: execution time must be between 1 and 10000 ms\n");
        return 1;
    }

    calibrate_dummy_load();
    test_dummy_load(ms_req);
    return 0;
}
