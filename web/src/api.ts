import type { Job } from './types'

/** Live job list over SSE. Returns an unsubscribe function. */
export function subscribeJobs(onJobs: (jobs: Job[]) => void, onError?: () => void): () => void {
  const es = new EventSource('/api/events')
  es.onmessage = (e) => {
    try {
      onJobs(JSON.parse(e.data) as Job[])
    } catch {
      /* ignore malformed frame */
    }
  }
  es.onerror = () => onError?.()
  return () => es.close()
}

/** Upload files with real byte-level progress (fetch cannot report upload progress). */
export function uploadFiles(files: File[], onProgress: (pct: number) => void): Promise<void> {
  return new Promise((resolve, reject) => {
    const form = new FormData()
    files.forEach((f) => form.append('file', f, f.name))

    const xhr = new XMLHttpRequest()
    xhr.open('POST', '/api/upload')
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) onProgress((e.loaded / e.total) * 100)
    }
    xhr.onload = () => (xhr.status >= 200 && xhr.status < 300 ? resolve() : reject(new Error(`HTTP ${xhr.status}`)))
    xhr.onerror = () => reject(new Error('network error'))
    xhr.send(form)
  })
}

/** Forget one job and delete its files on the server. */
export async function deleteJob(id: number): Promise<void> {
  const r = await fetch(`/api/job/${id}`, { method: 'DELETE' })
  if (!r.ok) throw new Error(`could not remove job ${id}`)
}

/** Forget every job. */
export async function clearJobs(): Promise<void> {
  const r = await fetch('/api/jobs', { method: 'DELETE' })
  if (!r.ok) throw new Error('could not clear jobs')
}

export const downloadUrl = (id: number) => `/api/download/${id}`
/** Same bytes as downloadUrl, served inline and seekable so <video> can play it. */
export const previewUrl = (id: number) => `/api/preview/${id}`
/** WebVTT sidecar for the fixed file — the only caption form <video> renders. */
export const captionsUrl = (id: number) => `/api/captions/${id}`
export const reportUrl = '/api/report.csv'

export function formatBytes(n: number): string {
  if (!n) return '—'
  const u = ['B', 'KB', 'MB', 'GB']
  let i = 0
  let v = n
  while (v >= 1024 && i < u.length - 1) {
    v /= 1024
    i++
  }
  return `${v.toFixed(v < 10 && i > 0 ? 1 : 0)} ${u[i]}`
}

export function formatDuration(s: number): string {
  if (!s || s < 0) return '—'
  const m = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  return m > 0 ? `${m}:${String(sec).padStart(2, '0')}` : `${s.toFixed(1)}s`
}
