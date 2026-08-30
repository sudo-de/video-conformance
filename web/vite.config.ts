import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// The C++ API server runs on :8787; the dev server proxies /api to it so the
// React app can be developed with HMR against the real backend.
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:8787',
        changeOrigin: true,
      },
    },
  },
  build: { outDir: 'dist' },
})
