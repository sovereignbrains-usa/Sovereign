// Sovereign helper, TikTok: the cookie banner answers itself with "decline".
// The banner is the <tiktok-cookie-banner> element with an open shadow root and
// two plain buttons (decline, accept) - no classes, so the decline button is
// found by its text. Only a button that reads as a refusal is ever pressed:
// if the wording is not recognised the banner is left to the user, never
// "accepted" by guess.
(() => {
  'use strict';

  const HOST = 'tiktok-cookie-banner';
  // "Decline additional cookies" in the languages TikTok's banner is shown in.
  const DECLINE = new RegExp([
    'отклон', 'відхил', 'decline', 'reject', 'refuse', 'deny', 'ablehnen',
    'rechaz', 'refus', 'rifiut', 'odrzu', 'recus', 'weiger', 'avvis',
    'afvis', 'avsl', 'hylk', 'odmít', 'odmiet', 'elutas', 'respin',
    'απόρριψη', 'reddet', 'отказ', 'otklon', 'tolak', 'từ chối', 'ปฏิเสธ',
    '拒绝', '拒絕', '拒否', '거부',
  ].join('|'), 'i');

  let tries = 0;
  let scheduled = false;

  function declineButton(banner) {
    const root = banner.shadowRoot;
    if (!root) return null;
    for (const b of root.querySelectorAll('button')) {
      const text = (b.textContent || '').trim();
      if (text && DECLINE.test(text)) return b;
    }
    return null;
  }

  function check() {
    scheduled = false;
    const banner = document.querySelector(HOST);
    if (!banner) return;
    const button = declineButton(banner);
    if (!button || ++tries > 5) return;
    button.click();
    // The banner leaves on its own once the choice is saved; if it does not
    // (cookies refused by the browser), the tries above stop the loop.
    setTimeout(schedule, 1500);
  }

  function schedule() {
    if (scheduled) return;
    scheduled = true;
    requestAnimationFrame(check);
  }

  // The banner is loaded dynamically and may come after the page, or again
  // after a move inside the single-page app: watch the page, cheaply.
  new MutationObserver(schedule).observe(document, {childList: true, subtree: true});
  schedule();
})();
