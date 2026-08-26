const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');
const http = require('http');

const doc = fs.readFileSync(process.argv[2], 'utf8');
const outdir = process.argv[3];
fs.mkdirSync(outdir, { recursive: true });

const html = fs.readFileSync(path.join(__dirname, 'index.html'));
const srv = http.createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
  res.end(html);
});

(async () => {
  await new Promise(r => srv.listen(8899, r));
  const browser = await chromium.launch({ executablePath: '/opt/pw-browsers/chromium-1194/chrome-linux/chrome', args: ['--no-sandbox'] });
  const page = await browser.newPage({ viewport: { width: 430, height: 932 }, deviceScaleFactor: 2 });

  await page.addInitScript(({ payload }) => {
    class FakeWS {
      constructor() {
        this.readyState = 1;
        this.sent = [];
        setTimeout(() => {
          if (this.onopen) this.onopen({});
          if (this.onmessage) this.onmessage({ data: payload });
        }, 30);
      }
      send(d) { this.sent.push(d); }
      close() {}
    }
    FakeWS.OPEN = 1;
    window.WebSocket = FakeWS;
  }, { payload: doc });

  await page.goto('http://127.0.0.1:8899/');
  await page.waitForTimeout(900);

  const shot = async (name) => {
    await page.waitForTimeout(350);
    await page.screenshot({ path: path.join(outdir, name + '.png'), fullPage: true });
  };

  const views = [
    ['01-language', () => page_navigate('language')],
    ['02-wifi',     () => page_navigate('wifi')],
    ['03-app',      () => { page_navigate('app'); card_navigate('sta'); }],
    ['04-sta',      () => { page_navigate('app'); card_navigate('sta'); }],
    ['05-ap',       () => { page_navigate('app'); card_navigate('ap'); }],
    ['06-printer',  () => { page_navigate('app'); card_navigate('printer'); }],
    ['07-settings', () => { page_navigate('app'); card_navigate('settings'); }],
    ['08-rgb-simple',  () => { page_navigate('app'); card_navigate('theme'); up_date_rgb_panel_display(0); }],
    ['09-rgb-advance', () => { page_navigate('app'); card_navigate('theme'); up_date_rgb_panel_display(1); }],
    ['10-rgb-warning', () => { page_navigate('app'); card_navigate('theme'); up_date_rgb_panel_display(2); }],
  ];
  for (const [name, fn] of views) {
    try { await page.evaluate(fn); } catch (e) { console.log('nav fail', name, e.message); }
    await shot(name);
  }

  await browser.close();
  srv.close();
  console.log('done ->', outdir);
})();
