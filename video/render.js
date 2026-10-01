// Рендер video/index.html в MP4: покадровые скриншоты (Chrome/Edge через puppeteer-core) -> ffmpeg.
//
//   npm install
//   node render.js              -> ../docs/demo.mp4
//   node render.js --still 40   -> still.png (один кадр на 40-й секунде, для проверки вёрстки)
//
// Пути можно задать переменными окружения: BROWSER_PATH (Chrome/Edge), FFMPEG_PATH.
const puppeteer = require('puppeteer-core');
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');

const FPS = 30;

function findBrowser() {
  if (process.env.BROWSER_PATH) return process.env.BROWSER_PATH;
  const candidates = [
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
    'C:/Program Files/Google/Chrome/Application/chrome.exe',
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
  ];
  const found = candidates.find(p => fs.existsSync(p));
  if (!found) throw new Error('Не найден Chrome/Edge. Задайте BROWSER_PATH.');
  return found;
}

const ffmpegPath = () => process.env.FFMPEG_PATH || 'ffmpeg';

(async () => {
  const browser = await puppeteer.launch({ executablePath: findBrowser(), headless: true });
  const page = await browser.newPage();
  await page.setViewport({ width: 1920, height: 1080 });
  await page.goto(require('url').pathToFileURL(path.join(__dirname, 'index.html')).href, { waitUntil: 'networkidle0' });
  await page.evaluate(() => document.fonts.ready);
  const duration = await page.evaluate(() => window.DURATION);

  const si = process.argv.indexOf('--still');
  if (si > 0) {
    await page.evaluate(t => render(t), parseFloat(process.argv[si + 1]));
    await page.screenshot({ path: path.join(__dirname, 'still.png') });
    await browser.close();
    return;
  }

  const out = path.join(__dirname, '..', 'docs', 'demo.mp4');
  fs.mkdirSync(path.dirname(out), { recursive: true });
  const ff = spawn(ffmpegPath(), ['-y', '-loglevel', 'error', '-f', 'image2pipe', '-framerate', String(FPS), '-i', '-',
    '-c:v', 'libx264', '-preset', 'medium', '-crf', '18', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', out],
    { stdio: ['pipe', 'inherit', 'inherit'] });
  const frames = Math.floor(duration * FPS);
  for (let i = 0; i < frames; i++) {
    await page.evaluate(t => render(t), i / FPS);
    const buf = await page.screenshot({ type: 'jpeg', quality: 95 });
    if (!ff.stdin.write(buf)) await new Promise(r => ff.stdin.once('drain', r));
    if (i % 150 === 0) console.log(i + '/' + frames);
  }
  ff.stdin.end();
  await new Promise(r => ff.on('close', r));
  await browser.close();
  console.log('done: ' + out);
})().catch(e => { console.error(e); process.exit(1); });
