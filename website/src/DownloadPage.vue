<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { downloads, loadDownloads, formatBytes, formatDate } from './downloads'

const { t, locale } = useI18n()
loadDownloads()

const state = computed(() => downloads.value.state)
const data = computed(() => downloads.value.data)
const latest = computed(() => data.value?.latest || null)
const releases = computed(() => data.value?.releases || [])
const releaseUrl = 'https://github.com/Haobot/VoiceStickPlus/releases/latest'

// 平台分组顺序即展示顺序；空组整组隐藏（如近期 Release 无 macOS 资产）
const PLATFORMS = ['windows', 'macos', 'firmware']

function assetsOf(entry, platform) {
  return (entry?.assets || []).filter((asset) => asset.platform === platform)
}

function platformTitle(platform) {
  return t(`downloads.platform.${platform}`)
}

// Windows 组里按站点语言把对应 culture 的 MSI 放前面并高亮
function sortedWindowsAssets(entry) {
  const assets = assetsOf(entry, 'windows')
  const preferred = locale.value === 'zh-CN' ? 'zh-CN.msi' : 'en-US.msi'
  return [...assets].sort((a, b) => Number(b.name.endsWith(preferred)) - Number(a.name.endsWith(preferred)))
}

function assetLabel(asset) {
  return asset.name
}

function isPrimaryWindows(asset) {
  const preferred = locale.value === 'zh-CN' ? 'zh-CN.msi' : 'en-US.msi'
  return asset.platform === 'windows' && asset.name.endsWith(preferred)
}
</script>

<template>
  <section class="downloads">
    <div class="section-inner downloads-inner">
      <header class="downloads-header">
        <p class="eyebrow">{{ t('downloads.eyebrow') }}</p>
        <h1>{{ t('downloads.title') }}</h1>
        <p class="lead">{{ t('downloads.lead') }}</p>
        <a class="downloads-github" :href="releaseUrl">{{ t('downloads.viewOnGithub') }}</a>
      </header>

      <p v-if="state === 'loading'" class="downloads-status">{{ t('downloads.loading') }}</p>

      <div v-else-if="state === 'error'" class="downloads-status error">
        <p>{{ t('downloads.loadFailed') }}</p>
        <a class="button secondary" :href="releaseUrl">{{ t('downloads.viewOnGithub') }}</a>
      </div>

      <template v-else>
        <article class="downloads-card">
          <div class="downloads-card-head">
            <h2>{{ t('downloads.latest') }} {{ latest.version }}</h2>
            <div class="downloads-card-meta">
              <span>{{ t('downloads.releasedAt', { date: formatDate(latest.date, locale) }) }}</span>
              <span v-if="latest.prerelease" class="downloads-badge">{{ t('downloads.prerelease') }}</span>
              <span v-if="latest.min_firmware_version" class="downloads-minfw">
                {{ t('downloads.minFirmware', { version: latest.min_firmware_version }) }}
              </span>
            </div>
          </div>

          <div v-for="platform in PLATFORMS" :key="platform" class="downloads-group">
            <template v-if="assetsOf(latest, platform).length">
              <h3>{{ platformTitle(platform) }}</h3>
              <ul class="downloads-assets">
                <li v-for="asset in (platform === 'windows' ? sortedWindowsAssets(latest) : assetsOf(latest, platform))"
                    :key="asset.name" :class="{ primary: isPrimaryWindows(asset) }">
                  <a class="downloads-asset-link"
                     :class="{ 'button primary': isPrimaryWindows(asset), 'button secondary': !isPrimaryWindows(asset) }"
                     :href="asset.url">
                    {{ assetLabel(asset) }}
                  </a>
                  <span class="downloads-asset-meta">
                    <span v-if="asset.size">{{ formatBytes(asset.size) }}</span>
                    <a v-if="asset.sha256" :href="asset.sha256" class="downloads-checksum">{{ t('downloads.checksum') }}</a>
                  </span>
                </li>
              </ul>
            </template>
          </div>
        </article>

        <section class="downloads-history">
          <h2>{{ t('downloads.history') }}</h2>
          <details v-for="entry in releases" :key="entry.version" class="downloads-release">
            <summary>
              <span class="downloads-release-version">v{{ entry.version }}</span>
              <span class="downloads-release-meta">
                {{ formatDate(entry.date, locale) }}
                <span v-if="entry.prerelease" class="downloads-badge">{{ t('downloads.prerelease') }}</span>
              </span>
            </summary>
            <div class="downloads-release-body">
              <p v-if="entry.notes" class="downloads-notes">{{ entry.notes }}</p>
              <ul class="downloads-assets compact">
                <li v-for="asset in entry.assets" :key="asset.name">
                  <a class="button secondary" :href="asset.url">{{ asset.name }}</a>
                  <span class="downloads-asset-meta">
                    <span v-if="asset.size">{{ formatBytes(asset.size) }}</span>
                    <a v-if="asset.sha256" :href="asset.sha256" class="downloads-checksum">{{ t('downloads.checksum') }}</a>
                  </span>
                </li>
              </ul>
            </div>
          </details>
        </section>
      </template>
    </div>
  </section>
</template>
