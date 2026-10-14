# CRnnoise — vendored xiph/rnnoise

上游：https://github.com/xiph/rnnoise ，master 分支 tarball（2026-10-14 抓取，
codeload.github.com/xiph/rnnoise/tar.gz/refs/heads/master）。BSD-2-Clause，见 COPYING。

用途：桌面端 ASR 前人声增强/降噪（`AudioEnhancer.swift`）。RNNoise 固定 48kHz/
480 样本（10ms）帧。

裁剪：仅保留推理所需最小集（denoise/rnn/rnn_data/kiss_fft/pitch/celt_lpc +
内部头），去掉 rnn_reader.c（外部模型加载）、examples/、training/、
构建系统文件（Makefile.am/configure.ac 等）。
