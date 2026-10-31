import Foundation

// 拼音同音/近音判定（自 Windows pinyin_guard.{h,cc} 1:1 移植；拼音数据
// PinyinData 同表同口径）。语义以 M0 spike 实测验证口径为准：
//   1. 码点完全相同 → 近音；
//   2. 任一方非 CJK 基本区汉字 → ASCII 折叠大小写比较（非 ASCII 判不等=保守拒绝）；
//   3. 双方汉字且韵母集合有交集 → 同音（声母不参与——spike 实测口径，
//      「渍 zì/词 cí」「马 mǎ/打 dǎ」声母不同仍须放行）；
//   4. 韵母命中模糊音对表（an/ang 等鼻音混淆、e/i 卷舌弱化）且声母集合
//      有交集 → 近音（设 shè/识 shí）。
// 拼音数据查不到的字按不同音处理（保守拒绝）。
//
// 本移植只取纠错对入表闸门所需的同音/近音判定基线（sameOrNear/sameOrNearText）；
// Windows 侧 NearVariantText/HotwordAligned/ApplyPinyinCorrections 服务于跨轮
// 纠错指令管线（锚点/攻击面语义），纠错对场景不需要，未移植。

public enum PinyinGuard {
    // 模糊音韵母对（M0 spike 实测口径）：an/ang 等鼻音前后混淆、e/i 卷舌弱化
    // （设/识、这/之、车/吃）。声母模糊对（n/l 等）在「韵母有交集即同音」口径下
    // 是死分支，不移植。
    private static let fuzzyFinalPairs: [(String, String)] = [
        ("an", "ang"), ("en", "eng"), ("in", "ing"),
        ("ian", "iang"), ("uan", "uang"), ("e", "i")
    ]

    private static func isCjk(_ cp: UInt32) -> Bool {
        return cp >= 0x4e00 && cp <= 0x9fff
    }

    private static func toLowerAscii(_ cp: UInt32) -> UInt32 {
        return (cp >= 65 && cp <= 90) ? cp + 32 : cp
    }

    /// 逗号分隔列表（PinyinEntry 的 initials/finals 格式）中是否含 item。
    private static func listContains(_ list: String, _ item: String) -> Bool {
        return list.split(separator: ",").contains { String($0) == item }
    }

    private static func setsIntersect(_ a: String, _ b: String) -> Bool {
        return a.split(separator: ",").contains { listContains(b, String($0)) }
    }

    private static func fuzzyFinalHit(_ ea: PinyinEntry, _ eb: PinyinEntry) -> Bool {
        for (x, y) in fuzzyFinalPairs {
            let forward = listContains(ea.finals, x) && listContains(eb.finals, y)
            let backward = listContains(ea.finals, y) && listContains(eb.finals, x)
            if forward || backward { return true }
        }
        return false
    }

    /// 单字同音/近音判定（Windows PinyinSameOrNear 同口径）。
    public static func sameOrNear(_ a: Unicode.Scalar, _ b: Unicode.Scalar) -> Bool {
        let x = a.value
        let y = b.value
        if x == y { return true }
        guard isCjk(x), isCjk(y) else {
            // 非双方汉字：ASCII 折叠大小写后相等（非 ASCII 大小写折叠判不等=保守拒绝）。
            return toLowerAscii(x) == toLowerAscii(y)
        }
        guard let ea = PinyinData.find(x), let eb = PinyinData.find(y) else {
            return false  // 表外字按不同音
        }
        // 同音（M0 spike 实测口径）：韵母集合有交集即放行——声母不参与。
        if setsIntersect(ea.finals, eb.finals) { return true }
        // 近音：韵母命中模糊对且声母集合有交集（设 shè/识 shí）。
        return setsIntersect(ea.initials, eb.initials) && fuzzyFinalHit(ea, eb)
    }

    /// 等长（码点数）文本逐字近音判定：任一位置非近音即 false；码点数不等 false；
    /// 两个空串 true（Windows SameOrNearText 同口径）。
    public static func sameOrNearText(_ a: String, _ b: String) -> Bool {
        let va = Array(a.unicodeScalars)
        let vb = Array(b.unicodeScalars)
        guard va.count == vb.count else { return false }
        for (x, y) in zip(va, vb) where !sameOrNear(x, y) {
            return false
        }
        return true
    }
}
