import AppKit
import CoreBluetooth
import VoiceStickCore

/// 配对结果：VS 仅用 deviceID；RC 附外设 UUID 字符串（落库 paired_device addr 段）
/// 与名称，hardware 固定 "xiaomi_remote_2_pro" 由 AppDelegate 落库时填。
struct PairDeviceResult {
    let deviceID: String // 归一化 4 位大写 hex
    let name: String
    let deviceClass: DeviceClass
    let peripheralUUID: String?
}

private struct PairingDevice {
    let identifier: UUID
    var name: String
    var deviceID: String
    var deviceClass: DeviceClass
    var rssi: Int
}

final class PairDeviceWindowController: NSWindowController, NSWindowDelegate, CBCentralManagerDelegate, CBPeripheralDelegate, NSTableViewDataSource, NSTableViewDelegate {
    private let tableView = NSTableView()
    private let statusLabel = NSTextField(labelWithString: "Scanning")
    private let existingDeviceIDs: Set<String>
    /// 已配对条目的外设 UUID 大写集合（macOS paired_device addr 段），
    /// 用于 RC「同一物理设备重复配对」拦截。
    private let existingPeripheralUUIDs: Set<String>
    private let onPair: (PairDeviceResult) -> Void
    private var central: CBCentralManager?
    private var devices: [PairingDevice] = []
    private var peripherals: [UUID: CBPeripheral] = [:]
    // RC 配对临时连接状态（窗口自管，与主 BleCentral 互不干扰）。
    private var pairingPeripheral: CBPeripheral?
    private var pairingDevice: PairingDevice?
    private var pairingTimeoutTimer: Timer?
    private static let pairingTimeout: TimeInterval = 15
    /// 窗口已关闭（红叉/Cancel/完成路径）：关窗后禁止任何扫描重启与配对回调。
    private var isClosed = false

    init(existingDeviceIDs: [String], existingPeripheralUUIDs: [String],
         onPair: @escaping (PairDeviceResult) -> Void) {
        self.existingDeviceIDs = Set(existingDeviceIDs)
        self.existingPeripheralUUIDs = Set(existingPeripheralUUIDs.map { $0.uppercased() })
        self.onPair = onPair

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 500, height: 280),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = "Pair VoiceStick"
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
        buildContent()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func show() {
        showWindow(nil)
        window?.center()
        NSApp.activate(ignoringOtherApps: true)
        central = CBCentralManager(delegate: self, queue: .main)
    }

    private func buildContent() {
        guard let contentView = window?.contentView else { return }

        let stack = NSStackView()
        stack.orientation = .vertical
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(stack)

        let scrollView = NSScrollView()
        scrollView.hasVerticalScroller = true
        scrollView.documentView = tableView

        tableView.addTableColumn(column(id: "name", title: "Device", width: 160))
        tableView.addTableColumn(column(id: "id", title: "ID", width: 70))
        tableView.addTableColumn(column(id: "type", title: "Type", width: 110))
        tableView.addTableColumn(column(id: "rssi", title: "RSSI", width: 60))
        tableView.delegate = self
        tableView.dataSource = self
        tableView.target = self
        tableView.doubleAction = #selector(pairSelectedDevice)

        let buttonRow = NSStackView()
        buttonRow.orientation = .horizontal
        buttonRow.alignment = .centerY
        buttonRow.spacing = 8

        let pairButton = NSButton(title: "Pair", target: self, action: #selector(pairSelectedDevice))
        let cancelButton = NSButton(title: "Cancel", target: self, action: #selector(cancel))
        buttonRow.addArrangedSubview(statusLabel)
        buttonRow.addArrangedSubview(NSView())
        buttonRow.addArrangedSubview(pairButton)
        buttonRow.addArrangedSubview(cancelButton)

        stack.addArrangedSubview(scrollView)
        stack.addArrangedSubview(buttonRow)

        scrollView.heightAnchor.constraint(equalToConstant: 200).isActive = true
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -16),
            stack.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 16),
            stack.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -16)
        ])
    }

    private func column(id: String, title: String, width: CGFloat) -> NSTableColumn {
        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id))
        column.title = title
        column.width = width
        return column
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard !isClosed else { return }
        guard central.state == .poweredOn else {
            statusLabel.stringValue = "Bluetooth unavailable"
            return
        }
        statusLabel.stringValue = "Scanning"
        central.scanForPeripherals(withServices: nil)
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard pairingPeripheral == nil else { return } // 配对进行中不再收新候选
        let selectedIdentifier = selectedDeviceIdentifier
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String)
            ?? peripheral.name
            ?? ""

        let device: PairingDevice
        if let vsID = BleCentral.deviceID(from: name) {
            // VS 通道原样不动。
            device = PairingDevice(
                identifier: peripheral.identifier,
                name: name,
                deviceID: vsID,
                deviceClass: .stickS3,
                rssi: RSSI.intValue
            )
        } else {
            // 小米通道（判定对齐 BleCentral.classifyDiscoveredPeripheral）：名称命中
            // 白名单/RC-XXXX，或广告含 ATVV service UUID。
            let serviceUUIDs = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]) ?? []
            let hasAtvvService = serviceUUIDs.contains {
                $0.uuidString.uppercased() == XiaomiAtvvProtocol.serviceUUID
            }
            guard BleProtocol.deviceClass(forName: name) == .xiaomiRemote2Pro || hasAtvvService else {
                return
            }
            device = PairingDevice(
                identifier: peripheral.identifier,
                name: name.isEmpty ? "Xiaomi Remote" : name,
                deviceID: Self.rcDeviceID(name: name, peripheralUUID: peripheral.identifier),
                deviceClass: .xiaomiRemote2Pro,
                rssi: RSSI.intValue
            )
        }
        peripherals[peripheral.identifier] = peripheral

        if let index = devices.firstIndex(where: { $0.identifier == peripheral.identifier }) {
            devices[index] = device
        } else {
            devices.append(device)
        }
        tableView.reloadData()
        restoreSelection(selectedIdentifier)
        statusLabel.stringValue = devices.isEmpty ? "Scanning" : "\(devices.count) found"
    }

    /// RC 候选 ID：RC-XXXX 名从名称取（与 BleCentral 扫描分类同源，否则配对后
    /// 主扫描按名取 ID 与落库 ID 对不上）；白名单名/无名由外设 UUID 确定性派生。
    private static func rcDeviceID(name: String, peripheralUUID: UUID) -> String {
        let upper = name.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        if upper.hasPrefix("RC-") {
            let suffix = upper.dropFirst(3).prefix(4)
            if suffix.count == 4, suffix.allSatisfy({ $0.isASCII && $0.isHexDigit }) {
                return String(suffix)
            }
        }
        return AppConfig.normalizedDeviceID(
            BleProtocol.rcDeviceID(fromPeripheralUUID: peripheralUUID.uuidString)
        )
    }

    func numberOfRows(in tableView: NSTableView) -> Int {
        devices.count
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard row < devices.count, let tableColumn else { return nil }
        let device = devices[row]
        let value: String
        switch tableColumn.identifier.rawValue {
        case "name":
            value = existingDeviceIDs.contains(device.deviceID) ? "\(device.name) (paired)" : device.name
        case "id":
            value = device.deviceID
        case "type":
            switch device.deviceClass {
            case .stickS3:
                value = "Voice Stick"
            case .xiaomiRemote2Pro:
                value = "Xiaomi Remote"
            }
        case "rssi":
            value = "\(device.rssi)"
        default:
            value = ""
        }
        return NSTextField(labelWithString: value)
    }

    @objc private func pairSelectedDevice() {
        let row = tableView.selectedRow
        guard row >= 0, row < devices.count else {
            statusLabel.stringValue = "Select a device"
            return
        }
        guard pairingPeripheral == nil else {
            statusLabel.stringValue = "Pairing in progress"
            return
        }
        let device = devices[row]
        if device.deviceClass == .xiaomiRemote2Pro {
            // 对齐 Windows CanPairCandidate 的 is_existing_device 拒绝语义（拒绝时
            // 不停扫描，用户可改选其他设备）：
            // 同 UUID 已在配对集 → 同一物理设备，不重复发起；
            // 派生 ID 已在配对 ID 集合但 UUID 不同 → SHA-256 派生撞车，拒绝配对。
            if existingPeripheralUUIDs.contains(device.identifier.uuidString.uppercased()) {
                statusLabel.stringValue = "Already paired"
                return
            }
            if existingDeviceIDs.contains(device.deviceID) {
                statusLabel.stringValue = "ID \(device.deviceID) conflicts with another paired device"
                return
            }
        }
        central?.stopScan()
        switch device.deviceClass {
        case .stickS3:
            onPair(PairDeviceResult(
                deviceID: device.deviceID, name: device.name,
                deviceClass: .stickS3, peripheralUUID: nil
            ))
            close()
        case .xiaomiRemote2Pro:
            startXiaomiPairing(device: device)
        }
    }

    // ---- RC 配对流（临时连接触发系统 Bond）----

    /// connect → discoverServices(ATVV) → discoverCharacteristics(Control/Audio) →
    /// 订阅 Control。Control 是加密特征，订阅触发 macOS 系统 Bond 弹窗；
    /// 订阅成功即视为配对成功，随后回调并断开临时连接。
    private func startXiaomiPairing(device: PairingDevice) {
        guard let peripheral = peripherals[device.identifier] else {
            statusLabel.stringValue = "Device lost; keep scanning"
            if let central, central.state == .poweredOn {
                central.scanForPeripherals(withServices: nil)
            }
            return
        }
        pairingDevice = device
        pairingPeripheral = peripheral
        statusLabel.stringValue = "Pairing \(device.name)..."
        peripheral.delegate = self
        central?.connect(peripheral)
        pairingTimeoutTimer = Timer.scheduledTimer(
            withTimeInterval: Self.pairingTimeout, repeats: false
        ) { [weak self] _ in
            self?.failXiaomiPairing("Pairing timed out. Pair manually in System Settings > Bluetooth.")
        }
    }

    private func finishXiaomiPairing(peripheral: CBPeripheral) {
        pairingTimeoutTimer?.invalidate()
        pairingTimeoutTimer = nil
        guard let device = pairingDevice else { return }
        let result = PairDeviceResult(
            deviceID: device.deviceID,
            name: device.name,
            deviceClass: .xiaomiRemote2Pro,
            peripheralUUID: peripheral.identifier.uuidString
        )
        pairingPeripheral = nil
        pairingDevice = nil
        peripheral.delegate = nil
        central?.cancelPeripheralConnection(peripheral)
        onPair(result)
        close()
    }

    private func failXiaomiPairing(_ message: String) {
        pairingTimeoutTimer?.invalidate()
        pairingTimeoutTimer = nil
        if let peripheral = pairingPeripheral {
            peripheral.delegate = nil
            central?.cancelPeripheralConnection(peripheral)
        }
        pairingPeripheral = nil
        pairingDevice = nil
        statusLabel.stringValue = message
        // 恢复扫描，用户可重试或改选其他设备。
        if let central, central.state == .poweredOn {
            central.scanForPeripherals(withServices: nil)
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard peripheral == pairingPeripheral else { return }
        peripheral.discoverServices([CBUUID(string: XiaomiAtvvProtocol.serviceUUID)])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        guard peripheral == pairingPeripheral else { return }
        failXiaomiPairing("Connect failed. Pair manually in System Settings > Bluetooth.")
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        guard peripheral == pairingPeripheral else { return }
        failXiaomiPairing("Disconnected during pairing. Pair manually in System Settings > Bluetooth.")
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard peripheral == pairingPeripheral else { return }
        let atvvServiceUUID = CBUUID(string: XiaomiAtvvProtocol.serviceUUID)
        guard error == nil,
              let service = (peripheral.services ?? []).first(where: { $0.uuid == atvvServiceUUID }) else {
            failXiaomiPairing("ATVV service not found. Pair manually in System Settings > Bluetooth.")
            return
        }
        peripheral.discoverCharacteristics([
            CBUUID(string: XiaomiAtvvProtocol.controlUUID),
            CBUUID(string: XiaomiAtvvProtocol.audioUUID),
        ], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard peripheral == pairingPeripheral else { return }
        let controlUUID = CBUUID(string: XiaomiAtvvProtocol.controlUUID)
        guard error == nil,
              let control = (service.characteristics ?? []).first(where: { $0.uuid == controlUUID }) else {
            failXiaomiPairing("ATVV characteristics missing. Pair manually in System Settings > Bluetooth.")
            return
        }
        peripheral.setNotifyValue(true, for: control)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard peripheral == pairingPeripheral,
              characteristic.uuid == CBUUID(string: XiaomiAtvvProtocol.controlUUID) else { return }
        guard error == nil, characteristic.isNotifying else {
            failXiaomiPairing("Subscribe failed. Pair manually in System Settings > Bluetooth.")
            return
        }
        finishXiaomiPairing(peripheral: peripheral)
    }

    @objc private func cancel() {
        teardown()
        close()
    }

    /// 红叉关窗与 Cancel 按钮同一清理路径（对齐 Windows 关窗=取消语义）。
    func windowWillClose(_ notification: Notification) {
        isClosed = true
        teardown()
    }

    /// 幂等清理：停扫描、取消进行中的配对连接、释放定时器与 peripheral delegate。
    /// 正常完成路径（finishXiaomiPairing 先回调 onPair 并自清状态再关窗）走到
    /// 这里时配对状态已空，仅剩 stopScan 空操作，不会误触发取消语义。
    private func teardown() {
        pairingTimeoutTimer?.invalidate()
        pairingTimeoutTimer = nil
        if let peripheral = pairingPeripheral {
            peripheral.delegate = nil
            central?.cancelPeripheralConnection(peripheral)
            pairingPeripheral = nil
            pairingDevice = nil
        }
        central?.stopScan()
    }

    private var selectedDeviceIdentifier: UUID? {
        let row = tableView.selectedRow
        guard row >= 0, row < devices.count else { return nil }
        return devices[row].identifier
    }

    private func restoreSelection(_ identifier: UUID?) {
        guard let identifier,
              let row = devices.firstIndex(where: { $0.identifier == identifier }) else {
            return
        }
        tableView.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
    }
}
