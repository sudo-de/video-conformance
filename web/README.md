# Frontend

React + TypeScript + Vite UI for the conformance server. See the
[project README](../README.md) for the full picture.

```sh
npm install
npm run dev     # needs the C++ API running: ../mp4web 8787
npm run build   # outputs to dist/, served by mp4web --dist
```

`vite.config.ts` proxies `/api` to `http://127.0.0.1:8787`.
