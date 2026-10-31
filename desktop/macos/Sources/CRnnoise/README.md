# CRnnoise — vendored xiph/rnnoise

上游：https://github.com/xiph/rnnoise ，master 分支 tarball（2026-10-14 抓取，
codeload.github.com/xiph/rnnoise/tar.gz/refs/heads/master）。BSD-2-Clause，见 COPYING。

用途：桌面端 ASR 前人声增强/降噪（`AudioEnhancer.swift`）。RNNoise 固定 48kHz/
480 样本（10ms）帧。

裁剪：仅保留推理所需最小集（denoise/rnn/rnn_data/kiss_fft/pitch/celt_lpc +
内部头），去掉 rnn_reader.c（外部模型加载）、examples/、training/、
构建系统文件（Makefile.am/configure.ac 等）。

本地补丁（与 `desktop/windows/third_party/rnnoise/` 双侧同步，2026-10-14）：
MSVC 编译兼容——上游代码假定 GCC/Clang（VLA + POSIX math.h），5 处 C99 VLA
（celt_lpc.c `xx[n]`、pitch.c `x_lp4/y_lp4/xcorr/yy_lookup`）改为头文件定长
上限 + `celt_assert` 兜底（`RNN_AUTOCORR_MAX_N`、`RNN_PITCH_SEARCH_*`、
`RNN_REMOVE_DOUBLING_MAX_HALFPERIOD`）。clang 下数值行为不变（同维度同内容）；
`M_PI` 无需处理（仅 Windows 构建层 `_USE_MATH_DEFINES`）。
