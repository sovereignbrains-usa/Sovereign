# Sovereign helper (browser extension)

Takes YouTube's ads out of the page, which a VPN client can't do from the
network: the ads come from the same names as the video, inside TLS.

- `yt-main.js` — in the page's world, before YouTube's scripts: removes
  `adPlacements`/`adSlots`/`playerAds` from player responses and ad renderers
  from the feed, search and the watch page (inline globals, `JSON.parse`,
  `Response.json` of `/youtubei/`).
- `yt-page.js` — an ad that still plays is muted, wound through and skipped;
  the "ad blockers" dialog is removed.
- `yt.css` — ad slots on the page.
- `tt-page.js` - TikTok: the cookie banner (`<tiktok-cookie-banner>`, open shadow root) is answered with its decline button, found by text; a button that does not read as a refusal is never pressed, so the banner is never "accepted" by guess. Checked on a clean Edge profile: `cookie-consent` comes out with every optional category `false`.

No permissions beyond the content scripts on youtube.com and tiktok.com.

**Known limit:** YouTube's server holds the video back for about the length of
the removed ad (the player shows buffering). The ad is neither seen nor heard;
the wait stays.

Install (Edge/Chrome): `edge://extensions` → Developer mode → Load unpacked →
this folder.
