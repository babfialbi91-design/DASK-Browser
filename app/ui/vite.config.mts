import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Two HTML entry points served inside CEF:
//   index.html  -> the browser toolbar (tab strip + navigation bar)
//   newtab.html -> the New Tab page shown in content tabs
export default defineConfig({
  plugins: [react()],
  base: './',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    rollupOptions: {
      input: {
        index: 'index.html',
        newtab: 'newtab.html',
      },
    },
  },
})
