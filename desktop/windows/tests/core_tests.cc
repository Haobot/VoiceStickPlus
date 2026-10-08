#include "air_mouse_kin.h"
#include "app_config.h"
#include "asr_client_tencent.h"
#include "asr_protocol.h"
#include "audio_opus_decoder.h"
#include "audio_opus_encoder.h"
#include "ble_protocol.h"
#include "test_suites.h"  // N8: suite registry for split test files
#include "ima_adpcm_decoder.h"
#include "pcm_postprocessor.h"
#include "xiaomi_atvv_protocol.h"
#include "xiaomi_atvv_session.h"
#include "xiaomi_buttons.h"
#include "xiaomi_keymap_interceptor.h"
#include "xiaomi_usage_tap.h"
#include "xiaomi_usage_tap_decoder.h"
#include "xiaomi_usage_tap_host.h"
#include "xiaomi_usage_tap_manager.h"
#include "xiaomi_usage_tap.h"
#include "cmd_line.h"
#include "com_port_selector.h"

#include <opus.h>
#include "byte_utils.h"
#include "clipboard_vault.h"
#include "cJSON.h"
#include "esptool_flash_command.h"
#include "esptool_progress.h"
#include "firmware_manifest.h"
#include "hotword_extractor.h"
#include "hotword_candidate_miner.h"
#include "hotword_selector.h"
#include "tencent_asr_vocab_client.h"
#include "key_spec.h"
#include "llm_refinement_client.h"
#include "log.h"
#include "localization.h"
#include "ogg_opus_muxer.h"
#include "ogg_opus_demuxer.h"
#include "local_asr_client_win.h"
#include "local_refinement_client.h"
#include "model_manifest.h"
#include "model_downloader.h"
#include "model_download_session.h"
#include "voice_stick_cloud_api_win.h"
#ifdef VOICESTICK_LOCAL_REFINE_ENABLED
#include "llama_cpp_engine.h"
#endif
#include "text_refiner.h"
#include "pinyin_guard.h"
#include "refine_history.h"
#include "mic_capture.h"
#include "license.h"
#include "serial_base32.h"
#include "wasapi_mic_capture.h"
#include "push_to_talk_key.h"
#include "provider_combo.h"
#include "selection_correction.h"
#include "shortcut_capture.h"
#include "onboarding_dialog.h"
#include "pair_device_helper.h"
#include "pcm_ring_buffer.h"
#include "power_log_monitor.h"
#include "voice_stick_coordinator.h"
#include "voice_stick_flash_tool.h"
#include "wasapi_render_sink.h"
#include "wasapi_virtual_mic_renderer.h"
#include "wechat_input_method_hotkey.h"
#include "default_audio_device_controller.h"
#include "device_switch_state.h"
#include "debug_audio_recorder.h"
#include "encoder_speed.h"

#include <algorithm>
#include <winsock2.h>
#include <windows.h>  // SEH 探针（AddVectoredExceptionHandlerFirst）；置于 winsock2 之后避免 winsock 冲突
#include <bcrypt.h>
#include <thread>
#include <utility>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cctype>
#include <cmath>
#include <crtdbg.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <future>
#include <optional>
#include <atomic>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace voicestick;

namespace {


#include "test_support.h"


static LONG WINAPI UnhandledExceptionProbe(PEXCEPTION_POINTERS info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // 广谱：所有 NTSTATUS 错误段（0xC000xxxx）+ C++ 异常（0xE06D7363，未捕获即
    // terminate→静默 abort）+ 断点。C++ 异常首机会也会为「已捕获」的异常触发，
    // 若为噪声以最后一条为准（死亡前最后一条即真凶）。
    if ((code & 0xC0000000u) == 0xC0000000u || code == 0xE06D7363u ||
        code == 0x80000003u) {
        std::printf("[probe] exception 0x%08lX addr=%p\n",
                    static_cast<unsigned long>(code),
                    info->ExceptionRecord->ExceptionAddress);
        if (code == 0xC0000005u && info->ExceptionRecord->ExceptionInformation[0] != 0) {
            std::printf("[probe] AV access addr=%p\n",
                        reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
        }
        fflush(stdout);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

int main() {
    SetUnhandledExceptionFilter(&UnhandledExceptionProbe);
#ifdef _DEBUG
    // CI/命令行友好：Debug 下 assert 失败写 stderr 后直接终止，
    // 避免 CRT 默认弹「Microsoft Visual C++ Runtime Library」对话框挂起测试进程。
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // Watson 报告与 abort() 弹窗都会在无人值守时挂死测试进程
#endif
    // stdout 重定向到文件时默认全缓冲，断言 abort 会丢掉之前的进度输出。
    setvbuf(stdout, nullptr, _IONBF, 0);
    // 全程异常围栏：未捕获 C++ 异常（0xE06D7363 → terminate → 静默 abort）打出 what()，
    // 否则只剩 SEH 探针的错误码，定位不到抛出点。
    try {
    RunDevicePlanMiscBatchTests();  // N8 cut15 (final): suite in core_tests_device_plan.cc
    RunLicenseImaAtvvBatchTests();  // N8 cut14: suite in core_tests_license_ima.cc
    RunCoordinatorBatch6Tests();  // N8 cut9: suite in core_tests_coordinator6.cc
    printf(">> TestCoordinatorDeviceSessionRoutesToLocalAsrWhenEnabled\n"); fflush(stdout);
    printf(">> TestCoordinatorLocalMicShortPressDiscards\n"); fflush(stdout);
    printf(">> TestCoordinatorLocalMicDisabledDoesNothing\n"); fflush(stdout);
    printf(">> TestCoordinatorLocalMicCaptureStartFailureCancelsSession\n"); fflush(stdout);
    printf(">> TestWasapiMicCaptureSmoke\n"); fflush(stdout);
    printf(">> wasapi smoke done\n"); fflush(stdout);
    printf(">> TestClipboardVaultMultiFormatRoundTrip\n"); fflush(stdout);
    printf(">> TestClipboardVaultSkipsHandleFormats\n"); fflush(stdout);
    printf(">> TestClipboardVaultEmptyClipboardSnapshot\n"); fflush(stdout);
    printf(">> TestClipboardVaultSaveThrowsWhenBusy\n"); fflush(stdout);
    printf(">> vault tests all done\n"); fflush(stdout);
    RunProtocolContractTests();  // N8: suite in core_tests_protocol.cc
    // N8 cut14: orphan registration (pre-check: def was never called)
    RunCodecFixtureSerialBatchTests();  // N8 cut13: suite in core_tests_codec_serial.cc
    printf(">> cluster: B14 hotword validation unified\n"); fflush(stdout);
    printf(">> cluster: C8 log rotation + url redaction + prompt cap\n"); fflush(stdout);
    printf(">> cluster: C7 cloud url TLS-only policy\n"); fflush(stdout);
    printf(">> cluster: C2 license binding devices union\n"); fflush(stdout);
    printf(">> cluster: C3 DateToDays pre-epoch clamp\n"); fflush(stdout);
    printf(">> cluster: B13 hotword candidates single writer\n"); fflush(stdout);
    printf(">> cluster: C6 firmware version compare robustness\n"); fflush(stdout);
    printf(">> TestOtaMaxInFlightBytes\n"); fflush(stdout);
    printf(">> cluster: D6 OTA chunk size for PDU\n"); fflush(stdout);
    printf(">> TestParseOtaCliArgs\n"); fflush(stdout);
    printf(">> cluster: coordinator hotkey/click/tap\n"); fflush(stdout);
    printf(">> cluster: B3 wechat start failure rolls back default capture\n"); fflush(stdout);
    RunCoordinatorBatchTests();  // N8 cut3: suite in core_tests_coordinator.cc
    RunDeviceInputMiscBatchTests();  // N8 cut12: suite in core_tests_device_misc.cc
    RunCoordinatorBatch2Tests();  // N8 cut4: suite in core_tests_coordinator2.cc
    RunTencentBatchTests();  // N8 cut11: suite in core_tests_tencent.cc
    printf(">> cluster: B7 tencent hotword vocab async sync\n"); fflush(stdout);
    printf(">> TestCoordinatorUpdateConfigDestroysOldAsrOffThread\n"); fflush(stdout);
    printf(">> TestCoordinatorConcurrentUpdateConfigStress\n"); fflush(stdout);
    RunXiaomiAtvvBatchTests();  // N8 cut7: suite in core_tests_xiaomi_atvv.cc
    RunXiaomiUsageTapBatchTests();  // N8 cut10: suite in core_tests_xiaomi_usage_tap.cc
    printf(">> cluster: B2 usage tap Stop bounded\n"); fflush(stdout);
    RunCoordinatorBatch4Tests();  // N8 cut6: suite in core_tests_coordinator4.cc
    RunCoordinatorBatch3Tests();  // N8 cut5: suite in core_tests_coordinator3.cc
    RunCoordinatorBatch5Tests();  // N8 cut8: suite in core_tests_coordinator5.cc
    printf(">> cluster: C5 model present sha256 verify\n"); fflush(stdout);
    printf(">> ALL TESTS DONE\n"); fflush(stdout);
    return 0;
    } catch (const std::exception& e) {
        printf("UNCAUGHT EXCEPTION: %s\n", e.what()); fflush(stdout);
        return 1;
    } catch (...) {
        printf("UNCAUGHT EXCEPTION: unknown type\n"); fflush(stdout);
        return 1;
    }
}
