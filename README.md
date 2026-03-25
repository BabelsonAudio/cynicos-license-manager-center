
**Prerequisites:**  Node.js


1. Install dependencies:
   `npm install`
2. Set the `GEMINI_API_KEY` in [.env.local](.env.local) to your Gemini API key
3. Run the app:
   `npm run dev`

4. rm -rf dist dist-electron && npm run build:mac

Electron:
`npm start`

// === Works === 
2. The Final Build Workflow
3.To ensure you aren't packaging old or missing files, always run these three commands in order:


3. `rm -rf node_modules/.vite`

=== === === === Clean old files === === === 

`rm -rf dist dist-electron`

`rm -rf dist dist-electron && npm run build:mac` 
(This runs vite build to create your React files)

=== === === === === === ===  WORKS === === === ===  ===

`npm run release:mac`

`npm run build` (This runs vite build to create your React files)

`npm run build:mac` (This runs electron-builder to wrap everything into an .app)

===dliub
# 1. Remove the local build and lock files
rm -rf node_modules package-lock.json

# 2. Clear the global npm cache to ensure you don't grab a corrupted binary
npm cache clean --force

# 3. Reinstall
npm install

xattr -rd com.apple.quarantine node_modules/@rollup/rollup-darwin-arm64/

npm run release:mac



