// Draws every test page in Chromium, the reference for tools/webcompat.
//   node browser_shots.js OUT_DIR [case.html ...]
// Fonts come from ui/fonts (the same files the engine uses), and the default
// font is Onest, like ui/web/html.rcss.
const path = require('path');
const fs = require('fs');
const { chromium } = require('playwright');

const here = __dirname;
const root = path.resolve(here, '..', '..');
const fonts = JSON.parse(fs.readFileSync(path.join(root, 'ui/fonts/fonts.json'), 'utf8')).fonts;
const faces = fonts.map(f => `@font-face{font-family:"${f.family}";font-weight:${f.weight || 400};` +
  `src:url("file://${path.join(root, 'ui/fonts', f.file)}");}`).join('\n') +
  '\nhtml{font-family:"Onest"}';

(async () => {
  const out = process.argv[2];
  const cases = process.argv.length > 3 ? process.argv.slice(3)
    : fs.readdirSync(path.join(here, 'cases')).filter(n => n.endsWith('.html')).sort().map(n => path.join(here, 'cases', n));
  fs.mkdirSync(out, { recursive: true });
  const browser = await chromium.launch(process.env.CHROMIUM ? { executablePath: process.env.CHROMIUM } : {});
  const page = await browser.newPage({ viewport: { width: 800, height: 600 }, deviceScaleFactor: 1 });
  for (const c of cases) {
    await page.goto('file://' + path.resolve(c));
    await page.addStyleTag({ content: faces });
    await page.evaluate(() => document.fonts.ready);
    await page.waitForTimeout(100);
    await page.screenshot({ path: path.join(out, path.basename(c, '.html') + '.png') });
  }
  await browser.close();
})();
