import Foundation
import VoiceStickCore

// N1 第四刀：displayName 回留 App（tr 本地化=UI 层关注点）；核心类型在 Core。

extension OutputTarget {
    var displayName: String {
        switch self {
        case .focusedApp:
            return tr(.outputFocusedApp)
        case .subtitle:
            return tr(.outputSubtitle)
        }
    }
}

extension TextTransform {
    var displayName: String {
        switch self {
        case .original:
            return tr(.textOriginal)
        case .translate:
            return tr(.menuTranslation)
        }
    }
}

extension ImuWakeSensitivity {
    var displayName: String {
        switch self {
        case .low: return tr(.wakeLow)
        case .medium: return tr(.wakeMedium)
        case .high: return tr(.wakeHigh)
        }
    }
}

extension EncoderLedColor {
    var displayName: String {
        switch self {
        case .red: return tr(.ledRed)
        case .green: return tr(.ledGreen)
        case .blue: return tr(.ledBlue)
        case .yellow: return tr(.ledYellow)
        case .purple: return tr(.ledPurple)
        case .cyan: return tr(.ledCyan)
        case .white: return tr(.ledWhite)
        case .off: return tr(.ledOff)
        }
    }
}

extension EncoderButtonAction {
    var displayName: String {
        switch self {
        case .recording: return tr(.actionRecording)
        case .key: return tr(.actionCustomKey)
        }
    }
}



// N1 闸6b：Overlay 三枚举 displayName 随类型下沉迁入。

extension OverlayThemeColor {
    var displayName: String {
        switch self {
        case .auto:
            return tr(.themeAuto)
        case .white:
            return tr(.themeWhite)
        case .black:
            return tr(.themeBlack)
        case .pink:
            return tr(.themePink)
        case .green:
            return tr(.themeGreen)
        case .yellow:
            return tr(.themeYellow)
        case .blue:
            return tr(.themeBlue)
        case .purple:
            return tr(.themePurple)
        }
    }
}

extension OverlayThemeSize {
    var displayName: String {
        switch self {
        case .big:
            return tr(.sizeBig)
        case .medium:
            return tr(.sizeMedium)
        case .small:
            return tr(.sizeSmall)
        }
    }
}

extension OverlayPosition {
    var displayName: String {
        switch self {
        case .center:
            return tr(.positionCenter)
        case .bottomCenter:
            return tr(.positionBottomCenter)
        case .topLeft:
            return tr(.positionTopLeft)
        case .topRight:
            return tr(.positionTopRight)
        case .bottomLeft:
            return tr(.positionBottomLeft)
        case .bottomRight:
            return tr(.positionBottomRight)
        }
    }
}

