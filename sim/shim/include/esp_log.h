/* sim shim — ESP_LOGx to stderr, one line per call. */
#ifndef SIM_ESP_LOG_H
#define SIM_ESP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void sim_log_write(char level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

#ifdef __cplusplus
}
#endif

#define ESP_LOGE(tag, fmt, ...) sim_log_write('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) sim_log_write('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) sim_log_write('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) sim_log_write('D', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) sim_log_write('V', tag, fmt, ##__VA_ARGS__)

#endif /* SIM_ESP_LOG_H */
