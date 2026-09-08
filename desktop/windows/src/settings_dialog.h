#pragma once

#include "app_config.h"
#include "shortcut_capture.h"

#include <Windows.h>

#include <functional>
#include <utility>
#include <vector>

namespace voicestick {

class SettingsDialog {
public:
    SettingsDialog(HINSTANCE instance, HWND parent, AppConfig config);
    ~SettingsDialog();

    void Show();

    std::function<void(AppConfig)> on_config_changed;

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void RebuildUi();
    void DestroyControls();
    void BuildControls();
    void LoadConfigIntoControls();
    void SaveSettings();
    void UpdateOutputTargetVisibility();
    void OnTriggerModeChanged();
    void UpdateProviderVisibility();
    void UpdateRefinePromptVisibility();
    void UpdateHotwordProcessPromptVisibility();
    // 开发者模式复选框切换：更新 developer_mode_ 并重新布局显隐高级功能。
    void OnDeveloperModeToggled();
    // 候选热词：从存储文件刷新待确认列表，并重填候选列表控件。
    void RefreshHotwordCandidates();
    void OnHotwordCandidateAdd();
    void OnHotwordCandidateDismiss();
    void ApplyTrialApiKey();
    void ChooseDebugDirectory();
    // 通用文件夹选择（IFileDialog FOS_PICKFOLDERS）：选定路径写入目标编辑框，
    // ChooseDebugDirectory 与本机麦克风模型目录共用。
    void ChooseFolderInto(HWND target_edit);
    // 本机麦克风：模型目录浏览 + 即时有效性回显（与启动校验同一口径）。
    void ChooseLocalMicModelsDir();
    void UpdateLocalMicModelsStatus();
    // 本机麦克风按住说话热键：录入式选择（ShortcutCapture 单键模式，修饰键
    // 本身即可作为热键，如 right ctrl）；Esc 取消，3 秒无事件弹 UIPI 引导。
    void OnCaptureLocalMicHotkey();
    void StopPttCaptureTimer();
    void RestorePttCaptureButtonText();
    // 启动频谱查看器（scripts/e2e_test/spectrogram_server.py，经 py/python 启动）。
    void OpenSpectrogramViewer();
    bool IsLabelControl(HWND control) const;
    int Dp(int px) const;
    // 按声明式布局表重新定位所有控件并按可见行数动态调整窗口高度。
    void Relayout();
    // 按客户区高度调整窗口尺寸（顶部固定，底部伸缩）。
    void ResizeWindow(int client_h);
    // 行内条件：apply_trial_button 显隐 + api_key_edit 宽度，在 Relayout 末尾调用。
    void ApplyApiKeyLayout();
    // 服务商下拉框索引 ↔ AsrProvider 映射；provider_combo_has_cloud_ 时 0 号位为 Cloud。
    AsrProvider ProviderAtComboIndex(int idx) const;
    int ComboIndexForProvider(AsrProvider provider) const;

    // 布局模型：把每行/块抽象为可独立显隐的条目，Relayout 统一应用定位。
    struct LayoutPart {
        HWND control;
        int x;
        int y_off;  // 相对行基线 y 的偏移
        int w;
        int h;
        // true=仅参与定位，显隐交给外部（如 apply_trial_button 行内条件按钮），
        // 避免 Relayout 在可见行上 ShowWindow(SW_SHOW) 覆盖外部隐藏。
        bool defer_visibility = false;
    };
    struct LayoutEntry {
        int advance;                         // 该项可见时推进的 y（Dp 换算后）
        std::vector<LayoutPart> parts;       // 该项的控件
        std::function<bool()> visible;       // 空 = 始终可见
    };

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    AppConfig config_;
    UINT dpi_ = 96;
    // 开发者模式：true 时设置页放出全部高级功能。从 config_.developer_mode 加载，
    // 切换复选框时实时更新并 Relayout，无需重建控件。
    bool developer_mode_ = false;

    HWND language_combo_ = nullptr;
    HWND developer_mode_check_ = nullptr;
    HWND provider_combo_ = nullptr;
    // 当前配置为 voicestick_cloud 时，下拉框 0 号位临时插入 "VoiceStick Cloud"（老配置兼容）。
    bool provider_combo_has_cloud_ = false;
    HWND api_key_edit_ = nullptr;
    HWND apply_trial_button_ = nullptr;
    HWND resource_combo_ = nullptr;
    HWND hotwords_edit_ = nullptr;
    HWND candidates_label_ = nullptr;
    HWND candidates_list_ = nullptr;
    HWND candidate_add_button_ = nullptr;
    HWND candidate_dismiss_button_ = nullptr;
    // 当前待确认的候选热词（与 candidates_list_ 行序一一对应）。
    std::vector<std::string> candidate_words_;
    HWND llm_base_url_edit_ = nullptr;
    HWND llm_api_key_edit_ = nullptr;
    HWND llm_model_edit_ = nullptr;
    HWND refine_check_ = nullptr;
    HWND refine_prompt_label_ = nullptr;
    HWND refine_prompt_edit_ = nullptr;
    HWND hotword_process_check_ = nullptr;
    HWND hotword_process_prompt_label_ = nullptr;
    HWND hotword_process_prompt_edit_ = nullptr;
    HWND launch_at_login_check_ = nullptr;
    HWND selection_hotword_check_ = nullptr;
    HWND debug_audio_check_ = nullptr;
    HWND show_imu_debug_check_ = nullptr;
    HWND output_target_combo_ = nullptr;
    HWND wechat_hotkey_edit_ = nullptr;
    HWND wechat_hotkey_label_ = nullptr;
    HWND trigger_mode_label_ = nullptr;
    HWND trigger_mode_hold_radio_ = nullptr;
    HWND trigger_mode_click_radio_ = nullptr;
    // 编辑框当前显示的热键所属触发模式（切换 radio 时据此存回对应模式字段）。
    InteractionMode loaded_hotkey_mode_ = InteractionMode::kHoldToTalk;
    HWND debug_dir_edit_ = nullptr;
    HWND resource_label_ = nullptr;
    // 本机麦克风（[local_asr]，Doc/Plan/local-mic-mode.md）：开关/模型目录/热键。
    HWND local_mic_enable_check_ = nullptr;
    HWND local_mic_models_dir_edit_ = nullptr;
    HWND local_mic_models_dir_browse_button_ = nullptr;
    // 模型目录有效性回显（✓ 就绪 / ✗ 缺文件），与 Resolve+Validate 同口径。
    HWND local_mic_models_status_label_ = nullptr;
    HWND local_mic_hotkey_edit_ = nullptr;
    HWND local_mic_hotkey_capture_button_ = nullptr;
    // 按住说话热键录入（ShortcutCapture 单键模式；成员随对话框生命周期，
    // 析构自动 Cancel）。
    ShortcutCapture ptt_capture_;
    HWND save_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    HFONT ui_font_ = nullptr;
    HFONT title_font_ = nullptr;
    int scroll_pos_ = 0;  // 垂直滚动位置（像素，Dp 换算后）
    std::vector<BYTE> dialog_template_;
    std::vector<HWND> all_controls_;
    std::vector<HWND> label_controls_;
    std::vector<HWND> title_controls_;
    std::vector<LayoutEntry> layout_;

    static constexpr int kClientWidth = 580;
    // 编码器区块与设备交互区块已迁出到设备级对话框（编码器 13 行 + 设备交互 6 行）。
    static constexpr int kClientHeight = 780;
    static constexpr UINT kIdLanguageCombo = 2000;
    static constexpr UINT kIdProviderCombo = 2001;
    static constexpr UINT kIdApiKeyEdit = 2002;
    static constexpr UINT kIdResourceCombo = 2003;
    static constexpr UINT kIdHotwordsEdit = 2004;
    static constexpr UINT kIdLlmBaseUrlEdit = 2005;
    static constexpr UINT kIdLlmApiKeyEdit = 2006;
    static constexpr UINT kIdLlmModelEdit = 2007;
    static constexpr UINT kIdRefineText = 2016;
    static constexpr UINT kIdLaunchAtLogin = 2009;
    static constexpr UINT kIdDebugAudio = 2010;
    static constexpr UINT kIdShowImuDebug = 2017;
    static constexpr UINT kIdDebugDirEdit = 2011;
    static constexpr UINT kIdChooseDir = 2012;
    static constexpr UINT kIdSave = 2013;
    static constexpr UINT kIdCancel = 2014;
    static constexpr UINT kIdApplyTrialApiKey = 2015;
    static constexpr UINT kIdRefinePromptEdit = 2022;
    static constexpr UINT kIdOutputTarget = 2026;
    static constexpr UINT kIdWechatHotkey = 2027;
    static constexpr UINT kIdWechatVirtualMic = 2028;
    static constexpr UINT kIdWechatAutoSwitch = 2029;
    static constexpr UINT kIdWechatVirtualMicCapture = 2030;
    static constexpr UINT kIdTriggerModeHold = 2031;
    static constexpr UINT kIdTriggerModeClick = 2032;
    static constexpr UINT kIdSelectionHotword = 2033;
    static constexpr UINT kIdHotwordProcessEnable = 2034;
    static constexpr UINT kIdHotwordProcessPromptEdit = 2035;
    static constexpr UINT kIdHotwordCandidateList = 2036;
    static constexpr UINT kIdHotwordCandidateAdd = 2037;
    static constexpr UINT kIdHotwordCandidateDismiss = 2038;
    static constexpr UINT kIdDeveloperMode = 2039;
    static constexpr UINT kIdOpenSpectrogram = 2040;
    static constexpr UINT kIdLocalMicEnable = 2041;
    static constexpr UINT kIdLocalMicModelsDirEdit = 2042;
    static constexpr UINT kIdLocalMicHotkeyEdit = 2043;
    static constexpr UINT kIdLocalMicModelsDirBrowse = 2044;
    static constexpr UINT kIdLocalMicHotkeyCapture = 2045;

    // 按住说话热键录入超时提示定时器：录入启动后 3 秒无键盘事件（UIPI 前台
    // 提权隔离等）弹一次引导，不中断进行中的捕获（对齐 hotkey_settings_dialog）。
    static constexpr UINT_PTR kPttCaptureHintTimerId = 0x5350;
    static constexpr UINT kPttCaptureHintTimeoutMs = 3000;
};

} // namespace voicestick
