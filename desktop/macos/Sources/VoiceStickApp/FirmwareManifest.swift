import CryptoKit
import Foundation

struct FirmwareManifest: Decodable {
    let hardware: String
    let version: String
    /// 最低兼容固件版本（manifest 下发；缺失时回退本地常量）。
    let minVersion: String?
    let otaURL: URL
    /// 主源失败时的回退源（GitHub Release 直链）；老 manifest 可能缺失。
    let otaURLFallback: URL?
    let otaSHA256: String
    let otaSize: Int
    let mergedURL: URL?
    let mergedURLFallback: URL?
    let mergedSHA256: String?
    let mergedSize: Int?

    enum CodingKeys: String, CodingKey {
        case hardware
        case version
        case minVersion = "min_version"
        case otaURL = "ota_url"
        case otaURLFallback = "ota_url_fallback"
        case otaSHA256 = "ota_sha256"
        case otaSize = "ota_size"
        case mergedURL = "merged_url"
        case mergedURLFallback = "merged_url_fallback"
        case mergedSHA256 = "merged_sha256"
        case mergedSize = "merged_size"
    }

    /// manifest 下发的最低兼容版本优先；空/缺失时用调用方回退值。
    func effectiveMinimumVersion(fallback: String) -> String {
        let trimmed = minVersion?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return trimmed.isEmpty ? fallback : trimmed
    }
}

struct DeviceFirmwareInfo {
    var hardware: String?
    var currentVersion: String?
    var latestVersion: String?
    var updateAvailable = false
    var isChecking = false
    var errorMessage: String?
}

enum FirmwareVersion {
    static func isVersion(_ current: String, olderThan latest: String) -> Bool {
        guard let current = ParsedVersion(current), let latest = ParsedVersion(latest) else {
            return false
        }
        return current < latest
    }

    private struct ParsedVersion: Comparable {
        let numbers: [Int]
        let suffix: String?

        init?(_ text: String) {
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !trimmed.isEmpty else { return nil }
            let parts = trimmed.split(separator: "-", maxSplits: 1).map(String.init)
            let numberParts = parts[0].split(separator: ".").map(String.init)
            guard !numberParts.isEmpty else { return nil }
            var parsedNumbers: [Int] = []
            for part in numberParts {
                guard let value = Int(part) else { return nil }
                parsedNumbers.append(value)
            }
            numbers = parsedNumbers
            suffix = parts.count > 1 ? parts[1] : nil
        }

        static func < (lhs: ParsedVersion, rhs: ParsedVersion) -> Bool {
            let count = max(lhs.numbers.count, rhs.numbers.count)
            for index in 0..<count {
                let left = index < lhs.numbers.count ? lhs.numbers[index] : 0
                let right = index < rhs.numbers.count ? rhs.numbers[index] : 0
                if left != right {
                    return left < right
                }
            }

            switch (lhs.suffix, rhs.suffix) {
            case (.some, nil):
                return true
            case (nil, .some):
                return false
            case let (.some(left), .some(right)):
                return left.localizedStandardCompare(right) == .orderedAscending
            case (nil, nil):
                return false
            }
        }
    }
}

final class FirmwareManifestClient {
    enum FirmwareManifestError: LocalizedError {
        case invalidResponse
        case checksumMismatch
        case sizeMismatch

        var errorDescription: String? {
            switch self {
            case .invalidResponse:
                return "Firmware update server returned an invalid response."
            case .checksumMismatch:
                return "Firmware checksum did not match the manifest."
            case .sizeMismatch:
                return "Firmware size did not match the manifest."
            }
        }
    }

    private let manifestURL: URL

    init(manifestURL: URL = AppConfig.firmwareManifestURL) {
        self.manifestURL = manifestURL
    }

    func fetchManifest(completion: @escaping (Result<FirmwareManifest, Error>) -> Void) {
        var request = URLRequest(
            url: manifestURL,
            cachePolicy: .reloadIgnoringLocalAndRemoteCacheData
        )
        request.setValue("no-cache", forHTTPHeaderField: "Cache-Control")
        request.setValue("no-cache", forHTTPHeaderField: "Pragma")

        URLSession.shared.dataTask(with: request) { data, response, error in
            if let error {
                completion(.failure(error))
                return
            }
            guard let data,
                  let httpResponse = response as? HTTPURLResponse,
                  (200..<300).contains(httpResponse.statusCode) else {
                completion(.failure(FirmwareManifestError.invalidResponse))
                return
            }

            do {
                let manifest = try JSONDecoder().decode(FirmwareManifest.self, from: data)
                completion(.success(manifest))
            } catch {
                completion(.failure(error))
            }
        }.resume()
    }

    func downloadOTA(from manifest: FirmwareManifest, completion: @escaping (Result<Data, Error>) -> Void) {
        // 主源失败且 manifest 带 fallback 时自动换源（与 Windows 的下载源序列对齐）。
        downloadAndVerify(url: manifest.otaURL, manifest: manifest) { [weak self] result in
            switch result {
            case .success:
                completion(result)
            case .failure(let primaryError):
                guard let self, let fallback = manifest.otaURLFallback else {
                    completion(.failure(primaryError))
                    return
                }
                NSLog("Firmware OTA primary source failed, retrying fallback: \(primaryError.localizedDescription)")
                self.downloadAndVerify(url: fallback, manifest: manifest) { fallbackResult in
                    switch fallbackResult {
                    case .success:
                        completion(fallbackResult)
                    case .failure:
                        completion(.failure(primaryError))
                    }
                }
            }
        }
    }

    private func downloadAndVerify(url: URL, manifest: FirmwareManifest,
                                   completion: @escaping (Result<Data, Error>) -> Void) {
        URLSession.shared.dataTask(with: url) { data, response, error in
            if let error {
                completion(.failure(error))
                return
            }
            guard let data,
                  let httpResponse = response as? HTTPURLResponse,
                  (200..<300).contains(httpResponse.statusCode) else {
                completion(.failure(FirmwareManifestError.invalidResponse))
                return
            }
            guard data.count == manifest.otaSize else {
                completion(.failure(FirmwareManifestError.sizeMismatch))
                return
            }
            let digest = SHA256.hash(data: data)
                .map { String(format: "%02x", $0) }
                .joined()
            guard digest.lowercased() == manifest.otaSHA256.lowercased() else {
                completion(.failure(FirmwareManifestError.checksumMismatch))
                return
            }
            completion(.success(data))
        }.resume()
    }
}
