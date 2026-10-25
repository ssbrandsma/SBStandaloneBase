"""Browser-only firmware UI regression fixtures; no Radio or network access.

Run with Python + Playwright Chromium. Optional second argument saves screenshots.
"""
import pathlib
import os
import sys
from playwright.sync_api import sync_playwright

root = pathlib.Path(sys.argv[1]).resolve()
screens = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None
with sync_playwright() as playwright:
    browser = playwright.chromium.launch(channel=os.environ.get('SB_TEST_BROWSER_CHANNEL'))
    page = browser.new_page(viewport={'width': 1100, 'height': 1100})
    page.set_content('<main id="app"></main>')
    page.add_style_tag(path=str(root / 'native/sbwebserver/web/css/style.css'))
    page.evaluate('''() => {
      window.app = document.getElementById('app');
      window.size = n => n + ' B';
      window.fixture = {installed:'9.0.2 r17146', available:'9.0.2 r17157', board:5,
        state:'ready', candidateReady:true, radioPermission:false, imageSize:16032273,
        calculatedSha256:'d'.repeat(64), checksumAlgorithm:'SHA-1', lastValidated:1792942366};
      window.load = async () => fixture;
      window.writes = [];
      window.write = async (path, options) => { writes.push({path, options}); return fixture; };
      window.setTimeout = callback => { window.nextPoll = callback; return 1; };
      window.clearTimeout = () => {};
      window.XMLHttpRequest = class {
        constructor() { this.upload={}; this.headers={}; window.xhr=this; }
        open() {} setRequestHeader(k,v) {this.headers[k]=v;} send(file) {this.file=file;}
        abort() {this.onabort(); this.onloadend();}
      };
    }''')
    page.add_script_tag(path=str(root / 'native/sbwebserver/web/js/firmware.js'))
    page.evaluate('async () => { await firmwarePage(); }')
    page.evaluate('''async () => {
      const assert = (condition, message) => {if (!condition) throw Error(message);};
      for (const n of [40,64]) {
        assert(firmwareUI.checksum('A'.repeat(n)) === 'a'.repeat(n), 'normalization');
        assert(!firmwareUI.checksum('g'.repeat(n)), 'invalid hex');
      }
      for (const n of [0,39,41,63,65]) assert(!firmwareUI.checksum('a'.repeat(n)), 'length');
      const find = id => document.getElementById(id);
      assert(find('fw-state').textContent === 'Firmware ready', 'ready state');
      assert(find('fw-permission').textContent === 'Disabled', 'permission');
      assert(find('firmware-transfer').hidden && find('firmware-cancel').hidden, 'idle progress hidden');
      const row = find('fw-installed');
      fixture.error = '<img src=x onerror="window.injection=true">';
      fixture.available = '<script>window.injection=true</script>';
      await nextPoll();
      assert(find('fw-error').textContent === fixture.error, 'safe error text');
      assert(find('fw-available').textContent === fixture.available, 'safe version text');
      assert(!window.injection && !app.querySelector('img,script'), 'no HTML injection');
      assert(row === find('fw-installed'), 'no DOM rebuild on poll');
      fixture.bootstrapRetained=false; await nextPoll();
      assert(!find('firmware-bootstrap-warning').hidden, 'missing retained bootstrap warning');
      fixture.bootstrapRetained=true; await nextPoll();
      assert(find('firmware-bootstrap-warning').hidden, 'retained bootstrap warning clears');
      fixture.state='preparing'; fixture.candidateReady=false;
      await nextPoll();
      assert(find('firmware-send').disabled && !find('firmware-transfer').hidden, 'busy controls');
      fixture.state='failed'; fixture.error='Checksum mismatch'; await nextPoll();
      assert(find('fw-state').textContent === 'Failed' && !find('firmware-send').disabled, 'failure');
      fixture.state='ready'; fixture.error=''; fixture.available='9.0.2 r17157'; fixture.candidateReady=true;
      await nextPoll();
      const radio = document.querySelector('input[value=url]'); radio.onchange();
      assert(!find('firmware-config').hidden && find('firmware-upload').hidden, 'source mode');
      find('firmware-config').elements.url.value='https://example.com/fw.bin';
      find('firmware-config').elements.checksum.value='A'.repeat(40);
      await find('firmware-config').onsubmit({preventDefault(){}});
      await Promise.resolve(); await Promise.resolve();
      assert(JSON.parse(writes[0].options.body).checksum === 'a'.repeat(40), 'SHA-1 config');
      await nextPoll();
      document.querySelector('input[value=upload]').onchange();
    }''')
    # An unsafe filename is never interpolated into HTML; the native input owns it.
    page.set_input_files('#firmware-file', {'name': '<img onerror=bad>.bin', 'mimeType':'application/octet-stream', 'buffer':b'fixture'})
    page.fill('#firmware-digest', 'B' * 40)
    page.evaluate('''async () => {
      const find=id=>document.getElementById(id), assert=(v,m)=>{if(!v)throw Error(m);};
      find('firmware-upload').onsubmit({preventDefault(){}});
      assert(xhr.headers['X-Firmware-Checksum']==='b'.repeat(40), 'upload SHA-1 header');
      assert(!find('firmware-cancel').hidden && find('firmware-send').disabled, 'upload controls');
      xhr.upload.onprogress({lengthComputable:true, total:7, loaded:7});
      assert(find('firmware-phase').textContent.includes('Validating'), 'validation phase');
      assert(find('firmware-progress-text').textContent.includes('KiB/s'), 'transfer speed');
      xhr.status=200; xhr.responseText=JSON.stringify(fixture); xhr.onload(); xhr.onloadend();
      assert(find('firmware-transfer').hidden && find('firmware-cancel').hidden, 'completed progress');
      assert(find('firmware-message').textContent.includes('Firmware ready'), 'success');
      fixture.state='unconfigured'; fixture.candidateReady=false; await nextPoll();
      assert(find('fw-state').textContent==='No update available', 'empty state');
      fixture.state='ready'; fixture.candidateReady=true; await nextPoll();
    }''')
    if screens:
        page.set_input_files('#firmware-file', {'name':'baby_9.0.2_r17157.bin', 'mimeType':'application/octet-stream', 'buffer':b'fixture'})
        page.fill('#firmware-digest', '9ab354c46ae98027d5a3164c2ae13166875d2c87')
        page.locator('#firmware-digest').blur()
        screens.mkdir(parents=True, exist_ok=True)
        page.screenshot(path=str(screens / 'firmware-desktop.png'), full_page=True)
        page.set_viewport_size({'width':390, 'height':844})
        page.screenshot(path=str(screens / 'firmware-mobile.png'), full_page=True)
    assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
    browser.close()
print('Firmware UI checksums, status, safe text, stable DOM, source modes, progress and responsive layout passed')
