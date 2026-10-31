// 下载页与 hero 按钮共享的 downloads.json 数据源（模块级单例，两个组件复用同一请求）。
// 失败/加载中时 hero 按钮回退到构建期 VERSION 拼接的 URL（见 App.vue），
// 下载页显示降级 UI 与 GitHub Releases 直链。
import { ref } from 'vue'

export const downloads = ref({ state: 'loading', data: null })

let started = false

export function loadDownloads() {
  if (started) {
    return
  }
  started = true
  const url = `${import.meta.env.BASE_URL}downloads.json`
  fetch(url, { cache: 'no-store' })
    .then((response) => (response.ok ? response.json() : Promise.reject(new Error(`HTTP ${response.status}`))))
    .then((data) => {
      downloads.value = { state: 'ready', data }
    })
    .catch(() => {
      downloads.value = { state: 'error', data: null }
    })
}

export function pickLatestAsset(data, predicate) {
  // 从新到旧扫描各版本，返回第一个匹配的资产：latest 可能没有某平台资产
  // （如 macOS CI 暂停期间），此时应指向最近一个实际发布的该平台产物，
  // 而不是构建期 VERSION 拼出的可能未发布的 URL
  const releases = data?.releases
  if (Array.isArray(releases)) {
    for (const release of releases) {
      const found = release.assets?.find(predicate)
      if (found) {
        return found
      }
    }
    return null
  }
  const assets = data?.latest?.assets
  if (!Array.isArray(assets)) {
    return null
  }
  return assets.find(predicate) || null
}

export function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) {
    return ''
  }
  if (bytes < 1024 * 1024) {
    return `${Math.round(bytes / 1024)} KB`
  }
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`
}

export function formatDate(iso, locale) {
  const date = new Date(iso)
  if (Number.isNaN(date.getTime())) {
    return iso || ''
  }
  return date.toLocaleDateString(locale === 'zh-CN' ? 'zh-CN' : 'en-US', {
    year: 'numeric',
    month: 'short',
    day: 'numeric',
  })
}
