// Sovereign helper, YouTube: the second line, in the extension's own world.
// yt-main.js keeps the ads out of the player; an ad that still plays (stitched
// in on the server, or a field YouTube renamed) is muted, wound to its end and
// skipped, and the "ad blockers are not allowed" dialog is taken away.
(() => {
  'use strict';

  const SKIP = [
    '.ytp-skip-ad-button',
    '.ytp-ad-skip-button',
    '.ytp-ad-skip-button-modern',
    '.ytp-ad-skip-button-slot button',
    '.ytp-ad-overlay-close-button',
  ].join(',');

  // What the ad changed, to give back when it is over: the user's own mute
  // and speed are theirs - only what we set is undone.
  let ours = null;  // {muted: bool, rate: number} while we hold an ad

  function windAd(player, video) {
    if (!ours) ours = {muted: false, rate: video.playbackRate};
    if (!video.muted) {
      video.muted = true;
      ours.muted = true;
    }
    // Fast, and near the end - not at it: a jump right to the end leaves the
    // player waiting on the ad for good (seen 15.11.2026: 20 s of 20, "ad"
    // still showing); half a second at 16x ends it the way it would end.
    if (video.playbackRate < 16) video.playbackRate = 16;
    const end = video.duration;
    if (Number.isFinite(end) && end > 1 && video.currentTime < end - 1) {
      video.currentTime = end - 0.5;
    }
    for (const button of player.querySelectorAll(SKIP)) button.click();
  }

  function release(video) {
    if (!ours) return;
    if (ours.muted) video.muted = false;
    if (video.playbackRate !== ours.rate) video.playbackRate = ours.rate;
    ours = null;
  }

  // "Ad blockers violate YouTube's Terms of Service": the dialog goes, the
  // video it paused plays on.
  function dropEnforcement() {
    const message = document.querySelector('ytd-enforcement-message-view-model');
    if (!message) return;
    const dialog = message.closest('tp-yt-paper-dialog') || message;
    dialog.remove();
    for (const backdrop of document.querySelectorAll('tp-yt-iron-overlay-backdrop')) backdrop.remove();
    const video = document.querySelector('.html5-video-player video');
    if (video && video.paused) video.play().catch(() => {});
  }

  function tick() {
    const player = document.querySelector('.html5-video-player');
    const video = player && player.querySelector('video');
    if (video) {
      if (player.classList.contains('ad-showing') || player.classList.contains('ad-interrupting')) {
        windAd(player, video);
      } else {
        release(video);
      }
    }
    dropEnforcement();
  }

  // A look three times a second is enough to cut an ad to a blink and costs
  // nothing; a hidden tab isn't looked at.
  setInterval(() => {
    if (document.visibilityState !== 'hidden') tick();
  }, 300);
  document.addEventListener('visibilitychange', tick);
})();
