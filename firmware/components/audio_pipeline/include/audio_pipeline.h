#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_start(uint32_t session_id);
esp_err_t audio_pipeline_stop(void);
esp_err_t audio_pipeline_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent);
const char *audio_pipeline_last_error_step(void);
uint32_t audio_pipeline_session_id(void);
