import AppKit
import VoiceStickCore

/// 模型下载窗口（对齐 Windows ModelDownloadDialog 的向导语义；进度/速度/ETA
/// UI 形态仿 FirmwareUpdateWindowController）：
/// - 条目勾选：必选项锁定勾选，可选项（本地 LLM，Phase B 起）可取消；
/// - 已就位（size+SHA 校验通过）的文件自动跳过，全就位秒完成；
/// - 取消保留 .part 断点续传；成功后回调 onCompleted（设置页刷新状态）。
final class ModelDownloadWindowController: NSWindowController, NSWindowDelegate {

    /// 一个勾选条目的 UI 状态。
    private struct ItemState {
        let entry: ModelManifest.EntrySpec
        let installRoot: URL
        let checkbox: NSButton
        let sizeLabel: NSTextField
        var installed = false
    }

    private var items: [ItemState] = []
    private var itemRows: [NSStackView] = []
    private let titleLabel = NSTextField(labelWithString: "")
    private let introLabel = NSTextField(labelWithString: "")
    private let spaceLabel = NSTextField(labelWithString: "")
    private let fileLabel = NSTextField(labelWithString: "")
    private let progressIndicator = NSProgressIndicator()
    private let percentLabel = NSTextField(labelWithString: "0%")
    private let speedLabel = NSTextField(labelWithString: "")
    private let timeLabel = NSTextField(labelWithString: "")
    private let resultLabel = NSTextField(labelWithString: "")
    private let startButton = NSButton(title: "", target: nil, action: nil)
    private let cancelButton = NSButton(title: "", target: nil, action: nil)
    private let closeButton = NSButton(title: "", target: nil, action: nil)

    private var downloader: ModelDownloader?
    private var startedAt: Date?
    /// 会话内已接收字节（速度估算用，含跳过项的完成量）。
    private var speedAnchorBytes: Int64 = 0

    /// 下载成功（或所选全部已就位）时回调（主线程）。
    var onCompleted: (() -> Void)?

    /// - Parameters:
    ///   - asrModelsDir: 设置页解析出的 SenseVoice 模型目录（模型与后续 LLM GGUF
    ///     安装根；下载期间目录字段再改不影响本会话已定的计划）。
    init(asrModelsDir: String) {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 480, height: 320),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = tr(.modelDownloadTitle)
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self

        // 安装根：asr/llm 条目 = 设置解析出的模型目录（GGUF 落模型目录，Windows
        // 同布局）；runtime 条目固定 Application Support（dylib 搜索序第 3 路径）。
        let asrRoot = URL(fileURLWithPath: asrModelsDir, isDirectory: true)
        var runtimeRoot: URL?
        if let appSupport = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask).first {
            runtimeRoot = appSupport.appendingPathComponent("VoiceStick/models/sherpa-onnx")
        }
        items = ModelManifest.entries.compactMap { entry in
            let root: URL
            switch entry.kind {
            case .asr, .llm: root = asrRoot
            case .runtime:
                guard let runtimeRoot else { return nil }
                root = runtimeRoot
            }
            return ItemState(
                entry: entry, installRoot: root,
                checkbox: NSButton(checkboxWithTitle: entryTitle(entry), target: self,
                                   action: #selector(itemToggled)),
                sizeLabel: NSTextField(labelWithString: Self.sizeText(entry)))
        }
        buildContent()
        refreshSpaceLabel()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func show() {
        showWindow(nil)
        window?.center()
        NSApp.activate(ignoringOtherApps: true)
    }

    // MARK: - UI 构建

    private func buildContent() {
        guard let contentView = window?.contentView else { return }
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(stack)

        titleLabel.stringValue = tr(.modelDownloadTitle)
        titleLabel.font = .systemFont(ofSize: 17, weight: .semibold)
        introLabel.stringValue = tr(.modelDownloadIntro)
        introLabel.textColor = .secondaryLabelColor

        for index in items.indices {
            let control = NSStackView()
            control.orientation = .horizontal
            control.alignment = .centerY
            control.spacing = 8
            let item = items[index]
            item.checkbox.state = .on
            item.checkbox.isEnabled = !item.entry.required   // 必选项锁定
            control.addArrangedSubview(item.checkbox)
            control.addArrangedSubview(item.sizeLabel)
            item.sizeLabel.textColor = .secondaryLabelColor
            itemRows.append(control)
        }

        spaceLabel.textColor = .secondaryLabelColor

        progressIndicator.isIndeterminate = false
        progressIndicator.minValue = 0
        progressIndicator.maxValue = 100
        progressIndicator.doubleValue = 0
        fileLabel.textColor = .secondaryLabelColor
        fileLabel.lineBreakMode = .byTruncatingMiddle
        percentLabel.alignment = .right
        percentLabel.widthAnchor.constraint(equalToConstant: 42).isActive = true
        speedLabel.textColor = .secondaryLabelColor
        timeLabel.textColor = .secondaryLabelColor

        let progressRow = NSStackView()
        progressRow.orientation = .horizontal
        progressRow.alignment = .centerY
        progressRow.spacing = 10
        progressRow.addArrangedSubview(progressIndicator)
        progressRow.addArrangedSubview(percentLabel)
        progressRow.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        progressIndicator.widthAnchor.constraint(greaterThanOrEqualToConstant: 300).isActive = true

        let detailRow = NSStackView()
        detailRow.orientation = .horizontal
        detailRow.alignment = .centerY
        detailRow.spacing = 12
        detailRow.addArrangedSubview(speedLabel)
        detailRow.addArrangedSubview(timeLabel)

        resultLabel.textColor = .secondaryLabelColor

        startButton.title = tr(.modelDownloadStart)
        startButton.keyEquivalent = "\r"
        startButton.target = self
        startButton.action = #selector(startDownload)
        cancelButton.title = tr(.cancel)
        cancelButton.target = self
        cancelButton.action = #selector(cancelDownload)
        cancelButton.isEnabled = false
        closeButton.title = tr(.close)
        closeButton.target = self
        closeButton.action = #selector(closeWindow)
        let buttonRow = NSStackView()
        buttonRow.orientation = .horizontal
        buttonRow.alignment = .centerY
        buttonRow.spacing = 8
        let spacer = NSView()
        spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        buttonRow.addArrangedSubview(spacer)
        buttonRow.addArrangedSubview(startButton)
        buttonRow.addArrangedSubview(cancelButton)
        buttonRow.addArrangedSubview(closeButton)
        buttonRow.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true

        stack.addArrangedSubview(titleLabel)
        stack.addArrangedSubview(introLabel)
        itemRows.forEach { stack.addArrangedSubview($0) }
        stack.addArrangedSubview(spaceLabel)
        stack.addArrangedSubview(fileLabel)
        stack.addArrangedSubview(progressRow)
        stack.addArrangedSubview(detailRow)
        stack.addArrangedSubview(resultLabel)
        stack.addArrangedSubview(buttonRow)

        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 24),
            stack.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -24),
            stack.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 22),
            stack.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -18),
        ])
    }

    private func entryTitle(_ entry: ModelManifest.EntrySpec) -> String {
        switch entry.kind {
        case .asr: return tr(.modelDownloadEntryAsr)
        case .runtime: return tr(.modelDownloadEntryRuntime)
        case .llm(let id): return tr(.modelDownloadEntryLlm, id)
        }
    }

    private static func sizeText(_ entry: ModelManifest.EntrySpec) -> String {
        let formatter = ByteCountFormatter()
        formatter.countStyle = .binary
        return "≈ " + formatter.string(fromByteCount: entry.totalBytes)
    }

    private func refreshSpaceLabel() {
        let selected = items.filter { $0.checkbox.state == .on }
            .reduce(Int64(0)) { $0 + $1.entry.totalBytes }
        let formatter = ByteCountFormatter()
        formatter.countStyle = .binary
        spaceLabel.stringValue = tr(.modelDownloadSpaceNeeded, formatter.string(fromByteCount: selected))
    }

    @objc private func itemToggled() {
        refreshSpaceLabel()
    }

    // MARK: - 下载会话

    @objc private func startDownload() {
        let plan = items.filter { $0.checkbox.state == .on }
            .map { ModelDownloader.PlanItem(entry: $0.entry, installRoot: $0.installRoot) }
        guard !plan.isEmpty else { return }

        startButton.isEnabled = false
        for index in items.indices { items[index].checkbox.isEnabled = false }
        cancelButton.isEnabled = true
        resultLabel.stringValue = ""
        fileLabel.stringValue = tr(.modelDownloadPreparing)
        progressIndicator.doubleValue = 0
        percentLabel.stringValue = "0%"
        speedLabel.stringValue = ""
        timeLabel.stringValue = ""
        startedAt = Date()

        let downloader = ModelDownloader()
        self.downloader = downloader
        downloader.download(
            plan: plan,
            progress: { [weak self] progress in
                DispatchQueue.main.async { self?.apply(progress) }
            },
            completion: { [weak self] result in
                guard let self else { return }
                self.downloader = nil
                self.finish(with: result)
            })
    }

    private func apply(_ progress: ModelDownloader.Progress) {
        fileLabel.stringValue = progress.currentItem
        let fraction = progress.fraction
        progressIndicator.doubleValue = fraction * 100
        percentLabel.stringValue = "\(Int(fraction * 100))%"

        let done = progress.completedBytes + (progress.totalBytes > 0
            ? min(progress.receivedBytes, progress.totalBytes) : progress.receivedBytes)
        if let startedAt, done > speedAnchorBytes {
            let elapsed = max(0.1, Date().timeIntervalSince(startedAt))
            let bps = Double(done - speedAnchorBytes) / elapsed
            speedLabel.stringValue = tr(.firmwareSpeed, Self.format(bytesPerSecond: bps))
            if bps > 1, progress.plannedBytes > 0 {
                let remaining = Double(max(0, progress.plannedBytes - done)) / bps
                timeLabel.stringValue = tr(.firmwareTimeRemaining, Self.format(duration: remaining))
            }
        }
    }

    private func finish(with result: Result<Void, Error>) {
        cancelButton.isEnabled = false
        closeButton.isEnabled = true
        startButton.isEnabled = true
        for index in items.indices { items[index].checkbox.isEnabled = !items[index].entry.required }
        switch result {
        case .success:
            progressIndicator.doubleValue = 100
            percentLabel.stringValue = "100%"
            fileLabel.stringValue = ""
            resultLabel.stringValue = tr(.modelDownloadDone)
            onCompleted?()
        case .failure(let error):
            resultLabel.stringValue = error.localizedDescription
        }
    }

    @objc private func cancelDownload() {
        cancelButton.isEnabled = false
        downloader?.cancel()
    }

    @objc private func closeWindow() {
        window?.close()
    }

    /// 关窗（含标题栏关闭钮）即中止会话——.part 保留供续传。
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        downloader?.cancel()
        return true
    }

    private static func format(duration: TimeInterval) -> String {
        let seconds = max(0, Int(duration.rounded()))
        if seconds < 60 { return "\(seconds)s" }
        return "\(seconds / 60)m \(seconds % 60)s"
    }

    private static func format(bytesPerSecond: Double) -> String {
        let formatter = ByteCountFormatter()
        formatter.allowedUnits = [.useKB, .useMB]
        formatter.countStyle = .binary
        return "\(formatter.string(fromByteCount: Int64(bytesPerSecond)))/s"
    }
}
