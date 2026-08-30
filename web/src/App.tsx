import { useCallback, useEffect, useState } from 'react'
import { Dropzone } from './components/Dropzone'
import { JobCard } from './components/JobCard'
import { clearJobs, deleteJob, reportUrl, subscribeJobs, uploadFiles } from './api'
import type { Job, PendingUpload } from './types'
import './App.css'

const SPEC = ['MP4', 'H.264 / avc1', 'AAC-LC stereo', '48 kHz', 'faststart']

export default function App() {
  const [jobs, setJobs] = useState<Job[]>([])
  const [pending, setPending] = useState<PendingUpload[]>([])
  const [live, setLive] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    const stop = subscribeJobs(
      (next) => {
        setLive(true)
        setJobs(next)
      },
      () => setLive(false),
    )
    return stop
  }, [])

  const handleFiles = useCallback(async (files: File[]) => {
    const batch: PendingUpload[] = files.map((f, i) => ({
      key: `${Date.now()}-${i}-${f.name}`,
      name: f.name,
      bytes: f.size,
      pct: 0,
    }))
    setPending((p) => [...p, ...batch])
    setError(null)

    const keys = new Set(batch.map((b) => b.key))
    try {
      await uploadFiles(files, (pct) =>
        setPending((p) => p.map((u) => (keys.has(u.key) ? { ...u, pct } : u))),
      )
    } catch (e) {
      setError(e instanceof Error ? e.message : 'upload failed')
    } finally {
      setPending((p) => p.filter((u) => !keys.has(u.key)))
    }
  }, [])

  const handleRemove = useCallback(async (id: number) => {
    setJobs((js) => js.filter((j) => j.id !== id))  // optimistic; the SSE frame confirms
    try {
      await deleteJob(id)
    } catch (e) {
      setError(e instanceof Error ? e.message : 'could not remove file')
    }
  }, [])

  // Upload the replacement first: a failed upload leaves the original card in place.
  const handleReplace = useCallback(
    async (job: Job, file: File) => {
      await handleFiles([file])
      await handleRemove(job.id)
    },
    [handleFiles, handleRemove],
  )

  const handleClear = useCallback(async () => {
    if (!window.confirm('Remove every file from the queue? Fixed files not yet downloaded are deleted.')) return
    setJobs([])
    try {
      await clearJobs()
    } catch (e) {
      setError(e instanceof Error ? e.message : 'could not clear the queue')
    }
  }, [])

  const ready = jobs.filter((j) => j.downloadable)
  const failed = jobs.filter((j) => j.status === 'failed')
  const working = jobs.some((j) => j.status === 'converting' || j.status === 'analyzing')

  return (
    <div className="app">
      <header className="masthead">
        <div className="brand">
          <h1>Video Conformance</h1>
          <span className={`live${live ? ' on' : ''}`}>{live ? 'connected' : 'offline'}</span>
        </div>
        <p className="lede">
          Upload reference videos. Every file is parsed atom-by-atom, re-encoded to the streaming spec,
          and handed back ready to upload to the backend.
        </p>
        <div className="chips">
          {SPEC.map((s) => (
            <span key={s} className="spec">{s}</span>
          ))}
        </div>
      </header>

      <Dropzone onFiles={handleFiles} pending={pending} />

      {error && <div className="banner bad">{error}</div>}
      {!live && (
        <div className="banner warn">
          Backend not reachable — start it with <code>./mp4web</code> in the project root.
        </div>
      )}

      {jobs.length > 0 && (
        <div className="toolbar">
          <span className="count">
            {jobs.length} file{jobs.length === 1 ? '' : 's'}
            {ready.length > 0 && ` · ${ready.length} ready`}
            {failed.length > 0 && ` · ${failed.length} need re-export`}
            {working && ' · working…'}
          </span>
          <div className="tools">
            {ready.length > 1 && (
              <button
                className="btn ghost"
                onClick={() =>
                  ready.forEach((j, i) =>
                    setTimeout(() => {
                      const a = document.createElement('a')
                      a.href = `/api/download/${j.id}`
                      a.download = ''
                      a.click()
                    }, i * 400),
                  )
                }
              >
                Download all ({ready.length})
              </button>
            )}
            <a className="btn ghost" href={reportUrl} download>
              Audit report .csv
            </a>
            <button className="btn ghost danger" onClick={handleClear}>
              Clear all
            </button>
          </div>
        </div>
      )}

      <div className="jobs">
        {jobs
          .slice()
          .reverse()
          .map((j) => (
            <JobCard key={j.id} job={j} onRemove={handleRemove} onReplace={handleReplace} />
          ))}
      </div>

      {jobs.length === 0 && pending.length === 0 && (
        <p className="empty">
          Nothing queued yet. Files never leave this machine — the encoder runs locally.
        </p>
      )}
    </div>
  )
}
