import AppKit

/// 前台应用追踪（三期按键映射按前台应用切换的底座）：NSWorkspace 激活通知 →
/// 缓存 bundle id + 变更回调。AppDelegate 经 `onActiveAppChange` 重新 resolve
/// 有效按键映射并刷新拦截层；回调与读写均发生在主线程（通知 queue: .main）。
final class FrontmostAppProvider {
    /// 当前前台应用 bundle id；取不到（登录窗等）为空串。
    private(set) var currentBundleIdentifier: String
    var onActiveAppChange: ((String) -> Void)?
    private var observer: NSObjectProtocol?

    init() {
        currentBundleIdentifier = NSWorkspace.shared.frontmostApplication?.bundleIdentifier ?? ""
        observer = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didActivateApplicationNotification,
            object: nil,
            queue: .main
        ) { [weak self] notification in
            guard let self else { return }
            let app = notification.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication
            let bundleID = app?.bundleIdentifier ?? ""
            guard bundleID != self.currentBundleIdentifier else { return }
            self.currentBundleIdentifier = bundleID
            self.onActiveAppChange?(bundleID)
        }
    }

    deinit {
        if let observer {
            NSWorkspace.shared.notificationCenter.removeObserver(observer)
        }
    }
}
