export type Verdict = 'PASS' | 'FAIL'

/** What the audio actually sounds like, measured by decoding it. */
export type Heard = 'not measured' | 'decodes to nothing' | 'silent' | 'inaudible' | 'quiet' | 'audible'

export interface Loudness {
  verdict: Heard
  samples: number
  /** dBFS. */
  peakDb: number
  /** EBU R128 integrated — the number that predicts what a listener hears. */
  lufs: number
}

export interface Snap {
  container: string
  video: string
  audio: string
  aacProfile: string
  /** Caption tracks, chapter lists excluded. */
  capCount: number
  capCodec: string
  /** ISO-639-2/T from mdhd; empty when the file does not say. */
  capLang: string
  verdict: Verdict
  rate: number
  ch: number
  width: number
  height: number
  seconds: number
  vkbps: number
  akbps: number
  faststart: boolean
  hasAudio: boolean
  loudness: Loudness | null
  fail: string[]
  warn: string[]
}

export type JobStatus = 'queued' | 'analyzing' | 'converting' | 'done' | 'failed'

export interface Job {
  id: number
  name: string
  status: JobStatus
  pct: number
  message: string
  plan: string
  inBytes: number
  outBytes: number
  downloadable: boolean
  /** A WebVTT sidecar was extracted from the fixed file — see captionsUrl. */
  captions: boolean
  before: Snap | null
  after: Snap | null
}

/** A file being pushed to the server; becomes a Job once the POST returns. */
export interface PendingUpload {
  key: string
  name: string
  bytes: number
  pct: number
}
