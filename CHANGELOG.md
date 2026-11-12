# Changelog

<!-- One line per paragraph or item: GitHub's release notes break lines wherever this file does. -->

What changed for someone using Sovereign, release by release. The release workflow puts a version's section into its GitHub release. Releases up to 0.5.130 are at the project's first address, [github.com/sovereignbrains/Sovereign](https://github.com/sovereignbrains/Sovereign/releases).

## 0.6.12 — 12.11.2026

**Subscriptions and your own servers are on two tabs of one page.** The page is now called "Подписки и серверы"; at its top a switch "Подписки · N | Мои серверы · M" shows either the subscriptions and configs, or the servers of your own (a VPS set up by Sovereign or marked as yours). Opening a configuration and coming back lands on its tab.
- **Ways to add are buttons with words, in their tab**, instead of the small icons in the title: "Из буфера", "QR-код" and "Из файла" under the subscriptions; "Добавить свой VPS" under your servers, with a line on what it needs (a clean Debian VPS and root login by an ssh key).
- **A configuration's page has a "Мой сервер" switch**: it moves the configuration to "Мои серверы" and back. It works for a subscription from your own server too — then Sovereign only checks the server over ssh and doesn't install over it, since its config comes from the link.
- **List lines say it plainly**: "2 сервера, 1 выкл. · обновлена сегодня в 19:20", "свой конфиг · 3 сервера", "выключена", "новая версия ждёт", "не обновилась".
- **The configuration's fields are "Откуда" and "Конфиг"** ("с твоими правками" or "без правок, как в подписке"), and the delete button says what it does: "Удалить подписку", "Удалить конфиг", or "Убрать из списка" for your own server, whose VPN stays on it.
- **The relay row is "Обновлять без VPN"**, short enough for its "?" not to overlap the words.

## 0.6.11 — 12.11.2026

**The Log page reads as events in plain words, not raw core lines.** Each row says what happened ("AnyTLS · Финляндия: не ответил вовремя", "youtube.com — через AnyTLS · Нидерланды", "mc.yandex.ru — заблокирован: реклама или слежка", "Нет сети", "VPN включён"), with a coloured dot for how it went and, under it, which server and what the core said. Repeats of the same event fold into one row with a "×N" count; the core's routine chatter (rule matches, DNS cache, sniffing) is hidden.
- **Filters: Всё, Проблемы, Серверы, Сайты**, each with how many kinds of event it holds, and a search box over the words and the raw lines.
- **A click on a row opens a card under the list** with the line exactly as the core wrote it and what can be done: copy it, show only events of that kind one by one with their times, or open the Servers page when the event is about a server.
- **Pause and copy stay on top; "⋯" holds the rest**: save to a file, how much the core writes, clear. Copying, saving and problem reports still use the raw lines.

## 0.6.10 — 12.11.2026

- **No more cut-off notes beside protocols on the Servers page.** The second subscription's ways no longer carry its name as a grey note ("REALITY · ewkereboss.xyz", cut to "REALI…"): the server's card already says which server they lead to. A note now appears only when the provider named a way in words of its own.
- **The games column says "нет" when UDP doesn't get through**, instead of "не отвечает" cut to "не отве…".

## 0.6.9 — 12.11.2026

**Any server on the Servers page opens without switching to it.** A chevron at the right of a server's head opens its card to show its protocols and folds it again; the connection stays where it is. A click on the head itself still switches to that server, as before.

- **The site marks are the services' own logos in their own colours** — YouTube's red, Telegram's blue, Instagram's pink, ChatGPT's knot. Faded means the site doesn't answer through this server, faded and struck out in red means it answers but refuses this country.
- **A protocol that doesn't connect stays on its server's card.** Before, a way whose exit could not be looked up (AnyTLS + ECH failing) stood as a server of its own; now it joins the server the rest of its configuration reaches.

## 0.6.8 — 12.11.2026

**A server on the Servers page says where it is and whose it is.** Its head shows the flag, the country and the city, and under them the address, the hosting (by the address's reverse name, e.g. hostbrr.com) and the provider whose network it is in, with the latency on the right; the protocols are under it as before. A folded server keeps its ways' summary on a line of its own. The sites checked through a server are drawn as small YouTube, Telegram, ChatGPT and Instagram marks instead of the letters YT, TG, GPT and IG — green opens, red refuses, dim doesn't answer; the head's tip says it in words, with the address, hosting and provider.

## 0.6.7 — 11.11.2026

**One update button instead of two.** When a new version is out, the Sovereign card's «Проверить» becomes a white «Обновить» right in its head, next to «вышла 0.6.7, скачана». The wide bar under the card is gone, and the arrow in the window title no longer shows on the Settings page, where the card already says it; on the other pages it stays.

## 0.6.6 — 11.11.2026

**The Sovereign card has a «Проверить» button you can see.** The ↻ that showed only under the pointer is gone from it. «обновлений нет» now says when it was checked: «обновлений нет, проверено в 18:27» — the client looks for a new version every 12 hours, so a release can be out for hours before it asks.

## 0.6.5 — 11.11.2026

**«Пропустить последние N» no longer lets everything through with one click.** It opens a menu of the names closed in the last 2 minutes: pick one to let only it through, or «Пропустить все N». For 10 minutes after, the card shows «Пропущено в 9:40: 3 адреса» with «Вернуть», which closes them again and drops a rule left empty. Names a rule already lets through no longer come back into the list, so a second click doesn't make a second rule of them. Rules made by earlier clicks are on the Routing page.

- **The minute history starts where the counting did.** In the first hour the trace begins at its start time instead of a blank «час назад», and the head no longer repeats the number next to it: «за 12 минут» beside «7 закрыто».
- **The DNS menus open under their text**, not at the card's edge, and the DNS rows' colour marks sit beside their names.

## 0.6.4 — 11.11.2026

**Settings are instruments now.** Five cards instead of sections with headings: Kill switch, «Закрыть от локальной сети», Блокировка, Тонкая настройка and Sovereign. Each card's head has a lamp, the name and one line about what it does right now, e.g. «готов: закроет интернет, если VPN оборвётся». A card that needs attention says so with an amber or red edge and a white line in its head. The «?» and ↻ in a head show while you point at it.

- **The kill switch is under a lid while it's on.** The first click lifts the lid, a second click within 5 seconds turns it off; then the lid closes by itself. A kill switch that failed to start says why in its head.
- **Whether the computer is seen in the local network is said in that card's head**, and ↻ checks again. The amber «Виден в сети» banner is gone; the red one for the firewall stays. The add row counts the devices around: «Ещё 2 устройства в сети — вход закрыт».
- **Блокировка shows how many addresses were closed since start and the last hour by minutes.** The categories are chips with a dot. «Недавно» shows the latest closed names, and «Пропустить последние N» lets through everything closed in the last 2 minutes at once. The minute history lives in the tray, so it starts empty after the tray restarts.
- **Тонкая настройка's head shows what's set**: DNS, QUIC and IPv4. Sovereign's head shows the versions and the update state; «Проверить сейчас» became the ↻ in it.

## 0.6.3 — 11.11.2026

**Updates no longer depend on one GitHub account.** A release is a signed description (update.json): the version, the installer's size and sha256, where the installer is and where to ask for the next description. The client trusts the signature, not the address it came from. It asks GitHub and updates.packetlab.tech, and remembers addresses a release names, so a release can move the next update. The .sha256 file is still published for versions older than this one.

## 0.6.2 — 11.11.2026

**«Виден ли компьютер в сети» no longer warns while «Закрыть от локальной сети» is on.** With the VPN on, Sovereign's filters keep the local network away from file sharing and the other services, so the line says «закрыт фильтром Sovereign» in green; its tooltip names the services and reminds that without the VPN they're open to the network again. The firewall turned off is still shown in red. Switching «Закрыть от локальной сети» checks the line again at once.

## 0.6.1 — 11.11.2026

**No "Проверка" page any more: the client checks its protection by itself.** While you're connected it looks at what sites see and whether anything leaks past the VPN (DNS, WebRTC, IPv6): a few seconds after connecting, when the exit address changes (another server, or auto moved), and every half hour. Nothing is shown while all is fine. A problem is said once in a notification and stays on the main screen as a banner with its fix, e.g. «Утечка мимо VPN: IPv6 — включить Kill switch». The server card's tooltip says what was found and how long ago; its right-click menu checks again.

**With the kill switch off while connected, the main screen says so**: «Защита от утечек выключена — включить».

### Servers

- **"Замерить" no longer hangs.** A speed run said «меряю…» for good and measured nothing.
- **Every server can be measured**, not just the one in use, and a «Напрямую» row measures your line without the VPN. The figure grows while the run goes.

### Moved from the checks page

- **«Не открывается сайт?»** is a line on the Routing page.
- **Whether the computer is seen in the network** is a line in Settings, checked by itself after connecting. With the firewall off or letting everything in, a banner opens Windows Firewall. With sharing services answering, a banner offers to close the local network.

## 0.6.0 — 11.11.2026

**A new home.** The project lives at [github.com/sovereignbrains-usa/Sovereign](https://github.com/sovereignbrains-usa/Sovereign) now. Updates, the problem report and the installer's links all point here.

**If you have 0.5.x, install this one by hand once.** 0.5.130 and older look for updates at the old address and won't find this one. From 0.6.0 on, updates come in place as before. Your settings and configurations stay.

### Protection

- **The kill switch lets DHCP through to the local network only**, from the client's own port (68/546). Before, UDP to port 67/547 of any address was a way past the tunnel for any program, no admin rights needed. IPv6 neighbour discovery is let through under the kill switch too (before it worked only with the local network closed).
- **A closed local network lets DHCP in only from a server's port** (67/547). While the network is closed the router's DNS is shut for the core as well, so a "system" local DNS and the bootstrap one are encrypted instead.
- **A restarted service remembers the filters.** After a crash or an update, the service used to take the filters in place for the kill switch alone. The closed local network and the devices let into it were lost, and the next connection applied that, until the tray next sent them. Now the filters carry their settings, and the service reads them back.

### Routing

- **Addresses in rules are checked strictly.** A bad one (`999.1.1.1`, `10.0.0.0/33`, `1::2::3`) is left out instead of the whole config being refused. `1.2.3.4:443` and `[2001:db8::1]:443` lose the port.
- **The per-app rule comes after the blocks**: the local network, blocked programs, your sites, the lists, DNS over HTTPS. In "only these programs" mode, those programs go through the VPN itself.
- **The public DNS-over-HTTPS names are always closed while the block is on.** Your own DNS servers' names among them are resolved first through the local DNS. Before, your server `security.cloudflare-dns.com` opened all of `cloudflare-dns.com`.
- **A blocked program stays blocked even toward the local network**, and a program sent directly keeps QUIC.
- **A rule list the core can't read no longer leaves you without the VPN.** It is deleted and downloaded again, and left out if it fails a second time. The first download fetches the missing lists faster and shows how many have come in: «Списки правил скачиваются: N из M».

### Service and tray

- **The core's DLL is tried again at the start**, 5 times 2 s apart. One failed load used to mean no VPN, at boot or from the tray, until the service restarted.
- **One tray per computer, not per session.** A second user's tray used to turn off the first one's kill switch, replace its boot config and restart the box with its own config. Started in another session, it now says so instead.
- **Autostart follows the copy that's running.** A Run entry pointing to a moved or deleted folder started an old tray, or nothing, at every sign-in. Now it is rewritten.
- **The uninstaller cleans up every user signed in**: their autostart entry and its Task Manager mark go, not only those of the user who uninstalls. A value of the same name that belongs to another program stays.
