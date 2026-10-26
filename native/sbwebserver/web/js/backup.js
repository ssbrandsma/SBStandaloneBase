/* Configuration backup UI. No credentials or backup content are persisted in
 * browser storage. Polling is bounded and exists only during a requested restore. */
(() => {
  const LIMIT = 768 * 1024;
  window.mountBackupControls = readonly => {
    const section = document.createElement('section');
    section.className = 'card';
    section.innerHTML = '<h3>Backup &amp; Restore</h3>' +
      '<p>Back up Base preferences, alarms, custom stations and Radio presets/favorites. ' +
      'Pairing credentials, firmware, binaries, caches and uploaded custom logos are excluded. Keep backups private: station URLs may contain credentials.</p>' +
      '<button data-download>Download configuration backup</button>' +
      '<hr><label>Restore configuration <input data-file type="file" accept=".json,application/json"></label> ' +
      '<button data-validate>Check backup</button> <button data-restore class="danger" disabled>Restore and restart SqueezePlay</button>' +
      '<p data-preview></p><p data-message role="status"></p>';
    app.appendChild(section);
    const file = section.querySelector('[data-file]');
    const check = section.querySelector('[data-validate]');
    const restore = section.querySelector('[data-restore]');
    const message = section.querySelector('[data-message]');
    const preview = section.querySelector('[data-preview]');
    let validated = null;
    check.disabled = file.disabled = !!readonly;
    if (readonly) message.textContent = 'Read-only mode: backup downloads remain available; restore is disabled.';
    file.onchange = () => { validated = null; restore.disabled = true; preview.textContent = ''; };
    section.querySelector('[data-download]').onclick = async () => {
      try {
        const backup = await load('/api/backup');
        const blob = new Blob([JSON.stringify(backup)], {type:'application/json'});
        const url = URL.createObjectURL(blob), link = document.createElement('a');
        link.href = url;
        link.download = 'standalone-base-backup-' + new Date().toISOString().slice(0,10) + '.json';
        link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
        message.textContent = 'Configuration backup downloaded.';
      } catch (e) { message.textContent = e.message; }
    };
    check.onclick = async () => {
      validated = null; restore.disabled = true;
      try {
        const chosen = file.files[0];
        if (!chosen) throw new Error('Choose a backup file first.');
        if (chosen.size > LIMIT) throw new Error('Backup exceeds 768 KiB.');
        const text = await chosen.text(), document = JSON.parse(text);
        await write('/api/backup/validate', {method:'POST', headers:{'Content-Type':'application/json'}, body:text});
        if (!section.isConnected) return;
        validated = text;
        preview.textContent = 'Validated sections: ' + Object.keys(document.files).join(', ') +
          '. These sections replace existing settings; omitted sections remain unchanged. All alarms will be globally off after restoring alarms.';
        restore.disabled = false; message.textContent = 'Backup is valid. Nothing has been restored.';
      } catch (e) { message.textContent = e.message; }
    };
    restore.onclick = async () => {
      if (!validated || !confirm('Replace the listed configuration and restart SqueezePlay? Playback will stop. Restored alarms will remain globally off. Restore only backups you trust.')) return;
      restore.disabled = check.disabled = file.disabled = true;
      try {
        await write('/api/backup/restore', {method:'POST', headers:{'Content-Type':'application/json'}, body:validated});
        message.textContent = 'Restore accepted. SqueezePlay is restarting; checking completion…';
        for (let n = 0; n < 90 && section.isConnected; n++) {
          await new Promise(resolve => setTimeout(resolve, 1000));
          const status = await load('/api/backup/status');
          if (status.result && status.result.ok === false) throw new Error('Restore failed. Check /tmp/standalonebase/backup-restore.log; configuration may remain locked if recovery is needed.');
          if (!status.busy) {
            if (!status.result || !status.result.ok) throw new Error('Restore did not complete successfully. Check /tmp/standalonebase/backup-restore.log.');
            message.textContent = 'Configuration restored. SqueezePlay is starting. Review alarms before enabling them.';
            return;
          }
        }
        throw new Error('Completion not confirmed. Check the Radio and restore log; do not submit a second restore.');
      } catch (e) { message.textContent = e.message; }
    };
  };
})();
