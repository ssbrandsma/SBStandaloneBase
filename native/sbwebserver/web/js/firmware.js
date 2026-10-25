/* Paired preparation only. No browser installation or permission controls. */
window.firmwareUI = {
  checksum(value) {
    const text = value.trim().toLowerCase();
    return /^(?:[a-f0-9]{40}|[a-f0-9]{64})$/.test(text) ? text : null;
  },
  state(status) {
    if (status.state === 'uploading') return 'Uploading';
    if (['validating', 'preparing'].includes(status.state)) return 'Validating firmware';
    if (['failed', 'blocked'].includes(status.state)) return 'Failed';
    if (status.candidateReady) return 'Firmware ready';
    return status.state === 'unconfigured' ? 'No update available' : 'Not checked';
  }
};

window.firmwarePage = async function () {
  let active = true, timer, upload, requestBusy = false, lastStatus = {};
  window.firmwarePageCleanup = () => { active = false; clearTimeout(timer); if (upload) upload.abort(); };
  const checksumField = (id) => '<div class="form-row"><label for="' + id + '">Expected firmware checksum</label>' +
    '<input id="' + id + '" name="checksum" pattern="([a-fA-F0-9]{40}|[a-fA-F0-9]{64})" maxlength="64" ' +
    'autocomplete="off" spellcheck="false" aria-describedby="' + id + '-help" required>' +
    '<small id="' + id + '-help">Enter the SHA-1 (40 characters) or SHA-256 (64 characters) checksum supplied with the firmware.</small></div>';
  const fact = (label, id) => '<dt>' + label + '</dt><dd id="fw-' + id + '">—</dd>';
  app.innerHTML = '<div class="firmware-page"><div class="page-title"><h2>Firmware Updates</h2>' +
    '<p>Upload or download firmware, validate it, and install it using the Radio’s built-in firmware updater.</p></div>' +
    '<section class="card"><h3>Current firmware</h3><dl class="facts firmware-facts">' +
    fact('Installed version', 'installed') + fact('Available version', 'available') + fact('Hardware', 'hardware') +
    fact('Status', 'state') + fact('Validation', 'validation') + fact('Radio permission', 'permission') + '</dl></section>' +
    '<section class="card"><h3>Firmware source</h3><fieldset class="firmware-source"><legend>Choose a source</legend>' +
    '<label><input type="radio" name="firmware-source" value="upload" checked> Upload firmware file</label>' +
    '<label><input type="radio" name="firmware-source" value="url"> Download from URL</label></fieldset>' +
    '<form id="firmware-upload"><div class="form-row"><label for="firmware-file">Native firmware file (.bin)</label>' +
    '<input id="firmware-file" type="file" accept=".bin" required aria-describedby="firmware-file-help">' +
    '<small id="firmware-file-help">Select the extracted native .bin, not the outer release ZIP. Maximum 24 MiB.</small></div>' +
    checksumField('firmware-digest') + '<button id="firmware-send" type="submit">Upload and Validate</button></form>' +
    '<form id="firmware-config" hidden><div class="form-row"><label for="firmware-url">Firmware URL</label>' +
    '<input id="firmware-url" name="url" type="url" maxlength="2047" required></div>' + checksumField('firmware-url-digest') +
    '<div class="firmware-actions"><button id="firmware-save" type="submit">Save Configuration</button>' +
    '<button id="firmware-check" type="button">Check for Updates</button></div></form>' +
    '<div class="firmware-actions"><button id="firmware-revalidate" type="button" hidden>Revalidate Staged Image</button></div>' +
    '<div id="firmware-transfer" hidden aria-live="polite"><h4 id="firmware-phase">Uploading firmware</h4>' +
    '<progress id="firmware-progress" max="100" value="0" aria-label="Firmware upload progress"></progress>' +
    '<p id="firmware-progress-text"></p><button id="firmware-cancel" type="button" hidden>Cancel Upload</button></div>' +
    '<p id="firmware-message" role="status" aria-live="polite"></p></section>' +
    '<section class="card"><h3>Installation on the Radio</h3><p>To install the validated firmware, enable Custom Firmware Updates on the physical Radio, ' +
    'then select Review Update. The Radio’s built-in installer will handle the installation.</p>' +
    '<p class="muted">The web interface cannot initiate firmware installation. Review does not start installation.</p>' +
    '<p id="firmware-bootstrap-warning" class="notice" hidden>Post-update startup support is not prepared. Repair Extended Storage startup integration on the Radio before updating. Firmware may remove unretained startup files.</p>' +
    '<details><summary>Help / About firmware checksums</summary><p>SHA-1 is supported for compatibility with community release metadata; SHA-256 is stronger. ' +
    'A matching checksum verifies consistency with the supplied value, not independent publisher authenticity. Obtain the expected checksum through a trusted channel.</p>' +
    '<p>The checksum must describe the extracted native .bin, not the outer ZIP. Checksums are never automatically trusted from an uploaded archive. ' +
    'SHA-256 remains the internal image identifier for both formats.</p><p>Active Extended Storage is required; uploads reserve an additional 4 MiB. ' +
    'Virtual Radio controls are disabled while physical firmware permission is enabled.</p></details></section>' +
    '<section class="card"><details><summary>Technical details</summary><dl class="facts firmware-facts">' +
    fact('Checksum algorithm', 'algorithm') + fact('Calculated SHA-256', 'digest') + fact('Firmware size', 'bytes') +
    fact('Hardware compatibility', 'compatibility') + fact('Last successful validation', 'checked') +
    fact('Last error', 'error') + fact('Local candidate', 'candidate') + fact('Post-update bootstrap', 'bootstrap') + '</dl></details></section></div>';
  const find = id => document.getElementById(id);
  const form = find('firmware-config'), uploadForm = find('firmware-upload');
  const message = find('firmware-message'), cancel = find('firmware-cancel');
  const progress = find('firmware-progress'), progressText = find('firmware-progress-text');
  const transfer = find('firmware-transfer'), phase = find('firmware-phase');
  function controls() {
    const busy = !!upload || requestBusy || ['uploading', 'validating', 'preparing'].includes(lastStatus.state);
    ['firmware-send', 'firmware-save', 'firmware-check', 'firmware-revalidate'].forEach(id => { find(id).disabled = busy; });
    document.querySelectorAll('.firmware-page input').forEach(input => { input.disabled = busy; });
    cancel.hidden = !upload;
    find('firmware-revalidate').hidden = !lastStatus.candidateReady;
  }
  document.querySelectorAll('input[name="firmware-source"]').forEach(input => {
    input.onchange = () => { form.hidden = input.value !== 'url'; uploadForm.hidden = input.value !== 'upload'; };
  });
  function render(status) {
    if (!active) return;
    lastStatus = status;
    const values = {
      installed: status.installed || 'Waiting for Radio identity', available: status.available || 'Not validated',
      hardware: status.board ? 'Squeezebox Radio — Baby rev. ' + status.board : 'Waiting for Radio identity',
      state: window.firmwareUI.state(status), validation: status.candidateReady ? 'Passed' : 'Not validated',
      permission: status.radioPermission ? 'Enabled on physical Radio' : 'Disabled',
      algorithm: status.checksumAlgorithm || 'Not configured', digest: status.calculatedSha256 || 'Not calculated',
      bytes: status.imageSize ? size(status.imageSize) : 'Not validated',
      compatibility: status.candidateReady ? 'Passed for Baby rev. ' + status.board : 'Not validated',
      checked: status.lastValidated ? new Date(status.lastValidated * 1000).toLocaleString() : 'Never',
      error: status.error || 'None', candidate: status.candidateReady ? 'Validated on Extended Storage' : 'Not ready',
      bootstrap: status.bootstrapRetained === true ? 'Retained boot support installed; future firmware compatibility is not guaranteed' :
        status.bootstrapRetained === false ? 'Not prepared — repair startup integration' : 'Not checked'
    };
    Object.entries(values).forEach(([key, value]) => {
      const node = find('fw-' + key);
      if (node.textContent !== String(value)) node.textContent = value;
    });
    find('fw-state').className = 'firmware-state ' + (status.candidateReady ? 'is-ready' : status.error ? 'is-failed' : '');
    find('firmware-bootstrap-warning').hidden = status.bootstrapRetained !== false;
    if (!upload) {
      transfer.hidden = !['preparing', 'validating', 'uploading'].includes(status.state);
      if (!transfer.hidden) {
        phase.textContent = window.firmwareUI.state(status); progress.removeAttribute('value');
        progressText.textContent = status.received ? size(status.received) + ' received' : 'Checking firmware; no installation is performed.';
      }
    }
    controls();
  }
  async function refresh() {
    try { render(await load('/api/firmware/status')); }
    catch (error) { if (active) message.textContent = error.message; }
    if (active) timer = setTimeout(refresh, 3000);
  }
  const initial = await load('/api/firmware/config');
  if (!active) return;
  form.elements.url.value = initial.url || '';
  form.elements.checksum.value = initial.urlChecksum || initial.urlSha256 || (initial.source !== 'upload' ? initial.checksum || initial.sha256 : '') || '';
  render(initial);
  async function action(path, options, success) {
    requestBusy = true; controls();
    try {
      await write(path, options);
      if (active) { message.textContent = success; render(await load('/api/firmware/status')); }
    } catch (error) { if (active) message.textContent = error.message; }
    finally { requestBusy = false; if (active) controls(); }
  }
  form.onsubmit = event => {
    event.preventDefault();
    const checksum = window.firmwareUI.checksum(form.elements.checksum.value);
    if (!checksum) { message.textContent = 'Enter 40 (SHA-1) or 64 (SHA-256) hexadecimal characters.'; return; }
    action('/api/firmware/config', {method: 'PUT', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({url: form.elements.url.value.trim(), checksum})}, 'Configuration saved. Select Check for Updates.');
  };
  find('firmware-check').onclick = () => action('/api/firmware/check', {method: 'POST'}, 'Validating firmware. No installation will be performed.');
  find('firmware-revalidate').onclick = () => {
    const typed = find('firmware-digest').value.trim(), checksum = typed ? window.firmwareUI.checksum(typed) : null;
    if (typed && !checksum) { message.textContent = 'Enter a valid SHA-1 or SHA-256 checksum.'; return; }
    action('/api/firmware/check', {method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(checksum ? {checksum} : {})}, 'Revalidating the staged image without downloading it again.');
  };
  uploadForm.onsubmit = event => {
    event.preventDefault();
    if (upload || requestBusy) return;
    const file = find('firmware-file').files[0], checksum = window.firmwareUI.checksum(find('firmware-digest').value);
    if (!file || !file.size || file.size > 24 * 1024 * 1024 || !checksum) {
      message.textContent = 'Select a non-empty native image up to 24 MiB and enter its SHA-1 or SHA-256 checksum.'; return;
    }
    const xhr = new XMLHttpRequest(), started = performance.now();
    upload = xhr; controls(); transfer.hidden = false; progress.value = 0; progressText.textContent = 'Starting transfer…';
    phase.textContent = 'Uploading firmware'; message.textContent = '';
    xhr.open('POST', '/api/firmware/upload'); xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.setRequestHeader('X-Firmware-Checksum', checksum); xhr.timeout = 660000;
    xhr.upload.onprogress = event => {
      if (!active) return;
      const total = event.lengthComputable ? event.total : file.size;
      progress.value = Math.min(100, event.loaded / total * 100);
      phase.textContent = event.loaded >= total ? 'Validating firmware…' : 'Uploading firmware';
      progressText.textContent = Math.round(progress.value) + '% — ' + size(event.loaded) + ' / ' + size(total) +
        ' · ' + (event.loaded / Math.max(.001, (performance.now() - started) / 1000) / 1024).toFixed(1) + ' KiB/s';
    };
    xhr.onload = () => {
      if (!active) return;
      let result;
      try { result = JSON.parse(xhr.responseText); } catch (_) { result = {error: 'Invalid server response'}; }
      if (xhr.status === 401) { window.showPairing(); return; }
      if (xhr.status >= 200 && xhr.status < 300) {
        render(result); message.textContent = 'Firmware ready — ' + result.available + '. Review it on the physical Radio.';
      } else message.textContent = result.error || 'Upload failed (' + xhr.status + ')';
    };
    xhr.onerror = () => { if (active) message.textContent = 'Upload connection failed. Check Radio status; no automatic retry.'; };
    xhr.ontimeout = () => { if (active) message.textContent = 'Upload timed out; no automatic retry.'; };
    xhr.onabort = () => { if (active) message.textContent = 'Upload cancelled. Previous candidate is retained.'; };
    xhr.onloadend = () => { upload = null; if (active) { transfer.hidden = true; controls(); } };
    xhr.send(file);
  };
  cancel.onclick = () => { if (upload) upload.abort(); };
  timer = setTimeout(refresh, 3000);
};
