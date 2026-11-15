// Sovereign helper, YouTube: runs in the page's own world before YouTube's
// scripts and takes the ads out of what the player and the feed are given.
//
// The network can't do this: the ads come from the same names as the video
// (googlevideo.com, youtube.com/youtubei), inside TLS. Here they are plain
// fields of JSON: the player's "adPlacements"/"adSlots"/"playerAds", and ad
// renderers among the items of the feed, search and the watch page.
//
// Three ways that JSON reaches the page, all covered:
//  - inline <script>: `var ytInitialPlayerResponse = {...}`, ytInitialData -
//    an accessor on window catches the assignment;
//  - JSON.parse of a response's text (and of the embed's player vars);
//  - Response.prototype.json of /youtubei/ requests.
// Ads stitched into the video itself on the server (SSAI) are not fields and
// stay; yt-page.js winds those it can see through.
(() => {
  'use strict';

  // Fields of a player response that schedule ads.
  //
  // What this doesn't win (measured 15.11.2026, Edge 157, a 20 s ad): the
  // server holds the video itself back for about the ad's length all the same
  // - no bytes, the player "buffering". Without the ad fields it starts at
  // ~19 s, with them kept and the ad wound through at ~15 s, watched at ~24 s;
  // keeping adBreakHeartbeatParams or playerAds changes nothing. So the gain
  // is no ad seen or heard, not the time.
  const AD_FIELDS = ['adPlacements', 'adSlots', 'playerAds'];

  // Renderers that are an ad: an item that is (or wraps as its content) one
  // of these is dropped from its list.
  const AD_RENDERERS = new Set([
    'adSlotRenderer',
    'displayAdRenderer',
    'promotedSparklesWebRenderer',
    'promotedSparklesTextSearchRenderer',
    'promotedVideoRenderer',
    'compactPromotedVideoRenderer',
    'searchPyvRenderer',
    'bannerPromoRenderer',
    'statementBannerRenderer',
    'brandVideoShelfRenderer',
    'brandVideoSingletonRenderer',
    'inFeedAdLayoutRenderer',
    'adsEngagementPanelContentRenderer',
    'actionCompanionAdRenderer',
    'companionAdRenderer',
  ]);

  // A cheap look at the raw text before walking a parsed object: most of what
  // the page parses is small and has none of these.
  const MARK = /"(?:adPlacements|adSlots|playerAds|adSlotRenderer|displayAdRenderer|promoted\w*Renderer|searchPyvRenderer|bannerPromoRenderer|statementBannerRenderer|brandVideo\w*Renderer|inFeedAdLayoutRenderer|adsEngagementPanelContentRenderer|\w*CompanionAdRenderer|companionAdRenderer)"/;

  const hasOwn = Object.prototype.hasOwnProperty;
  const isObject = (v) => v !== null && typeof v === 'object';

  // Is this list item an ad: an ad renderer itself, or a one-key wrapper
  // (richItemRenderer, richSectionRenderer, engagementPanelSectionListRenderer)
  // whose content is one - two wrappers deep at most.
  function isAd(item) {
    let v = item;
    for (let depth = 0; depth < 3; depth++) {
      if (!isObject(v) || Array.isArray(v)) return false;
      for (const key in v) {
        if (AD_RENDERERS.has(key)) return true;
      }
      const keys = Object.keys(v);
      if (keys.length !== 1) return false;
      const inner = v[keys[0]];
      v = isObject(inner) ? inner.content : undefined;
    }
    return false;
  }

  // Takes the ads out of a parsed tree in place; how many it took.
  function clean(node, depth) {
    if (!isObject(node) || depth > 64) return 0;
    let cut = 0;
    if (Array.isArray(node)) {
      for (let i = node.length - 1; i >= 0; i--) {
        if (isAd(node[i])) {
          node.splice(i, 1);
          cut++;
        } else {
          cut += clean(node[i], depth + 1);
        }
      }
      return cut;
    }
    for (const field of AD_FIELDS) {
      if (hasOwn.call(node, field)) {
        delete node[field];
        cut++;
      }
    }
    for (const key in node) {
      if (hasOwn.call(node, key)) cut += clean(node[key], depth + 1);
    }
    return cut;
  }

  const safeClean = (v) => {
    try {
      return clean(v, 0);
    } catch (_) {
      return 0;  // never break the page over an ad
    }
  };

  // The inline globals: the setter cleans before anyone reads them. A `var`
  // declaration of a name that already is a property of window doesn't
  // redefine it - the assignment goes through the setter.
  for (const name of ['ytInitialPlayerResponse', 'ytInitialData', 'playerResponse']) {
    let value;
    try {
      Object.defineProperty(window, name, {
        configurable: true,
        enumerable: true,
        get() {
          return value;
        },
        set(v) {
          safeClean(v);
          value = v;
        },
      });
    } catch (_) {
      // already defined by someone else: the hooks below still see responses
    }
  }

  // A Proxy keeps the native function's identity for what YouTube checks
  // (typeof, name, length; toString says native code).
  JSON.parse = new Proxy(JSON.parse, {
    apply(target, self, args) {
      const out = Reflect.apply(target, self, args);
      const text = args[0];
      if (isObject(out) && typeof text === 'string' && MARK.test(text)) safeClean(out);
      return out;
    },
  });

  Response.prototype.json = new Proxy(Response.prototype.json, {
    apply(target, self, args) {
      const pending = Reflect.apply(target, self, args);
      let url = '';
      try {
        url = self.url || '';
      } catch (_) {
        return pending;
      }
      if (!url.includes('/youtubei/')) return pending;
      return pending.then((v) => {
        safeClean(v);
        return v;
      });
    },
  });
})();
