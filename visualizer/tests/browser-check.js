/* Load after dist/app.js in a temporary copy of index.html for headless QA. */
window.addEventListener('load', async () => {
  const status = document.createElement('pre'); status.id = 'browser-check-result'; document.body.append(status);
  const assert = (condition, message) => { if (!condition) throw new Error(message); };
  const byId = id => document.getElementById(id);
  const app = window.FlowApp;
  const errors = []; window.addEventListener('error', event => errors.push(event.message));
  try {
    assert(Boolean(app), 'Application loaded');
    byId('next').click(); assert(app.getIndex() === 1, 'Next button advances');
    byId('previous').click(); assert(app.getIndex() === 0, 'Previous button rewinds');
    for (const key of Object.keys(window.RedisFlow.presets)) {
      app.setScenario(key);
      const trace = app.getTrace();
      for (let i = 0; i < trace.frames.length; i++) app.setIndex(i);
      assert(byId('step-label').textContent.endsWith('/ ' + trace.frames.length), key + ' renders its final step');
      assert(byId('client-replies').textContent.length > 0, key + ' renders client replies');
    }
    app.setScenario('compaction');
    const compactIndex = app.getTrace().frames.findIndex(f => f.title === 'Compact only when append needs space');
    app.setIndex(compactIndex);
    assert(byId('buffer-copied').textContent === '15 B', 'Compaction count rendered');
    byId('byte-format').click(); assert(byId('byte-format').getAttribute('aria-pressed') === 'true', 'Hex toggle works');
    byId('byte-format').click();
    byId('timeline').value = 4; byId('timeline').dispatchEvent(new Event('input', { bubbles: true }));
    assert(app.getIndex() === 4, 'Timeline scrubbing works');
    document.querySelector('[data-stage="storage"]').dispatchEvent(new MouseEvent('click', { bubbles: true }));
    assert(app.getTrace().frames[app.getIndex()].stage === 'storage', 'Diagram jump works');
    byId('customize').click(); assert(!byId('custom-panel').hidden, 'Custom form opens');
    byId('commands').value = 'SET sample "<img src=x onerror=alert(1)>"\nGET sample';
    byId('custom-panel').dispatchEvent(new Event('submit', { cancelable: true, bubbles: true }));
    app.setIndex(app.getTrace().frames.length - 1);
    assert(byId('client-replies').textContent.includes('<img src=x onerror=alert(1)>'), 'Custom trace produces literal data');
    assert(!document.querySelector('img[src="x"]'), 'Command input is rendered as text, not HTML');
    byId('commands').value = 'ECHO "unterminated';
    byId('custom-panel').dispatchEvent(new Event('submit', { cancelable: true, bubbles: true }));
    assert(byId('input-error').textContent.includes('double quote'), 'Invalid command feedback');
    app.setScenario('normal'); byId('speed').value = '160'; app.play();
    await new Promise(resolve => setTimeout(resolve, 420));
    assert(app.getIndex() > 0, 'Playback advances'); app.pause();
    const stopped = app.getIndex(); await new Promise(resolve => setTimeout(resolve, 200));
    assert(app.getIndex() === stopped, 'Pause retains step');
    assert(document.documentElement.scrollWidth <= window.innerWidth + 1, 'Responsive layout has no page-wide horizontal overflow');
    assert(errors.length === 0, errors.join('; '));
    app.setScenario('compaction');
    app.setIndex(app.getTrace().frames.findIndex(f => f.title === 'Consume by advancing begin_' && f.snapshot.begin > 0));
    if (!byId('custom-panel').hidden) byId('customize').click();
    document.activeElement.blur(); window.scrollTo(0, 0);
    status.textContent = 'BROWSER_CHECK_OK: all seven scenarios, playback, stepping, scrubbing, jumps, custom commands, escaping, and responsive overflow.';
    status.hidden = true;
  } catch (error) { status.textContent = 'BROWSER_CHECK_ERROR: ' + error.message; }
});
