import { useRef } from 'react'
import type { Job, Loudness, Snap } from '../types'
import { captionsUrl, downloadUrl, formatBytes, formatDuration, previewUrl } from '../api'
import { ACCEPT } from './Dropzone'

const STATUS_LABEL: Record<Job['status'], string> = {
  queued: 'Queued',
  analyzing: 'Analyzing',
  converting: 'Converting',
  done: 'Ready',
  failed: 'Cannot fix',
}

/** The container says a track exists; only this says whether it makes a sound. */
function loudnessText(l: Loudness | null): string {
  if (!l) return 'not measured'
  if (l.samples === 0) return 'decodes to nothing'
  return `${l.verdict} · ${l.lufs.toFixed(1)} LUFS · peak ${l.peakDb.toFixed(1)} dBFS`
}

function loudnessOk(l: Loudness | null): boolean | undefined {
  if (!l) return undefined
  if (l.verdict === 'audible') return true
  if (l.verdict === 'quiet') return undefined
  return false
}

/** Closed captions are never a blocker — present is good, absent is neutral. */
function captionText(s: Snap): string {
  if (!s.capCount) return 'none'
  const tracks = `${s.capCount} track${s.capCount === 1 ? '' : 's'}`
  return `${s.capCodec} · ${tracks}${s.capLang ? ` · ${s.capLang}` : ''}`
}

function Row({ label, value, ok }: { label: string; value: string; ok?: boolean }) {
  return (
    <div className="srow">
      <span className="skey">{label}</span>
      <span className={`sval${ok === false ? ' bad' : ok === true ? ' good' : ''}`}>{value}</span>
    </div>
  )
}

function SnapView({ snap, title }: { snap: Snap | null; title: string }) {
  if (!snap) {
    return (
      <div className="snap">
        <h4>{title}</h4>
        <div className="snap-empty">—</div>
      </div>
    )
  }
  const isH264 = snap.video.startsWith('avc1') || snap.video.startsWith('h264')
  const isAac = snap.audio === 'mp4a' || snap.audio === 'aac'
  return (
    <div className="snap">
      <h4>
        {title}
        <span className={`pill ${snap.verdict === 'PASS' ? 'ok' : 'bad'}`}>{snap.verdict}</span>
      </h4>
      <Row label="container" value={snap.container || '—'} ok={snap.container.startsWith('MP4')} />
      <Row
        label="video"
        value={snap.width ? `${snap.video} ${snap.width}×${snap.height}` : snap.video || 'none'}
        ok={isH264}
      />
      <Row
        label="audio"
        value={snap.hasAudio ? `${snap.audio} ${snap.aacProfile}`.trim() : 'NO AUDIO TRACK'}
        ok={snap.hasAudio && isAac}
      />
      <Row
        label="channels"
        value={snap.hasAudio ? `${snap.ch === 1 ? 'mono' : snap.ch === 2 ? 'stereo' : `${snap.ch}ch`} · ${snap.rate || '—'} Hz` : '—'}
        ok={snap.ch === 2 && (snap.rate === 48000 || snap.rate === 44100)}
      />
      <Row label="loudness" value={loudnessText(snap.loudness)} ok={loudnessOk(snap.loudness)} />
      <Row label="captions" value={captionText(snap)} ok={snap.capCount > 0 ? true : undefined} />
      <Row label="faststart" value={snap.faststart ? 'yes' : 'no'} ok={snap.faststart} />
      <Row
        label="bitrate"
        value={`${snap.vkbps || '—'} / ${snap.akbps || '—'} kbps`}
      />
    </div>
  )
}

interface Props {
  job: Job
  onRemove: (id: number) => void
  /** Replace this job with a freshly picked file — the usual answer to a FAIL. */
  onReplace: (job: Job, file: File) => void
}

export function JobCard({ job, onRemove, onReplace }: Props) {
  const picker = useRef<HTMLInputElement>(null)
  const active = job.status === 'converting' || job.status === 'analyzing' || job.status === 'queued'
  const issues = job.before?.fail ?? []
  const warns = job.before?.warn ?? []

  return (
    <article className={`card status-${job.status}`}>
      <header className="card-head">
        <div className="ctitle">
          <span className="cname" title={job.name}>{job.name}</span>
          <span className="cmeta">
            {formatBytes(job.inBytes)}
            {job.before?.seconds ? ` · ${formatDuration(job.before.seconds)}` : ''}
            {job.outBytes ? ` → ${formatBytes(job.outBytes)}` : ''}
          </span>
        </div>
        <span className={`chip ${job.status}`}>{STATUS_LABEL[job.status]}</span>
      </header>

      {active && (
        <div className="progress">
          <span className={`bar${job.status !== 'converting' ? ' indet' : ''}`}>
            <span className="bar-fill" style={{ width: `${job.status === 'converting' ? job.pct : 100}%` }} />
          </span>
          <span className="ppct">
            {job.status === 'converting' ? `${Math.round(job.pct)}%` : STATUS_LABEL[job.status] + '…'}
          </span>
        </div>
      )}

      {job.plan && <p className="plan">{job.plan}</p>}

      <div className="snaps">
        <SnapView snap={job.before} title="Uploaded" />
        <div className="arrow" aria-hidden>→</div>
        <SnapView snap={job.after} title="Fixed" />
      </div>

      {(issues.length > 0 || warns.length > 0 || job.message) && (
        <ul className="issues">
          {issues.map((t, i) => (
            <li key={`f${i}`} className="bad">{t}</li>
          ))}
          {warns.map((t, i) => (
            <li key={`w${i}`} className="warn">{t}</li>
          ))}
          {job.message && job.status === 'failed' && <li className="bad">{job.message}</li>}
          {job.message && job.status === 'done' && <li className="warn">{job.message}</li>}
        </ul>
      )}

      {job.downloadable && (
        <video className="player" src={previewUrl(job.id)} controls preload="metadata" playsInline>
          {/* The mov_text track inside the MP4 renders in no browser; the sidecar does. */}
          {job.captions && (
            <track
              kind="captions"
              default
              src={captionsUrl(job.id)}
              srcLang={job.after?.capLang || undefined}
              label={job.after?.capLang ? `Captions (${job.after.capLang})` : 'Captions'}
            />
          )}
        </video>
      )}

      <div className="card-actions">
        {job.downloadable && (
          <a className="btn" href={downloadUrl(job.id)} download>
            <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" strokeWidth="2">
              <path d="M12 4v11m0 0l-4-4m4 4l4-4" strokeLinecap="round" strokeLinejoin="round" />
              <path d="M4 19h16" strokeLinecap="round" />
            </svg>
            Download fixed MP4
          </a>
        )}
        <input
          ref={picker}
          type="file"
          accept={ACCEPT}
          hidden
          onChange={(e) => {
            const f = e.target.files?.[0]
            e.target.value = ''
            if (f) onReplace(job, f)
          }}
        />
        <button className="btn ghost" onClick={() => picker.current?.click()}>
          Re-upload
        </button>
        <button className="btn ghost danger" onClick={() => onRemove(job.id)}>
          Remove
        </button>
      </div>
    </article>
  )
}
