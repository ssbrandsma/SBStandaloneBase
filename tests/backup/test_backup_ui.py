"""Optional Playwright UI tests with mocked APIs; never contacts a Radio."""
from pathlib import Path
import sys
import os
from playwright.sync_api import sync_playwright, expect

source=Path(sys.argv[1])/"native/sbwebserver/web/js/backup.js"
with sync_playwright() as playwright:
    browser=playwright.chromium.launch(headless=True,channel=os.getenv("SB_TEST_BROWSER_CHANNEL"))
    page=browser.new_page(accept_downloads=True)
    page.evaluate('(source) => { new Function(source); return true; }',source.with_name('app.js').read_text(encoding='utf-8'))
    page.set_content('<main id="app"></main>')
    page.add_script_tag(content='''
      const app=document.querySelector('#app');
      window.requests=[];
      window.load=async path => path.endsWith('/status') ? {busy:false,result:{ok:true}} : {format:'StandaloneBaseBackup',version:1,files:{settings:{}}};
      window.write=async (path,options) => {requests.push({path,body:options.body});return {valid:true};};
    ''')
    page.add_script_tag(path=str(source))
    page.evaluate('window.mountBackupControls(false)')
    expect(page.locator('[data-restore]')).to_be_disabled()
    page.locator('[data-validate]').click()
    expect(page.locator('[data-message]')).to_contain_text('Choose a backup')
    payload=b'{"format":"StandaloneBaseBackup","version":1,"files":{"settings":{}}}'
    page.locator('[data-file]').set_input_files({'name':'test.json','mimeType':'application/json','buffer':payload})
    page.locator('[data-validate]').click()
    expect(page.locator('[data-restore]')).to_be_enabled()
    expect(page.locator('[data-preview]')).to_contain_text('settings')
    assert page.evaluate('requests[0].path')=='/api/backup/validate'
    with page.expect_download() as download:
        page.locator('[data-download]').click()
    assert download.value.suggested_filename.startswith('standalone-base-backup-')
    page.once('dialog',lambda dialog:dialog.dismiss())
    page.locator('[data-restore]').click()
    assert page.evaluate('requests.length')==1
    page.once('dialog',lambda dialog:dialog.accept())
    page.locator('[data-restore]').click()
    expect(page.locator('[data-message]')).to_contain_text('Configuration restored',timeout=5000)
    assert page.evaluate('requests[1].path')=='/api/backup/restore'
    page.evaluate('app.innerHTML="";window.mountBackupControls(true)')
    expect(page.locator('[data-file]')).to_be_disabled()
    expect(page.locator('[data-validate]')).to_be_disabled()
    expect(page.locator('[data-download]')).to_be_enabled()
    browser.close()
print('backup browser validation, download, confirmation, completion and read-only UI passed')
