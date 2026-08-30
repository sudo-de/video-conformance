import { useCallback, useRef, useState } from 'react'
import type { PendingUpload } from '../types'
import { formatBytes } from '../api'

interface Props {
  onFiles: (files: File[]) => void
  pending: PendingUpload[]
  disabled?: boolean
}

export const ACCEPT = '.mp4,.mov,.m4v,.webm,.mkv,.avi,.ts,video/*'

export function Dropzone({ onFiles, pending, disabled }: Props) {
  const [over, setOver] = useState(false)
  const input = useRef<HTMLInputElement>(null)

  const take = useCallback(
    (list: FileList | null) => {
      if (!list?.length) return
      onFiles(Array.from(list))
    },
    [onFiles],
  )

  return (
    <section
      className={`dropzone${over ? ' over' : ''}${disabled ? ' busy' : ''}`}
      onDragOver={(e) => {
        e.preventDefault()
        setOver(true)
      }}
      onDragLeave={() => setOver(false)}
      onDrop={(e) => {
        e.preventDefault()
        setOver(false)
        if (!disabled) take(e.dataTransfer.files)
      }}
      onClick={() => !disabled && input.current?.click()}
      role="button"
      tabIndex={0}
      onKeyDown={(e) => e.key === 'Enter' && input.current?.click()}
    >
      <input
        ref={input}
        type="file"
        multiple
        accept={ACCEPT}
        hidden
        onChange={(e) => {
          take(e.target.files)
          e.target.value = ''
        }}
      />

      <div className="dz-icon" aria-hidden>
        <svg viewBox="0 0 24 24" width="34" height="34" fill="none" stroke="currentColor" strokeWidth="1.6">
          <path d="M12 16V4m0 0L7.5 8.5M12 4l4.5 4.5" strokeLinecap="round" strokeLinejoin="round" />
          <path d="M3.5 15v3A2.5 2.5 0 0 0 6 20.5h12a2.5 2.5 0 0 0 2.5-2.5v-3" strokeLinecap="round" />
        </svg>
      </div>
      <p className="dz-title">Drop reference videos here</p>
      <p className="dz-sub">
        or click to browse · MP4, MOV, WebM, MKV, AVI · multiple files supported
      </p>

      {pending.length > 0 && (
        <ul className="dz-pending" onClick={(e) => e.stopPropagation()}>
          {pending.map((p) => (
            <li key={p.key}>
              <span className="dz-pname">{p.name}</span>
              <span className="dz-psize">{formatBytes(p.bytes)}</span>
              <span className="bar bar-sm">
                <span className="bar-fill" style={{ width: `${p.pct}%` }} />
              </span>
              <span className="dz-ppct">{Math.round(p.pct)}%</span>
            </li>
          ))}
        </ul>
      )}
    </section>
  )
}
