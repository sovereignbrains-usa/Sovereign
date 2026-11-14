#include "main_window.h"

#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite_3.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cube.h"
#include "diagnostics.h"
#include "flags.h"
#include "icons.h"
#include "log_events.h"
#include "resource.h"
#include "ui_style.h"

namespace sovereign::tray {

namespace {

using ui::FromColorRef;
using ui::Rgb;

constexpr wchar_t kClassName[] = L"SovereignMainWindow";

// Layout, in DIPs (1/96 inch): render targets work in DIPs at the window's
// DPI, so nothing below is scaled by hand.
constexpr float kDefaultWidth = 420;
constexpr float kDefaultHeight = 660;
constexpr float kMinWidth = 360;
constexpr float kMinHeight = 440;
constexpr float kPad = 16;         // the page's margins
constexpr float kCaption = 36;     // our own title bar (Painter::Caption): the system's is gone
constexpr float kMaxPage = 560;    // wider windows keep the page at this, centered
constexpr float kRow = 44;         // a list row
constexpr float kSettingsRow = 34;  // a row of the settings: at 44 they did not fit a screen (30.10.2026, his: "не помещаются")
constexpr float kLatencyRow = 56;  // a server to pick: its name and latency, what the measurement is made of
constexpr float kProfileRow = 54;  // a configuration's row: its name and its state
constexpr float kPillPad = 8;      // a Pill's text from its edge, each side
constexpr float kCheckRow = 42;    // a check: its title and "?", its result on the right
constexpr float kSwitchRow = 48;   // a switch whose explanation is in its "?"
constexpr float kHint = 18;        // a "?" that shows a tip when pointed at

constexpr float kRouteButton = 100;  // the route's "Проверить", a word, not an arrow
constexpr float kStackLine = 18;     // what a row grows by when its value stands under its title (2026-11-03: words wrap, they aren't cut with "…")
constexpr float kServerRow = 60;   // a server on a configuration's page: its name, protocol and switch
constexpr float kButton = 34;
constexpr float kGap = 8;          // between cards (the law's 8)
constexpr float kSettingsGap = 8;   // between the settings' cards
constexpr float kRadius = 8;
constexpr float kWheelStep = 64;
constexpr float kAppIcon = 20;     // a program's icon in the per-app list
constexpr float kTag = 32;         // a thing on the routing scheme, and its "+"
// The routing scheme (Routing): its lines down a strip at the page's left, the words and things
// past it (the metro map he took, 03.11.2026: "симуляция веток метро").
constexpr float kSchemeIndent = 36;  // the words and things, from the page's left
constexpr float kLineX = 10;         // the main line, from the page's left
constexpr float kLineTurn = 10;      // a branch's turn off it: its radius
constexpr float kStationX = 24;      // a station, from the page's left
constexpr float kMenuDot = 28;       // the mode menu: a row's words from its left (the dot of the way's colour before them)
constexpr float kMenuCheck = 32;     // and from its right (the check)
constexpr float kPillSide = 12;      // the mode's pill: its words from its ends
constexpr float kChevron = 14;       // and the chevron after them
constexpr ULONGLONG kToggleGraceMs = 400;  // a tray click right after the window lost focus hides it

// The log page: a row's height (an event: what happened, and with what under
// it), and how many lines it keeps (the tray keeps no more either).
constexpr float kLogRow = 44;
constexpr std::size_t kLogKeep = 5000;
// Its scrollbar: the strip at its right edge a press takes - wider than the
// thumb drawn. The middle button's scrolling: a frame, the still zone round
// where it was pressed, and held this long (or moved) it ends on release.
constexpr float kLogBar = 14;
constexpr UINT_PTR kPanTimer = 3;
constexpr UINT_PTR kLidTimer = 4;    // the kill switch's lid lifted (ItemAction::LiftLid): it shuts again after this
constexpr UINT kLidUpMs = 5000;
constexpr UINT kPanFrameMs = 16;
constexpr float kPanStill = 10;
constexpr ULONGLONG kPanHoldMs = 300;

// The log's right-click menu.
// The edit box typed in lost the focus: keep what's in it (posted - the box
// can't be destroyed inside its own message).
constexpr UINT kEndEditMessage = WM_APP + 41;

constexpr UINT kLogMenuCopy = 1;
constexpr UINT kLogMenuCopyAll = 2;
constexpr UINT kLogMenuSelectAll = 3;
constexpr UINT kLogMenuSave = 4;
constexpr UINT kLogMenuClear = 5;
constexpr UINT kLogMenuLevel = 6;  // the "⋯" key's: how much the core writes (UiCommand::ChooseLogLevel)

// A right-click menu's items, on anything but the log (ItemMenu).
constexpr UINT kMenuOpen = 101;
constexpr UINT kMenuTestDelays = 103;
constexpr UINT kMenuCopyName = 104;
constexpr UINT kMenuCopyIp = 105;
constexpr UINT kMenuToggle = 106;
constexpr UINT kMenuRefresh = 107;
constexpr UINT kMenuCopyLink = 108;
constexpr UINT kMenuRename = 109;
constexpr UINT kMenuRemove = 110;
constexpr UINT kMenuRun = 111;
constexpr UINT kMenuCopyResult = 112;
constexpr UINT kMenuCopyAll = 113;
constexpr UINT kMenuToggleExitIp = 114;
constexpr UINT kMenuCopyValue = 115;
constexpr UINT kMenuRefreshDirect = 116;
constexpr UINT kMenuCheckProtection = 117;
constexpr UINT kMenuHysteriaSpeed = 120;  // + its index in kHysteriaSpeeds

// A tip (TipText) shows when its item is pointed at this long; a "?"'s at once.
constexpr UINT_PTR kTipTimer = 1;
constexpr UINT kTipDelayMs = 500;
// The connect cube's motion (cube.h): frames while it moves, a turn into a
// new state's pose, the turning while it connects (degrees per ms).
constexpr UINT_PTR kCubeTimer = 2;
constexpr UINT kCubeFrameMs = 15;
constexpr UINT kCubeIdleFrameMs = 33;  // the idle life (Idle) is slow: 30 frames a second are plenty
constexpr ULONGLONG kCubeTurnMs = 520;
constexpr UINT kQuestionInMs = 170;  // MainWindow::Ask's card coming in
constexpr UINT kFoldMs = 240;        // a fold opening or closing (law.md --d-fold)
constexpr float kCubeSpin = 0.12f;

// Segoe Fluent Icons (Windows 11; the same code points in Segoe MDL2 Assets).
constexpr const wchar_t* kGlyphSync = L"\xE895";
constexpr const wchar_t* kGlyphBack = L"\xE72B";
constexpr const wchar_t* kGlyphChevron = L"\xE76C";
constexpr const wchar_t* kGlyphChevronDown = L"\xE70D";  // a fold that opens
constexpr const wchar_t* kGlyphChevronUp = L"\xE70E";    // ...and closes
constexpr const wchar_t* kGlyphLog = L"\xE8A5";
constexpr const wchar_t* kGlyphSettings = L"\xE713";
constexpr const wchar_t* kGlyphClose = L"\xE8BB";  // ChromeClose
constexpr const wchar_t* kGlyphMinimize = L"\xE921";  // ChromeMinimize
constexpr const wchar_t* kGlyphPower = L"\xE7E8";
constexpr const wchar_t* kGlyphPaste = L"\xE77F";
constexpr const wchar_t* kGlyphRefresh = L"\xE72C";
constexpr const wchar_t* kGlyphRemove = L"\xE711";
constexpr const wchar_t* kGlyphAdd = L"\xE710";
constexpr const wchar_t* kGlyphFile = L"\xE8E5";
constexpr const wchar_t* kGlyphFolder = L"\xE8B7";
constexpr const wchar_t* kGlyphCopy = L"\xE8C8";
constexpr const wchar_t* kGlyphCheck = L"\xE73E";
constexpr const wchar_t* kGlyphWarning = L"\xE7BA";
constexpr const wchar_t* kGlyphProgram = L"\xE7C4";
constexpr const wchar_t* kGlyphStopwatch = L"\xE916";
constexpr const wchar_t* kGlyphDownload = L"\xE896";
constexpr const wchar_t* kGlyphUndo = L"\xE7A7";
constexpr const wchar_t* kGlyphSave = L"\xE74E";
constexpr const wchar_t* kGlyphPause = L"\xE769";
constexpr const wchar_t* kGlyphPlay = L"\xE768";
constexpr const wchar_t* kGlyphFilter = L"\xE71C";
constexpr const wchar_t* kGlyphMore = L"\xE712";    // "⋯": what else can be done here
constexpr const wchar_t* kGlyphSearch = L"\xE721";
constexpr const wchar_t* kGlyphQrCode = L"\xED14";
constexpr const wchar_t* kGlyphDelete = L"\xE74D";
constexpr const wchar_t* kGlyphRename = L"\xE8AC";
constexpr const wchar_t* kGlyphClock = L"\xE917";
constexpr const wchar_t* kGlyphRoute = L"\xE8F0";
constexpr const wchar_t* kGlyphShield = L"\xEA18";
constexpr const wchar_t* kGlyphLightning = L"\xE945";   // auto: the fastest server by itself
constexpr const wchar_t* kGlyphDiagnostic = L"\xE9D9";  // the measurements in full
constexpr const wchar_t* kGlyphReport = L"\xEBE8";
constexpr const wchar_t* kGlyphServer = L"\xE839";  // Network: a server of one's own  // Bug: "something's wrong", plainer than the feedback bubble

// The three ways to add a configuration, each a key of its own with its tip
// where there was one "+" with a list dropping from it (his, 30.10.2026:
// 'вместо кнопки добавить и плюсика эти 3 расположить и подсказку на каждую').
struct AddWay {
  const wchar_t* name;
  const wchar_t* glyph;
  UiCommand command;
  const wchar_t* tip;
};
const std::array<AddWay, 4> kAddWays = {{
    {L"Вставить из буфера", kGlyphPaste, UiCommand::PasteSubscription,
     L"Вставить из буфера: ссылка подписки, ключ или конфиг"},
    {L"QR-код с экрана", kGlyphQrCode, UiCommand::ScanScreen, L"QR-код с экрана: найти его и добавить подписку"},
    {L"Из файла…", kGlyphFile, UiCommand::ImportFile, L"Из файла: конфиг, ключи или картинка с QR-кодом"},
    {L"Свой сервер…", kGlyphServer, UiCommand::AddOwnServer,
     L"Свой сервер: адрес VPS (root@1.2.3.4), клиент зайдёт по ssh-ключу и поставит на него VPN"},
}};

enum class Kind : std::uint8_t {
  Card,          // a rounded panel behind other items
  Divider,       // a line across a card
  Title,         // the page's title
  Heading,       // a card's title
  Text,          // one line
  Muted,         // one line, secondary
  Wrap,          // wrapped paragraph, secondary
  Caption,       // wrapped paragraph, secondary, small
  ErrorText,     // wrapped paragraph with a warning glyph, red
  Field,         // a label on the left, a value on the right
  Power,         // the overview's on/off button
  ServerCard,    // the server in use: flag and name, its network and latency; opens the servers
  Banner,        // a one-line call to action, lit (`checked`: what's wrong, amber)
  Button,        // a normal button
  AccentButton,  // the page's main action
  DangerButton,  // quit
  IconButton,    // a square button with a glyph; `checked`: a dot on it (something new there)
  Switch,        // a row with a toggle switch
  Choice,        // a pickable row with a radio mark
  ProfileRow,    // a configuration: name, its state under it (`warn`: in red), dimmed when off (!`checked`)
  Toggle,        // just a switch, no text (`checked`: on)
  CheckRow,      // a check: a dot in its status' colour (index: UiCheck::Status), title, result, what it means
  Segment,       // half of a two-way switch
  Chip,          // an on/off filter: tinted when on
  AppRow,        // a program in the per-app list
  LogBox,        // the log's lines
  Hint,          // a "?": pointed at, its text shows in a tip
  ValueRow,      // a setting with its value on the right and a chevron: a click picks another
  Meter,         // a thin bar: how much is used (index: per mille)
  Tag,           // a thing on the routing board: a program's icon (text2: its exe), its name, a detail after it
  AppTitle,      // our title bar's name, after the app's icon (text2: the exe it's taken from)
  Question,      // MainWindow::Ask's card, the window dimmed round it (index: its UiQuestion::Tone)
  Pill,          // a small label in a rounded outline, not a button (`warn`: amber)
  Day,           // a calendar's key on a question's card: a day (text: its number, checked: chosen, sub 1: today) or a step (glyph set, sub: -1 back, 1 on)
  Lead,          // the routing page's opening words, larger: "Всё идёт"
  ModePill,      // the routing mode, a pill with a chevron: a click drops its menu (`checked`: it's open)
  Route,         // a line of the routing scheme (rail: its way; sub: kRouteSpine, kRouteStation, kRouteEnd)
  GhostAdd,      // a dashed key adding to a way (rail: the way); its text empty: a square "+"
  Menu,          // a dropped menu's panel (the routing mode's), over the page
  MenuItem,      // a choice on it: its way's dot (rail), a title, what it means (detail), a check (`checked`)
  ServerColumns, // the heads of a server's ways' columns: ОТКРЫТИЕ, ИГРЫ, СКОРОСТЬ, over them
  Way,           // a way to a server (index: protocols; checked: the traffic goes by it, sub 1: pinned by hand): dot, name, figures
  Link,          // a word that is a button, in the VPN's blue (a way's "замерить")
  Instrument,    // a Settings card's head: a lamp (lamp), the name, what it does now under it (detail; `loud`: in white),
                 // on the right a switch (sub 1; `lid`: under a lid) or a fold's chevron (sub 2) - circle = state
  Dot,           // an on/off filter as a thing: a hairline rectangle, a dot in it that's lit when on (`checked`)
  Trace,         // the last hour of blocks, a bar a minute (text2: one wchar per minute, its height in 0..1000)
  Readout,       // a big number with its unit under it, right-aligned (text: the number, detail: the unit)
  Find,          // the log's search: a box typed in on a click (text: what's sought; empty: what can be)
};

// The servers' columns at a Choice's right edge, from the edge in: the failures (a dot and a count), then the
// latency. The latency's is as wide as "нет ответа" (or "48 ±3 мс" in full) needs.
constexpr float kFailCol = 30;
constexpr float kDelayColNarrow = 80;
constexpr float kDelayColFull = 88;
constexpr float kColGap = 12;  // the Choice's inner margin to the right of the last column
constexpr float kColSplit = 4;  // between the latency's column and the failures'

// The routing scheme's lines (Kind::Route's sub). The main line runs from
// the mode down to the way everything else goes, a station at its end; a
// way of its own branches off it to a station, the block to a dead end.
constexpr int kRouteSpine = 0;    // rect.top: the mode's node, rect.bottom: where it turns into its way
constexpr int kRouteStation = 1;  // a branch, the station on the middle of its rect
constexpr int kRouteEnd = 2;      // a branch ending in a bar: the block

// A Muted line with a lamp (Item::lamp): the dot's room before the text.
constexpr float kMutedLamp = 13;

// EditName: the configuration `index`'s name, typed over the page's title.
// Hint: nothing on a click - it's there to be pointed at.
enum class ItemAction : std::uint8_t {
  None,
  Command,
  Page,
  OpenProfile,
  EditName,
  LogFilter,   // the log's filter (index: kLogFilterNames)
  PauseLogs,
  SaveLogs,
  CopyLogs,    // the log's lines as the core wrote them (index 0: the selected rows', 1: all)
  LogMore,     // the log's "⋯": save, how much the core writes, clear
  FindLog,     // the log's search box: typed in
  LogOnly,     // the selected row's lines one by one, nothing else; or back to all
  RouteLog,    // the routing board's "ещё N": the log of the sites that went that way (index: bucket)
  LogServer,   // the servers' page, for the selected row's server
  ToggleMore,  // the configurations' relay fold, folded or not (the window's own)
  ProfileTab,  // the configurations' tab (index: 0 subscriptions, 1 own servers; the window's own)
  ToggleServer,  // a server's card opened or folded (text2: its key, index: open now; the window's own)
  Hide,        // our title bar's cross: the window away to the tray
  RouteMenu,   // the routing mode's pill: its menu dropped or put away (the window's own)
  Hint,
  Answer,      // a question's button (index: 1 yes, 0 no); while one is open, the only ones that take a click
  LiftLid      // the kill switch's lid: lifted for 5 s, the next click turns the switch (the window's own)
};

struct Item {
  Kind kind = Kind::Text;
  D2D1_RECT_F rect{};
  std::wstring_view glyph;  // empty: none (never a null pointer - /analyze)
  std::wstring text;
  std::wstring detail;
  std::wstring text2;  // a third line where a kind has one (CheckRow: what the result means)
  std::wstring tip;    // what's in it and what a click does, where its label doesn't say: shown pointed at a while
  std::string logo;    // a Tag's service logo (icons.h ServiceLogo): the service's id; a ValueRow's, before its title
  ItemAction action = ItemAction::None;
  UiCommand command = UiCommand::Toggle;
  int index = 0;
  int sub = 0;  // a second index for the command: a server within a configuration
  bool checked = false;
  bool enabled = true;
  bool warn = false;    // something's wrong with it (a ProfileRow's refresh failed)
  bool glass = false;   // a Card on the main screen: see-through - the light and the streams behind it show
  bool scrolls = true;  // lives in the page (scrolls, clipped), not in the rail
  bool hinted = false;  // a ValueRow with a "?" after its title: the value starts after that
  bool stacked = false;  // a ValueRow or CheckRow whose value didn't fit beside its title: it stands under it, the row taller (kStackLine)
  bool key = false;     // an IconButton drawn as a key with height (DrawKey), not flat
  bool chrome = false;  // an IconButton of the title bar (DrawChrome): its pill inset from the bar's edges
  bool engraved = false;  // a Muted label set as engraved capitals (engraved_)
  int rail = -1;        // a Card or Pill of the routing board: the way it's of (a bucket, BucketColor); -1: none
  bool quiet = false;   // a Card with a rail: only the rail, no panel - an empty way, a line to find
  int tick = -1;        // a Switch or ValueRow of known fate (a bucket, BucketColor): its mark on the card's edge; -1: none
  bool tickAlways = false;  // the mark stays with the switch off (a DNS way); else it burns while the switch is on
  float fold = -1;      // a fold's handle: how open it is (0..1), its chevron turned by it; -1: not a fold's
  float clip = -1;      // a fold's row while the fold moves: seen and clicked above this line only; -1: whole
  float indent = 0;     // a row (ValueRow, Switch, Divider) in an instrument's card: in by this, under its name
  int lamp = 0;         // an Instrument's lamp: 0 none (nothing to check), 1 works, 2 worth fixing, 3 triggered
  int verdict = 0;      // a Card's outline: 0 none, 2 amber (worth fixing), 3 red (triggered)
  bool shy = false;     // drawn only while pointed at (it or what it sits on) or focused: a ↻, a "?"
  bool lid = false;     // an Instrument's switch under a lid: a click lifts it first
  bool lifted = false;  // the lid up: a click turns the switch now (its hinge drawn over the switch)
  bool loud = false;    // an Instrument's detail in white: it tells what's wrong
};

// What of an item is seen: its rectangle, a moving fold's row cut at the fold's foot.
inline D2D1_RECT_F Seen(const Item& it) {
  D2D1_RECT_F r = it.rect;
  if (it.clip >= 0) {
    r.bottom = std::min(r.bottom, it.clip);
  }
  return r;
}

// How open a fold is: while it moves, the window's number; at rest, its flag.
inline float FoldOf(bool open, float moving) { return moving >= 0 ? moving : (open ? 1.0f : 0.0f); }


// A way's colour on the routing board: a rail down its card's left edge and
// a dot on its labels - the traffic's own (ui_style.h), the cube's blue and
// the dots flying out of it, not a palette of the board's. Dull where it's
// drawn (30.10.2026, his: "менее цвета неоновые"): kRailAlpha. By bucket: 0
// directly, 1 through the VPN, 2 blocked, 3 WARP.
constexpr COLORREF BucketColor(int bucket) {
  switch (bucket) {
    case kBucketDirect: return ui::kDirect;
    case kBucketBlock: return ui::kDanger;
    case kBucketWarp: return ui::kWarp;
    default: return ui::kVpn;
  }
}
constexpr float kRailAlpha = 0.7f;
constexpr float kRailQuiet = 12;  // an empty way's rail: a short stroke, not a pale long one
// One line for every drawn icon - the title bar's, a row's chevron and "+",
// the "?"'s ring, a switch's groove: a family of one weight (30.10.2026, a
// review's "иконки смешаны по стилю").
constexpr float kIconStroke = 1.3f;
constexpr float kSiteIcon = 16;  // a site's mark on a server's head (DrawSiteIcon)
constexpr float kServerFold = 32;  // a server's chevron (opens its card) at its head's right, the latency left of it

// A Settings card as an instrument (docs/design/settings-instruments.html, 11.11.2026): its head a lamp,
// the name and what it does now, its rows in under the name. From the head's left (4 in from the card):
// the lamp's middle on 20, the name on 34 - the line 38 its rows' text keeps (kInstrumentIndent past 20).
constexpr float kInstrumentLamp = 20;
constexpr float kInstrumentText = 34;
constexpr float kInstrumentIndent = 18;
constexpr float kInstrumentHead = 56;  // at least: a detail that wraps makes it taller
constexpr float kDotInk = 31;          // a Dot chip's room besides its word: 9 + the dot 6 + 6 + the word + 10
constexpr float kDotHeight = 28;
constexpr float kTraceBars = 22;       // the hour's bars; under them "час назад" and "сейчас"
constexpr float kTraceHeight = 42;

constexpr float kReadoutWidth = 110;   // a count read off on an instrument's right: "12 345" over "закрыто с 9:12"

// What an Instrument's right keeps for its control: a switch, a fold's chevron, a count (sub 3: a Readout laid over it).
constexpr float kHeadButtonWidth = 112;  // a button on an instrument's right: "Проверить", "Проверяю…", "Обновить"
constexpr float InstrumentControl(const Item& it) {
  switch (it.sub) {
    case 1: return 16 + 40 + 12;
    case 2: return 16 + 14 + 8;
    case 3: return 16 + kReadoutWidth + 10;
    case 4: return 16 + kHeadButtonWidth + 10;
    default: return 16;
  }
}

// What an item's tip says: a "?"'s text, its own tip, an icon button's name;
// empty: it has none.
std::wstring_view TipText(const Item& it) {
  if (!it.tip.empty()) {
    return it.tip;
  }
  return it.kind == Kind::Hint || it.kind == Kind::IconButton ? std::wstring_view(it.text) : std::wstring_view();
}

struct Layout {
  std::vector<Item> items;
  float contentHeight = 0;  // the page's full height from viewTop, for scrolling
  float scroll = 0;         // how far the page is scrolled
  float viewTop = 0;        // the scrolling viewport, in window DIPs
  float viewBottom = 0;
  std::optional<D2D1_RECT_F> logBox;
  std::optional<D2D1_RECT_F> glow;  // the main screen's switch: a light behind it, under it all (DrawBackdrop)
  float boardLeft = 0;              // the routing scheme's ways, left to right: a site is typed across them
  float boardRight = 0;
};

// A fold's body built in full from `from` to `to`, shown as far as the fold has opened: its
// card ends at the foot, the rows over it are cut there and the ones past it are gone.
// Returns the foot. (law.md: the disclosure runs 240 ms, --d-fold.)
inline float FoldBody(Layout& l, std::size_t first, float from, float to, float open) {
  if (open >= 1) {
    return to;
  }
  const float foot = from + (to - from) * std::max(open, 0.0f);
  l.items.erase(std::remove_if(l.items.begin() + static_cast<std::ptrdiff_t>(first), l.items.end(),
                               [&](const Item& it) { return it.rect.top >= foot; }),
                l.items.end());
  for (std::size_t i = first; i < l.items.size(); ++i) {
    if (l.items[i].rect.bottom > foot) {
      l.items[i].clip = foot;
    }
  }
  return foot;
}

// The log page's filters (logs-concept.html, variant A): everything, what
// went wrong (warnings, errors), the servers' ways, the sites.
constexpr int kLogFilters = 4;
constexpr const wchar_t* kLogFilterNames[kLogFilters] = {L"Всё", L"Проблемы", L"Серверы", L"Сайты"};

bool InLogFilter(const LogEvent& event, int filter) {
  switch (filter) {
    case 1: return event.tone == LogTone::Warn || event.tone == LogTone::Bad;
    case 2: return event.topic == LogTopic::Server;
    case 3: return event.topic == LogTopic::Site;
    default: return true;
  }
}

// `what` in `text`, as a person reads it: case aside ("YouTube", "youtube").
bool HasText(std::wstring_view text, std::wstring_view what) {
  if (what.empty()) {
    return true;
  }
  if (text.size() < what.size()) {
    return false;
  }
  return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | NORM_IGNORECASE, text.data(),
                         static_cast<int>(text.size()), what.data(), static_cast<int>(what.size()), nullptr, nullptr,
                         nullptr, 0) >= 0;
}

// The servers' ways as the log's events name them: the core's tag, what it
// is, where its server is (UiContent's protocols, protocolTitles, locations).
std::vector<LogWay> WaysOf(const UiContent& c) {
  std::vector<LogWay> ways;
  for (std::size_t i = 0; i < c.protocols.size(); ++i) {
    LogWay way;
    way.tag = c.protocols[i];
    way.title = i < c.protocolTitles.size() ? c.protocolTitles[i] : std::wstring();
    if (i < c.locations.size()) {
      const UiLocation& at = c.locations[i];
      way.where = at.ip;
      if (!at.countryName.empty()) {
        way.where = way.where.empty() ? at.countryName : way.where + L" · " + at.countryName;
      }
    }
    ways.push_back(std::move(way));
  }
  return ways;
}

// The log page's state: the core's lines as it wrote them (newest last -
// what's copied, saved and goes into a report) and each read as an event
// (log_events.h); the rows the filter and the search show, one per kind of
// event with how many times it came ("Только такие": that kind's one by
// one), how far they are scrolled and which are selected - from anchor to
// caret, inclusive, as rows. Paused, new lines wait in `held` and the view
// stays still.
struct LogView {
  std::deque<std::wstring> lines;
  std::deque<LogEvent> events;  // one per line
  std::vector<LogWay> ways;     // what the events name the servers' ways by
  std::vector<int> shown;       // a row's newest line, oldest row first
  std::vector<int> repeats;     // how many lines a row stands for
  int filter = 0;               // kLogFilterNames
  std::wstring only;            // a kind's key: its lines one by one, nothing else; empty: all
  std::wstring onlyTitle;       // what that kind is
  LogRoute route = LogRoute::None;  // only the sites that went this way (the routing board's "ещё N"); None: all
  std::wstring search;          // only the rows with it in their words or lines
  std::array<int, kLogFilters> counts{};  // kinds of event per filter
  bool paused = false;
  std::vector<std::wstring> held;
  float scroll = 0;     // DIPs from the first row
  bool follow = true;   // stays at the bottom as lines come
  int anchor = -1;      // -1: no selection
  int caret = -1;
  bool barHot = false;  // the pointer on the scrollbar, or dragging it: drawn wider
  std::optional<D2D1_POINT_2F> pan;  // the middle button's scrolling: where it was pressed

  int Count() const { return static_cast<int>(shown.size()); }
  const std::wstring& Line(int i) const { return lines[static_cast<std::size_t>(shown[static_cast<std::size_t>(i)])]; }
  const LogEvent& Event(int i) const { return events[static_cast<std::size_t>(shown[static_cast<std::size_t>(i)])]; }
  int Repeats(int i) const { return repeats[static_cast<std::size_t>(i)]; }
  bool HasSelection() const { return anchor >= 0 && caret >= 0 && caret < Count(); }
  bool Selected(int i) const { return HasSelection() && i >= std::min(anchor, caret) && i <= std::max(anchor, caret); }
  float Height() const { return static_cast<float>(shown.size()) * kLogRow; }
  // A row about one of the servers' ways: there's a server to show.
  bool AboutWay(int i) const {
    const std::wstring& way = Event(i).way;
    return !way.empty() && std::any_of(ways.begin(), ways.end(), [&](const LogWay& w) { return w.tag == way; });
  }

  void Push(std::wstring line) {
    events.push_back(EventOf(line, ways));
    lines.push_back(std::move(line));
  }

  // The oldest `n` lines gone.
  void Cut(std::size_t n) {
    n = std::min(n, lines.size());
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(n));
    events.erase(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(n));
    for (int& line : shown) {
      line -= static_cast<int>(n);  // a row's line gone: below 0, its row found again by its kind
    }
  }

  void Clear() {
    lines.clear();
    events.clear();
  }

  // The servers' ways changed (a refresh, a renamed server): the lines read again. False: the same.
  bool SetWays(std::vector<LogWay> fresh) {
    if (fresh == ways) {
      return false;
    }
    ways = std::move(fresh);
    for (std::size_t i = 0; i < lines.size(); ++i) {
      events[i] = EventOf(lines[i], ways);
    }
    return true;
  }

  bool Matches(std::size_t i) const {
    if (search.empty()) {
      return true;
    }
    const LogEvent& event = events[i];
    return HasText(event.title, search) || HasText(event.detail, search) || HasText(lines[i], search);
  }

  // The rows again, from the newest line back: a kind seen already adds to its row. The
  // selection stays on its kinds where they are still shown.
  void Rebuild() {
    // A selected row's newest line and its kind (none: -1, empty).
    const auto lineOf = [this](int row) { return row >= 0 && row < Count() ? shown[static_cast<std::size_t>(row)] : -1; };
    const auto keyOf = [this](int line) {
      return line >= 0 && static_cast<std::size_t>(line) < events.size() ? events[static_cast<std::size_t>(line)].key
                                                                          : std::wstring();
    };
    const bool selection = HasSelection();
    const int anchorLine = selection ? lineOf(anchor) : -1;
    const int caretLine = selection ? lineOf(caret) : -1;
    const std::wstring anchorKey = keyOf(anchorLine);
    const std::wstring caretKey = keyOf(caretLine);
    shown.clear();
    repeats.clear();
    counts.fill(0);
    std::array<std::unordered_set<std::wstring_view>, kLogFilters> kinds;
    std::unordered_map<std::wstring_view, std::size_t> rows;
    const bool merge = only.empty();
    for (std::size_t i = events.size(); i-- > 0;) {
      const LogEvent& event = events[i];
      if (event.hidden) {
        continue;
      }
      for (std::size_t f = 0; f < kinds.size(); ++f) {
        if (InLogFilter(event, static_cast<int>(f)) && kinds[f].insert(event.key).second) {
          ++counts[f];
        }
      }
      if (route != LogRoute::None && event.route != route) {
        continue;
      }
      if (merge ? !InLogFilter(event, filter) : event.key != only) {
        continue;
      }
      if (!Matches(i)) {
        continue;
      }
      if (merge) {
        if (const auto [at, fresh] = rows.try_emplace(event.key, shown.size()); !fresh) {
          ++repeats[at->second];
          continue;
        }
      }
      shown.push_back(static_cast<int>(i));
      repeats.push_back(1);
    }
    std::reverse(shown.begin(), shown.end());
    std::reverse(repeats.begin(), repeats.end());
    // The selection: on the same lines where they are rows still (one by one), else on the same kinds.
    const auto rowOf = [this](int line, const std::wstring& key) {
      int byKey = -1;
      for (int r = 0; r < Count(); ++r) {
        if (shown[static_cast<std::size_t>(r)] == line) {
          return r;
        }
        if (byKey < 0 && !key.empty() && Event(r).key == key) {
          byKey = r;
        }
      }
      return byKey;
    };
    if (selection) {
      caret = rowOf(caretLine, caretKey);
      anchor = rowOf(anchorLine, anchorKey);
      if (caret < 0 || anchor < 0) {
        anchor = caret = -1;
      }
    } else {
      anchor = caret = -1;
    }
  }
};

// The log's scrollbar thumb in `box` - its strip's width, the drawn part is
// thinner; none while every line fits.
std::optional<D2D1_RECT_F> LogThumb(D2D1_RECT_F box, const LogView& log) {
  const float view = box.bottom - box.top;
  if (log.Height() <= view + 1) {
    return std::nullopt;
  }
  const float thumb = std::max(32.0f, view * view / log.Height());
  const float top = box.top + (view - thumb) * (log.scroll / (log.Height() - view));
  return D2D1_RECT_F{box.right - kLogBar, top, box.right, top + thumb};
}

// What the log page's layout shows of the log's state.
struct LogState {
  bool paused = false;
  std::size_t held = 0;
  std::array<int, kLogFilters> counts{};
  int filter = 0;
  std::wstring search;
  bool only = false;       // a kind's lines one by one (LogView::only)
  std::wstring onlyTitle;  // that kind
  std::wstring route;      // the sites of one way only (LogView::route): which, in words; empty: all
  bool selected = false;   // a row is: its card under the list
  std::wstring detail;     // the selected row's line as the core wrote it
  bool server = false;     // it's about one of the servers' ways: "Показать сервер"
};

// The live traffic round the main screen's switch (UiContent::flow*): dots
// along paths - up through the cube for the VPN (WARP's in its own colour),
// round its side for what goes directly - as many and as fast as the rates
// say; a blocked ad flies at the cube and breaks on it in red sparks. Moved
// a frame a screen refresh while the overview shows (Impl::liveAt), never
// with Windows' animations off.
enum class Stream : std::uint8_t { Vpn, Warp, Direct, Blocked, Spark };

struct Particle {
  Stream stream = Stream::Vpn;
  float t = 0;         // 0..1 along its path; a spark: its age, seconds
  float speed = 0;     // t a second
  float offset = 0;    // -1..1 across its path
  float size = 2;      // DIPs
  D2D1_POINT_2F at{};  // a spark: where it broke off
  D2D1_POINT_2F v{};   // a spark: DIPs a second
};

struct LiveTraffic {
  std::vector<Particle> particles;
  std::array<float, 3> owed{};  // dots due a stream (Vpn, Warp, Direct), the fraction kept
  std::uint64_t blockedSeen = 0;
  bool blockedKnown = false;
  std::uint32_t seed = 0x2545F491u;

  float Random() {  // 0..1
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 8) / 16777216.0f;
  }
};

constexpr std::size_t kMaxParticles = 600;
constexpr float kSparkLife = 0.6f;

// Dots a second for `rate` bytes a second: a few for a trickle, a river for
// a video or two (about 130 at 1 MB/s, 160 at most); none for nothing.
float DotsPerSecond(double rate) {
  return rate < 64 ? 0.0f : std::min(160.0f, 3 + 26 * std::log2(1 + static_cast<float>(rate) / 32768));
}

// A dot's size: bigger as the stream swells, DIPs.
float DotSize(double rate, float random) {
  return 1.2f + 1.2f * random + std::min(1.2f, std::log2(1 + static_cast<float>(rate) / 262144) * 0.5f);
}

// How fast: a trip of a little over two seconds, quicker as the rate grows.
float DotSpeed(double rate) {
  return 0.42f + 0.3f * std::min(1.0f, std::log2(1 + static_cast<float>(rate) / 65536) / 5);
}

// Where a dot is, round the switch's square `cube`: from under it up to over it.
D2D1_POINT_2F DotAt(const Particle& p, D2D1_RECT_F cube) {
  const float s = cube.right - cube.left;
  const D2D1_POINT_2F c{(cube.left + cube.right) / 2, (cube.top + cube.bottom) / 2};
  const float bottom = c.y + s * 1.1f;
  const float top = c.y - s * 1.1f;
  switch (p.stream) {
    case Stream::Vpn:
    case Stream::Warp: {
      // Up through the cube, drawn in to its middle on the way.
      const float squeeze = 1 - 0.75f * std::sin(std::numbers::pi_v<float> * std::clamp(p.t, 0.0f, 1.0f));
      const float lane = p.stream == Stream::Warp ? s * 0.07f : 0.0f;
      return {c.x + lane + p.offset * s * 0.22f * squeeze, bottom + (top - bottom) * p.t};
    }
    case Stream::Direct: {
      // Up round the cube's side - its own lane past the glass.
      const float side = p.offset < 0 ? -1.0f : 1.0f;
      const D2D1_POINT_2F a{c.x + p.offset * s * 0.18f, bottom};
      const D2D1_POINT_2F m{c.x + side * s * 1.05f, c.y};
      const D2D1_POINT_2F b{c.x + p.offset * s * 0.18f, top};
      const float u = 1 - p.t;
      return {u * u * a.x + 2 * u * p.t * m.x + p.t * p.t * b.x, u * u * a.y + 2 * u * p.t * m.y + p.t * p.t * b.y};
    }
    case Stream::Blocked:
      // Up at the cube, as far as its lower face.
      return {c.x + p.offset * s * 0.2f, bottom + (c.y + s * 0.22f - bottom) * p.t};
    case Stream::Spark:
      return {p.at.x + p.v.x * p.t, p.at.y + p.v.y * p.t + 90 * p.t * p.t};
  }
  return c;
}

// One step of `dt` seconds round `cube`: the dots move, new ones come by the
// rates (none unless `on`), a blocked ad that reached the cube breaks up.
void StepLive(LiveTraffic& live, const UiContent& c, bool on, float dt, D2D1_RECT_F cube) {
  std::vector<Particle> sparks;
  for (Particle& p : live.particles) {
    if (p.stream == Stream::Spark) {
      p.t += dt;
      continue;
    }
    p.t += p.speed * dt;
    if (p.stream == Stream::Blocked && p.t >= 1) {
      Particle hit = p;
      hit.t = 1;
      const D2D1_POINT_2F at = DotAt(hit, cube);
      for (int i = 0; i < 6 && live.particles.size() + sparks.size() < kMaxParticles; ++i) {
        // Down and out, as from a blow from below.
        const float angle = std::numbers::pi_v<float> * (0.1f + 0.8f * live.Random());
        const float speed = 50 + 80 * live.Random();
        sparks.push_back({.stream = Stream::Spark, .t = 0, .speed = 0, .offset = 0, .size = 1.2f + live.Random(),
                          .at = at, .v = {std::cos(angle) * speed, std::sin(angle) * speed * 0.6f - 40}});
      }
    }
  }
  std::erase_if(live.particles,
                [](const Particle& p) { return p.stream == Stream::Spark ? p.t >= kSparkLife : p.t >= 1; });
  live.particles.insert(live.particles.end(), sparks.begin(), sparks.end());

  const auto spawn = [&](Stream stream, double rate) {
    if (live.particles.size() < kMaxParticles) {
      live.particles.push_back({.stream = stream, .t = 0, .speed = DotSpeed(rate) * (0.85f + 0.3f * live.Random()),
                                .offset = live.Random() * 2 - 1, .size = DotSize(rate, live.Random()), .at = {}, .v = {}});
    }
  };
  const std::array<std::pair<Stream, double>, 3> streams = {
      {{Stream::Vpn, c.flowVpn}, {Stream::Warp, c.flowWarp}, {Stream::Direct, c.flowDirect}}};
  for (std::size_t i = 0; i < streams.size(); ++i) {
    live.owed[i] = on ? live.owed[i] + DotsPerSecond(streams[i].second) * dt : 0.0f;
    for (; live.owed[i] >= 1; live.owed[i] -= 1) {
      spawn(streams[i].first, streams[i].second);
    }
  }
  // The ads blocked since the last step: a dot each, a few at most at once.
  if (!live.blockedKnown || c.blockedCount < live.blockedSeen) {
    live.blockedKnown = true;
    live.blockedSeen = c.blockedCount;
  }
  for (int n = 0; on && c.blockedCount > live.blockedSeen && n < 4; ++n) {
    spawn(Stream::Blocked, 0);
    live.particles.back().speed = 0.9f;
  }
  live.blockedSeen = c.blockedCount;
}

// What a paint depends on besides the layout.
struct Interaction {
  int hover = -1;
  int head = -1;  // the settings' card head (Kind::Instrument) the pointer is on: its shy "?" and ↻ show
  int pressed = -1;
  int focus = -1;
  bool focusVisible = false;
  int tip = -1;  // the item whose tip shows: a "?" pointed at, anything else with one (TipText) pointed at a while
  // A way's line typed over (MainWindow::EditInPlace): the plate the native box sits on, rounded and
  // ringed in the way's colour; what it covers (the "+", a chip) is not drawn. editWay -1: no colour of its own.
  std::optional<D2D1_RECT_F> edit;
  int editWay = -1;
  std::optional<CubePose> cube;  // the switch mid-motion (cube.h); none: at rest in its state's pose
};

bool Contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

D2D1_RECT_F Inflate(const D2D1_RECT_F& r, float by) { return {r.left - by, r.top - by, r.right + by, r.bottom + by}; }

// The calendar on a question's card (UiCalendar): dates as days since
// 1970-01-01 in the Gregorian calendar, to move by days and by months.
// A calendar key's Item::index as an answer (below them: 0 no, 1 yes, 2 other).
constexpr int kAnswerMonthBack = 10;
constexpr int kAnswerMonthOn = 11;
constexpr int kAnswerYearBack = 12;
constexpr int kAnswerYearOn = 13;
constexpr int kAnswerDay = 100;  // + the day of the month
constexpr int kCalendarFirstYear = 2000;
constexpr int kCalendarLastYear = 2099;
constexpr std::array<const wchar_t*, 12> kMonthNames = {L"Январь", L"Февраль", L"Март",    L"Апрель",
                                                        L"Май",    L"Июнь",    L"Июль",    L"Август",
                                                        L"Сентябрь", L"Октябрь", L"Ноябрь",  L"Декабрь"};
constexpr std::array<const wchar_t*, 7> kWeekdayNames = {L"пн", L"вт", L"ср", L"чт", L"пт", L"сб", L"вс"};

std::int64_t DaysFromCivil(int year, int month, int day) {
  const int y = year - (month <= 2 ? 1 : 0);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<std::int64_t>(y - era * 400);
  const std::int64_t shifted = month > 2 ? month - 3 : month + 9;
  const std::int64_t doy = (153 * shifted + 2) / 5 + day - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void CivilFromDays(std::int64_t days, int& year, int& month, int& day) {
  const std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

int DaysInMonth(int year, int month) {
  constexpr std::array<int, 12> days = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return days.at(static_cast<std::size_t>(std::clamp(month, 1, 12) - 1)) + (month == 2 && leap ? 1 : 0);
}

// 0 for Monday: 1970-01-01 was a Thursday.
int WeekdayOf(int year, int month, int day) {
  return static_cast<int>(((DaysFromCivil(year, month, day) + 3) % 7 + 7) % 7);
}

void CalendarShiftDays(UiCalendar& c, int days) {
  CivilFromDays(DaysFromCivil(c.year, c.month, c.day) + days, c.year, c.month, c.day);
  c.year = std::clamp(c.year, kCalendarFirstYear, kCalendarLastYear);
  c.day = std::min(c.day, DaysInMonth(c.year, c.month));
}

// A month or a year on: the day stays, or the month's last if it has fewer.
void CalendarShiftMonths(UiCalendar& c, int months) {
  const int total = std::clamp(c.year * 12 + (c.month - 1) + months, kCalendarFirstYear * 12,
                               kCalendarLastYear * 12 + 11);
  c.year = total / 12;
  c.month = total % 12 + 1;
  c.day = std::min(c.day, DaysInMonth(c.year, c.month));
}

std::wstring_view Glyph(const wchar_t* glyph) { return glyph != nullptr ? std::wstring_view(glyph) : std::wstring_view(); }

Item Make(Kind kind, D2D1_RECT_F rect, std::wstring text = {}, const wchar_t* glyph = nullptr) {
  Item i;
  i.kind = kind;
  i.rect = rect;
  i.text = std::move(text);
  i.glyph = Glyph(glyph);
  return i;
}

// What a check's result keeps clear on the right of its row: its button's room
// (Item::checked), or just the edge.
float CheckButtonRoom(const Item& it) {
  if (!it.checked) {
    return 16;  // the line 20 from the card, the row being 4 in
  }
  const bool word = it.index == static_cast<int>(UiCheck::Status::NotRun);  // CheckLine's "Проверить"
  return word ? 16 + kRouteButton + 8 : 44;  // the word's button ends on the line 20; a key's zone on 14
}

Item CommandItem(Kind kind, D2D1_RECT_F rect, std::wstring text, const wchar_t* glyph, UiCommand command, int index = 0) {
  Item i = Make(kind, rect, std::move(text), glyph);
  i.action = ItemAction::Command;
  i.command = command;
  i.index = index;
  return i;
}

// A configuration's report for support; while one waits for its test of the
// servers, says so and takes no clicks.
Item ReportButton(const UiContent& c, bool support, D2D1_RECT_F rect, int index) {
  Item i = CommandItem(Kind::Button, rect,
                       c.reportPending ? L"Проверяю серверы…"
                       : support       ? L"Написать в поддержку"
                                       : L"Отчёт для поддержки",
                       kGlyphDownload, UiCommand::SupportReport, index);
  i.enabled = !c.reportPending;
  i.tip = support ? L"Проверить серверы, сохранить отчёт, открыть поддержку"
                  : L"Проверить серверы и сохранить отчёт для поддержки";
  return i;
}

bool Interactive(const Item& item) { return item.action != ItemAction::None && item.enabled; }

std::wstring StateDetailBase(const UiContent& c);

// The overview's words over the cube - the cube's pose says the state, so
// only what it can't: what's wrong and what to do about it, and when the
// kill switch holds the internet closed, that. Empty while all is well.
std::wstring StateDetail(const UiContent& c) {
  std::wstring detail = StateDetailBase(c);
  if (c.killSwitchActive && c.display != Display::On && c.display != Display::Off) {
    detail += L"\nИнтернет закрыт Kill switch'ем, пока подключение не восстановится.";
  } else if (c.display == Display::ServiceDown && c.killSwitch) {
    detail += L"\nЕсли включён kill switch, интернета нет, пока служба не запустится; снять вручную: "
              L"sovereign-core.exe --unblock от администратора.";
  }
  return detail;
}

std::wstring StateDetailBase(const UiContent& c) {
  switch (c.display) {
    case Display::ServiceDown:
      return L"sovereign-core не отвечает. Установи службу от администратора: sovereign-core.exe --install";
    case Display::Off:
      // Nothing to run: why (no configuration at all - the first screen says it).
      return c.hasConfig || c.profiles.empty() ? std::wstring() : L"Включи конфигурацию: " + c.combineError;
    case Display::Starting:
    case Display::On: return {};
    case Display::Error: return c.error.empty() ? std::wstring(L"ядро не запустилось") : c.error;
  }
  return {};
}

// What's wrong, each in a few words for the main screen's one line and in
// full, in red, at the top of the log (30.10.2026, his: "эту хуйню красную
// уберем ... ошибки красные в журнале писать"). The subscription's trouble
// last: its report button goes under it.
struct Issue {
  std::wstring brief;
  std::wstring full;
};

std::vector<Issue> Issues(const UiContent& c) {
  std::vector<Issue> issues;
  if (c.display == Display::ServiceDown) {
    issues.push_back({L"Служба Sovereign не отвечает", StateDetail(c)});
  } else if (c.display == Display::Error) {
    issues.push_back({L"Не удалось подключиться", StateDetail(c)});
  }
  if (c.lockedOut) {
    issues.push_back({L"Интернет закрыт Kill switch'ем",
                      L"Интернет закрыт Kill switch'ем: подключиться не удаётся — " + c.lockoutReason +
                          L". Так настоящий IP не утечёт, но и интернета нет."});
  }
  if (!c.killSwitchError.empty()) {
    issues.push_back({L"Kill switch не включился", L"Kill switch: " + c.killSwitchError});
  }
  if (!c.localNetwork.empty()) {
    issues.push_back({L"Сбоит твоя сеть, а не VPN", c.localNetwork});
  }
  if (!c.trouble.empty() && c.troubleProfile >= 0) {
    issues.push_back({L"Проблема с подпиской", c.trouble});
  }
  return issues;
}

bool AnyWaiting(const UiContent& c) {
  return std::any_of(c.profiles.begin(), c.profiles.end(), [](const UiProfile& p) { return p.waiting; });
}

// Where a page's "back" (and Esc) leads: a configuration to the list, the
// rest to the overview.
UiPage BackOf(UiPage page) {
  switch (page) {
    case UiPage::Profile: return UiPage::Subscription;
    default: return UiPage::Overview;  // the routing and the log too: the title bar's keys open them
  }
}

// "Скачиваю версию 0.5.113 · 42% · ещё 12 с"; once downloaded, the installer's half minute.
std::wstring DownloadingText(const UiContent& c) {
  if (c.updateInstalling) {
    return L"Ставлю версию " + c.updateVersion + L" · около 30 с, VPN на это время отключится";
  }
  std::wstring text = L"Скачиваю версию " + c.updateVersion;
  if (c.updateProgress >= 0) {
    text += L" · " + std::to_wstring(c.updateProgress) + L"%";
    if (c.updateSecondsLeft >= 0) {
      text += c.updateSecondsLeft >= 60 ? L" · ещё " + std::to_wstring((c.updateSecondsLeft + 59) / 60) + L" мин"
                                        : L" · ещё " + std::to_wstring(c.updateSecondsLeft) + L" с";
    }
  } else {
    text += L"…";
  }
  return text;
}

std::wstring UpdateStatus(const UiContent& c) {
  switch (c.update) {
    case UiUpdate::Idle:
      return c.updateMode == UpdateMode::Off       ? L"Обновления: только кнопкой справа."
             : c.updateMode == UpdateMode::Install ? L"Обновления ставятся сами."
                                                   : L"Обновления — раз в 12 часов.";
    case UiUpdate::Checking: return L"Проверяю…";
    case UiUpdate::UpToDate: return L"Это последняя версия.";
    case UiUpdate::Available:
      return c.updateError.empty() ? L"Доступна версия " + c.updateVersion : L"Не удалось: " + c.updateError;
    case UiUpdate::Downloading: return DownloadingText(c);
    case UiUpdate::Waiting: return L"Версия " + c.updateVersion + L" скачана, ждёт паузы.";
    case UiUpdate::Failed: return L"Не удалось проверить: " + c.updateError;
    case UiUpdate::Off: return L"Сборка для разработки: обновления не проверяются.";
  }
  return {};
}

// A flag emoji in a text layout (flags.h FindFlagEmoji), drawn as the
// sprite's flag, 4:3, centered on the text's capitals. Kept next to the
// layout it's in and outliving it, so AddRef and Release count nothing.
// Without a target - a layout only measured - it takes its room and draws
// nothing.
class FlagInline final : public IDWriteInlineObject {
 public:
  FlagInline(float fontSize, std::size_t index, ID2D1RenderTarget* target, ID2D1Bitmap* sprite)
      : fontSize_(fontSize), height_(std::round(fontSize * 0.86f)), index_(index), target_(target), sprite_(sprite) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject)) {
      *object = static_cast<IDWriteInlineObject*>(this);
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
  ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

  HRESULT STDMETHODCALLTYPE Draw(void* /*context*/, IDWriteTextRenderer* /*renderer*/, FLOAT originX, FLOAT originY,
                                 BOOL /*sideways*/, BOOL /*rightToLeft*/, IUnknown* /*effect*/) noexcept override {
    if (target_ == nullptr || sprite_ == nullptr) {
      return S_OK;
    }
    const auto n = static_cast<float>(index_);
    const float column = std::fmod(n, static_cast<float>(kFlagColumns));
    const float row = std::floor(n / static_cast<float>(kFlagColumns));
    const D2D1_RECT_F source{column * kFlagWidth, row * kFlagHeight, (column + 1) * kFlagWidth,
                             (row + 1) * kFlagHeight};
    const D2D1_RECT_F place{originX + 1, originY, originX + 1 + Width(), originY + height_};
    // A small enamel badge, not a bare rectangle: rounded corners, the flag
    // inset a pixel of the sprite (no neighbour's colour at the edge), a
    // gloss down from the top, a dark hairline round it and a light one inside.
    const D2D1_RECT_F inner{source.left + 1, source.top + 1, source.right - 1, source.bottom - 1};
    const float scaleX = (place.right - place.left) / (inner.right - inner.left);
    const float scaleY = (place.bottom - place.top) / (inner.bottom - inner.top);
    wil::com_ptr<ID2D1BitmapBrush> brush;
    if (FAILED(target_->CreateBitmapBrush(
            sprite_, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
            D2D1::BrushProperties(1.0f, D2D1::Matrix3x2F::Scale(scaleX, scaleY) *
                                            D2D1::Matrix3x2F::Translation(place.left - inner.left * scaleX,
                                                                          place.top - inner.top * scaleY)),
            brush.put()))) {
      target_->DrawBitmap(sprite_, place, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &source);
      return S_OK;
    }
    const float radius = std::max(1.5f, height_ * 0.17f);
    const D2D1_ROUNDED_RECT shape{place, radius, radius};
    target_->FillRoundedRectangle(shape, brush.get());
    wil::com_ptr<ID2D1SolidColorBrush> ink;
    if (SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.16f), ink.put()))) {
      const D2D1_ROUNDED_RECT light{
          D2D1::RectF(place.left + 0.5f, place.top + 0.5f, place.right - 0.5f, place.bottom - 0.5f),
          radius - 0.5f, radius - 0.5f};
      target_->DrawRoundedRectangle(light, ink.get(), 1.0f);
      ink->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
      const D2D1_ROUNDED_RECT dark{
          D2D1::RectF(place.left - 0.5f, place.top - 0.5f, place.right + 0.5f, place.bottom + 0.5f),
          radius + 0.5f, radius + 0.5f};
      target_->DrawRoundedRectangle(dark, ink.get(), 1.0f);
    }
    wil::com_ptr<ID2D1GradientStopCollection> stops;
    wil::com_ptr<ID2D1LinearGradientBrush> gloss;
    const std::array<D2D1_GRADIENT_STOP, 2> pair{{{0, D2D1::ColorF(1, 1, 1, 0.22f)}, {1, D2D1::ColorF(1, 1, 1, 0)}}};
    if (SUCCEEDED(target_->CreateGradientStopCollection(pair.data(), 2, stops.put())) &&
        SUCCEEDED(target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({place.left, place.top},
                                                {place.left, place.top + (place.bottom - place.top) * 0.55f}),
            stops.get(), gloss.put()))) {
      target_->FillRoundedRectangle(shape, gloss.get());
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* metrics) noexcept override {
    // The flag's middle where the capitals' is: about 0.35 em over the baseline.
    *metrics = {.width = Width() + 3, .height = height_, .baseline = height_ / 2 + fontSize_ * 0.35f,
                .supportsSideways = FALSE};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* overhangs) noexcept override {
    *overhangs = {};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before,
                                               DWRITE_BREAK_CONDITION* after) noexcept override {
    *before = DWRITE_BREAK_CONDITION_NEUTRAL;
    *after = DWRITE_BREAK_CONDITION_NEUTRAL;
    return S_OK;
  }

 private:
  float Width() const { return std::round(height_ * 4 / 3); }

  float fontSize_;
  float height_;
  std::size_t index_;
  ID2D1RenderTarget* target_;
  ID2D1Bitmap* sprite_;
};

// `text` laid out in `format` with its flag emoji as flags, drawn onto
// `target` from `sprite` (none: measured only). The flags are kept in
// `flags`, which must outlive the layout. Null if DirectWrite fails.
wil::com_ptr<IDWriteTextLayout> MakeTextLayout(IDWriteFactory* dwrite, std::wstring_view text, IDWriteTextFormat* format,
                                               float width, float height, std::vector<FlagInline>& flags,
                                               ID2D1RenderTarget* target = nullptr, ID2D1Bitmap* sprite = nullptr) {
  wil::com_ptr<IDWriteTextLayout> layout;
  if (FAILED(dwrite->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, std::max(width, 0.0f),
                                      std::max(height, 0.0f), layout.put()))) {
    return nullptr;
  }
  const std::vector<FlagEmoji> found = FindFlagEmoji(text);
  flags.clear();
  flags.reserve(found.size());  // no reallocation below: the layout holds their addresses
  for (const FlagEmoji& flag : found) {
    flags.emplace_back(format->GetFontSize(), flag.index, target, sprite);
    layout->SetInlineObject(&flags.back(), {static_cast<UINT32>(flag.at), 4});
  }
  return layout;
}

// Device-independent resources and everything between content and pixels:
// the layout of a page and its drawing onto any Direct2D target - a window's
// or, for snapshots, a WIC bitmap's.
class Painter {
 public:
  // MainWindow::Ask's card coming in (0..1): the dimming deepens, the card
  // grows to its size from a little smaller.
  float questionShown = 1;

  Painter() {
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put()));
    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite_.put())));
    wic_ = wil::CoCreateInstanceNoThrow<IWICImagingFactory>(CLSID_WICImagingFactory);
    fonts_ = LoadFontCollection(dwrite_.get());
    THROW_IF_FAILED(dwrite_->CreateEllipsisTrimmingSign(MakeFormat(Text(), 14).get(), ellipsis_.put()));
    lead_ = MakeFormat(Display(), ui::kTLead, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    state_ = MakeFormat(Display(), ui::kTTitle, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    delay_ = MakeFormat(Text(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    delay_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    heading_ = MakeFormat(Text(), 15.75f, DWRITE_FONT_WEIGHT_SEMI_BOLD);  // 14 x 1.125: the scale caption 12.5 / body 14 / heading 15.75 / title 20
    body_ = MakeFormat(Text(), 14);
    caption_ = MakeFormat(Text(), 12.5f);
    button_ = MakeFormat(Text(), 14);
    button_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    chip_ = MakeFormat(Text(), 12.5f);
    chip_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    center_ = MakeFormat(Text(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    center_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    strong_ = MakeFormat(Text(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    wrap_ = MakeFormat(Text(), 14, DWRITE_FONT_WEIGHT_NORMAL, true);
    captionWrap_ = MakeFormat(Text(), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
    mono_ = MakeFormat(L"Consolas", 12.5f);
    trailing_ = MakeFormat(Text(), 12.5f);
    trailing_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    pill_ = MakeFormat(Text(), 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    engraved_ = MakeFormat(Text(), ui::kTMicro, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hint_ = MakeFormat(Text(), 11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hint_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    tip_ = MakeFormat(Text(), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
    ask_ = MakeFormat(Display(), ui::kTLead, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    const std::wstring glyphs = ui::GlyphFamily(dwrite_.get());
    glyph_ = MakeFormat(glyphs.c_str(), 16);
    glyph_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    THROW_IF_FAILED(d2d_->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0,
        roundCaps_.put()));
    // A ghost key's outline: dashes of 3 and gaps of 3 at a width of 1.
    constexpr std::array<float, 2> kDashes = {3, 3};
    THROW_IF_FAILED(d2d_->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND,
                                    10, D2D1_DASH_STYLE_CUSTOM, 0),
        kDashes.data(), static_cast<UINT32>(kDashes.size()), dashed_.put()));
    // The main line's traffic: round dots 11.5 apart at a width of 1.5 (in widths: a dash of 1/3, a gap of 7.33).
    constexpr std::array<float, 2> kDots = {0.5f / 1.5f, 11.0f / 1.5f};
    THROW_IF_FAILED(d2d_->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                    D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_CUSTOM, 0),
        kDots.data(), static_cast<UINT32>(kDots.size()), dots_.put()));
  }

  ID2D1Factory* D2d() const { return d2d_.get(); }

  // The layout of `page` in a client area of `width` x `height` DIPs, the
  // page scrolled by `scroll` (clamped here).
  Layout Build(const UiContent& c, UiPage page, const std::string& profile, float width, float height, float& scroll,
               const LogState& logState) const {
    Layout l;
    // The log takes a wide window whole: its lines are long.
    const float pageWidth = std::min(page == UiPage::Logs ? width : kMaxPage, std::max(280.0f, width - 2 * kPad));
    const float x0 = std::max(kPad, (width - pageWidth) / 2);
    const float x1 = x0 + pageWidth;
    l.viewTop = kCaption;  // the page scrolls under our title bar
    l.viewBottom = height;
    // fixed: before the page's items, which scroll
    const bool nameWhole = Caption(l, c, width, page, HeadOf(c, page, profile));
    const std::size_t first = l.items.size();

    // An inner page's first line 12 under the bar: its name is the bar's now.
    float y = kCaption + (page == UiPage::Overview ? 8.0f : 12.0f);
    switch (page) {
      case UiPage::Overview: y = Overview(l, c, x0, x1, y, l.viewBottom - kPad); break;
      case UiPage::Servers: y = Servers(l, c, x0, x1, y); break;
      case UiPage::Subscription: y = Subscription(l, c, x0, x1, y); break;
      case UiPage::Logs: y = Logs(l, c, logState, x0, x1, y, height); break;
      case UiPage::Settings: y = Settings(l, c, x0, x1, y); break;
      case UiPage::Profile: y = ProfilePage(l, c, profile, x0, x1, y, nameWhole); break;
      case UiPage::Routing: y = Routing(l, c, x0, x1, y); break;
    }
    l.contentHeight = y + kPad - l.viewTop;  // from under our title bar, as the viewport

    scroll = std::clamp(scroll, 0.0f, std::max(0.0f, l.contentHeight - (l.viewBottom - l.viewTop)));
    l.scroll = scroll;
    for (std::size_t i = first; i < l.items.size(); ++i) {
      l.items[i].rect.top -= scroll;
      l.items[i].rect.bottom -= scroll;
    }
    if (l.logBox) {
      l.logBox->top -= scroll;
      l.logBox->bottom -= scroll;
    }
    if (l.glow) {
      l.glow->top -= scroll;
      l.glow->bottom -= scroll;
    }
    if (c.question) {
      // Over everything, the page's clip off: its card, then its buttons as
      // they stand, left to right (Tab's order).
      const UiQuestion& q = *c.question;
      const AskBoxes box = AskBox(q, width, height);
      Item card = Make(Kind::Question, box.card, q.title);
      card.index = static_cast<int>(q.tone);
      card.scrolls = false;
      l.items.push_back(std::move(card));
      Item yes = Make(q.tone == UiQuestion::Tone::Danger ? Kind::DangerButton : Kind::AccentButton, box.yes, q.yes);
      yes.action = ItemAction::Answer;
      yes.index = 1;
      yes.sub = static_cast<int>(q.tone);
      yes.scrolls = false;
      l.items.push_back(std::move(yes));
      if (!q.other.empty()) {
        Item other = Make(Kind::Button, box.other, q.other);
        other.action = ItemAction::Answer;
        other.index = 2;
        other.scrolls = false;
        l.items.push_back(std::move(other));
      }
      if (!q.no.empty()) {
        Item no = Make(Kind::Button, box.no, q.no);
        no.action = ItemAction::Answer;
        no.scrolls = false;
        l.items.push_back(std::move(no));
      }
      if (q.calendar) {
        // The calendar's keys are answers too (kAnswerMonth.. and a day's
        // kAnswerDay + number): they keep the card open, Answer moves the date.
        const UiCalendar& cal = *q.calendar;
        const CalendarKeys keys = CalendarHead(box.calHead);
        const auto step = [&](D2D1_RECT_F rect, int index, int direction) {
          Item it = Make(Kind::Day, rect, {}, kGlyphChevron);
          it.action = ItemAction::Answer;
          it.index = index;
          it.sub = direction;
          it.scrolls = false;
          l.items.push_back(std::move(it));
        };
        step(keys.monthBack, kAnswerMonthBack, -1);
        step(keys.monthOn, kAnswerMonthOn, 1);
        step(keys.yearBack, kAnswerYearBack, -1);
        step(keys.yearOn, kAnswerYearOn, 1);
        const int firstWeekday = WeekdayOf(cal.year, cal.month, 1);
        for (int day = 1; day <= DaysInMonth(cal.year, cal.month); ++day) {
          Item it = Make(Kind::Day, CalendarDay(box.calGrid, firstWeekday, day), std::to_wstring(day));
          it.action = ItemAction::Answer;
          it.index = kAnswerDay + day;
          it.checked = day == cal.day;
          it.sub = cal.year == cal.todayYear && cal.month == cal.todayMonth && day == cal.todayDay ? 1 : 0;
          it.scrolls = false;
          l.items.push_back(std::move(it));
        }
      }
    }
    return l;
  }

  // Where MainWindow::Ask's card and what's on it go in a window of
  // width x height: a little above the middle, a glyph in a circle, the
  // title and what it means beside it, the buttons on a band at the bottom -
  // yes first, as Windows' own dialogs have it.
  struct AskBoxes {
    D2D1_RECT_F card{};
    D2D1_RECT_F circle{};
    D2D1_RECT_F title{};
    D2D1_RECT_F text{};
    float band = 0;  // the buttons' band's top
    D2D1_RECT_F yes{};
    D2D1_RECT_F other{};
    D2D1_RECT_F no{};
    D2D1_RECT_F calHead{};  // a calendar's month and year keys; the weekdays' row and the days' grid under it
    D2D1_RECT_F calWeek{};
    D2D1_RECT_F calGrid{};
  };
  // A calendar's parts in the card's inner width (AskBoxes::calHead): the
  // month's name between its two steps on the left, the year's on the right.
  static constexpr float kCalCell = 26;  // a day's row
  static constexpr int kCalRows = 6;
  struct CalendarKeys {
    D2D1_RECT_F monthBack, monthName, monthOn, yearBack, yearName, yearOn;
  };
  static CalendarKeys CalendarHead(D2D1_RECT_F head) {
    const float step = 28;
    const float month = std::round((head.right - head.left - 8) * 0.56f);  // 8: the gap between the groups
    const float yearLeft = head.left + month + 8;
    return {{head.left, head.top, head.left + step, head.bottom},
            {head.left + step, head.top, head.left + month - step, head.bottom},
            {head.left + month - step, head.top, head.left + month, head.bottom},
            {yearLeft, head.top, yearLeft + step, head.bottom},
            {yearLeft + step, head.top, head.right - step, head.bottom},
            {head.right - step, head.top, head.right, head.bottom}};
  }
  // The cell of day `day` of a month starting on `firstWeekday` (0: Monday).
  static D2D1_RECT_F CalendarDay(D2D1_RECT_F grid, int firstWeekday, int day) {
    const int slot = firstWeekday + day - 1;
    const float width = (grid.right - grid.left) / 7;
    const int column = slot % 7;
    const int row = slot / 7;
    const float left = grid.left + width * static_cast<float>(column);
    const float top = grid.top + kCalCell * static_cast<float>(row);
    return {left + 3, top + 1, left + width - 3, top + kCalCell - 1};
  }
  AskBoxes AskBox(const UiQuestion& q, float width, float height) const {
    constexpr float kAskPad = 22;
    constexpr float kCircle = 38;
    constexpr float kBand = 68;
    constexpr float kAskButton = 34;
    const float w = std::min(392.0f, width - 2 * kPad);
    const float textLeft = kAskPad + kCircle + 14;
    const float textWidth = w - textLeft - kAskPad;
    const float titleHeight = TextHeight(q.title, ask_.get(), textWidth);
    const float textHeight = q.text.empty() ? 0.0f : TextHeight(q.text, wrap_.get(), textWidth);
    // The title's first line centred on the circle.
    const float titleTop = kAskPad + (kCircle - 24) / 2;
    const float body = std::max(kAskPad + kCircle, titleTop + titleHeight + (q.text.empty() ? 0.0f : 6 + textHeight));
    // With a second way on, the buttons go in two rows: yes across the card,
    // then the other and no side by side.
    // A calendar goes under it all, and with it the three buttons stand in one
    // row (the card has to fit the window's least height).
    const bool rows = !q.other.empty() && !q.calendar;
    const float calendar = q.calendar ? 14 + 30 + 22 + kCalCell * kCalRows : 0.0f;
    const float h = body + calendar + kAskPad + kBand + (rows ? kAskButton + 8 : 0.0f);
    const float left = std::round((width - w) / 2);
    const float top = std::round(std::max(kCaption + 8, (height - h) * 0.42f));
    AskBoxes b;
    b.card = {left, top, left + w, top + h};
    b.circle = {left + kAskPad, top + kAskPad, left + kAskPad + kCircle, top + kAskPad + kCircle};
    b.title = {left + textLeft, top + titleTop, left + textLeft + textWidth, top + titleTop + titleHeight};
    b.text = {b.title.left, b.title.bottom + 6, b.title.right, b.title.bottom + 6 + textHeight};
    if (q.calendar) {
      const float inner = left + kAskPad;
      const float headTop = top + body + 14;
      b.calHead = {inner, headTop, left + w - kAskPad, headTop + 30};
      b.calWeek = {inner, headTop + 30, left + w - kAskPad, headTop + 52};
      b.calGrid = {inner, headTop + 52, left + w - kAskPad, headTop + 52 + kCalCell * kCalRows};
    }
    b.band = top + body + calendar + kAskPad;
    const float buttonTop = b.band + (kBand - kAskButton) / 2;
    const float half = (w - 2 * kAskPad - 8) / 2;
    // Alone (a note), its one button takes the right half.
    b.yes = q.no.empty() ? D2D1_RECT_F{left + w - kAskPad - half, buttonTop, left + w - kAskPad, buttonTop + kAskButton}
                         : D2D1_RECT_F{left + kAskPad, buttonTop, left + kAskPad + half, buttonTop + kAskButton};
    b.no = {left + w - kAskPad - half, buttonTop, left + w - kAskPad, buttonTop + kAskButton};
    if (rows) {
      const float lower = buttonTop + kAskButton + 8;
      b.yes = {left + kAskPad, buttonTop, left + w - kAskPad, buttonTop + kAskButton};
      b.other = {left + kAskPad, lower, left + kAskPad + half, lower + kAskButton};
      b.no = {left + w - kAskPad - half, lower, left + w - kAskPad, lower + kAskButton};
    }
    if (q.calendar && !q.other.empty()) {
      const float third = (w - 2 * kAskPad - 16) / 3;
      const float x = left + kAskPad;
      b.yes = {x, buttonTop, x + third, buttonTop + kAskButton};
      b.other = {x + third + 8, buttonTop, x + 2 * third + 8, buttonTop + kAskButton};
      b.no = {x + 2 * third + 16, buttonTop, left + w - kAskPad, buttonTop + kAskButton};
    }
    return b;
  }

  // Draws a layout.
  void Draw(ID2D1RenderTarget* t, const Layout& l, const UiContent& c, const Interaction& in, const LogView& log,
            const LiveTraffic* live = nullptr) const {
    wil::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(t->CreateSolidColorBrush(Rgb(0, 0, 0), brush.put()))) {
      return;
    }
    Canvas k{t, brush.get(), dwrite_.get(), Flags(t), in.cube ? &*in.cube : nullptr};
    const D2D1_SIZE_F size = t->GetSize();
    t->Clear(FromColorRef(ui::kWindowColor));
    if (l.glow) {
      DrawBackdrop(t, size, *l.glow, c.display == Display::On);
      if (live != nullptr) {
        DrawLive(k, *live, *l.glow);  // under everything else: behind the glass, under the cards
      }
    }

    bool clipped = false;
    // Fixed items after the page's (the switch standing up over the
    // navigation): drawn once the page's clip is off, over everything.
    std::vector<std::size_t> over;
    // A question coming in (questionShown): its card and buttons grow from
    // a little smaller round the card's middle, fading in.
    std::optional<D2D1_POINT_2F> askAt;
    for (const Item& it : l.items) {
      if (it.kind == Kind::Question) {
        askAt = D2D1::Point2F((it.rect.left + it.rect.right) / 2, (it.rect.top + it.rect.bottom) / 2);
      }
    }
    const float shown = EaseOut(std::clamp(questionShown, 0.0f, 1.0f));
    const auto draw = [&](std::size_t i) {
      const Item& it = l.items[i];
      const auto n = static_cast<int>(i);
      if (it.shy && n != in.hover && !(n == in.focus && in.focusVisible)) {
        // Shy (a ↻, a "?" of an instrument): seen while the pointer is on it or on what it lies on.
        const auto on = [&](int at) {
          if (at < 0 || static_cast<std::size_t>(at) >= l.items.size()) {
            return false;
          }
          const D2D1_RECT_F h = l.items[static_cast<std::size_t>(at)].rect;
          const float x = (it.rect.left + it.rect.right) / 2;
          const float y = (it.rect.top + it.rect.bottom) / 2;
          return x >= h.left && x <= h.right && y >= h.top && y <= h.bottom;
        };
        const bool under = on(in.hover) || on(in.head);
        if (!under) {
          return;
        }
      }
      if (in.edit && (it.kind == Kind::GhostAdd || it.kind == Kind::Tag) && it.rect.left < in.edit->right &&
          it.rect.right > in.edit->left && it.rect.top < in.edit->bottom && it.rect.bottom > in.edit->top) {
        return;  // under the line being typed
      }
      if (it.kind == Kind::Question) {
        k.Fill({0, 0, size.width, size.height}, Rgb(0, 0, 0, 0.58f * shown));  // the page dimmed under it
      }
      wil::com_ptr<ID2D1Layer> layer;
      D2D1_MATRIX_3X2_F was{};
      if (askAt && shown < 1 && (it.kind == Kind::Question || it.action == ItemAction::Answer) &&
          SUCCEEDED(t->CreateLayer(nullptr, layer.put()))) {
        const float scale = 0.95f + 0.05f * shown;
        t->GetTransform(&was);
        t->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, *askAt) * was);
        t->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                           D2D1::IdentityMatrix(), shown),
                     layer.get());
      }
      if (it.clip >= 0) {  // a row of a fold that is moving: cut at its foot
        t->PushAxisAlignedClip({0, l.viewTop, size.width, std::max(l.viewTop, it.clip)}, D2D1_ANTIALIAS_MODE_ALIASED);
      }
      if (it.kind == Kind::LogBox && l.logBox) {
        DrawLog(k, *l.logBox, log);
      } else {
        DrawItem(k, it, c, n == in.hover, n == in.pressed);
      }
      if (it.clip >= 0) {
        t->PopAxisAlignedClip();
      }
      if (n == in.focus && in.focusVisible) {
        // A title bar key's ring goes round its pill: the cell runs to the window's edges.
        const float radius = it.chrome ? 10 : it.kind == Kind::ModePill ? (it.rect.bottom - it.rect.top) / 2 + 2 : kRadius;
        t->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(it.chrome ? KeyPill(it) : it.rect, 2), radius, radius),
                                SetColor(brush.get(), Rgb(255, 255, 255, 0.9f)), 2);
      }
      if (layer) {
        t->PopLayer();
        t->SetTransform(was);
      }
    };
    for (std::size_t i = 0; i < l.items.size(); ++i) {
      const Item& it = l.items[i];
      if (!it.scrolls && clipped) {
        over.push_back(i);
        continue;
      }
      if (it.scrolls && !clipped) {
        t->PushAxisAlignedClip({0, l.viewTop, size.width, l.viewBottom}, D2D1_ANTIALIAS_MODE_ALIASED);
        clipped = true;
      }
      if (it.scrolls && (it.rect.bottom < l.viewTop || it.rect.top > l.viewBottom)) {
        continue;
      }
      draw(i);
    }
    if (in.edit) {
      k.Round(*in.edit, kRadius, FromColorRef(ui::kCardColor));
      k.Outline(*in.edit, kRadius,
                in.editWay >= 0 ? FromColorRef(BucketColor(in.editWay), 0.7f) : Rgb(255, 255, 255, 0.3f));
    }
    if (clipped) {
      // The scroll position, when there is somewhere to scroll.
      const float view = l.viewBottom - l.viewTop;
      if (l.contentHeight > view + 1) {
        const float thumb = std::max(32.0f, view * view / l.contentHeight);
        const float top = l.viewTop + (view - thumb) * (l.scroll / (l.contentHeight - view));
        k.Round({size.width - 7, top + 4, size.width - 4, top + thumb - 4}, 1.5f, Rgb(255, 255, 255, 0.22f));
      }
      t->PopAxisAlignedClip();
    }
    for (const std::size_t i : over) {
      draw(i);
    }
    if (in.tip >= 0 && static_cast<std::size_t>(in.tip) < l.items.size()) {
      DrawTip(k, l.items[static_cast<std::size_t>(in.tip)], size);
    }
  }

  IWICImagingFactory* Wic() const { return wic_.get(); }

  // How wide a line of body text is: for what the window lays over the page.
  float BodyWidth(std::wstring_view text) const { return TextWidth(text, body_.get()); }

  // A layout as text, an item a line - its kind, rectangle, text - with "!"
  // where a line of text needs more room than the item gives it (it's cut
  // with "…" then), and "x" where two clickable items overlap other than a
  // "?" or a button over its row, ">" where it leaves the window, and "e"
  // where its edge from the card that holds it is off the law's three lines
  // (OnTheLines). For tests/ui: the layout checked without looking at pictures.
  //
  // docs/design/law.md, Grid Logic: from a card's edge a row's zone lies 4
  // off it, a plate (a segment) 8, the content - text, things, dividers - 20,
  // and a key, whose zone reaches 6 past its ink, 14. `left` and `right` are
  // the distances from the card's edges. What flows (text, things, a count)
  // may lie anywhere inside the content's lines.
  static bool OnTheLines(const Item& it, float left, float right) {
    const auto at = [](float v, float line) { return std::abs(v - line) <= 0.5f; };
    switch (it.kind) {
      case Kind::Switch:
      case Kind::Choice:
      case Kind::ValueRow:
      case Kind::CheckRow:
      case Kind::AppRow:
      case Kind::ServerCard:
      case Kind::ServerColumns:
      case Kind::Way:
      case Kind::Instrument: return at(left, 4) && at(right, 4);
      case Kind::Link: return right >= 16 - 0.5f;  // in a way's speed cell, on the row's inner edge
      case Kind::ProfileRow: return at(left, 4) && right >= 4 - 0.5f;  // cut short by its switch
      case Kind::Toggle: return at(right, 8);  // a plate 64 wide round a switch 40: the switch's ink on 20
      case Kind::Divider: return at(left, 20 + it.indent) && at(right, 20);  // under an instrument's name: in by it
      case Kind::Segment: return at(left, 8) || at(right, 8);  // the outer edge on the line 8, the inner 4 off the middle
      case Kind::IconButton: {  // the zone reaches past the glyph, which keeps off the line 20
        const float bleed = (it.rect.right - it.rect.left - 16) / 2;
        return std::min(left, right) + bleed >= 20 - 0.5f;
      }
      case Kind::Banner:
      case Kind::LogBox: return true;  // not on a card's lines: a card of its own, or the page's
      default: return left >= 20 - 0.5f && right >= 20 - 0.5f;
    }
  }
  static std::wstring OneLine(std::wstring_view text, std::size_t max) {
    std::wstring line(text.substr(0, max));
    std::replace(line.begin(), line.end(), L'\n', L' ');
    return line;
  }
  std::wstring Describe(const Layout& l, float width) const {
    static constexpr std::array<const wchar_t*, 48> kNames = {
        L"Card",     L"Divider",  L"Title",    L"Heading",  L"Text",     L"Muted",  L"Wrap",   L"Caption",
        L"Error",    L"Field",    L"Power",    L"Server",   L"Banner",
        L"Button",   L"Accent",   L"Danger",   L"Icon",     L"Switch",   L"Choice", L"Profile", L"Toggle",
        L"Check",    L"Segment",  L"Chip",     L"App",      L"Log",      L"Hint",   L"Value",   L"Meter",  L"Tag",
        L"AppTitle", L"Question", L"Pill",     L"Day",      L"Lead",   L"Mode",    L"Route",  L"Ghost",
        L"Menu",     L"MenuItem", L"Columns",  L"Way",      L"Link",   L"Instrument", L"Dot",  L"Trace",
        L"Readout",  L"Find"};
    // One per Kind, in its order: a kind added or gone shifts every name after it.
    static_assert(kNames.size() == static_cast<std::size_t>(Kind::Find) + 1);
    std::wstring out;
    for (std::size_t i = 0; i < l.items.size(); ++i) {
      const Item& it = l.items[i];
      const D2D1_RECT_F r = Seen(it);
      const float w = r.right - r.left;
      // Room for the text on the item's line, as DrawItem gives it.
      float room = 0;
      float need = 0;
      switch (it.kind) {
        case Kind::Title:
        case Kind::Heading: room = w; need = TextWidth(it.text, heading_.get()); break;
        case Kind::Lead: room = w; need = TextWidth(it.text, lead_.get()); break;
        case Kind::ModePill: room = w - kPillSide - 8 - kChevron - kPillSide; need = TextWidth(it.text, lead_.get()); break;
        case Kind::GhostAdd:
          if (!it.text.empty()) {
            room = w - 2 * 12;
            need = TextWidth(it.text, body_.get());
          }
          break;
        case Kind::Button:
        case Kind::AccentButton:
        case Kind::DangerButton:
          room = w - 4;
          need = TextWidth(it.text, body_.get()) + (it.glyph.empty() ? 0.0f : 26.0f);
          break;
        case Kind::Switch: room = w - 16 - 76 - it.indent; need = TextWidth(it.text, body_.get()); break;
        case Kind::Instrument: room = w - kInstrumentText - InstrumentControl(it); need = TextWidth(it.text, heading_.get()); break;
        case Kind::Dot: room = w - kDotInk; need = TextWidth(it.text, caption_.get()); break;
        case Kind::Choice: room = w - 42 - (kDelayColNarrow + kFailCol + kColSplit + kColGap) - 8; need = TextWidth(it.text, body_.get()); break;
        case Kind::Way:
          room = it.stacked ? w - kWayLeft - kColGap : w - kWayLeft - WayColumnsWidth() - 8;
          need = TextWidth(it.text, body_.get());
          break;
        case Kind::Link: room = w; need = TextWidth(it.text, trailing_.get()); break;
        case Kind::ProfileRow: room = w - 14 - 28; need = TextWidth(it.text, body_.get()); break;
        case Kind::ServerCard: room = w - 16 - 34; need = TextWidth(it.text, heading_.get()); break;
        case Kind::ValueRow:
          room = w - 16 - 30 - it.indent;
          need = it.stacked  // the value under the title: each on a line of its own
                     ? std::max(TextWidth(it.text, body_.get()) + (it.hinted ? kHint + 6 : 0.0f),
                                TextWidth(it.detail, trailing_.get()))
                     : TextWidth(it.text, body_.get()) + 12 + TextWidth(it.detail, trailing_.get()) +
                           (it.logo.empty() ? 0.0f : 22.0f) + (it.hinted ? kHint + 6 : 0.0f);
          break;
        case Kind::Segment: room = w - 8; need = TextWidth(it.text, button_.get()); break;
        case Kind::CheckRow:
          room = w - 34 - CheckButtonRoom(it);
          need = it.stacked ? std::max(TextWidth(it.text, body_.get()) + 6 + kHint, TextWidth(it.detail, trailing_.get()))
                            : TextWidth(it.text, body_.get()) + 6 + kHint + 8 + TextWidth(it.detail, trailing_.get());
          break;
        case Kind::Muted:  // a line of its own; a taller one wraps
          if (it.rect.bottom - it.rect.top <= 24 && it.text.find(L'\n') == std::wstring::npos) {
            room = w;
            need = TextWidth(it.text, caption_.get()) + (it.lamp > 0 ? kMutedLamp : 0.0f);
          }
          break;
        default: break;
      }
      bool overlaps = false;
      if (Interactive(it) && it.kind != Kind::Hint) {
        for (std::size_t j = i + 1; j < l.items.size(); ++j) {
          const Item& o = l.items[j];
          const D2D1_RECT_F s = Seen(o);
          const bool over = Interactive(o) && s.left < r.right && s.right > r.left && s.top < r.bottom && s.bottom > r.top;
          // A card that is a link holds buttons and lines that are links of their own: they take the click.
          // A question's buttons lie over the page, which takes no click while it's open; so do the
          // routing mode's choices.
          const bool allowed = o.kind == Kind::Hint || o.action == ItemAction::Answer || o.kind == Kind::MenuItem ||
                               (o.kind == Kind::IconButton &&
                                (it.kind == Kind::Title || it.kind == Kind::Card || it.kind == Kind::Instrument)) ||
                               (o.action == ItemAction::ToggleServer && it.kind == Kind::Choice) ||  // a server's chevron
                               (o.kind == Kind::Link && it.kind == Kind::Way) ||
                               ((o.kind == Kind::Muted || o.kind == Kind::Button) && it.kind == Kind::Card);
          overlaps = overlaps || (over && !allowed);
        }
      }
      const bool outside = r.left < 0 || r.right > width + 0.5f;
      // The card that holds it: the nearest one before it that has it inside. A question's buttons and the
      // calendar's days lie over the page, not on a card.
      bool off = false;
      if (it.kind != Kind::Card && it.kind != Kind::Question && it.kind != Kind::Day && it.action != ItemAction::Answer) {
        for (std::size_t j = i; j-- > 0;) {
          const D2D1_RECT_F c = l.items[j].rect;
          if (l.items[j].kind != Kind::Card || c.left > r.left + 0.5f || c.right < r.right - 0.5f ||
              c.top > r.top + 0.5f || c.bottom < r.bottom - 0.5f) {
            continue;
          }
          off = !OnTheLines(it, r.left - c.left, c.right - r.right);
          if (it.kind == Kind::Tag) {  // things flow: the first of a row on the line 20, the rest 4 after the one before
            const Item* before = nullptr;
            for (std::size_t k = i; k-- > j + 1 && before == nullptr;) {
              before = l.items[k].kind == Kind::Tag ? &l.items[k] : nullptr;
            }
            const bool sameRow = before != nullptr && std::abs(before->rect.top - r.top) <= 0.5f;
            const bool first = std::abs(r.left - c.left - 20) <= 0.5f;
            off = off || (sameRow ? std::abs(before->rect.right + 4 - r.left) > 0.5f
                                  : !first || (before != nullptr && std::abs(before->rect.bottom + 4 - r.top) > 0.5f));
          }
          break;
        }
      }
      out += std::format(L"{}{}{}{} {:<12} [{:6.1f} {:6.1f} {:6.1f} {:6.1f}] {}", need > room + 0.5f ? L"!" : L" ",
                         overlaps ? L"x" : L" ", outside ? L">" : L" ", off ? L"e" : L" ",
                         kNames[static_cast<std::size_t>(it.kind)], r.left, r.top, r.right, r.bottom, OneLine(it.text, 60));
      if (need > room + 0.5f) {
        out += std::format(L"   (needs {:.0f}, has {:.0f})", need, room);
      }
      if (!it.detail.empty() && it.kind != Kind::AppRow) {
        out += L"  | " + OneLine(it.detail, 50);
      }
      out += L"\n";
    }
    return out;
  }

  // The program icons and the flags are the render target's: a new target
  // needs new ones.
  void ForgetIcons() const {
    icons_.clear();
    logos_.clear();
    flags_.reset();
    flagsLoaded_ = false;
  }

 private:
  // Geist, in the exe (a resource); Segoe UI if it could not be loaded (a
  // test's exe has none).
  const wchar_t* Display() const { return fonts_ ? L"Geist" : L"Segoe UI Variable Display"; }
  const wchar_t* Text() const { return fonts_ ? L"Geist" : L"Segoe UI Variable Text"; }

  // The fonts of the exe's resources as a collection; null if any is missing.
  static wil::com_ptr<IDWriteFontCollection> LoadFontCollection(IDWriteFactory* factory) {
    wil::com_ptr<IDWriteFactory5> factory5;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(factory5.put())))) {
      return nullptr;
    }
    // The loader must outlive every format that uses the collection.
    static wil::com_ptr<IDWriteInMemoryFontFileLoader> loader;
    if (!loader) {
      if (FAILED(factory5->CreateInMemoryFontFileLoader(loader.put())) ||
          FAILED(factory5->RegisterFontFileLoader(loader.get()))) {
        loader.reset();
        return nullptr;
      }
    }
    wil::com_ptr<IDWriteFontSetBuilder1> builder;
    if (FAILED(factory5->CreateFontSetBuilder(builder.put()))) {
      return nullptr;
    }
    for (const int id : {IDR_FONT_TEXT}) {
      const HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
      const HGLOBAL loaded = found != nullptr ? LoadResource(nullptr, found) : nullptr;
      const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
      wil::com_ptr<IDWriteFontFile> file;
      if (data == nullptr ||
          FAILED(loader->CreateInMemoryFontFileReference(factory5.get(), data, SizeofResource(nullptr, found), nullptr,
                                                          file.put())) ||
          FAILED(builder->AddFontFile(file.get()))) {
        return nullptr;
      }
    }
    wil::com_ptr<IDWriteFontSet> set;
    wil::com_ptr<IDWriteFontCollection1> collection;
    if (FAILED(builder->CreateFontSet(set.put())) ||
        FAILED(factory5->CreateFontCollectionFromFontSet(set.get(), collection.put()))) {
      return nullptr;
    }
    return collection.query<IDWriteFontCollection>();
  }

  // Drawing helpers over one reusable brush.
  struct Canvas {
    ID2D1RenderTarget* t;
    ID2D1SolidColorBrush* b;
    IDWriteFactory* dwrite;  // for text with flags in it
    ID2D1Bitmap* sprite;     // the flags (flags.h); null if it couldn't be had
    const CubePose* cube;    // the switch's pose mid-motion; null: its state's (cube.h)

    ID2D1Brush* Color(D2D1_COLOR_F c) const { return SetColor(b, c); }
    void Fill(D2D1_RECT_F r, D2D1_COLOR_F c) const { t->FillRectangle(r, Color(c)); }
    void Round(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) const {
      t->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Color(c));
    }
    void Outline(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width = 1) const {
      t->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(r, -width / 2), radius, radius), Color(c), width);
    }
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F z, D2D1_COLOR_F c, float width = 1) const {
      t->DrawLine(a, z, Color(c), width);
    }
    // Metal under a light from above: a face of `r`, a bright 1 px edge along
    // its top, a dark one along its bottom, a hairline round it. Pressed, the
    // light is gone and the face sinks (a dark edge on top instead).
    void Bevel(D2D1_RECT_F r, float radius, float face, bool pressed) const {
      Round(r, radius, Rgb(255, 255, 255, pressed ? face * 0.5f : face));
      Outline(r, radius, Rgb(255, 255, 255, 0.07f));
      const float inset = radius * 0.8f;
      if (pressed) {
        Line({r.left + inset, r.top + 1.5f}, {r.right - inset, r.top + 1.5f}, Rgb(0, 0, 0, 0.40f));
      } else {
        Line({r.left + inset, r.top + 1.5f}, {r.right - inset, r.top + 1.5f}, Rgb(255, 255, 255, 0.11f));
        Line({r.left + inset, r.bottom - 1.5f}, {r.right - inset, r.bottom - 1.5f}, Rgb(0, 0, 0, 0.32f));
      }
    }
    // A pilot lamp: a point of `c` in a halo that thins out.
    void Led(D2D1_POINT_2F at, float radius, D2D1_COLOR_F c) const {
      for (const auto [grow, alpha] : {std::pair{3.0f, 0.07f}, std::pair{2.0f, 0.12f}, std::pair{1.0f, 0.22f}}) {
        t->FillEllipse(D2D1::Ellipse(at, radius + grow, radius + grow), Color({c.r, c.g, c.b, c.a * alpha}));
      }
      t->FillEllipse(D2D1::Ellipse(at, radius, radius), Color(c));
    }
    // `s` in colour `c`, but the `count` characters from `from` in `dim`
    // (a latency's digits the measurement doesn't prove).
    void TextDimmed(std::wstring_view s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c, UINT32 from, UINT32 count,
                    D2D1_COLOR_F dim) const {
      wil::com_ptr<IDWriteTextLayout> layout;
      wil::com_ptr<ID2D1SolidColorBrush> dimBrush;
      if (s.empty() || count == 0 || FAILED(dwrite->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), f, r.right - r.left,
                                                                    r.bottom - r.top, layout.put())) ||
          FAILED(t->CreateSolidColorBrush(dim, dimBrush.put()))) {
        Text(s, f, r, c);
        return;
      }
      layout->SetDrawingEffect(dimBrush.get(), {from, count});
      t->DrawTextLayout({r.left, r.top}, layout.get(), Color(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void Text(std::wstring_view s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) const {
      if (s.empty()) {
        return;
      }
      if (sprite == nullptr || FindFlagEmoji(s).empty()) {
        t->DrawText(s.data(), static_cast<UINT32>(s.size()), f, r, Color(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return;
      }
      std::vector<FlagInline> flags;
      if (const auto layout = MakeTextLayout(dwrite, s, f, r.right - r.left, r.bottom - r.top, flags, t, sprite)) {
        t->DrawTextLayout({r.left, r.top}, layout.get(), Color(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
      }
    }
  };

  static ID2D1Brush* SetColor(ID2D1SolidColorBrush* b, D2D1_COLOR_F c) {
    b->SetColor(c);
    return b;
  }

  wil::com_ptr<IDWriteTextFormat> MakeFormat(const wchar_t* family, float size,
                                             DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                                             bool wrap = false) const {
    wil::com_ptr<IDWriteTextFormat> f;
    THROW_IF_FAILED(dwrite_->CreateTextFormat(family, fonts_ && wcscmp(family, L"Geist") == 0 ? fonts_.get() : nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"ru-ru", f.put()));
    if (wrap) {
      f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
      f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    } else {
      f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      if (ellipsis_) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trimming, ellipsis_.get());
      }
    }
    return f;
  }

  // This exe's path: the app's icon is taken from it (Kind::AppTitle).
  static const std::wstring& SelfPath() {
    static const std::wstring path = [] {
      std::wstring buffer(MAX_PATH, L'\0');
      const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
      buffer.resize(n > 0 && n < buffer.size() ? n : 0);
      return buffer;
    }();
    return path;
  }

  // A program's icon from its exe, at the target's DPI; null if the path is
  // unknown or the file has none. Loaded once per path (a miss too).
  ID2D1Bitmap* ProgramIcon(ID2D1RenderTarget* target, const std::wstring& path) const {
    if (path.empty() || !wic_) {
      return nullptr;
    }
    const auto [it, fresh] = icons_.try_emplace(path);
    if (!fresh) {
      return it->second.get();
    }
    FLOAT dpiX = 96;
    FLOAT dpiY = 96;
    target->GetDpi(&dpiX, &dpiY);
    const auto size = static_cast<UINT>(std::lround(kAppIcon * dpiX / 96.0f));
    HICON extracted = nullptr;
    if (FAILED(SHDefExtractIconW(path.c_str(), 0, 0, &extracted, nullptr, size)) || extracted == nullptr) {
      return nullptr;
    }
    const wil::unique_hicon icon(extracted);
    wil::com_ptr<IWICBitmap> source;
    wil::com_ptr<IWICFormatConverter> converter;
    if (SUCCEEDED(wic_->CreateBitmapFromHICON(icon.get(), source.put())) &&
        SUCCEEDED(wic_->CreateFormatConverter(converter.put())) &&
        SUCCEEDED(converter->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                        0, WICBitmapPaletteTypeCustom))) {
      target->CreateBitmapFromWicBitmap(converter.get(), nullptr, it->second.put());
    }
    return it->second.get();
  }

  // A service's logo (icons.h ServiceLogo), 16 DIPs at the target's DPI;
  // null if it has none. Drawn once per id (a miss too).
  ID2D1Bitmap* Logo(ID2D1RenderTarget* target, const std::string& id) const {
    const auto [it, fresh] = logos_.try_emplace(id);
    if (!fresh) {
      return it->second.get();
    }
    FLOAT dpiX = 96;
    FLOAT dpiY = 96;
    target->GetDpi(&dpiX, &dpiY);
    const auto size = static_cast<int>(std::lround(16 * dpiX / 96.0f));
    const std::vector<std::uint32_t> pixels = ServiceLogo(id, size);
    if (!pixels.empty()) {
      const auto side = static_cast<UINT32>(size);
      target->CreateBitmap({side, side}, pixels.data(), side * 4,
                           D2D1::BitmapProperties(
                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpiX, dpiY),
                           it->second.put());
    }
    return it->second.get();
  }

  // The flags sprite (flags.h) from the exe's resources, as a bitmap of
  // `target` at 96 DPI - so a flag's source rectangle is its pixels. Null if
  // it can't be had; tried once per target.
  ID2D1Bitmap* Flags(ID2D1RenderTarget* target) const {
    if (!flagsLoaded_) {
      flagsLoaded_ = true;
      flags_ = ResourceBitmap(target, IDR_FLAGS);
    }
    return flags_.get();
  }

  // A picture in the exe's resources (RCDATA: PNG or JPEG) as a bitmap of
  // `target` at 96 DPI - its pixels are DIPs. Null if it can't be had.
  wil::com_ptr<ID2D1Bitmap> ResourceBitmap(ID2D1RenderTarget* target, int id) const {
    wil::com_ptr<ID2D1Bitmap> bitmap;
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL data = resource != nullptr ? LoadResource(module, resource) : nullptr;
    void* bytes = data != nullptr ? LockResource(data) : nullptr;
    if (bytes == nullptr || !wic_) {
      return bitmap;
    }
    wil::com_ptr<IWICStream> stream;
    wil::com_ptr<IWICBitmapDecoder> decoder;
    wil::com_ptr<IWICBitmapFrameDecode> frame;
    wil::com_ptr<IWICFormatConverter> converter;
    if (SUCCEEDED(wic_->CreateStream(stream.put())) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(bytes), SizeofResource(module, resource))) &&
        SUCCEEDED(wic_->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) &&
        SUCCEEDED(decoder->GetFrame(0, frame.put())) && SUCCEEDED(wic_->CreateFormatConverter(converter.put())) &&
        SUCCEEDED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                        0, WICBitmapPaletteTypeCustom))) {
      const D2D1_BITMAP_PROPERTIES props =
          D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
      target->CreateBitmapFromWicBitmap(converter.get(), &props, bitmap.put());
    }
    return bitmap;
  }

  // The height `text` takes wrapped to `width` in `format`.
  float TextHeight(std::wstring_view text, IDWriteTextFormat* format, float width) const {
    const auto metrics = Metrics(text, format, width, 10000);
    return metrics ? std::ceil(metrics->height) : 20;
  }

  // What `text` takes in `format` within width x height, laid out as
  // Canvas::Text lays it out - flags and all. Nullopt if DirectWrite fails.
  std::optional<DWRITE_TEXT_METRICS> Metrics(std::wstring_view text, IDWriteTextFormat* format, float width,
                                             float height) const {
    std::vector<FlagInline> flags;
    const auto layout = MakeTextLayout(dwrite_.get(), text, format, width, height, flags);
    DWRITE_TEXT_METRICS metrics{};
    if (!layout || FAILED(layout->GetMetrics(&metrics))) {
      return std::nullopt;
    }
    return metrics;
  }

  /* ---- layout ---- */

  // How wide `text` is on one line in `format`.
  float TextWidth(std::wstring_view text, IDWriteTextFormat* format) const {
    const auto m = Metrics(text, format, 10000, 100);
    return m ? m->widthIncludingTrailingWhitespace : 0.0f;
  }

  // A "?" at `x`, centered on `cy`: pointed at, `text` shows in a tip.
  static void AddHint(Layout& l, float x, float cy, std::wstring text, bool scrolls = true) {
    Item hint = Make(Kind::Hint, {x, cy - kHint / 2, x + kHint, cy + kHint / 2}, std::move(text));
    hint.action = ItemAction::Hint;
    hint.scrolls = scrolls;
    l.items.push_back(std::move(hint));
  }

  // A "?" right after `text` drawn in `format` from `left` (as far as `right`
  // lets it), on the line centered on `cy`.
  void HintAfter(Layout& l, std::wstring_view text, IDWriteTextFormat* format, float left, float right, float cy,
                 std::wstring hint) const {
    const float x = std::min(left + TextWidth(text, format) + 6, right - kHint);
    AddHint(l, x, cy, std::move(hint));
  }

  // An inner page's name and what it is for (its "?"): the title bar's, in
  // place of the app's icon and name, back before it (03.11.2026, his:
  // "стрелочка назад ... делать ее в шапке вместо иконки и названия клиента";
  // the page's own top row of back and title is gone). `rename`: the
  // configuration it names - a click on it renames it; -1 else.
  struct PageHead {
    std::wstring title;
    std::wstring hint;
    int rename = -1;
  };
  static PageHead HeadOf(const UiContent& c, UiPage page, const std::string& profile) {
    switch (page) {
      case UiPage::Overview: return {};
      case UiPage::Servers:
        return {L"Серверы",
                L"Флаг и провайдер — где сервер на самом деле: его выход, узнанный через него самого, а не то, "
                L"что написано в названии.\n\nЗадержка — медиана нескольких запросов через уже открытое "
                L"соединение, округлённая до 5 мс: разница меньше — шум сети. «Подробные замеры» внизу "
                L"покажут разброс (±), число запросов и время соединения. Авто берёт лучший по замерам и "
                L"меняет его, только когда другой заметно быстрее два замера подряд.\n\nПравый клик по "
                L"серверу — копировать имя или IP выхода."};
      case UiPage::Subscription:
        // Two tabs (12.11.2026): what services gave, and VPS of one's own.
        return {L"Подписки и серверы",
                L"«Подписки» — то, что дали сервисы: ссылка, QR-код или файл с конфигом. «Мои серверы» — свои VPS: "
                L"клиент заходит на них по ssh, проверяет и ставит на них VPN.\n\nВключённые работают вместе: их "
                L"серверы — в общем списке «Серверы».\n\nНажми на запись — имя, обновление, её серверы. Правый "
                L"клик — обновить, копировать ссылку, удалить."};
      case UiPage::Profile: {
        const auto found =
            std::find_if(c.profiles.begin(), c.profiles.end(), [&](const UiProfile& p) { return p.id == profile; });
        if (found == c.profiles.end()) {
          return {L"Конфигурация", {}};
        }
        return {found->name, {}, static_cast<int>(found - c.profiles.begin())};
      }
      case UiPage::Routing:
        return {L"Маршруты",
                L"Куда идёт трафик. Решает Sovereign: из подписок берутся только серверы, их правила и DNS не "
                L"используются.\n\nРежим наверху — куда идёт всё, о чём списки молчат. Списки ниже — то, что идёт "
                L"иначе: через VPN, напрямую, через WARP или никуда. У каждого режима свои списки: при смене они "
                L"не теряются.\n\nНаведи на сайт или программу — подсветится её путь. Нажми — перенести или "
                L"убрать."};
      case UiPage::Logs: return {L"Журнал", {}};
      case UiPage::Settings:
        return {L"Настройки",
                L"Крестик закрывает клиент совсем, чёрточка прячет окно в трей. Подключение остаётся и после "
                L"крестика: им управляет служба.\n\nВсё настроенное здесь, конфиги и их история лежат в «Папке "
                L"данных» внизу — %LOCALAPPDATA%\\Sovereign."};
    }
    return {};
  }

  // A card's heading, a "?" after it with `hint`.
  float Heading(Layout& l, const std::wstring& text, float x0, float x1, float y, std::wstring hint = {}) const {
    l.items.push_back(Make(Kind::Heading, {x0, y, x1, y + 28}, text));
    if (!hint.empty()) {
      HintAfter(l, text, heading_.get(), x0, x1, y + 14, std::move(hint));
    }
    return y + 28 + 6;
  }

  // A card of switches, one row each; one with a hint has a "?" after its
  // title and no line under it, unless it has a detail too.
  struct SwitchRow {
    std::wstring title;
    std::wstring detail;  // a state worth a line ("при обрыве..."); empty: the title alone
    std::wstring hint;
    bool on = false;
    UiCommand command = UiCommand::Toggle;
    int index = 0;
    bool enabled = true;
  };
  static SwitchRow Row(std::wstring title, std::wstring hint, bool on, UiCommand command, std::wstring detail = {},
                       int index = 0, bool enabled = true) {
    SwitchRow row;
    row.title = std::move(title);
    row.hint = std::move(hint);
    row.on = on;
    row.command = command;
    row.detail = std::move(detail);
    row.index = index;
    row.enabled = enabled;
    return row;
  }
  float Switches(Layout& l, const std::vector<SwitchRow>& rows, float x0, float x1, float y) const {
    float h = 0;
    for (const SwitchRow& row : rows) {
      h += row.detail.empty() ? kSwitchRow : 60;
    }
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
    float top = y;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const SwitchRow& row = rows[i];
      const float rowHeight = row.detail.empty() ? kSwitchRow : 60;
      if (i > 0) {
        l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
      }
      Item item = CommandItem(Kind::Switch, {x0 + 4, top + 2, x1 - 4, top + rowHeight - 2}, row.title, nullptr,
                              row.command, row.index);
      item.detail = row.detail;
      item.checked = row.on;
      item.enabled = row.enabled;
      l.items.push_back(std::move(item));
      if (!row.hint.empty()) {
        // Where Kind::Switch draws the title: 16 in, on the row's middle or its top line.
        const float cy = row.detail.empty() ? top + rowHeight / 2 : top + 2 + 10 + 12;
        HintAfter(l, row.title, body_.get(), x0 + 20, x1 - 80, cy, row.hint);
      }
      top += rowHeight;
    }
    return y + h;
  }

  // The routing scheme, a metro line (03.11.2026, the mockup he took: "симуляция веток метро ...
  // ахуенно"): the mode's sentence on top - "Всё идёт [через VPN ▾]", its pill drops the two modes
  // (RouteMenu) - its node on the line beside it; the spine down the left in the mode's colour to
  // the way everything else goes, at the foot; off it a branch to each other way's station. A
  // way's name and what goes there by its station - chips, a dashed "+" after them (BoardAdd:
  // what can go there, a site typed across the ways); a click on a chip: where it goes instead,
  // or away (ChipMenu). Pointing at a chip, a way's name or its "+" lights its route and puts the
  // others back (Draw's `hot`). Each mode keeps its own lists (RoutingSettings); the way
  // everything else goes shows what's named there anyway, quieter (UiChip::idle). Returns the
  // foot; `leadBottom`: where the sentence ends (the mode's menu hangs from it).
  float Board(Layout& l, const UiContent& c, float x0, float x1, float y, float& leadBottom) const {
    const UiRouting& r = c.routing;
    const int mode = r.finalDirect ? kBucketDirect : kBucketProxy;
    const float left = x0 + kSchemeIndent;
    l.boardLeft = left;
    l.boardRight = x1;
    const std::size_t lines = l.items.size();  // the scheme's lines go in here, under all of it

    // "Всё идёт" and the pill on one line while they fit, the pill on a line of its own else.
    const std::wstring lead = L"Всё идёт";
    const float leadWidth = std::ceil(TextWidth(lead, lead_.get()));
    l.items.push_back(Make(Kind::Lead, {left, y, left + leadWidth + 4, y + kTag}, lead));
    const std::wstring word = r.smart ? L"в обход блокировок" : r.finalDirect ? L"напрямую" : L"через VPN";
    const float pillWidth = kPillSide + std::ceil(TextWidth(word, lead_.get())) + 8 + kChevron + kPillSide;
    float pillX = left + leadWidth + 8;
    float pillY = y;
    if (pillX + pillWidth > x1) {
      pillX = left;
      pillY = y + kTag + 8;
    }
    Item pill = Make(Kind::ModePill, {pillX, pillY, pillX + pillWidth, pillY + kTag}, word);
    pill.action = ItemAction::RouteMenu;
    pill.checked = c.routeMenu;
    l.items.push_back(std::move(pill));
    const float nodeY = y + kTag / 2;
    leadBottom = pillY + kTag;
    // No "кроме того, что в списках ниже" under it: the line's end says "и всё
    // остальное", an empty way its "+ сайт, сервис или программа" (03.11.2026).
    y = leadBottom + 20;

    struct Way {
      const wchar_t* name;
      const wchar_t* verb;  // what a thing on it does: "YouTube идёт через VPN"
      std::wstring hint;
    };
    // By bucket: direct, proxy, block, WARP.
    const std::array<Way, 4> ways = {{
        {L"Напрямую", L"идёт напрямую", L"Мимо VPN, как без него. Для банков и сайтов, которым VPN мешает."},
        {L"Через VPN", L"идёт через VPN", L"Через сервер VPN."},
        {L"Блок", L"заблокирован",
         L"Сайт или программа останутся без сети: не откроются ни через VPN, ни напрямую. Например, сайт, который не "
         L"хочешь открывать, или назойливый апдейтер.\n\nРеклама, трекеры, баннеры cookie и вредоносные сайты тоже здесь: их включают в «+» "
         L"или в настройках.\n\nСайты, которые пропустил кнопкой «Сломалось только что?», стоят в строке «Не закрывать»: "
         L"нажми на значок — и они снова закрыты.\n\nОсторожно с «Microsoft»: в этот список входят GitHub, Windows Update, OneDrive и Office."},
        {L"Через WARP", L"идёт через WARP",
         L"Через Cloudflare WARP поверх VPN. Для сайтов, которые не пускают с адреса VPN: сайт видит адрес "
         L"Cloudflare.\n\nGoogle его адреса не любит: Gemini и поиск через WARP упрутся в капчу. Не проходит поверх "
         L"VPN — Sovereign сам пустит WARP напрямую."},
    }};
    // The exceptions first, the mode's own way last: the spine runs down to it.
    const std::array<int, 4> order = r.finalDirect
                                         ? std::array<int, 4>{kBucketProxy, kBucketWarp, kBucketBlock, kBucketDirect}
                                         : std::array<int, 4>{kBucketDirect, kBucketWarp, kBucketBlock, kBucketProxy};
    const auto idle = std::count_if(r.board.begin(), r.board.end(), [](const UiChip& chip) { return chip.idle; });
    std::vector<Item> routes;  // the branches, then the spine goes in front of them
    float restY = nodeY;
    for (const int id : order) {
      const Way& way = ways.at(static_cast<std::size_t>(id));
      const bool rest = id == mode;
      const float cy = y + 14;  // the name's line: its station on it
      const float nameWidth = std::ceil(TextWidth(way.name, heading_.get()));
      Item head = Make(Kind::Heading, {left, y, left + nameWidth + 4, y + 28}, way.name);
      head.action = ItemAction::Hint;  // a tip, no key: the way lights while pointed at
      head.rail = id;
      head.tip = rest && idle > 0 ? way.hint + L"\n\n" + RestHint(idle) : way.hint;
      l.items.push_back(std::move(head));
      float top = y + 28;
      // What's said by the name: the rest's "всё, что не попало выше", how WARP goes. Beside it while
      // it fits whole, else a line of its own under it.
      const std::wstring aside = rest && r.smart ? std::wstring(L"всё остальное; заблокированное — через VPN")
                                 : rest           ? std::wstring(L"всё остальное")
                                 : id == kBucketWarp ? r.warpState
                                 : id == kBucketBlock && c.blockedTotal > 0
                                     ? L"закрыто с подключения: " + Grouped(c.blockedTotal)
                                     : std::wstring();
      if (!aside.empty()) {
        const float asideLeft = left + nameWidth + 8;
        // WARP's state has a lamp: green while it answers, amber while it doesn't.
        const int lamp = id == kBucketWarp ? (r.warpTone > 0 ? 1 : r.warpTone < 0 ? 2 : 0) : 0;
        const float lampRoom = lamp > 0 ? kMutedLamp : 0.0f;
        if (asideLeft + std::ceil(TextWidth(aside, caption_.get())) + lampRoom + 4 <= x1) {
          Item note = Make(Kind::Muted, {asideLeft, y, x1, y + 28}, aside);
          note.lamp = lamp;
          l.items.push_back(std::move(note));
        } else {
          const float h = TextHeight(aside, captionWrap_.get(), x1 - left);
          l.items.push_back(Make(Kind::Caption, {left, top, x1, top + h}, aside));
          top += h;
        }
      }
      // Its things, then the "+", in rows from the left, wrapped at the right.
      std::vector<std::pair<std::size_t, D2D1_RECT_F>> placed;
      float cx = left;
      float rowTop = top + 8;
      float rowHeight = kTag;  // the tallest of the row: a chip wider than the page wraps
      const auto place = [&](float w, float h) {
        if (cx > left && cx + w > x1) {
          cx = left;
          rowTop += rowHeight + 8;
          rowHeight = kTag;
        }
        rowHeight = std::max(rowHeight, h);
        const D2D1_RECT_F at{cx, rowTop, cx + w, rowTop + h};
        cx += w + 8;
        return at;
      };
      for (std::size_t n = 0; n < r.board.size(); ++n) {
        const UiChip& chip = r.board[n];
        if (chip.bucket != id || (rest && chip.idle) ||
            chip.kind == UiChip::Kind::Pass) {  // (what only goes here anyway: no word of it, the line ends here; a pass: its own row)
          continue;
        }
        // Whole pixels up: a chip as wide as its text to the last float got its
        // last letters trimmed to "…" (brave.exe -> "brave.e…", 30.10.2026).
        float w = 32 + std::ceil(TextWidth(chip.text, body_.get())) + 12 +
                  (chip.detail.empty() ? 0.0f : 8 + std::ceil(TextWidth(chip.detail, caption_.get())));
        float h = kTag;
        if (w > x1 - left) {  // wider than the page: the whole width, the text wrapped, the detail under it
          w = x1 - left;
          h = ChipWrappedHeight(chip, w);
        }
        placed.emplace_back(n, place(w, h));
      }
      for (const auto& [n, at] : placed) {
        const UiChip& chip = r.board[n];
        Item tag = CommandItem(Kind::Tag, at, chip.text, nullptr, UiCommand::ChipMenu, static_cast<int>(n));
        // What it does, read off its way; a program keeps the sites that have a way of their own.
        tag.tip = !chip.icon.empty() && id != kBucketBlock  // (icon: a program's exe)
                      ? std::format(L"{} {}, кроме сайтов и сервисов, у которых свой путь", chip.text, way.verb)
                      : std::format(L"{} {}", chip.text, way.verb);
        if (chip.kind == UiChip::Kind::Ads) {
          tag.tip = L"Реклама блокируется: списки рекламных адресов.";
        } else if (chip.kind == UiChip::Kind::Cookies) {
          tag.tip = L"Баннеры cookie блокируются: окна «принять cookie?» на сайтах.";
        } else if (chip.kind == UiChip::Kind::Trackers) {
          tag.tip = L"Трекеры блокируются: серверы слежки и аналитики, по которым сайты узнают тебя.";
        } else if (chip.kind == UiChip::Kind::Malware) {
          tag.tip = L"Вредоносные сайты блокируются: вирусы, фишинг и мошенничество по спискам.";
        }
        // The number after a list's name: how many it has shut since the connection came up.
        if ((chip.kind == UiChip::Kind::Ads || chip.kind == UiChip::Kind::Cookies ||
             chip.kind == UiChip::Kind::Trackers || chip.kind == UiChip::Kind::Malware) &&
            !chip.detail.empty()) {
          tag.tip += L"\nЦифра рядом: сколько обращений закрыто с подключения — " + chip.detail + L".";
        }
        tag.detail = chip.detail;
        tag.stacked = at.bottom - at.top > kTag + 0.5f;
        tag.text2 = chip.icon;
        tag.logo = chip.logo;
        tag.quiet = chip.idle;
        tag.rail = id;
        l.items.push_back(std::move(tag));
      }
      // Nothing yet through the VPN while only what's chosen goes there: said in amber.
      if (r.finalDirect && !r.smart && id == kBucketProxy && placed.empty()) {
        const std::wstring none = L"Пока через VPN не идёт ничего.";
        const float h = TextHeight(none, captionWrap_.get(), x1 - left);
        l.items.push_back(Make(Kind::Caption, {left, top + 4, x1, top + 4 + h}, none));
        l.items.back().warn = true;
        top += 4 + h;
        rowTop = top + 8;
      }
      // The way everything else goes to has no list of its own to fill: only what's an exception
      // still (a service through a country) stands there, with its "+"; else it is the line's end.
      const bool bare = rest && placed.empty();
      // The "+": a square after the things, a word where there are none.
      if (!bare) {
        const std::wstring label = !placed.empty()        ? std::wstring()
                                   : id == kBucketBlock   ? std::wstring(L"+ отрезать сайт или программу")
                                                          : std::wstring(L"+ сайт, сервис или программа");
        float w = kTag;
        float h = kTag;
        if (!label.empty()) {
          w = std::min(x1 - left, 12 + std::ceil(TextWidth(label, body_.get())) + 12);
          h = std::max(kTag, TextHeight(label, wrap_.get(), w - 24) + 12);
        }
        Item add = CommandItem(Kind::GhostAdd, place(w, h), label, nullptr, UiCommand::BoardAdd, id);
        add.rail = id;
        add.tip = label.empty() ? std::wstring(L"Добавить сайт, сервис или программу") : std::wstring();
        l.items.push_back(std::move(add));
      }
      float foot = bare ? top : rowTop + rowHeight;  // the last line's bottom: what comes under goes from here
      // Under the block: the sites let through its lists, by their service's name, small - a click
      // gives them back to the block (ChipMenu).
      if (id == kBucketBlock) {
        constexpr float kSmall = 28;
        const std::wstring label = L"Не закрывать:";
        const float labelWidth = std::ceil(TextWidth(label, caption_.get())) + 4;
        float px = left;
        float py = foot + 10;
        bool any = false;
        for (std::size_t n = 0; n < r.board.size(); ++n) {
          const UiChip& chip = r.board[n];
          if (chip.kind != UiChip::Kind::Pass) {
            continue;
          }
          if (!any) {
            l.items.push_back(Make(Kind::Muted, {left, py, left + labelWidth, py + kSmall}, label));
            px = left + labelWidth + 8;
            any = true;
          }
          const float w = std::min(x1 - left, 32 + std::ceil(TextWidth(chip.text, body_.get())) + 12);
          if (px > left && px + w > x1) {
            px = left;
            py += kSmall + 8;
          }
          Item tag = CommandItem(Kind::Tag, {px, py, px + w, py + kSmall}, chip.text, nullptr, UiCommand::ChipMenu,
                                 static_cast<int>(n));
          tag.tip = L"Не закрывается: " + chip.sites + L"\nНажми, чтобы закрывать снова.";
          tag.logo = chip.logo;
          tag.rail = id;
          l.items.push_back(std::move(tag));
          px += w + 8;
        }
        if (any) {
          foot = py + kSmall;
        }
      }
      // What goes this way just now, off the log: a few sites, the rest counted; "ещё N" opens the log of them.
      const auto& now = c.routeNow.at(static_cast<std::size_t>(id));
      if (!now.empty()) {
        const std::wstring prefix = id == kBucketBlock ? L"сейчас закрыто: " : L"сейчас: ";
        const std::size_t total = now.size() + c.routeNowMore.at(static_cast<std::size_t>(id));
        std::size_t shown = now.size();
        std::wstring text;
        std::wstring more;
        for (; shown >= 1; --shown) {  // as many as the line holds, one at least
          text = prefix;
          for (std::size_t i = 0; i < shown; ++i) {
            text += (i > 0 ? L", " : L"") + now[i];
          }
          more = total > shown ? std::format(L"ещё {}", total - shown) : std::wstring();
          if (!more.empty()) {
            text += L" и";
          }
          const float used = std::ceil(TextWidth(text, caption_.get())) + 4 +
                             (more.empty() ? 0.0f : std::ceil(TextWidth(more, trailing_.get())));
          if (used <= x1 - left || shown == 1) {
            break;
          }
        }
        const float ly = foot + 6;
        const float textWidth = std::ceil(TextWidth(text, caption_.get())) + 4;
        l.items.push_back(Make(Kind::Muted, {left, ly, std::min(x1, left + textWidth), ly + 22}, text));
        if (!more.empty()) {
          const float linkWidth = std::ceil(TextWidth(more, trailing_.get())) + 2;
          Item link = Make(Kind::Link, {left + textWidth, ly, left + textWidth + linkWidth, ly + 22}, more);
          link.action = ItemAction::RouteLog;
          link.index = id;
          link.tip = L"Открыть журнал: только такие сайты";
          l.items.push_back(std::move(link));
        }
        foot = ly + 22;
      }
      Item route = Make(Kind::Route, {x0, cy - kLineTurn, x0 + kSchemeIndent - 4, cy + kLineTurn});
      route.rail = id;
      route.sub = id == kBucketBlock ? kRouteEnd : kRouteStation;
      if (rest) {
        restY = cy;
      } else {
        routes.push_back(std::move(route));
      }
      y = foot + 20;
    }
    // What the client worked out itself in «в обход блокировок»: the sites its first way
    // couldn't open and where they go now - a line each, under the board.
    if (r.smart && !r.learned.empty()) {
      const float nameWidth = std::ceil(TextWidth(L"Клиент решил сам", heading_.get()));
      Item head = Make(Kind::Heading, {left, y, left + nameWidth + 4, y + 28}, L"Клиент решил сам");
      head.action = ItemAction::Hint;
      head.tip = L"Сайты, которые напрямую не открылись: клиент проверил сам и пускает их через VPN. "
                 L"Запоминает на 6 часов; потом проверит снова.";
      l.items.push_back(std::move(head));
      std::wstring text;
      for (const std::wstring& line : r.learned) {
        text += (text.empty() ? L"" : L"\n") + line;
      }
      if (r.learnedMore > 0) {
        text += std::format(L"\nи ещё {}", r.learnedMore);
      }
      const float h = TextHeight(text, captionWrap_.get(), x1 - left);
      l.items.push_back(Make(Kind::Caption, {left, y + 32, x1, y + 32 + h}, text));
      y += 32 + h + 20;
    }
    Item spine = Make(Kind::Route, {x0, nodeY, x0 + kSchemeIndent - 4, restY});
    spine.rail = mode;
    spine.sub = kRouteSpine;
    routes.insert(routes.begin(), std::move(spine));
    l.items.insert(l.items.begin() + static_cast<std::ptrdiff_t>(lines), std::make_move_iterator(routes.begin()),
                   std::make_move_iterator(routes.end()));
    return y - 20;
  }

  // The routing mode's menu, dropped from the sentence's pill over the page: the two modes, a
  // dot of their way's colour, what each means, a check by the one on; under them that each
  // keeps its lists. Returns its foot.
  float RouteMenu(Layout& l, const UiRouting& r, float x0, float x1, float top) const {
    struct Choice {
      const wchar_t* title;
      const wchar_t* detail;
      int bucket;
    };
    const std::array<Choice, 3> choices = {{
        {L"Через VPN", L"Всё, кроме списка «Напрямую»", kBucketProxy},
        {L"Напрямую", L"Только список «Через VPN»", kBucketDirect},
        // Sovereign tries each site directly and through the VPN at once, keeps directly what
        // opens (gocore/smart.go, 04.11.2026).
        {L"В обход блокировок", L"Напрямую, а что не открывается — через VPN", kBucketProxy},
    }};
    // Not the page's width: as wide as its words need, under the sentence's own left edge.
    float need = 0;
    for (const Choice& choice : choices) {
      need = std::max({need, std::ceil(TextWidth(choice.title, strong_.get())), std::ceil(TextWidth(choice.detail, caption_.get()))});
    }
    x0 += kSchemeIndent;
    x1 = std::min(x1, x0 + 4 + kMenuDot + need + 2 + kMenuCheck + 4);
    const std::size_t panel = l.items.size();
    l.items.push_back(Make(Kind::Menu, {x0, top, x1, top}));
    float y = top + 4;
    const float textWidth = (x1 - 4) - (x0 + 4) - kMenuDot - kMenuCheck;
    for (std::size_t i = 0; i < choices.size(); ++i) {
      const Choice& choice = choices[i];
      const float h = 6 + 20 + 2 + TextHeight(choice.detail, captionWrap_.get(), textWidth) + 6;
      Item item = CommandItem(Kind::MenuItem, {x0 + 4, y, x1 - 4, y + h}, choice.title, nullptr,
                              UiCommand::SetRouteMode, static_cast<int>(i));
      item.detail = choice.detail;
      item.rail = choice.bucket;
      item.checked = i == 0 ? !r.finalDirect : i == 1 ? r.finalDirect && !r.smart : r.smart;
      l.items.push_back(std::move(item));
      y += h;
    }
    y += 4;
    l.items[panel].rect.bottom = y;
    return y;
  }

  // What the hidden things on the way everything else goes to say of
  // themselves (UiChip::idle); empty with none.
  static std::wstring RestHint(std::ptrdiff_t idle) {
    return idle > 0 ? std::wstring(L"Сюда идёт всё, что не названо выше. Названное здесь скрыто: оно шло бы сюда "
                                   L"и без записи, поэтому ничего не меняет. Если переключить режим наверху, "
                                   L"оно появится в своём списке.")
                    : std::wstring();
  }
  float Paragraph(Layout& l, Kind kind, const std::wstring& text, float x0, float x1, float y) const {
    const float indent = kind == Kind::ErrorText ? 28.0f : 0.0f;
    const float h =
        std::max(20.0f, TextHeight(text, kind == Kind::Caption ? captionWrap_.get() : wrap_.get(), x1 - x0 - indent));
    l.items.push_back(Make(kind, {x0, y, x1, y + h}, text, kind == Kind::ErrorText ? kGlyphWarning : nullptr));
    return y + h;
  }

  // Our own title bar (the system's is gone - Impl's WM_NCCALCSIZE): the
  // name on the left, the report and a new version by it; on the right the
  // cross, hiding the window to the tray, and where they belong: on the main
  // screen the routing, the log and the settings; on the configurations' list adding one (the
  // clipboard, a QR code, a file) - the main screen has it on its
  // subscription card. Inside the settings they were in the way (he asked
  // them gone, 16.10.2026). A new version found, its key on every page (it
  // was the main screen's banner till 24.10.2026). The rest of it drags the
  // window (WM_NCHITTEST).
  // On an inner page back and the page's name (HeadOf) stand where the icon and the name are; the
  // report goes after the name, as it does after the app's. Returns false when the name (a configuration's) did not
  // fit: the bar says "Конфигурация" and the page carries the name, wrapped.
  bool Caption(Layout& l, const UiContent& c, float width, UiPage page, const PageHead& head) const {
    // Keys of one width, the cross's too (it was Windows' 46): their pills
    // (DrawChrome) a step apart, the cross's as far from the window's right
    // edge as from its top. Its cell still runs to the edge - a throw of the
    // mouse into the corner finds it.
    float right = width - 40;  // the next key's right edge, leftwards from the cross
    const auto chrome = [](Item item) {
      item.scrolls = false;
      item.chrome = true;
      return item;
    };
    // The cross closes the client (the connection stays: the service holds
    // it); the dash beside it only puts the window away to the tray.
    Item close = chrome(CommandItem(Kind::IconButton, {right, 0, width, kCaption}, L"Закрыть клиент (подключение останется)",
                                 kGlyphClose, UiCommand::Exit));
    close.tip = L"Закрыть клиент. VPN останется: его держит служба";
    Item hide = chrome(Make(Kind::IconButton, {right - 40, 0, right, kCaption}, L"Свернуть в трей", kGlyphMinimize));
    hide.action = ItemAction::Hide;
    hide.tip = L"Убрать окно в трей. VPN работает дальше";
    right -= 40;
    // The keys between them a step closer (36): on the main screen five of
    // them, the name, the report and a new version's pill fit into 360.
    constexpr float kKey = 36;
    std::vector<Item> keys;
    const auto page_key = [&](const wchar_t* text, const wchar_t* glyph, UiPage to, const wchar_t* tip) {
      Item key = chrome(Make(Kind::IconButton, {right - kKey, 0, right, kCaption}, text, glyph));
      key.action = ItemAction::Page;
      key.index = static_cast<int>(to);
      key.tip = tip;
      keys.push_back(std::move(key));
      right -= kKey;
    };
    // On the main screen what is opened now and then a click away (the
    // wrench's menu of 17.10 gone, 30.10.2026, his: "журнал и маршруты рядом
    // с шестиренкой").
    if (page == UiPage::Overview) {
      page_key(L"Настройки", kGlyphSettings, UiPage::Settings, L"Настройки: Kill switch, автозапуск, локальная сеть, блокировка");
      page_key(L"Журнал", kGlyphLog, UiPage::Logs, L"Журнал: что делает ядро, ошибки");
      page_key(L"Маршруты", kGlyphRoute, UiPage::Routing, L"Маршруты: что идёт через VPN, а что напрямую");
    }
    // The configurations' ways to add are their tabs' own keys, words beside the icons (12.11.2026:
    // three bare icons in the title said nothing), not the title's.
    if (page == UiPage::Servers) {
      // Always to press: with the connection off the page says so, while one
      // runs a press changes nothing.
      Item test = chrome(CommandItem(Kind::IconButton, {right - kKey, 0, right, kCaption}, L"Проверить задержку",
                                     kGlyphStopwatch, UiCommand::TestDelays));
      test.tip = L"Замерить задержку до всех серверов";
      keys.push_back(std::move(test));
      right -= kKey;
      // `keys` is read backwards: the figures in full, then auto, then the test.
      Item details = chrome(CommandItem(Kind::IconButton, {right - kKey, 0, right, kCaption}, L"Подробные замеры",
                                        kGlyphDiagnostic, UiCommand::ToggleDelayDetails));
      details.checked = c.delaysDetailed;
      details.tip = L"Подробные замеры: ± разброс, число запросов, время соединения; ошибка сервера как её вернуло ядро";
      keys.push_back(std::move(details));
      right -= kKey;
      if (c.autoOption >= 0 && static_cast<std::size_t>(c.autoOption) < c.protocols.size()) {
        Item autoKey = chrome(CommandItem(Kind::IconButton, {right - kKey, 0, right, kCaption}, L"Авто",
                                          kGlyphLightning, UiCommand::SetProtocol, c.autoOption));
        autoKey.checked = c.protocol == c.autoOption;
        autoKey.tip = c.autoServer.empty() ? L"Авто: выберет лучший сервер после первого замера"
                                           : L"Авто: сам берёт самый быстрый. Сейчас — " + c.autoServer;
        keys.push_back(std::move(autoKey));
        right -= kKey;
      }
    }
    const bool inner = page != UiPage::Overview;
    // By the name: the app's own icon, its name, and the report beside them
    // (his: "сообщить о проблеме около иконки приложения"). On an inner page
    // the report follows the page's name the same way, not the keys on the
    // right (11.11.2026: one place to find it). Its glyph a speech bubble;
    // on a bad day - the kill switch closed the internet - its words too,
    // where they fit.
    Item name = Make(Kind::AppTitle, {12, 0, right - 14, kCaption}, L"Sovereign");
    name.text2 = SelfPath();
    name.scrolls = false;
    float left = 12 + 24 + TextWidth(name.text, caption_.get()) + 6;
    const std::wstring reportWords = L"Сообщить о проблеме";
    const float wideReport = 44 + std::ceil(TextWidth(reportWords, caption_.get()));
    Item report = chrome(CommandItem(Kind::IconButton, {left, 0, left + kKey, kCaption}, reportWords, kGlyphReport,
                                     UiCommand::ReportProblem));
    report.tip = L"Сообщить о проблеме: отчёт без ключей и адресов, его можно прочитать перед отправкой";
    // Where it goes, as far as `reach`: its width.
    const auto placeReport = [&](float at, float reach) {
      const bool loud = c.lockedOut && at + wideReport <= reach;
      const float w = loud ? wideReport : kKey;
      report.rect = {at, 0, at + w, kCaption};
      report.loud = loud;
      report.detail = loud ? reportWords : std::wstring();
      return w;
    };
    if (!inner) {
      left += placeReport(left, right - 4);
    }
    // A new version: the pill after the report, solid accent with its
    // version on it where there's room, the arrow alone where there isn't
    // (DrawChrome). Downloading, a click opens the settings, where the
    // update's state is told in full. Waiting for a quiet moment, a click
    // installs it now.
    // Not on the settings: the Sovereign card there says it and has its button - two buttons for one
    // update were one too many (11.11.2026).
    std::optional<Item> update;
    if (page != UiPage::Settings &&
        (c.update == UiUpdate::Available || c.update == UiUpdate::Downloading || c.update == UiUpdate::Waiting)) {
      // On an inner page the arrow alone, left of the report: the room is the page's name's.
      const float wide = 4 + 22 + TextWidth(c.updateVersion, pill_.get()) + 12;
      const float w = !inner && left + wide <= right - 4 ? wide : kKey;
      const float at = inner ? right - kKey : left;
      if (inner) {
        right -= kKey;
      }
      update = chrome(CommandItem(Kind::IconButton, {at, 0, at + w, kCaption}, L"Обновить до " + c.updateVersion,
                                  kGlyphDownload, UiCommand::InstallUpdate));
      update->checked = true;
      if (w > kKey) {
        update->detail = c.updateVersion;
      }
      if (c.update == UiUpdate::Downloading) {
        update->action = ItemAction::Page;
        update->index = static_cast<int>(UiPage::Settings);
        update->tip = DownloadingText(c);
      } else if (c.update == UiUpdate::Waiting) {
        update->tip = L"Версия " + c.updateVersion +
                      L" скачана и поставится, когда компьютер будет свободен. Нажми, чтобы поставить сейчас";
      } else if (!c.updateError.empty()) {
        update->tip = L"Обновление не удалось: " + c.updateError + L". Нажми, чтобы повторить";
      } else {
        update->tip = L"Доступна версия " + c.updateVersion + L". Нажми, чтобы обновить";
      }
    }
    bool whole = true;
    if (inner) {
      // Back on the left edge, a key's cell like the cross's on the right; the
      // name after it, the "?" after the name. A name too long for what's
      // left says "Конфигурация" and goes on the page.
      Item back = chrome(Make(Kind::IconButton, {0, 0, 40, kCaption}, L"Назад", kGlyphBack));
      back.action = ItemAction::Page;
      back.index = static_cast<int>(BackOf(page));
      back.tip = L"Назад (Esc)";
      l.items.push_back(std::move(back));
      constexpr float kTitleLeft = 44;
      const float room = right - 8 - kTitleLeft - (head.hint.empty() ? 0.0f : 6 + kHint) - (4 + kKey);
      std::wstring title = head.title;
      if (head.rename >= 0 && std::ceil(TextWidth(title, heading_.get())) > room) {
        title = L"Конфигурация";
        whole = false;
      }
      const float titleWidth = std::min(std::ceil(TextWidth(title, heading_.get())), std::max(room, 0.0f));
      // The report after the name and its "?".
      placeReport(kTitleLeft + titleWidth + 4 + (head.hint.empty() ? 0.0f : 2 + kHint), right - 4);
      Item named = Make(Kind::Title, {kTitleLeft, 0, kTitleLeft + titleWidth + 4, kCaption}, title);
      named.scrolls = false;
      if (head.rename >= 0) {  // a click on the name renames it
        named.action = ItemAction::EditName;
        named.index = head.rename;
        named.tip = L"Нажми, чтобы переименовать";
      }
      l.items.push_back(std::move(named));
      if (!head.hint.empty()) {
        AddHint(l, kTitleLeft + titleWidth + 6, kCaption / 2, head.hint, false);
      }
    } else {
      name.rect.right = report.rect.left;
      l.items.push_back(std::move(name));
    }
    // Left to right, as Tab goes: the report, the update, the page keys or "+", the cross.
    l.items.push_back(std::move(report));
    if (update) {
      l.items.push_back(std::move(*update));
    }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
      l.items.push_back(std::move(*it));
    }
    l.items.push_back(std::move(hide));
    l.items.push_back(std::move(close));
    return whole;
  }

  // The one screen for every day, top to bottom: the state, what waits for a
  // click (a new subscription), the switch, the server it runs on and
  // where it comes out, and what the quiet protection check found (a banner
  // with its fix). The rest of the window - routing, the log, the kill
  // switch - is in the settings, a key in the title bar.
  float Overview(Layout& l, const UiContent& c, float x0, float x1, float y, float bottom) const {
    // The state is the cube's: words only when there's something to say.
    // What's wrong is one quiet line, the whole of it in the log (Issues).
    if (const std::vector<Issue> issues = Issues(c); !issues.empty()) {
      std::wstring brief = issues.front().brief;
      if (issues.size() > 1) {
        brief += std::format(L" и ещё {}", issues.size() - 1);
      }
      Item line = Make(Kind::Banner, {x0, y, x1, y + 40}, brief, kGlyphWarning);
      line.checked = true;
      line.action = ItemAction::Page;
      line.index = static_cast<int>(UiPage::Logs);
      line.tip = L"Подробности — в журнале";
      l.items.push_back(std::move(line));
      y += 40 + 8;
    } else if (const std::wstring detail = StateDetail(c); !detail.empty()) {
      const float h = TextHeight(detail, captionWrap_.get(), x1 - x0);
      l.items.push_back(Make(Kind::Caption, {x0, y, x1, y + h}, detail));
      y += h + kGap;
    }

    // What waits for a click: one line each (a new version: a key in the title bar).
    if (AnyWaiting(c)) {
      Item waiting = Make(Kind::Banner, {x0, y, x1, y + 40}, L"Новая версия подписки — выбрать", kGlyphSync);
      waiting.action = ItemAction::Page;
      waiting.index = static_cast<int>(UiPage::Subscription);
      l.items.push_back(std::move(waiting));
      y += 40 + 8;
    }

    // Locked out: on, the kill switch holding the internet shut, nothing
    // getting through (main.cpp UpdateLockout) - why, and the way out.
    if (c.lockedOut) {
      l.items.push_back(CommandItem(Kind::Banner, {x0, y, x1, y + 40}, L"Открыть интернет без VPN", kGlyphWarning,
                                    UiCommand::OpenWithoutVpn));
      y += 40 + 8;
    }
    y = ProtectionBanners(l, c, x0, x1, y);

    y = SubscriptionCard(l, c, x0, x1, y);

    // Nothing to connect with yet: the way to fix that comes first.
    // The first screen: what the clipboard holds is offered as it is ("Добавить
    // подписку с packetlab.tech"); else how to bring one - pasted, a QR code on
    // the screen, a file.
    if (c.profiles.empty()) {
      const bool offer = !c.clipboardOffer.empty();
      const std::wstring hint = offer ? L"В буфере обмена есть " + c.clipboardOffer + L"."
                                      : L"Скопируй ссылку подписки, ключ или конфиг — клиент найдёт его в буфере сам.";
      const float hintH = TextHeight(hint, captionWrap_.get(), x1 - x0 - 32);
      const float h = 12 + 26 + hintH + 12 + kButton + 8 + kButton + 14;
      const float mid = (x0 + x1) / 2;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      l.items.push_back(Make(Kind::Text, {x0 + 16, y + 12, x1 - 16, y + 38}, L"Добавь подписку"));
      float top = Paragraph(l, Kind::Caption, hint, x0 + 16, x1 - 16, y + 38) + 12;
      l.items.push_back(CommandItem(Kind::AccentButton, {x0 + 16, top, x1 - 16, top + kButton},
                                    offer ? L"Добавить " + c.clipboardOffer : std::wstring(L"Вставить из буфера"),
                                    kGlyphPaste, UiCommand::PasteSubscription));
      top += kButton + 8;
      Item qr = CommandItem(Kind::Button, {x0 + 16, top, mid - 4, top + kButton}, L"QR-код с экрана", kGlyphQrCode,
                            UiCommand::ScanScreen);
      qr.tip = L"Найти на экране QR-код подписки и добавить её";
      l.items.push_back(std::move(qr));
      l.items.push_back(
          CommandItem(Kind::Button, {mid + 4, top, x1 - 16, top + kButton}, L"Из файла…", kGlyphFile, UiCommand::ImportFile));
      y += h + kGap;
    }

    // The server at the foot; the switch in the middle of what's between,
    // big, a light behind it (DrawBackdrop) and the live traffic round it -
    // no words: its light and the streams say the state.
    // The switch's room no more than its least size wants: more, and two
    // subscriptions on a short window made the page scroll by a few pixels.
    constexpr float kServerCard = 60;
    constexpr float kLeastCube = 88;
    const float serverTop = std::max(y + kLeastCube + 40, bottom - kServerCard);
    const float room = serverTop - y;
    const float size = std::clamp(std::min(room - 40, (x1 - x0) * 0.6f), kLeastCube, 220.0f);
    const float top = y + std::max(0.0f, (room - size) / 2);
    const float mid = (x0 + x1) / 2;
    const D2D1_RECT_F cube{mid - size / 2, top, mid + size / 2, top + size};
    Item power = CommandItem(Kind::Power, cube, {}, kGlyphPower, UiCommand::Toggle);
    power.checked = c.on;
    power.tip = c.on ? L"Отключить VPN" : L"Подключить VPN";
    l.items.push_back(std::move(power));
    l.glow = cube;
    return ServerCard(l, c, x0, x1, serverTop);
  }

  // What it connects with, over the switch: a card of glass that is a link
  // to the configurations (refreshed, added, switched there; "+" in the
  // title bar adds one too) - every configuration on, the subscriptions
  // first, a line each: its name, till when it's paid (the panel's word, or
  // the user's for their own server: a click - paid again, another date),
  // and only what's wrong besides (a failed refresh, the traffic running
  // out). No buttons: the card says, it doesn't ask. Nothing before there's
  // one on.
  float SubscriptionCard(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    std::vector<std::size_t> shown;
    for (const bool subscription : {true, false}) {
      for (std::size_t i = 0; i < c.profiles.size(); ++i) {
        if (c.profiles[i].enabled && c.profiles[i].subscription == subscription) {
          shown.push_back(i);
        }
      }
    }
    if (shown.empty()) {
      return y;
    }
    // A line's height: the name and the paid line, and a line more for each thing wrong.
    constexpr float kHeader = 40;
    const auto rowHeight = [&](const UiProfile& p) {
      return 6.0f + 26 + 22 + (p.failed ? 20.0f : 0.0f) + (p.trafficShare >= 0.8f ? 20.0f : 0.0f) + 6;
    };
    float h = kHeader + 1;
    for (const std::size_t i : shown) {
      h += rowHeight(c.profiles[i]);
    }
    const float left = x0 + 20;  // the content's line (law.md, Grid Logic)
    const float right = x1 - 20;

    Item card = Make(Kind::Card, {x0, y, x1, y + h}, L"Конфигурации");
    card.action = ItemAction::Page;
    card.index = static_cast<int>(UiPage::Subscription);
    card.glass = true;
    card.tip = L"Конфигурации: обновить, выключить отдельные серверы";
    l.items.push_back(std::move(card));

    // The head: how many, and the three ways to add another configuration
    // (the clipboard, a QR code, a file) as keys, each with its tip (he
    // wanted adding here 16.10.2026; the "+ Добавить" and its list gone,
    // 30.10.2026).
    constexpr float kAddKey = 32;
    constexpr float kAddStep = kAddKey + 2;
    const float addWidth = kAddStep * static_cast<float>(kAddWays.size()) - 2;
    // The last key's glyph (16) ends on the line 20: its zone reaches past it.
    const float keysRight = right + (kAddKey - 16) / 2;
    float addLeft = keysRight - addWidth;
    for (const AddWay& way : kAddWays) {
      Item add = CommandItem(Kind::IconButton, {addLeft, y + 4, addLeft + kAddKey, y + kHeader - 4}, way.name, way.glyph,
                             way.command);
      add.tip = way.tip;
      l.items.push_back(std::move(add));
      addLeft += kAddStep;
    }
    Item label = Make(Kind::Muted, {left, y + 9, keysRight - addWidth - 8, y + kHeader - 9},
                      std::format(L"КОНФИГУРАЦИИ · {}", shown.size()));
    label.engraved = true;
    l.items.push_back(std::move(label));
    l.items.push_back(Make(Kind::Divider, {left, y + kHeader, right, y + kHeader + 1}));

    float top = y + kHeader + 1;
    for (const std::size_t i : shown) {
      const auto index = static_cast<int>(i);
      const UiProfile& p = c.profiles[i];
      const float rowTop = top;
      const float rowHeightNow = rowHeight(p);
      if (i != shown.front()) {
        l.items.push_back(Make(Kind::Divider, {left, rowTop, right, rowTop + 1}));
      }
      // Right to left: the arrow, the days left, and the name stops short of them.
      const float arrow = right - 20;
      l.items.push_back(Make(Kind::IconButton, {arrow, rowTop, right, rowTop + rowHeightNow}, {}, kGlyphChevron));
      float nameRight = arrow - 8;
      if (p.daysLeft >= 0) {
        const std::wstring days = p.daysLeft == 0 ? std::wstring(L"закончилась")
                                                  : std::format(L"{} {}", p.daysLeft, DaysWord(p.daysLeft));
        const float w = TextWidth(days, caption_.get()) + 2 * kPillPad;
        const float mid = rowTop + rowHeightNow / 2;
        Item pill = Make(Kind::Pill, {arrow - 8 - w, mid - 11, arrow - 8, mid + 11}, days);
        pill.warn = p.daysLeft <= 3;
        nameRight = pill.rect.left - 8;
        l.items.push_back(std::move(pill));
      }
      // The name, and what kind of configuration it is after it - if there's room.
      const float nameWidth = std::min(TextWidth(p.name, heading_.get()) + 2, nameRight - left);
      l.items.push_back(Make(Kind::Heading, {left, rowTop + 6, left + nameWidth, rowTop + 32}, p.name));
      // One's own first: a subscription from one's own server is on "Мои серверы" too (12.11.2026).
      const std::wstring badge = !p.ownHost.empty() ? L"свой сервер" : p.subscription ? L"подписка" : L"свой конфиг";
      const float badgeWidth = TextWidth(badge, caption_.get()) + 2 * kPillPad;
      if (left + nameWidth + 8 + badgeWidth <= nameRight) {
        l.items.push_back(Make(Kind::Pill, {left + nameWidth + 8, rowTop + 8, left + nameWidth + 8 + badgeWidth, rowTop + 30},
                               badge));
      }
      float line = rowTop + 34;
      if (p.paidTill.empty()) {
        // Not said by its panel, not by the user yet: a click to say it.
        const std::wstring ask = L"Указать, до какого числа оплачено ›";
        const float w = std::min(TextWidth(ask, caption_.get()) + 4, right - left);
        l.items.push_back(
            CommandItem(Kind::Muted, {left, line, left + w, line + 20}, ask, nullptr, UiCommand::PaidMenu, index));
      } else {
        std::wstring paid = (p.paidByHand ? L"Сервер оплачен до " : L"Оплачено до ") + p.paidTill.substr(0, 5);
        if (p.paidByHand) {
          paid += L" ›";
          const float w = std::min(TextWidth(paid, caption_.get()) + 4, right - left);
          Item item = CommandItem(Kind::Muted, {left, line, left + w, line + 20}, paid, nullptr, UiCommand::PaidMenu, index);
          item.warn = p.daysLeft <= 3;
          item.tip = L"Оплатил — продлить на месяц или указать дату";
          l.items.push_back(std::move(item));
        } else {
          Item item = Make(Kind::Muted, {left, line, right, line + 20}, paid);
          item.warn = p.daysLeft <= 3;
          l.items.push_back(std::move(item));
        }
      }
      line += 22;
      if (p.failed) {
        Item failed = Make(Kind::Muted, {left, line, right, line + 20}, L"Подписка не обновилась");
        failed.warn = true;
        l.items.push_back(std::move(failed));
        line += 20;
      }
      if (p.trafficShare >= 0.8f) {
        Item traffic =
            Make(Kind::Muted, {left, line, right, line + 20}, L"Трафик: " + p.trafficUsed + L" из " + p.trafficTotal);
        traffic.warn = true;
        l.items.push_back(std::move(traffic));
      }
      top += rowHeightNow;
    }
    return y + h + kGap;
  }

  // The server the box runs on as a card: the flag of where it really is,
  // its name, its network and latency; a click opens the servers. With auto,
  // the server auto picked.
  float ServerCard(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    const float h = 60;
    Item glass = Make(Kind::Card, {x0, y, x1, y + h});
    glass.glass = true;
    l.items.push_back(std::move(glass));
    Item card = Make(Kind::ServerCard, {x0 + 4, y + 2, x1 - 4, y + h - 2}, {}, kGlyphChevron);
    card.action = ItemAction::Page;
    card.index = static_cast<int>(UiPage::Servers);
    if (c.protocols.empty()) {
      // No servers (every configuration or server switched off): the card
      // leads to the configurations, where they are switched on again - not a
      // dead end with nothing to click.
      card.index = static_cast<int>(UiPage::Subscription);
      card.text = c.profiles.empty() ? L"Нет серверов" : L"Выбора сервера нет";
      card.detail = c.profiles.empty() ? L"добавь конфигурацию" : L"включи конфигурацию с серверами";
      l.items.push_back(std::move(card));
      return y + h;
    }
    // The server: the pick, or what auto runs on.
    int shown = c.protocol;
    const bool isAuto = c.protocol >= 0 && c.protocol == c.autoOption;
    if (isAuto && !c.autoServer.empty()) {
      const auto it = std::find(c.protocols.begin(), c.protocols.end(), c.autoServer);
      shown = it == c.protocols.end() ? c.protocol : static_cast<int>(it - c.protocols.begin());
    }
    const auto i = static_cast<std::size_t>(std::max(shown, 0));
    std::wstring name = shown >= 0 && i < c.protocols.size() ? OptionTitle(c, i) : std::wstring(L"выбор сервера");
    const UiLocation* location = shown >= 0 && i < c.locations.size() ? &c.locations[i] : nullptr;
    std::wstring country = location != nullptr ? location->country : std::wstring();
    if (country.empty() && c.display == Display::On) {
      country = c.exitCountry;  // where it comes out, until its own lookup is in
    }
    if (country.size() == 2) {
      if (const std::wstring flag =
              FlagEmojiFor(std::string{static_cast<char>(country[0]), static_cast<char>(country[1])});
          !flag.empty()) {
        name = flag + L" " + WithoutFlagEmoji(name);
      }
    }
    std::vector<std::wstring> parts;
    if (isAuto) {
      parts.emplace_back(c.autoServer.empty() ? L"авто: выберет после замера" : L"авто");
    }
    // Where the traffic comes out - unless hidden (the right-click menu).
    if (c.display == Display::On && !c.hideExitIp) {
      if (!c.exitIp.empty()) {
        parts.push_back(c.exitIp);
      } else if (c.exitPending) {
        parts.emplace_back(L"определяю IP…");
      }
    }
    if (location != nullptr && !location->isp.empty()) {
      parts.push_back(location->isp);
    }
    if (static_cast<std::size_t>(c.protocol) < c.delays.size()) {
      if (std::wstring delay = DelayLabel(c.delays[static_cast<std::size_t>(c.protocol)], c.delaysDetailed); !delay.empty()) {
        parts.push_back(std::move(delay));
      }
    }
    std::wstring detail;
    for (const std::wstring& part : parts) {
      detail += (detail.empty() ? L"" : L" · ") + part;
    }
    card.text = std::move(name);
    card.detail = detail.empty() ? std::wstring(L"нажми, чтобы выбрать сервер") : std::move(detail);
    card.tip = L"Выбрать сервер; авто сам берёт самый быстрый";
    // What the quiet check found (main.cpp ScheduleProtectionCheck) and how long ago: the place
    // where the checks' page said it, the exit known here anyway.
    if (c.display == Display::On && c.checks.size() > kCheckLeaks) {
      const UiCheck& exit = c.checks[kCheckExit];
      const UiCheck& leaks = c.checks[kCheckLeaks];
      if (exit.status == UiCheck::Status::Running || leaks.status == UiCheck::Status::Running) {
        card.tip += L"\n\nЗащита: проверяю…";
      } else if (exit.status != UiCheck::Status::NotRun) {
        const std::wstring when = c.protectionMinutes < 0   ? std::wstring()
                                  : c.protectionMinutes < 1 ? std::wstring(L" только что")
                                                            : std::format(L" {} мин назад", c.protectionMinutes);
        card.tip += L"\n\nЗащита проверена" + when + L": выход — " + exit.summary + L", утечки — " +
                    (leaks.summary.empty() ? std::wstring(L"—") : leaks.summary) +
                    L". Проверяется сама после подключения, при смене сервера и раз в полчаса.";
      }
    }
    l.items.push_back(std::move(card));
    return y + h;
  }

  // Whether a hoster's domain names the network's owner: "hetzner.com" and "Hetzner Online" -
  // its first label in the owner's letters and digits (or theirs in it), case aside.
  static bool SameCompany(std::wstring_view hoster, std::wstring_view isp) {
    const auto squeeze = [](std::wstring_view s) {
      std::wstring out;
      for (const wchar_t ch : s) {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9')) {
          out.push_back(ch);
        } else if (ch >= L'A' && ch <= L'Z') {
          out.push_back(static_cast<wchar_t>(ch - L'A' + L'a'));
        }
      }
      return out;
    };
    const std::wstring name = squeeze(hoster.substr(0, hoster.find(L'.')));
    const std::wstring owner = squeeze(isp);
    return name.size() >= 3 && !owner.empty() &&
           (owner.find(name) != std::wstring::npos || (owner.size() >= 3 && name.find(owner) != std::wstring::npos));
  }

  // What option `i` is (UiContent::protocolTitles), or its name if that's unknown.
  static std::wstring OptionTitle(const UiContent& c, std::size_t i) {
    if (i < c.protocolTitles.size() && !c.protocolTitles[i].empty()) {
      return c.protocolTitles[i];
    }
    return i < c.protocols.size() ? c.protocols[i] : std::wstring();
  }

  // The servers of the subscription's selector (06.11.2026, docs/design/servers-concept.html): a
  // server is a machine - one card, its latency, a click takes its fastest way; its protocols are
  // ways to it, laid out under the one in use: whether each works here (the dot), how fast it opens
  // a site, how steady its UDP is for games, its speed. The other servers are a line each.
  float Servers(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    // What the measuring says now, over the list (its key is the title bar's).
    std::wstring note;
    if (!c.selectError.empty()) {
      note = L"Не удалось переключить сервер: " + c.selectError;
    } else if (c.delaysTesting) {
      note = L"Проверяю задержку…";
    } else if (!c.canTestDelays && !c.protocols.empty()) {
      note = L"Задержку можно проверить, когда подключение включено.";
    } else if (!c.delayError.empty()) {
      note = L"Не удалось проверить: " + c.delayError;
    }
    if (!note.empty()) {
      y = Paragraph(l, Kind::Caption, note, x0, x1, y) + kGap;
    }
    if (c.protocols.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 56}));
      l.items.push_back(Make(Kind::Muted, {x0 + 16, y, x1 - 16, y + 56},
                             c.hasConfig ? L"В конфиге нет выбора сервера." : L"Ни одна конфигурация не включена."));
      return y + 56;
    }
    const bool onAuto = c.autoOption >= 0 && c.protocol == c.autoOption;
    // The way the traffic goes by now: auto's pick, or the one picked by hand.
    int now = c.protocol;
    if (onAuto) {
      const auto at = std::find(c.protocols.begin(), c.protocols.end(), c.autoServer);
      now = at == c.protocols.end() ? -1 : static_cast<int>(at - c.protocols.begin());
    }
    // Auto is a key in the title bar; here what it took and why, in words.
    if (onAuto) {
      std::wstring line = L"Авто: выберет лучший после первого замера";
      if (!c.autoServer.empty()) {
        std::wstring where = c.autoServer;
        if (now >= 0) {
          const auto i = static_cast<std::size_t>(now);
          const UiLocation* loc = i < c.locations.size() ? &c.locations[i] : nullptr;
          where = (loc != nullptr && !loc->isp.empty() ? loc->isp + L" · " : L"") + WithoutFlagEmoji(OptionTitle(c, i));
        }
        line = L"Авто: сейчас " + where +
               (c.autoIsBest ? L" — лучший по замерам" : L" — другой чуть быстрее, но разница мала");
      }
      y = Paragraph(l, Kind::Caption, line, x0, x1, y) + kGap;
    }
    const std::vector<std::size_t> order = c.serverOrder.empty() ? ServerOrder(c) : c.serverOrder;
    // One physical server is one card: the exit's address tells them apart; without it, network and
    // country. A way with neither (it doesn't connect, so its exit is never seen) goes with its
    // configuration's other ways when they all reach one server - AnyTLS + ECH failing stood as a
    // server of its own (12.11.2026, his: "почему он отдельно"); else it is a server of its own.
    const auto groupKey = [&](std::size_t i) -> std::wstring {
      const UiLocation* loc = i < c.locations.size() ? &c.locations[i] : nullptr;
      if (loc != nullptr && !loc->ip.empty()) {
        return loc->ip;
      }
      if (loc != nullptr && !loc->isp.empty()) {
        return loc->country + L"/" + loc->isp;
      }
      return {};
    };
    const auto sourceOf = [&](std::size_t i) {
      return i < c.protocolSources.size() ? c.protocolSources[i] : std::wstring();
    };
    std::vector<std::wstring> keys;  // groups in the order of their first (fastest) member
    std::vector<std::vector<std::size_t>> members;
    std::vector<std::size_t> unplaced;
    for (const std::size_t i : order) {
      if (i >= c.protocols.size() || static_cast<int>(i) == c.autoOption) {
        continue;
      }
      const std::wstring key = groupKey(i);
      if (key.empty()) {
        unplaced.push_back(i);
        continue;
      }
      const auto at = std::find(keys.begin(), keys.end(), key);
      if (at == keys.end()) {
        keys.push_back(key);
        members.push_back({i});
      } else {
        members[static_cast<std::size_t>(at - keys.begin())].push_back(i);
      }
    }
    const std::size_t placedGroups = members.size();
    for (const std::size_t i : unplaced) {
      const std::wstring source = sourceOf(i);
      std::size_t home = placedGroups;  // the one server its configuration's ways reach; none: placedGroups
      for (std::size_t g = 0; g < placedGroups && !source.empty(); ++g) {
        const bool its = std::any_of(members[g].begin(), members[g].end(),
                                     [&](std::size_t m) { return sourceOf(m) == source; });
        if (its) {
          home = home == placedGroups ? g : placedGroups + 1;  // a second: not one server
        }
        if (home > placedGroups) {
          break;
        }
      }
      if (home < placedGroups) {
        members[home].push_back(i);
      } else {
        keys.push_back(L"#" + c.protocols[i]);
        members.push_back({i});
      }
    }
    if (members.empty()) {
      return y - kGap;
    }
    // Open: the server the traffic goes to (none yet - the fastest), and the ones opened by their
    // chevrons to look in without switching (12.11.2026, his: "не могу посмотреть что на сервере не
    // переключившись на него"); folded by it, the one in use too.
    std::size_t open = 0;
    for (std::size_t g = 0; g < members.size() && now >= 0; ++g) {
      if (std::find(members[g].begin(), members[g].end(), static_cast<std::size_t>(now)) != members[g].end()) {
        open = g;
      }
    }
    const auto isOpen = [&](std::size_t g) {
      for (const auto& [key, opened] : c.serverFolds) {
        if (key == keys[g]) {
          return opened;
        }
      }
      return g == open;
    };
    const bool columns = AnyDelayAnswered(c);
    for (std::size_t g = 0; g < members.size(); ++g) {
      const std::vector<std::size_t>& ways = members[g];
      const std::size_t first = ways.front();  // the fastest of them: the order is
      const UiLocation* loc = first < c.locations.size() ? &c.locations[first] : nullptr;
      const std::wstring flag = loc != nullptr && loc->country.size() == 2
                                    ? FlagEmojiFor(std::string{static_cast<char>(loc->country[0]),
                                                               static_cast<char>(loc->country[1])})
                                    : std::wstring();
      // Its name: where it is - the flag, the country, the city (11.11.2026, his: "флаг, название
      // страны, ip, хостинг, провайдера, задержку до него, а ниже уже протоколы"); under it its
      // address, who hosts it and whose network it's in. Not placed yet: a lone way's own name.
      const bool placed = loc != nullptr && !loc->country.empty();
      std::wstring name;
      if (placed) {
        name = loc->countryName.empty() ? loc->country : loc->countryName;
        if (!loc->city.empty()) {
          name += L", " + loc->city;
        }
      } else {
        name = ways.size() > 1 && loc != nullptr && !loc->ip.empty() ? loc->ip : WithoutFlagEmoji(OptionTitle(c, first));
      }
      if (!flag.empty()) {
        name = std::format(L"{} {}", flag, name);
      }
      const bool expanded = isOpen(g);
      const std::wstring ip = loc != nullptr && !loc->ip.empty() && !c.hideExitIp && name.find(loc->ip) == std::wstring::npos
                                  ? loc->ip
                                  : std::wstring();
      // The hoster said once: "hetzner.com" over "Hetzner Online" is the same company twice.
      const bool hosterShown = loc != nullptr && !loc->hoster.empty() && !SameCompany(loc->hoster, loc->isp);
      std::vector<std::wstring> facts;
      if (!ip.empty()) {
        facts.push_back(ip);
      }
      if (hosterShown) {
        facts.push_back(loc->hoster);
      }
      if (loc != nullptr && !loc->isp.empty()) {
        facts.push_back(loc->isp);
      }
      std::wstring detail;
      for (const std::wstring& fact : facts) {
        detail += (detail.empty() ? L"" : L" · ") + fact;
      }
      if (!expanded) {  // folded: how its ways do, on a line of its own
        if (const std::wstring summary = ServerSummary(c, ways); !summary.empty()) {
          detail += (detail.empty() ? L"" : L"\n") + summary;
        }
      }
      const std::size_t card = l.items.size();
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + kLatencyRow}));
      Item head = CommandItem(Kind::Choice, {x0 + 4, y + 3, x1 - 4, y + kLatencyRow - 3}, name, nullptr,
                              UiCommand::SetProtocol, static_cast<int>(first));
      head.checked = now >= 0 && std::find(ways.begin(), ways.end(), static_cast<std::size_t>(now)) != ways.end();
      head.detail = std::move(detail);
      // More than a line under the name (the summary, or facts that wrap): the head grows, the name stays.
      const float detailWidth = (x1 - 4) - (x0 + 4) - 42 - 12;
      if (const float lines = TextHeight(head.detail, captionWrap_.get(), detailWidth);
          !head.detail.empty() && lines > TextHeight(L"0", captionWrap_.get(), detailWidth) + 1) {
        head.stacked = true;
        head.rect.bottom = std::max(head.rect.bottom, head.rect.top + (kLatencyRow - 6) / 2 + 2 + lines + 6);
      }
      head.tip = L"Взять этот сервер: самый быстрый способ до него";
      if (placed) {
        head.tip += L"\n\nГде: " + (loc->countryName.empty() ? loc->country : loc->countryName) +
                    (loc->city.empty() ? L"" : L", " + loc->city);
      }
      if (!ip.empty()) {
        head.tip += (placed ? L"\n" : L"\n\n") + (L"IP: " + ip);
      }
      if (loc != nullptr && !loc->hoster.empty()) {
        head.tip += L"\nХостинг: " + loc->hoster + L" (по обратному имени адреса)";
      }
      if (loc != nullptr && !loc->isp.empty()) {
        head.tip += L"\nПровайдер, чья сеть: " + loc->isp;
      }
      if (first < c.sites.size() && !c.sites[first].empty()) {
        std::wstring sites;
        for (const UiSite& site : c.sites[first]) {
          const wchar_t* state = site.state == SiteCheck::State::Ok        ? L"открывается"
                                 : site.state == SiteCheck::State::Blocked ? L"не пускает отсюда"
                                                                           : L"не отвечает";
          if (!sites.empty()) {
            sites += L", ";
          }
          sites += site.name.empty() ? site.label : site.name;
          sites += L" — ";
          sites += state;
        }
        head.tip += L"\n\nСайты: " + sites;
      }
      const float headBottom = head.rect.bottom;
      // Its chevron on the head's first line, right of the latency: opens and folds the card, the
      // server in use stays (over the head: the later item takes the click).
      const float chevronY = head.detail.empty() ? (head.rect.top + head.rect.bottom) / 2 : head.rect.top + 15.5f;
      head.fold = expanded ? 1.0f : 0.0f;
      l.items.push_back(std::move(head));
      Item chevron = Make(Kind::IconButton, {x1 - 12 - kServerFold, chevronY - 14, x1 - 12, chevronY + 14},
                          expanded ? L"Свернуть" : L"Показать способы", expanded ? kGlyphChevronUp : kGlyphChevronDown);
      chevron.action = ItemAction::ToggleServer;
      chevron.text2 = keys[g];
      chevron.index = expanded ? 1 : 0;
      chevron.tip = expanded ? L"Свернуть" : L"Показать способы до этого сервера, не переключаясь на него";
      l.items.push_back(std::move(chevron));
      float top = headBottom + 3;
      if (expanded) {
        l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
        top += 4;
        if (columns) {
          l.items.push_back(Make(Kind::ServerColumns, {x0 + 4, top, x1 - 4, top + 20}, L""));
          top += 20;
        }
        for (const std::size_t i : ways) {
          top = Way(l, c, i, now, onAuto, x0 + 4, x1 - 4, top);
        }
        top += 6;
      }
      l.items[card].rect.bottom = top;
      y = top + (g + 1 < members.size() ? kGap : 0.0f);
    }
    return DirectWay(l, c, x0, x1, y + kGap);
  }

  // The line itself, without the VPN, under the servers: its speed beside theirs tells a slow server
  // from a slow line (the checks' page measured only the tray's own way, which in "only what's
  // chosen" is this one - "—" beside a VPN that worked, 11.11.2026).
  float DirectWay(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    const UiDelay& delay = c.directSpeed;
    const std::size_t card = l.items.size();
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y}));
    const float top = y + 4;
    Item way = Make(Kind::Way, {x0 + 4, top, x1 - 4, top + kWayLine}, L"Напрямую, без VPN");
    way.index = kDirectWay;
    way.stacked = std::ceil(TextWidth(way.text, body_.get())) > x1 - x0 - 8 - kWayLeft - WayColumnsWidth() - 8;
    way.detail = WayNote(delay);  // the run's error, if any: it has no latency of its own
    const float h = kWayLine + (way.stacked ? kWayNote + 4 : 0.0f) + (way.detail.empty() ? 0.0f : kWayNote);
    way.rect.bottom = top + h;
    const float line = way.stacked ? top + kWayLine - 4 : top;
    l.items.push_back(std::move(way));
    if (CanMeasure(c, delay)) {
      l.items.push_back(MeasureKey(delay, WayCells(x1 - 4, line, line + kWayLine)[2], kDirectWay, L"без VPN"));
    }
    l.items[card].rect.bottom = top + h + 4;
    return top + h + 4;
  }

  // The columns of a way, right to left from its row's `right`: what a new site waits (ОТКРЫТИЕ),
  // UDP's spread (ИГРЫ), the speed (СКОРОСТЬ) - their right edges and widths.
  static constexpr float kWayOpen = 46;
  static constexpr float kWayGames = 56;
  static constexpr float kWaySpeed = 76;
  static constexpr float kWaySplit = 6;
  static constexpr float kWayLeft = 38;  // the name's left, after the dot
  static constexpr float kWayLine = 30;  // a way's line; a note under it, kWayNote more
  static constexpr float kWayNote = 18;
  static std::array<D2D1_RECT_F, 3> WayCells(float right, float top, float bottom) {
    const float speed = right - kColGap;
    const float games = speed - kWaySpeed - kWaySplit;
    const float open = games - kWayGames - kWaySplit;
    return {D2D1_RECT_F{open - kWayOpen, top, open, bottom}, D2D1_RECT_F{games - kWayGames, top, games, bottom},
            D2D1_RECT_F{speed - kWaySpeed, top, speed, bottom}};
  }
  static float WayColumnsWidth() { return kWayOpen + kWayGames + kWaySpeed + 2 * kWaySplit + kColGap; }

  // A way to the open server: its row (a click pins it), and "замерить" where its speed is unknown.
  float Way(Layout& l, const UiContent& c, std::size_t i, int now, bool onAuto, float left, float right, float top) const {
    const UiDelay none{};
    const UiDelay& delay = i < c.delays.size() ? c.delays[i] : none;
    Item way = CommandItem(Kind::Way, {left, top, right, top + kWayLine}, WithoutFlagEmoji(OptionTitle(c, i)), nullptr,
                           UiCommand::SetProtocol, static_cast<int>(i));
    if (i < c.protocolNotes.size() && !c.protocolNotes[i].empty()) {
      way.text2 = WithoutFlagEmoji(c.protocolNotes[i]);
      way.text2.erase(0, way.text2.find_first_not_of(L' '));
    }
    way.checked = static_cast<int>(i) == now;
    way.sub = way.checked && !onAuto ? 1 : 0;  // picked by hand: pinned
    // No room beside the columns for the name: it takes a line of its own over them.
    const float room = right - left - kWayLeft - WayColumnsWidth() - 8 - (way.checked ? 72.0f : 0.0f);
    way.stacked = std::ceil(TextWidth(way.text, body_.get())) > room;
    float h = kWayLine + (way.stacked ? kWayNote + 4 : 0.0f);
    way.detail = WayNote(delay);
    if (!way.detail.empty()) {
      h += kWayNote;
    }
    way.rect.bottom = top + h;
    way.tip = way.sub == 1 ? L"Закреплён: авто его не сменит. Авто — ключ в заголовке" : L"Закрепить этот способ";
    const float line = way.stacked ? top + kWayLine - 4 : top;  // the columns' line
    l.items.push_back(std::move(way));
    if (CanMeasure(c, delay)) {
      const D2D1_RECT_F cell = WayCells(right, line, line + kWayLine)[2];
      l.items.push_back(MeasureKey(delay, cell, static_cast<int>(i), L"через этот способ"));
    }
    return top + h;
  }

  // The speed cell takes a click whenever a run can start: "замерить" where nothing is known, the
  // figure itself where something is - "замерить" stood on one way of five, the others showing
  // what they had carried, with no way to measure them (11.11.2026).
  static bool CanMeasure(const UiContent& c, const UiDelay& delay) {
    return c.canTestDelays && delay.state == UiDelay::State::Ok && !delay.measuring;
  }
  Item MeasureKey(const UiDelay& delay, const D2D1_RECT_F& cell, int index, const std::wstring& through) const {
    const std::wstring figure = SpeedText(delay);
    Item measure = CommandItem(Kind::Link, {cell.left + 8, cell.top + 4, cell.right, cell.bottom - 4},
                               figure.empty() ? std::wstring(L"замерить") : figure, nullptr, UiCommand::MeasureSpeed,
                               index);
    measure.sub = figure.empty() ? 0 : 1;  // a figure: its own ink, the key's blue under the mouse
    const std::wstring run = L"8 секунд загрузки " + through + L" (до 100 МБ)";
    measure.tip = figure.empty()               ? L"Замерить: " + run
                  : delay.measuredKbps > 0 ? L"Замерено. Нажми — ещё раз: " + run
                                           : L"Столько через него шло за последний час. Нажми — замерить: " + run;
    return measure;
  }

  // A way's row (Servers' Way): the dot of whether it works here, its name - "сейчас" or
  // "закреплён" after it, its note dimmer - and its three figures; what's wrong with it under them.
  void DrawWay(const Canvas& k, const Item& it, const UiContent& c, float hoverAlpha) const {
    const D2D1_RECT_F r = Seen(it);
    if (it.checked) {
      k.Round(r, 6, Rgb(255, 255, 255, 0.035f));
    }
    if (hoverAlpha > 0) {
      k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
    }
    const UiDelay none{};
    const bool direct = it.index == kDirectWay;  // DirectWay: no dot, it isn't a way to a server
    const UiDelay& delay = direct ? c.directSpeed
                           : it.index >= 0 && static_cast<std::size_t>(it.index) < c.delays.size()
                               ? c.delays[static_cast<std::size_t>(it.index)]
                               : none;
    const D2D1_COLOR_F primary = FromColorRef(ui::kPrimaryText);
    const D2D1_COLOR_F secondary = FromColorRef(ui::kSecondaryText);
    const D2D1_COLOR_F dim = FromColorRef(ui::kSecondaryText, 0.6f);
    const WayTone tone = WayToneOf(delay);
    const D2D1_COLOR_F toneInk = tone == WayTone::Ok     ? FromColorRef(ui::kUpload)
                                 : tone == WayTone::Warn ? FromColorRef(ui::kWarning)
                                 : tone == WayTone::Bad  ? FromColorRef(ui::kDanger)
                                                         : dim;
    const D2D1_RECT_F first{r.left, r.top, r.right, r.top + kWayLine};
    const float cy = (first.top + first.bottom) / 2;
    if (!direct) {
      k.t->FillEllipse(D2D1::Ellipse({r.left + 22, cy}, 4, 4), k.Color(toneInk));
    }
    const D2D1_RECT_F figures = it.stacked ? D2D1_RECT_F{r.left, first.bottom - 4, r.right, first.bottom + kWayLine - 4}
                                           : first;
    const float nameRight = it.stacked ? r.right - kColGap : WayCells(r.right, 0, 0)[0].left - 8;
    float x = r.left + kWayLeft;
    const float nameWidth = std::min(std::ceil(TextWidth(it.text, body_.get())) + 2, nameRight - x);
    k.Text(it.text, body_.get(), {x, first.top, x + nameWidth, first.bottom}, primary);
    x += nameWidth + 8;
    if (it.checked) {
      const std::wstring badge = it.sub == 1 ? L"закреплён" : L"сейчас";
      const float w = std::ceil(TextWidth(badge, pill_.get())) + 10;
      if (x + w <= nameRight) {
        const D2D1_RECT_F b{x, cy - 9, x + w, cy + 9};
        k.Round(b, 4, FromColorRef(ui::kLit));
        k.Text(badge, pill_.get(), {b.left + 5, b.top, b.right, b.bottom}, FromColorRef(ui::kLitInk));
        x += w + 8;
      }
    }
    if (!it.text2.empty() && x < nameRight) {
      k.Text(it.text2, caption_.get(), {x, first.top, nameRight, first.bottom}, secondary);
    }
    const auto cells = WayCells(r.right, figures.top, figures.bottom);
    const std::wstring open = OpenText(delay);
    const std::wstring games = GamesText(delay);
    const std::wstring speed = SpeedText(delay);
    const std::wstring dash = delay.state == UiDelay::State::Failed ? L"—" : L"";
    k.Text(open.empty() ? dash : open, trailing_.get(), cells[0], open.empty() ? dim : (delay.connect >= 1500 ? FromColorRef(ui::kWarning) : primary));
    k.Text(games.empty() ? dash : games, trailing_.get(), cells[1],
           games.empty() || games == L"нет UDP" ? dim : (GamesWarn(delay) || games == L"нет" ? FromColorRef(ui::kWarning) : primary));
    if (!CanMeasure(c, delay)) {  // else its key (MeasureKey) says it
      k.Text(speed.empty() ? dash : speed, trailing_.get(), cells[2], delay.measuring || speed.empty() ? dim : primary);
    }
    if (!it.detail.empty()) {
      const D2D1_COLOR_F ink = tone == WayTone::Bad    ? FromColorRef(ui::kDanger)
                               : tone == WayTone::Warn ? FromColorRef(ui::kWarning)
                                                       : secondary;
      k.Text(it.detail, caption_.get(), {r.left + kWayLeft, r.bottom - kWayNote - 4, r.right - kColGap, r.bottom - 2}, ink);
    }
  }

  float Subscription(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    // Two lists on one page (12.11.2026, asked: "разделить вообще на разные экраны свои впс с
    // подписками"): subscriptions and configs, and servers of one's own - a tab each, each with
    // its own ways to add. One's own: marked so (UiProfile::ownHost), a subscription too.
    const bool mine = c.profileTab == 1;
    std::vector<std::size_t> listed;  // indices into c.profiles: what the tab holds
    std::size_t owned = 0;
    for (std::size_t i = 0; i < c.profiles.size(); ++i) {
      const bool own = !c.profiles[i].ownHost.empty();
      owned += own ? 1 : 0;
      if (own == mine) {
        listed.push_back(i);
      }
    }
    {
      const float mid = (x0 + x1) / 2;
      const std::size_t subscribed = c.profiles.size() - owned;
      Item subs = Make(Kind::Segment, {x0, y, mid - 4, y + 36},
                       subscribed > 0 ? std::format(L"Подписки · {}", subscribed) : std::wstring(L"Подписки"));
      subs.action = ItemAction::ProfileTab;
      subs.index = 0;
      subs.checked = !mine;
      subs.tip = L"Подписки от сервисов и конфиги, добавленные ссылкой, QR-кодом или файлом";
      l.items.push_back(std::move(subs));
      Item own = Make(Kind::Segment, {mid + 4, y, x1, y + 36},
                      owned > 0 ? std::format(L"Мои серверы · {}", owned) : std::wstring(L"Мои серверы"));
      own.action = ItemAction::ProfileTab;
      own.index = 1;
      own.checked = mine;
      own.tip = L"Свои VPS: клиент заходит на них по ssh, проверяет и ставит VPN";
      l.items.push_back(std::move(own));
      y += 36 + kGap;
    }
    // The tab's list: the switch turns one on and off, a click opens it.
    if (listed.empty()) {
      const std::wstring none = mine ? L"Своих серверов пока нет."
                                     : L"Подписок пока нет. Добавь кнопками ниже: ссылка из буфера, QR-код с экрана "
                                       L"или файл.";
      const float h = std::max(56.0f, TextHeight(none, captionWrap_.get(), x1 - x0 - 40) + 32);
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      l.items.push_back(Make(Kind::Caption, {x0 + 20, y + (h - TextHeight(none, captionWrap_.get(), x1 - x0 - 40)) / 2,
                                             x1 - 20, y + h - 16},
                             none));
      y += h;
    } else {
      const float h = static_cast<float>(listed.size()) * kProfileRow;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      for (std::size_t n = 0; n < listed.size(); ++n) {
        const std::size_t i = listed[n];
        const UiProfile& p = c.profiles[i];
        const float top = y + static_cast<float>(n) * kProfileRow;
        if (n > 0) {
          l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
        }
        Item row = Make(Kind::ProfileRow, {x0 + 4, top + 3, x1 - 76, top + kProfileRow - 3}, p.name, kGlyphChevron);
        row.action = ItemAction::OpenProfile;
        row.index = static_cast<int>(i);
        row.detail = p.detail;
        row.checked = p.enabled;
        row.warn = p.failed;
        row.tip = L"Открыть: имя, обновление, серверы. Правый клик — меню";
        l.items.push_back(std::move(row));
        Item toggle = CommandItem(Kind::Toggle, {x1 - 72, top + 3, x1 - 8, top + kProfileRow - 3},
                                  p.enabled ? L"Выключить" : L"Включить", nullptr, UiCommand::ToggleProfile,
                                  static_cast<int>(i));
        toggle.checked = p.enabled;
        l.items.push_back(std::move(toggle));
      }
      y += h;
    }
    y += kGap;
    if (!c.profiles.empty() && !c.hasConfig && !c.combineError.empty()) {
      y = Paragraph(l, Kind::ErrorText, L"Нечего запускать: " + c.combineError, x0, x1, y) + kGap;
    }
    for (const std::wstring& note : c.combineNotes) {
      y = Paragraph(l, Kind::Caption, L"Не вошло: " + note, x0, x1, y) + 6;
    }
    if (mine) {
      // A server of the user's own: its address typed over this button (06.11.2026: 'поставил
      // нету кнопок и профиль я удалил').
      Item own = CommandItem(Kind::Button, {x0, y, x1, y + kButton}, L"Добавить свой VPS", kGlyphServer,
                             UiCommand::AddOwnServer);
      own.tip = L"Адрес VPS (root@1.2.3.4): клиент зайдёт по ssh-ключу и поставит на него VPN";
      l.items.push_back(std::move(own));
      y += kButton + 8;
      return Paragraph(l, Kind::Caption,
                       L"Нужен VPS с чистым Debian и вход root по ssh-ключу — клиент сам поставит на него VPN. "
                       L"Подписка уже с твоего сервера? Открой её и включи «Мой сервер» — она переедет сюда.",
                       x0 + 4, x1, y);
    }
    // The ways to add a subscription, each with its word (12.11.2026: three bare icons in the
    // title said nothing): the clipboard, a QR code on the screen, a file.
    {
      constexpr std::array<std::pair<const wchar_t*, UiCommand>, 3> kWays = {{
          {L"Из буфера", UiCommand::PasteSubscription},
          {L"QR-код", UiCommand::ScanScreen},
          {L"Из файла", UiCommand::ImportFile},
      }};
      const float w = (x1 - x0 - 16) / 3;
      for (std::size_t i = 0; i < kWays.size(); ++i) {
        const float left = x0 + static_cast<float>(i) * (w + 8);
        const auto way = std::find_if(kAddWays.begin(), kAddWays.end(),
                                      [&](const AddWay& it) { return it.command == kWays[i].second; });
        Item add = CommandItem(Kind::Button, {left, y, i + 1 == kWays.size() ? x1 : left + w, y + kButton},
                               kWays[i].first, way != kAddWays.end() ? way->glyph : nullptr, kWays[i].second);
        add.tip = way != kAddWays.end() ? std::wstring(way->tip) : std::wstring();
        l.items.push_back(std::move(add));
      }
      y += kButton + kGap;
    }

    // The subscription relay (relay.h), folded: the address and the key, each
    // typed over its row; the key is never shown. Folded, only whether it's
    // on - its address is the fold's. (In the settings till 17.10.2026: it's
    // how subscriptions download, so here.)
    {
      const auto row = [&](float top) { return D2D1_RECT_F{x0 + 4, top + 2, x1 - 4, top + kRow - 2}; };
      const auto line = [&](float top) { l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1})); };
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y}));
      const std::size_t card = l.items.size() - 1;
      const float fold = FoldOf(c.settingsMore, c.relayFold);
      Item relay = Make(Kind::ValueRow, row(y), L"Обновлять без VPN", kGlyphChevronDown);
      relay.action = ItemAction::ToggleMore;
      relay.fold = fold;
      if (fold < 0.5f) {  // the summary is the folded fold's; open, the rows say it
        relay.detail = !c.relayHost.empty() ? L"свой" : (c.sharedRelay ? L"Sovereign" : L"выключено");
      }
      l.items.push_back(std::move(relay));
      HintAfter(l, L"Обновлять без VPN", body_.get(), x0 + 20, x1 - 160, y + kRow / 2,
                L"Пока подключение работает, подписки скачиваются через него. Когда его нет, напрямую их сервер "
                L"увидел бы твой настоящий IP — поэтому они скачиваются через пересыльщик (Cloudflare Worker): сервер "
                L"подписки видит адрес Cloudflare, а ссылку на подписку — тот, кто держит пересыльщик.\n\n"
                L"«Через сервер Sovereign» — пересыльщик автора, ничего не настраивать. Хочешь, чтобы ссылки видел "
                L"только ты, — задай свой: адрес https://….workers.dev/ и ключ из relay.json, который пишет "
                L"tools/relay/deploy.ps1. Свой главнее.\n\nБез пересыльщика новую подписку предложат скачать "
                L"напрямую, остальные ждут подключения.");
      float top = y + kRow;
      if (fold > 0) {
        const std::size_t first = l.items.size();
        const float bodyTop = top;
        line(top);
        Item shared = CommandItem(Kind::Switch, {x0 + 4, top + 2, x1 - 4, top + 56 - 2}, L"Через сервер Sovereign", nullptr,
                                  UiCommand::ToggleSharedRelay);
        shared.detail = c.relayHost.empty() ? std::wstring(L"сервер видит ссылку на подписку")
                                            : std::wstring(L"пока задан свой — он главнее");
        shared.checked = c.sharedRelay;
        l.items.push_back(std::move(shared));
        top += 56;
        line(top);
        Item url = CommandItem(Kind::ValueRow, row(top), L"Свой адрес", nullptr, UiCommand::EditRelayUrl);
        url.detail = c.relayHost.empty() ? std::wstring(L"не задан") : c.relayHost;
        l.items.push_back(std::move(url));
        top += kRow;
        line(top);
        Item key = CommandItem(Kind::ValueRow, row(top), L"Свой ключ", nullptr, UiCommand::EditRelayKey);
        key.detail = c.relaySet ? L"задан" : L"не задан";
        key.enabled = !c.relayHost.empty();
        l.items.push_back(std::move(key));
        top = FoldBody(l, first, bodyTop, top + kRow, fold);
      }
      l.items[card].rect.bottom = top;
      y = top;
    }
    // Adding one is the main screen's: its "+" (the clipboard, a file, a QR code).
    return y;
  }

  // One configuration: its name, on/off, its subscription's refreshing, a
  // waiting version, and its servers - each can be switched off.
  // `nameWhole`: the title bar has its name (Caption); else the page starts with it, wrapped.
  float ProfilePage(Layout& l, const UiContent& c, const std::string& id, float x0, float x1, float y,
                    bool nameWhole) const {
    const auto found = std::find_if(c.profiles.begin(), c.profiles.end(), [&](const UiProfile& p) { return p.id == id; });
    if (found == c.profiles.end()) {
      l.items.push_back(Make(Kind::Muted, {x0, y, x1, y + 24}, L"Её больше нет."));
      return y + 24;
    }
    const UiProfile& p = *found;
    const int index = static_cast<int>(found - c.profiles.begin());
    if (!nameWhole) {
      y = Paragraph(l, Kind::Wrap, p.name, x0, x1, y) + kGap;
    }
    const float mid = (x0 + x1) / 2;

    // The switches: on, refreshing by itself (a subscription), and which tab it is on - a server
    // of one's own goes to "Мои серверы" (12.11.2026), a subscription from it too.
    const bool ownable = p.subscription || !p.ownHost.empty() || !p.servers.empty();
    const float cardHeight = 64.0f * static_cast<float>(1 + (p.subscription ? 1 : 0) + (ownable ? 1 : 0));
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + cardHeight}));
    Item on = CommandItem(Kind::Switch, {x0 + 4, y + 4, x1 - 4, y + 60}, L"Включена", nullptr, UiCommand::ToggleProfile,
                          index);
    on.detail = p.enabled ? L"её серверы — в общем списке" : L"не участвует в подключении";
    on.checked = p.enabled;
    l.items.push_back(std::move(on));
    float switchTop = y + 64;
    if (p.subscription) {
      l.items.push_back(Make(Kind::Divider, {x0 + 20, switchTop, x1 - 20, switchTop + 1}));
      Item autoUpdate = CommandItem(Kind::Switch, {x0 + 4, switchTop + 4, x1 - 4, switchTop + 60}, L"Автообновление",
                                    nullptr, UiCommand::ToggleAutoUpdate, index);
      autoUpdate.detail = p.autoUpdate ? p.period : L"только кнопкой «Обновить»";
      autoUpdate.checked = p.autoUpdate;
      l.items.push_back(std::move(autoUpdate));
      switchTop += 64;
    }
    if (ownable) {
      l.items.push_back(Make(Kind::Divider, {x0 + 20, switchTop, x1 - 20, switchTop + 1}));
      Item own = CommandItem(Kind::Switch, {x0 + 4, switchTop + 4, x1 - 4, switchTop + 60}, L"Мой сервер", nullptr,
                             UiCommand::ToggleOwnServer, index);
      own.detail = !p.ownHost.empty() ? L"в «Моих серверах» · " + p.ownHost : std::wstring(L"включи, если сервер твой");
      own.tip = p.ownHost.empty()
                    ? L"Включи, если это твой VPS: запись переедет в «Мои серверы», клиент сможет проверить его по ssh. "
                      L"Адрес берётся из конфига"
                    : L"Выключи — запись вернётся в «Подписки». Сам сервер не меняется";
      own.checked = !p.ownHost.empty();
      l.items.push_back(std::move(own));
    }
    y += cardHeight + kGap;

    if (p.subscription) {
      const std::vector<std::pair<const wchar_t*, std::wstring>> fields = {
          {L"Обновлена", p.failed ? std::wstring(L"не удалась") : p.updated},
          {L"Откуда", p.host.empty() ? std::wstring(L"—") : p.host},
          {L"Конфиг", p.edited ? L"с твоими правками" : L"без правок, как в подписке"},
      };
      const float h = static_cast<float>(fields.size()) * kRow;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      for (std::size_t i = 0; i < fields.size(); ++i) {
        const float top = y + static_cast<float>(i) * kRow;
        if (i > 0) {
          l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
        }
        Item field = Make(Kind::Field, {x0 + 20, top, x1 - 20, top + kRow}, fields[i].first);
        field.detail = fields[i].second;
        l.items.push_back(std::move(field));
      }
      y += h + kGap;
      if (!p.error.empty()) {
        y = Paragraph(l, Kind::ErrorText, p.error, x0, x1, y) + kGap;
      }
      if (p.waiting) {
        y = Waiting(l, p, index, x0, x1, y) + kGap;
      }
      if (!p.mergeNotes.empty()) {
        std::wstring where;
        for (std::size_t i = 0; i < p.mergeNotes.size() && i < 5; ++i) {
          where += (i > 0 ? L", " : L"") + p.mergeNotes[i];
        }
        if (p.mergeNotes.size() > 5) {
          where += std::format(L" и ещё {}", p.mergeNotes.size() - 5);
        }
        y = Paragraph(l, Kind::Wrap,
                      L"Правки перенесены. Там, где вы с подпиской изменили одно и то же, осталось твоё — проверь: " + where,
                      x0, x1, y) + kGap;
      }
      if (!p.usage.empty()) {
        y = Paragraph(l, Kind::Caption, p.usage, x0 + 4, x1, y) + kGap;
      }
      Item refresh =
          CommandItem(Kind::Button, {x0, y, mid - 4, y + kButton}, L"Обновить", kGlyphRefresh, UiCommand::RefreshProfile, index);
      refresh.tip = L"Скачать подписку заново сейчас";
      l.items.push_back(std::move(refresh));
      l.items.push_back(CommandItem(Kind::Button, {mid + 4, y, x1, y + kButton}, L"Копировать ссылку", kGlyphCopy,
                                    UiCommand::CopyProfileLink, index));
      y += kButton + 8;
      // What's wrong with which server, for the provider's support: a file,
      // a short version on the clipboard, their link opened if the panel gave one.
      l.items.push_back(ReportButton(c, p.support, {x0, y, x1, y + kButton}, index));
      y += kButton + 8;
      if (p.edited || p.waiting) {
        Item revert =
            CommandItem(Kind::Button, {x0, y, x1, y + kButton}, L"Как в подписке", kGlyphUndo, UiCommand::RevertConfig, index);
        revert.tip = L"Вернуть конфиг из подписки; твой останется в истории";
        l.items.push_back(std::move(revert));
        y += kButton + 8;
      }
    }
    // Marking it one's own is the switch's above (12.11.2026): renaming has the row whole then.
    Item rename = Make(Kind::Button, {x0, y, p.subscription ? mid - 4 : x1, y + kButton}, L"Переименовать",
                       kGlyphRename);
    rename.action = ItemAction::EditName;
    rename.index = index;
    l.items.push_back(std::move(rename));
    if (p.subscription) {
      Item period =
          CommandItem(Kind::Button, {mid + 4, y, x1, y + kButton}, L"Период…", kGlyphClock, UiCommand::ChooseRefreshPeriod, index);
      period.tip = L"Как часто обновлять подписку";
      l.items.push_back(std::move(period));
    }
    y += kButton + kGap + 4;

    // A server of their own: a look at it over ssh - what the system is, whether the installer can go on.
    // A subscription from it only the look: the installer would put its own config in place of the link's.
    if (p.subscription && !p.ownHost.empty()) {
      Item probe = CommandItem(Kind::Button, {x0, y, x1, y + kButton},
                               p.ownProbing ? L"Смотрю на сервер…" : L"Проверить сервер", nullptr,
                               UiCommand::ProbeOwnServer, index);
      probe.tip = L"Зайти по ssh (ключ из ssh-agent или ~\\.ssh) и посмотреть: система, root, занятые порты";
      probe.enabled = !p.ownProbing && !p.ownInstalling;
      l.items.push_back(std::move(probe));
      y += kButton + 8;
      if (!p.ownProbe.empty()) {
        y = Paragraph(l, Kind::Caption, p.ownProbe, x0 + 4, x1, y) + kGap;
      }
      y += kGap;
    }
    if (!p.subscription && !p.ownHost.empty()) {
      const bool busy = p.ownProbing || p.ownInstalling;
      Item probe = CommandItem(Kind::Button, {x0, y, mid - 4, y + kButton},
                               p.ownProbing ? L"Смотрю на сервер…" : L"Проверить сервер", nullptr,
                               UiCommand::ProbeOwnServer, index);
      probe.tip = L"Зайти по ssh (ключ из ssh-agent или ~\\.ssh) и посмотреть: система, root, занятые порты";
      probe.enabled = !busy;
      l.items.push_back(std::move(probe));
      // The installer (own_install.h): the VPN on the server, the config here becomes its client side.
      Item install = CommandItem(Kind::Button, {mid + 4, y, x1, y + kButton},
                                 p.ownInstalling ? L"Ставлю…"
                                 : p.ownInstalled ? L"Переустановить VPN"
                                                  : L"Поставить VPN",
                                 nullptr, UiCommand::InstallOwnServer, index);
      install.tip = L"Поставить на сервер packetlab: Hysteria2 с salamander на 443/UDP и перескоком портов, "
                    L"REALITY и AnyTLS + REALITY на 443/TCP — и взять их в этот конфиг. Только на чистый Debian: "
                    L"firewall сервера packetlab настроит заново, ssh останется открыт. Сбой — нажми ещё раз, "
                    L"установка продолжится. Журнал — install.log в папке конфигурации";
      install.enabled = !busy;
      l.items.push_back(std::move(install));
      y += kButton + 8;
      if (!p.ownProbe.empty()) {
        y = Paragraph(l, Kind::Caption, p.ownProbe, x0 + 4, x1, y) + kGap;
      }
      y += kGap;
    }

    // A chain (combine.h): its servers connect through another configuration's.
    {
      Item via = CommandItem(Kind::ValueRow, {x0, y, x1, y + kRow}, L"Подключаться через", nullptr,
                             UiCommand::ChooseVia, index);
      via.detail = p.via.empty() ? std::wstring(L"напрямую") : p.via;
      l.items.push_back(Make(Kind::Card, via.rect));
      via.rect = {x0 + 4, via.rect.top + 2, x1 - 4, via.rect.bottom - 2};
      l.items.push_back(std::move(via));
      HintAfter(l, L"Подключаться через", body_.get(), x0 + 20, x1 - 120, y + kRow / 2,
                L"Напрямую — серверы этой конфигурации видят твой настоящий IP. Через другую твою конфигурацию — "
                L"они видят адрес её сервера, а не твой: для сервисов, которым не хочется показывать себя.\n\n"
                L"Если та конфигурация выключена или удалена, серверы этой не подключаются вовсе — не напрямую.");
      y += kRow + 6;
      if (p.viaBroken) {
        y = Paragraph(l, Kind::ErrorText, L"«" + p.via + L"» выключена или удалена — серверы этой конфигурации не работают.",
                      x0, x1, y) + 6;
      }
      y += kGap;
    }

    // Its servers: a switch each.
    const auto off = std::count_if(p.servers.begin(), p.servers.end(), [](const UiServer& s) { return !s.enabled; });
    y = Heading(l,
                off > 0 ? std::format(L"Серверы: {}, выключено {}", p.servers.size(), off)
                        : std::format(L"Серверы: {}", p.servers.size()),
                x0, x1, y,
                L"Выключенный сервер не попадает в общий список и в авто. Правый клик — копировать его имя.");
    if (p.servers.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 56}));
      l.items.push_back(Make(Kind::Muted, {x0 + 16, y, x1 - 16, y + 56}, L"В конфиге нет серверов."));
      y += 56;
    } else {
      const float h = static_cast<float>(p.servers.size()) * kServerRow;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      for (std::size_t i = 0; i < p.servers.size(); ++i) {
        const float top = y + static_cast<float>(i) * kServerRow;
        if (i > 0) {
          l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
        }
        Item server = CommandItem(Kind::Switch, {x0 + 4, top + 2, x1 - 4, top + kServerRow - 2}, p.servers[i].name,
                                  nullptr, UiCommand::ToggleServer, index);
        server.sub = static_cast<int>(i);
        server.detail = p.servers[i].label;
        server.checked = p.servers[i].enabled;
        l.items.push_back(std::move(server));
      }
      y += h;
    }
    y += kGap;
    // Said as what it is: a server of one's own only leaves the list, the server and its VPN stay.
    Item remove = CommandItem(Kind::DangerButton, {x0, y, x1, y + kButton},
                              !p.ownHost.empty() ? L"Убрать из списка"
                              : p.subscription   ? L"Удалить подписку"
                                                 : L"Удалить конфиг",
                              kGlyphDelete, UiCommand::RemoveProfile, index);
    if (!p.ownHost.empty()) {
      remove.tip = L"Убрать запись из клиента. Сам сервер и VPN на нём останутся";
    }
    l.items.push_back(std::move(remove));
    return y + kButton;
  }

  // A newer subscription met the user's edits: the three ways on.
  float Waiting(Layout& l, const UiProfile& p, int index, float x0, float x1, float y) const {
    const float left = x0 + 16;
    const float right = x1 - 16;
    const wchar_t* explain = L"В конфиге есть твои правки, поэтому он не заменён сам — пока работает прежний. "
                             L"Перенести: твои правки лягут поверх новой версии. Что бы ты ни выбрал, "
                             L"прежний конфиг сохранится в папке history.";
    const float textHeight = TextHeight(explain, wrap_.get(), right - left);
    const bool inRow = right - left >= 3 * 150 + 2 * 12;
    const float buttons = inRow ? kButton : 3 * kButton + 2 * 8;
    const std::wstring& error = p.choiceError;
    const float errorHeight = error.empty() ? 0 : TextHeight(error, wrap_.get(), right - left - 28) + 12;
    const float h = 16 + 28 + 6 + textHeight + 16 + errorHeight + buttons + 20;
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
    float top = y + 16;
    l.items.push_back(Make(Kind::Heading, {left, top, right, top + 28}, L"Пришла новая версия подписки"));
    top += 28 + 6;
    l.items.push_back(Make(Kind::Wrap, {left, top, right, top + textHeight}, explain));
    top += textHeight + 16;
    if (!error.empty()) {
      top = Paragraph(l, Kind::ErrorText, error, left, right, top) + 12;
    }
    const std::array<std::tuple<Kind, const wchar_t*, const wchar_t*, UiCommand>, 3> choices = {{
        {Kind::AccentButton, L"Перенести правки", kGlyphSync, UiCommand::CarryOverEdits},
        {Kind::Button, L"Взять новую", kGlyphDownload, UiCommand::TakeSubscription},
        {Kind::Button, L"Оставить мою", kGlyphCheck, UiCommand::KeepConfig},
    }};
    const float width = inRow ? (right - left - 2 * 12) / 3 : right - left;
    for (std::size_t i = 0; i < choices.size(); ++i) {
      const auto& [kind, label, glyph, command] = choices[i];
      const float bx = inRow ? left + static_cast<float>(i) * (width + 12) : left;
      const float by = inRow ? top : top + static_cast<float>(i) * (kButton + 8);
      l.items.push_back(CommandItem(kind, {bx, by, bx + width, by + kButton}, label, glyph, command, index));
    }
    return y + h;
  }

  // Where traffic goes (routing.h): the metro scheme (Board), the lists' state under it, the listed
  // programs' mode; the mode's menu over all of it while open. The route check is a line under the
  // lists again since the checks' page went (11.11.2026): one compact row, its "Проверить" the
  // only key (03.11.2026, his: "ни разу не пригодилась кнопка" - the tiles' one went).
  float Routing(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    const UiRouting& r = c.routing;
    float leadBottom = y;
    y = Board(l, c, x0, x1, y, leadBottom);
    if (!r.lists.empty()) {  // only while the lists are downloading, or failed
      y = Paragraph(l, r.listsFailed ? Kind::ErrorText : Kind::Caption, r.lists, x0 + kSchemeIndent, x1, y) + 4;
    }
    y += kGap;
    if (c.checks.size() > kCheckRoute) {
      const std::size_t card = l.items.size();
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y}));
      y += kCheckRow + CheckLine(l, c, kCheckRoute, x0, x1, y);
      l.items[card].rect.bottom = y;
      y += kGap;
    }
    const float end = c.appsInclude ? Apps(l, c, x0, x1, y) : y;
    return c.routeMenu ? std::max(end, RouteMenu(l, r, x0, x1, leadBottom + 8)) : end;
  }

  // What a row grows by (0 or kStackLine) when its value doesn't fit beside its title in the room the
  // row's rectangle gives: the value then stands under the title (Item::stacked), whole. The same
  // measure as Describe's, so what the layout test calls cut is what grows.
  // A board chip wider than the page: the room for its text (as DrawItem gives it: 32 in past its
  // icon, 12 off the right) and the height it then takes - the text wrapped, the detail on a line
  // under it, 6 above and below. kTag at least.
  static float ChipTextRoom(float width) { return width - 32 - 12; }
  float ChipWrappedHeight(const UiChip& chip, float width) const {
    const float room = ChipTextRoom(width);
    const float detail = chip.detail.empty() ? 0.0f : TextHeight(chip.detail, captionWrap_.get(), room);
    return std::max(kTag, TextHeight(chip.text, wrap_.get(), room) + detail + 12);
  }

  float StackExtra(const Item& it) const {
    if (it.detail.empty()) {
      return 0;
    }
    const float w = it.rect.right - it.rect.left;
    if (it.kind == Kind::ValueRow) {
      const float need = TextWidth(it.text, body_.get()) + 12 + TextWidth(it.detail, trailing_.get()) +
                         (it.logo.empty() ? 0.0f : 22.0f) + (it.hinted ? kHint + 6 : 0.0f);
      return need > w - 16 - 30 - it.indent ? kStackLine : 0;
    }
    if (it.kind == Kind::CheckRow) {
      const float need = TextWidth(it.text, body_.get()) + 6 + kHint + 8 + TextWidth(it.detail, trailing_.get());
      return need > w - 34 - CheckButtonRoom(it) ? kStackLine : 0;
    }
    return 0;
  }

  // A check's line (c.checks[i]) at `top`: its status dot, its title and
  // "?" (what it does, and the last result in full), the result on the
  // right; an address and the exposure with a button of their own (the routing
  // page's line and the settings' - the rest is said on the overview).
  // Returns what the line grew by (0, or kStackLine: the result under the title).
  float CheckLine(Layout& l, const UiContent& c, std::size_t i, float x0, float x1, float top) const {
    const UiCheck& check = c.checks[i];
    const bool hasButton = i == kCheckRoute || i == kCheckExposure;
    Item row = Make(Kind::CheckRow, {x0 + 4, top + 1, x1 - 4, top + kCheckRow - 1}, check.title);
    // Not run, a dash - unless the row's own button says what to do (the route's
    // "—" read as a "minus" to fold, 30.10.2026).
    row.detail = check.status != UiCheck::Status::NotRun ? check.summary
                 : hasButton                             ? std::wstring()
                                                         : std::wstring(L"—");
    row.index = static_cast<int>(check.status);
    row.sub = static_cast<int>(i);
    row.text2 = check.detail;
    row.checked = hasButton;  // the result stops short of the button
    const float grown = StackExtra(row);
    if (grown > 0) {
      row.rect.bottom += grown;
      row.stacked = true;
    }
    l.items.push_back(std::move(row));

    std::wstring hint = check.hint;
    if (check.status != UiCheck::Status::NotRun && check.status != UiCheck::Status::Running) {
      hint += L"\n\nСейчас: " + check.summary;
      if (!check.detail.empty()) {
        hint += L"\n" + check.detail;
      }
    }
    // With a result, its room (120) is the "?"'s limit; with the word "Проверить" and no result - the
    // button's edge: at 350 wide the "?" sat on "сайт?" (31.10.2026).
    const bool word = hasButton && check.status == UiCheck::Status::NotRun;
    const float hintRight = word ? x1 - 20 - kRouteButton - 6 : x1 - 120;
    HintAfter(l, check.title, body_.get(), x0 + 4 + 34, hintRight, top + kCheckRow / 2, std::move(hint));

    if (hasButton) {
      // The route's a word, not an arrow: an arrow there read as "unfold" (30.10.2026);
      // with a result in the room, "again" - the icons' refresh. The word's button ends on
      // the line 20, the key (28) in the zone that ends on 14: its glyph ends on 20.
      const bool route = i == kCheckRoute;
      const UiCommand command = route ? UiCommand::CheckRoute : UiCommand::RunChecks;
      const int index = route ? 0 : static_cast<int>(i);
      Item run = word ? CommandItem(Kind::Button, {x1 - 20 - kRouteButton, top + 4, x1 - 20, top + kCheckRow - 4},
                                    L"Проверить", nullptr, command, index)
                      : CommandItem(Kind::IconButton, {x1 - 14 - 28, top + 7, x1 - 14, top + kCheckRow - 7},
                                    route ? L"Проверить другой сайт…" : L"Проверить снова", kGlyphRefresh, command,
                                    index);
      run.enabled = !c.checksRunning;
      run.tip = route ? L"Ввести сайт и узнать, каким путём он пойдёт"
                      : L"Проверить ещё раз, что видно этот компьютер в локальной сети";
      const float runLeft = run.rect.left;
      l.items.push_back(std::move(run));
      // The route's address is typed here, in a well of its own (a click anywhere on it, as on the button),
      // while the room after the title holds one.
      if (route && word) {
        const float wellLeft = x0 + 4 + 30 + std::ceil(TextWidth(check.title, body_.get())) + kHint + 16;
        if (runLeft - 8 - wellLeft >= 120) {
          Item well = CommandItem(Kind::Find, {wellLeft, top + 5, runLeft - 8, top + kCheckRow - 5}, L"", nullptr,
                                  command, index);
          well.text2 = L"youtube.com";
          well.enabled = !c.checksRunning;
          l.items.push_back(std::move(well));
        }
      }
    }
    return grown;
  }

  // A failed check's way out on the page: what the banner says, and what a
  // click does - a command, or a page opened; `titles`: the checks it helps.
  struct FixAction {
    std::wstring label;
    ItemAction action = ItemAction::Command;
    UiCommand command{};
    int index = 0;
    std::wstring titles;
  };

  // What the page offers for a failed check (UiCheck::Fix). The kill switch
  // already on, a leak is the connection's: it's restarted.
  static FixAction FixWay(UiCheck::Fix fix, const UiContent& c) {
    const auto page = [](const wchar_t* label, UiPage to) {
      return FixAction{
          .label = label, .action = ItemAction::Page, .command = {}, .index = static_cast<int>(to), .titles = {}};
    };
    const auto reconnect = [] {
      return FixAction{
          .label = L"переподключить", .action = ItemAction::Command, .command = UiCommand::Reconnect, .index = 0, .titles = {}};
    };
    switch (fix) {
      case UiCheck::Fix::KillSwitch:
        if (!c.killSwitch) {
          return FixAction{.label = L"включить Kill switch",
                     .action = ItemAction::Command,
                     .command = UiCommand::ToggleKillSwitch,
                     .index = kFromChecks,
                     .titles = {}};
        }
        return reconnect();
      case UiCheck::Fix::Routing: return page(L"открыть маршруты", UiPage::Routing);
      case UiCheck::Fix::Dns: return page(L"сменить DNS в маршрутах", UiPage::Routing);
      case UiCheck::Fix::LocalNetwork: return page(L"открыть настройки локальной сети", UiPage::Settings);
      case UiCheck::Fix::Reconnect:
      case UiCheck::Fix::None: break;
    }
    return reconnect();
  }

  // The protection on the overview (11.11.2026, the checks' page gone, his: "надо думать не как
  // обертка"): the quiet check's failures (main.cpp ScheduleProtectionCheck) a banner each way out,
  // what it helps named in it; and the kill switch off while on - nothing holds the traffic when the
  // VPN drops. Nothing at all while it's fine.
  float ProtectionBanners(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    std::vector<FixAction> ways;
    std::vector<std::wstring> tips;
    for (const std::size_t i : {kCheckExit, kCheckLeaks}) {
      if (c.display != Display::On || i >= c.checks.size() || c.checks[i].status != UiCheck::Status::Fail) {
        continue;
      }
      const UiCheck& check = c.checks[i];
      std::wstring what;
      if (i == kCheckExit) {
        what = check.fix == UiCheck::Fix::KillSwitch ? L"Сайты видят не адрес VPN" : L"Через VPN сайты не открываются";
      } else {
        constexpr std::wstring_view kHas = L"есть: ";
        const std::wstring_view names = std::wstring_view(check.summary).starts_with(kHas)
                                            ? std::wstring_view(check.summary).substr(kHas.size())
                                            : std::wstring_view(check.summary);
        what = L"Утечка мимо VPN: " + std::wstring(names);
      }
      FixAction way = FixWay(check.fix, c);
      const auto same =
          std::find_if(ways.begin(), ways.end(), [&](const FixAction& w) { return w.label == way.label; });
      if (same != ways.end()) {
        same->titles += L"; " + what;
        tips[static_cast<std::size_t>(same - ways.begin())] += L"\n\n" + check.detail;
        continue;
      }
      way.titles = std::move(what);
      ways.push_back(std::move(way));
      tips.push_back(check.detail);
    }
    bool offered = false;  // a banner above already turns the kill switch on
    for (std::size_t k = 0; k < ways.size(); ++k) {
      const FixAction& way = ways[k];
      offered = offered || (way.action == ItemAction::Command && way.command == UiCommand::ToggleKillSwitch);
      Item banner = Make(Kind::Banner, {x0, y, x1, y + 40}, way.titles + L" — " + way.label, kGlyphWarning);
      banner.action = way.action;
      banner.command = way.command;
      banner.index = way.index;
      banner.tip = tips[k];
      l.items.push_back(std::move(banner));
      y += 40 + 8;
    }
    if (c.on && !c.killSwitch && !offered) {
      Item off = CommandItem(Kind::Banner, {x0, y, x1, y + 40}, L"Защита от утечек выключена — включить", kGlyphShield,
                             UiCommand::ToggleKillSwitch, kFromChecks);
      off.tip = L"Kill switch выключен: если VPN оборвётся, программы пойдут напрямую, с твоим адресом. Включить — "
                L"интернет закроется до восстановления подключения. Он же в «Настройках».";
      l.items.push_back(std::move(off));
      y += 40 + 8;
    }
    return y;
  }

  // The per-app list - a section of Routing: which programs go through the proxy.
  float Apps(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    y = Heading(l, L"Приложения", x0, x1, y,
                L"Какие программы идут через VPN. Имена — как у exe-файла, без пути: правило ловит программу, "
                L"где бы она ни лежала.");
    const float mid = (x0 + x1) / 2;
    Item except = CommandItem(Kind::Segment, {x0, y, mid - 4, y + 36}, L"Все, кроме списка", nullptr,
                              UiCommand::SetAppsMode, 0);
    except.checked = !c.appsInclude;
    Item only = CommandItem(Kind::Segment, {mid + 4, y, x1, y + 36}, L"Только список", nullptr, UiCommand::SetAppsMode, 1);
    only.checked = c.appsInclude;
    l.items.push_back(std::move(except));
    l.items.push_back(std::move(only));
    y += 36 + 6;
    y = Paragraph(l, Kind::Caption,
                  c.appsInclude ? L"Через VPN идут только программы из списка, остальной трафик — напрямую."
                                : L"Весь трафик идёт через VPN, кроме программ из списка — они ходят напрямую.",
                  x0 + 4, x1, y) +
        8;

    if (c.apps.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 64}));
      l.items.push_back(Make(Kind::Muted, {x0 + 16, y, x1 - 16, y + 64},
                             c.appsInclude ? L"Список пуст — через VPN не идёт ничего." : L"Список пуст — всё идёт через VPN."));
      y += 64;
    } else {
      const float h = static_cast<float>(c.apps.size()) * kRow;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      for (std::size_t i = 0; i < c.apps.size(); ++i) {
        const float top = y + static_cast<float>(i) * kRow;
        if (i > 0) {
          l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
        }
        Item app = Make(Kind::AppRow, {x0 + 8, top, x1 - 56, top + kRow}, c.apps[i], kGlyphProgram);
        app.detail = i < c.appPaths.size() ? c.appPaths[i] : std::wstring();  // the icon's exe
        app.index = static_cast<int>(i);  // for its right-click menu
        l.items.push_back(std::move(app));
        const float bt = top + (kRow - kButton) / 2;
        Item remove = CommandItem(Kind::IconButton, {x1 - 12 - kButton, bt, x1 - 12, bt + kButton}, L"Убрать",
                                  kGlyphRemove, UiCommand::RemoveApp, static_cast<int>(i));
        l.items.push_back(std::move(remove));
      }
      y += h;
    }
    y += kGap;
    const float half = (x0 + x1) / 2;
    l.items.push_back(CommandItem(Kind::Button, {x0, y, half - 4, y + kButton}, L"Из запущенных", kGlyphAdd,
                                  UiCommand::AddRunning));
    l.items.push_back(
        CommandItem(Kind::Button, {half + 4, y, x1, y + kButton}, L"Файл exe…", kGlyphFile, UiCommand::AddExe));
    return y + kButton;
  }

  // The log (logs-concept.html, variant A): the search on top with pause, copy and "⋯" (save, how much the
  // core writes, clear) on its right; what's wrong right now; the filter; the events, a row each, filling
  // the window; the selected one's card under them - its line as the core wrote it, and what can be done.
  // Right-click for copy, select all, save, clear.
  float Logs(Layout& l, const UiContent& c, const LogState& s, float x0, float x1, float y, float height) const {
    const float titleTop = y - 1;  // the keys of 36 on the row of kButton's line
    y += kButton + kGap;
    float right = x1;
    Item more = Make(Kind::IconButton, {right - 36, titleTop, right, titleTop + 36}, L"Ещё", kGlyphMore);
    more.action = ItemAction::LogMore;
    more.tip = c.logLevel.empty() ? std::wstring(L"Сохранить в файл, подробность записи (авто), очистить")
                                  : L"Сохранить в файл, подробность записи (" + c.logLevel + L"), очистить";
    l.items.push_back(std::move(more));
    right -= 40;
    Item copy = Make(Kind::IconButton, {right - 36, titleTop, right, titleTop + 36}, L"Копировать всё", kGlyphCopy);
    copy.action = ItemAction::CopyLogs;
    copy.index = 1;
    copy.tip = L"Скопировать журнал как есть - для поддержки";
    l.items.push_back(std::move(copy));
    right -= 40;
    const bool waiting = s.paused && s.held > 0;
    const float pauseWidth = waiting ? 84.0f : 36.0f;
    Item pause = Make(waiting ? Kind::Button : Kind::IconButton, {right - pauseWidth, titleTop, right, titleTop + 36},
                      waiting ? std::format(L"+{}", s.held) : std::wstring(s.paused ? L"Продолжить" : L"Пауза"),
                      s.paused ? kGlyphPlay : kGlyphPause);
    pause.action = ItemAction::PauseLogs;
    pause.checked = s.paused;
    pause.tip = waiting    ? std::format(L"Новых строк: {}. Показать их и продолжить", s.held)
                : s.paused ? L"Продолжить: новые строки снова пойдут"
                           : L"Пауза: новые строки подождут, пока читаешь";
    l.items.push_back(std::move(pause));
    right -= pauseWidth + 8;
    Item find = Make(Kind::Find, {x0, titleTop, right, titleTop + 36}, s.search);
    find.action = ItemAction::FindLog;
    l.items.push_back(std::move(find));

    // What's wrong right now, in full and in red, over the lines (the main
    // screen has one line of it); the subscription's report under its trouble.
    for (const Issue& issue : Issues(c)) {
      y = Paragraph(l, Kind::ErrorText, issue.full, x0, x1, y) + 8;
    }
    if (!c.trouble.empty() && c.troubleProfile >= 0) {
      l.items.push_back(ReportButton(c, c.troubleSupport, {x0, y, x1, y + kButton}, c.troubleProfile));
      y += kButton + kGap;
    }

    // Which events are shown, with how many kinds of each; one kind one by one: the way back, and which.
    if (s.only || !s.route.empty()) {
      constexpr float kBack = 120;
      Item back = Make(Kind::Chip, {x0, y, x0 + kBack, y + 30}, L"← Все записи");
      back.action = ItemAction::LogOnly;
      back.index = 1;
      back.checked = true;
      back.tip = L"Снова все записи";
      l.items.push_back(std::move(back));
      l.items.push_back(
          Make(Kind::Caption, {x0 + kBack + 10, y, x1, y + 30}, L"только: " + (s.only ? s.onlyTitle : s.route)));
    } else {
      std::array<std::wstring, kLogFilters> labels;
      float used = 0;
      for (std::size_t i = 0; i < labels.size(); ++i) {
        labels[i] = std::format(L"{} {}", kLogFilterNames[i], s.counts[i]);
        used += TextWidth(labels[i], chip_.get()) + 20;
      }
      // As wide as each says, what's left shared out: no word cut.
      const float gaps = 6.0f * static_cast<float>(kLogFilters - 1);
      const float spare = std::max(0.0f, (x1 - x0 - gaps - used) / static_cast<float>(kLogFilters));
      float left = x0;
      for (std::size_t i = 0; i < labels.size(); ++i) {
        const float w = TextWidth(labels[i], chip_.get()) + 20 + spare;
        Item filter = Make(Kind::Chip, {left, y, left + w, y + 30}, labels[i]);
        filter.action = ItemAction::LogFilter;
        filter.index = static_cast<int>(i);
        filter.checked = s.filter == static_cast<int>(i);
        l.items.push_back(std::move(filter));
        left += w + 6;
      }
    }
    y += 30 + 10;

    // Fills the window: the rows scroll inside, the page doesn't.
    constexpr float kActions = 30;
    const float lineHeight =
        s.selected ? std::min(std::max(36.0f, height * 0.16f), TextHeight(s.detail, captionWrap_.get(), x1 - x0 - 28))
                   : 0.0f;
    const float detailHeight = s.selected ? 12 + lineHeight + 10 + kActions + 12 : 0.0f;
    const float bottom = std::max(y + 120, height - kPad - (detailHeight > 0 ? detailHeight + 8 : 0.0f));
    l.items.push_back(Make(Kind::Card, {x0, y, x1, bottom}));
    l.items.push_back(Make(Kind::LogBox, {x0, y, x1, bottom}));
    l.logBox = D2D1_RECT_F{x0 + 1, y + 8, x1 - 1, bottom - 8};
    if (detailHeight > 0) {
      // The selected row: its line as the core wrote it, and what can be done with it.
      const float top = bottom + 8;
      l.items.push_back(Make(Kind::Card, {x0, top, x1, top + detailHeight}));
      l.items.push_back(Make(Kind::Caption, {x0 + 14, top + 12, x1 - 14, top + 12 + lineHeight}, s.detail));
      const float keys = top + 12 + lineHeight + 10;
      float left = x0 + 14;
      // A glyph: a key of it alone (its words in the tip); words: as wide as they are, none past the card.
      const auto action = [&](std::wstring text, const wchar_t* glyph, ItemAction what, std::wstring tip) {
        const float w = glyph != nullptr ? 36.0f : std::min(TextWidth(text, button_.get()) + 28, x1 - 14 - left);
        Item key = Make(glyph != nullptr ? Kind::IconButton : Kind::Button, {left, keys, left + w, keys + kActions},
                        std::move(text), glyph);
        key.action = what;
        key.tip = std::move(tip);
        l.items.push_back(std::move(key));
        left += w + 8;
      };
      action(L"Скопировать", kGlyphCopy, ItemAction::CopyLogs, L"Скопировать выбранное как есть");
      action(s.only ? std::wstring(L"Все записи") : std::wstring(L"Только такие"), nullptr, ItemAction::LogOnly,
             s.only ? L"Снова все записи" : L"Только такие записи, каждая отдельно - когда и с чем");
      if (s.server) {
        action(L"Показать сервер", nullptr, ItemAction::LogServer, L"Открыть страницу серверов");
      }
      return top + detailHeight - kPad + 1;
    }
    return bottom - kPad + 1;  // exactly the view: nothing to scroll
  }

  // The fine settings, the fold's body under its instrument's head: QUIC, IPv4 and DNS, a row each, in
  // the head's own card - the fold opens in place, not in a card under it (31.10.2026); in under the
  // head's name since 11.11.2026. `top` is where the head ends; returns where the last row does.
  // (The DNS had a heading and a card of its own till 30.10.2026, his: the
  // settings didn't fit a screen. Hysteria 2's speed is its servers' menu's
  // since 17.10.2026.)
  float FineRows(Layout& l, const UiContent& c, float x0, float x1, float top) const {
    const UiRouting& r = c.routing;
    const float in = x0 + 20 + kInstrumentIndent;
    const auto line = [&](float at) {
      Item d = Make(Kind::Divider, {in, at, x1 - 20, at + 1});
      d.indent = kInstrumentIndent;
      l.items.push_back(std::move(d));
    };
    const auto row = [&](Kind kind, float at, const wchar_t* title, UiCommand command) {
      Item it = CommandItem(kind, {x0 + 4, at + 2, x1 - 4, at + kSettingsRow - 2}, title, nullptr, command);
      it.indent = kInstrumentIndent;
      return it;
    };
    line(top);
    Item quic = row(Kind::ValueRow, top, L"QUIC", UiCommand::ChooseQuic);
    quic.detail = r.quic == 1     ? L"выкл."
                  : r.quic == 2   ? L"вкл."
                  : r.quicBlocked ? L"авто (сейчас выкл.)"
                                  : L"авто (сейчас вкл.)";
    quic.hinted = true;
    l.items.push_back(std::move(quic));
    HintAfter(l, L"QUIC", body_.get(), in, x1 - 8, top + kSettingsRow / 2,
              L"QUIC — HTTP/3 по UDP. Через серверы на TCP (VLESS, Trojan, AnyTLS) он идёт хуже обычного TCP, "
              L"поэтому выключенный QUIC не мешает: браузеры сразу берут TCP.\n\n«Авто» включает QUIC, только "
              L"если все твои серверы — Hysteria2 или TUIC. Есть хоть один на TCP — выключен.");
    top += kSettingsRow;
    line(top);
    Item v4 = row(Kind::Switch, top, L"Только IPv4", UiCommand::ToggleIpv4Only);
    v4.checked = r.ipv4Only;
    l.items.push_back(std::move(v4));
    HintAfter(l, L"Только IPv4", body_.get(), in, x1 - 80, top + kSettingsRow / 2,
              L"Сайты получают только IPv4-адреса. У многих серверов VPN нет IPv6: без этого браузер сначала "
              L"пробует IPv6, ждёт и лишь потом идёт по IPv4 — сайт открывается с задержкой или не открывается.");
    top += kSettingsRow;

    // DNS: who answers for the names of each kind - auto, a pick, or a
    // server of the user's own (typed over its row).
    line(top);
    Item remote = row(Kind::ValueRow, top, L"DNS через VPN", UiCommand::ChooseRemoteDns);
    remote.detail = r.remoteDns;
    remote.hinted = true;
    remote.tick = kBucketProxy;
    remote.tickAlways = true;
    l.items.push_back(std::move(remote));
    HintAfter(l, L"DNS через VPN", body_.get(), in, x1 - 8, top + kSettingsRow / 2,
              L"Имена всех сайтов, кроме прямых: запрос идёт через VPN, провайдер его не видит.\n\n«Авто» "
              L"подходит почти всем: это Cloudflare. Свой адрес пишется так: 1.2.3.4, tls://dns.example, "
              L"https://dns.example/dns-query, quic://dns.example.");
    top += kSettingsRow;
    line(top);
    Item local = row(Kind::ValueRow, top, L"DNS напрямую", UiCommand::ChooseLocalDns);
    local.detail = r.localDns;
    local.hinted = true;
    local.tick = kBucketDirect;
    local.tickAlways = true;
    l.items.push_back(std::move(local));
    HintAfter(l, L"DNS напрямую", body_.get(), in, x1 - 8, top + kSettingsRow / 2,
              L"Российские сайты, твои прямые и адреса самих серверов.\n\nЕсли этот DNS перестанет отвечать, "
              L"Sovereign возьмёт следующий: Cloudflare, Google, Quad9, Яндекс, в конце DNS Windows. Яндекс — "
              L"только здесь: в России его не блокируют.");
    top += kSettingsRow;
    return top;
  }

  // 12345 as "12 345": a no-break space between the thousands.
  static std::wstring Grouped(std::uint64_t n) {
    const std::wstring digits = std::to_wstring(n);
    std::wstring out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
      if (i > 0 && (digits.size() - i) % 3 == 0) {
        out += L' ';
      }
      out += digits[i];
    }
    return out;
  }

  // The word for `n` of a thing in Russian: 1, 21 - `one`; 2-4, 22-24 - `few`; 5-20, 25-30 - `many`.
  static const wchar_t* Plural(std::size_t n, const wchar_t* one, const wchar_t* few, const wchar_t* many) {
    const std::size_t tens = n % 100;
    const std::size_t units = n % 10;
    if (tens >= 11 && tens <= 14) {
      return many;
    }
    return units == 1 ? one : (units >= 2 && units <= 4) ? few : many;
  }

  float Settings(Layout& l, const UiContent& c, float x0, float x1, float y) const {

    // Instruments (docs/design/settings-instruments.html, 11.11.2026, his: "приборы больше понравились"):
    // a card each - the kill switch, the local network, the blocks, the fine settings, the app - its
    // head a lamp, the name and what it does right now, its switch or fold on the right; its rows in
    // under the name. No headings: the head is the card's name. A card that went off or is worth fixing
    // says so in its edge and its head's words, white. The head's "?" and a ↻ show while it's pointed at.
    // (Till 11.11.2026: headings over cards of switches - "Подключение", "Блокировка", "О Sovereign".)
    const auto open = [&](float top) {
      l.items.push_back(Make(Kind::Card, {x0, top, x1, top}));
      return l.items.size() - 1;
    };
    const auto close = [&](std::size_t card, float bottom) {
      l.items[card].rect.bottom = bottom;
      return bottom + kSettingsGap;
    };
    const float in = x0 + 20 + kInstrumentIndent;  // the rows' text, under the heads' names
    const auto line = [&](float top) {
      Item d = Make(Kind::Divider, {in, top, x1 - 20, top + 1});
      d.indent = kInstrumentIndent;
      l.items.push_back(std::move(d));
    };
    const auto row = [&](float top) { return D2D1_RECT_F{x0 + 4, top + 2, x1 - 4, top + kSettingsRow - 2}; };
    // A row put in place, in under the name; with no room beside its title it grows and the value
    // stands under it. Returns what it grew by - the next row starts that much lower.
    const auto put = [&](Item it) {
      it.indent = kInstrumentIndent;
      const float grown = StackExtra(it);
      if (grown > 0) {
        it.rect.bottom += grown;
        it.stacked = true;
      }
      l.items.push_back(std::move(it));
      return grown;
    };
    // A card's head at `top`: taller when what it says wraps. Its index - the fields are set through it.
    const auto head = [&](float top, std::wstring name, std::wstring detail, int lamp, int sub) {
      Item h = Make(Kind::Instrument, {x0 + 4, top + 2, x1 - 4, top + kInstrumentHead - 2}, std::move(name));
      h.sub = sub;
      h.lamp = lamp;
      const float width = (x1 - 4) - (x0 + 4) - kInstrumentText - InstrumentControl(h);
      const float lines = detail.empty() ? 0.0f : TextHeight(detail, captionWrap_.get(), width);
      h.rect.bottom = std::max(h.rect.bottom, h.rect.top + 30 + lines + 6);
      h.detail = std::move(detail);
      l.items.push_back(std::move(h));
      return l.items.size() - 1;
    };
    const auto foot = [&](std::size_t at) { return l.items[at].rect.bottom + 2; };
    // The head's "?" after its name, shy; returns where it ends (a ↻ may follow).
    const auto headHint = [&](std::size_t at, std::wstring hint) {
      const std::wstring name = l.items[at].text;
      const float left = l.items[at].rect.left + kInstrumentText;
      const float right = l.items[at].rect.right - InstrumentControl(l.items[at]);
      HintAfter(l, name, heading_.get(), left, right, l.items[at].rect.top + 18, std::move(hint));
      l.items.back().shy = true;
      return l.items.back().rect.right;
    };
    // A ↻ after the head's "?", shy too: it's seldom needed (the checks run again by themselves).
    const auto headRefresh = [&](std::size_t at, float after, std::wstring name, UiCommand command, int index,
                                 bool enabled, std::wstring tip) {
      const float cy = l.items[at].rect.top + 18;
      const float right = l.items[at].rect.right - InstrumentControl(l.items[at]);
      const float x = std::min(after + 2, right - 28);
      Item run = CommandItem(Kind::IconButton, {x, cy - 14, x + 28, cy + 14}, std::move(name), kGlyphRefresh, command,
                             index);
      run.enabled = enabled;
      run.tip = std::move(tip);
      run.shy = true;
      l.items.push_back(std::move(run));
    };
    const bool connected = c.display == Display::On;

    // The kill switch: under a lid while on - a click lifts it, the next one (in 5 s) turns it off.
    {
      const std::size_t card = open(y);
      int lamp = 0;
      bool loud = false;
      std::wstring said;
      if (c.lockedOut) {
        lamp = 3;
        loud = true;
        said = L"закрыл интернет: " + (c.lockoutReason.empty() ? std::wstring(L"VPN не отвечает") : c.lockoutReason);
      } else if (!c.killSwitch) {
        said = L"выключен: если VPN оборвётся, трафик пойдёт мимо него";
      } else if (!c.killSwitchError.empty()) {
        lamp = 2;
        loud = true;
        said = L"не включился: " + c.killSwitchError;
      } else if (c.killSwitchActive) {
        lamp = 1;
        said = L"готов: закроет интернет, если VPN оборвётся";
      } else {
        said = L"включится вместе с VPN";
      }
      const std::size_t at = head(y, L"Kill switch", std::move(said), lamp, 1);
      {
        Item& h = l.items[at];
        h.checked = c.killSwitch;
        h.loud = loud;
        h.lid = c.killSwitch && !c.lidUp;
        h.lifted = c.killSwitch && c.lidUp;
        if (h.lid) {
          h.action = ItemAction::LiftLid;
          h.tip = L"Поднять крышку: выключить можно в следующие 5 секунд";
        } else {
          h.action = ItemAction::Command;
          h.command = UiCommand::ToggleKillSwitch;
        }
      }
      l.items[card].verdict = lamp >= 2 ? lamp : 0;
      const float bottom = foot(at);
      headHint(at, L"Если подключение оборвётся, трафик не пойдёт мимо VPN: интернет закрыт, пока оно не "
                   L"восстановится. Выключишь VPN сам — интернет работает как обычно.\n\nВключённый он под "
                   L"крышкой, чтобы не выключить его случайно: первое нажатие поднимает её, второе в следующие "
                   L"5 секунд выключает.");
      y = close(card, bottom);
    }

    // The local network: open, or closed both ways but for what the user lets in - the router, a device
    // around, an address typed in. Closed, a row per device let in (a click: shut again, LanChipMenu)
    // and one for the rest around (LanAdd: let one in). What the network sees of this computer (the
    // checks' page's till 11.11.2026, then a line of the card) is said by the head; its ↻ looks again.
    const UiCheck* seen = c.checks.size() > kCheckExposure ? &c.checks[kCheckExposure] : nullptr;
    {
      const std::size_t card = open(y);
      std::size_t allowed = 0;
      std::size_t others = 0;
      for (const UiLanHost& h : c.lanHosts) {
        if (h.allowed) {
          ++allowed;
        } else if (h.seen) {
          ++others;
        }
      }
      int lamp = 0;
      bool loud = false;
      std::wstring said;
      if (seen != nullptr && seen->status == UiCheck::Status::Fail) {
        lamp = 2;
        loud = true;
        said = seen->summary;  // the firewall off, or letting in; its way out under the card
      } else if (seen != nullptr && seen->status == UiCheck::Status::Warn && !c.lanClosed) {
        // "работает: общий доступ к файлам" - said as what is open to the network.
        std::wstring what = seen->summary;
        constexpr std::wstring_view kRuns = L"работает: ";
        if (what.starts_with(kRuns)) {
          what.erase(0, kRuns.size());
        }
        lamp = 2;
        loud = true;
        said = (what.find(L',') != std::wstring::npos ? L"из сети открыты: " : L"из сети открыт ") + what;
      } else if (c.lanClosed) {
        lamp = connected ? 1 : 0;
        said = !connected     ? L"закроется вместе с VPN"
               : allowed > 0 ? L"из сети подключаются только разрешённые"
                             : L"из сети к компьютеру не подключиться";
      } else {
        said = L"из сети можно подключиться к компьютеру";
      }
      const std::size_t at = head(y, L"Закрыть от локальной сети", std::move(said), lamp, 1);
      l.items[at].checked = c.lanClosed;
      l.items[at].loud = loud;
      l.items[at].action = ItemAction::Command;
      l.items[at].command = UiCommand::ToggleLanClosed;
      l.items[card].verdict = lamp >= 2 ? lamp : 0;
      float top = foot(at);
      std::wstring hint =
          L"Фильтр только на этом компьютере. Роутер, телефоны и другие устройства работают как обычно и "
          L"пользуются интернетом — они просто не могут связаться с этим компьютером, а он с ними: общие "
          L"папки, принтер, трансляция на телевизор, передача файлов с телефона.\n\nИнтернет этого "
          L"компьютера тоже идёт через роутер, но зашифрованным: роутер не видит, какие сайты открыты, "
          L"кроме прямых. "
          L"Закрытый роутер — это только его настройки и его DNS.\n\nСовсем спрятаться нельзя: роутер "
          L"видит, что компьютер подключён, а сканер сети на телефоне — что по его адресу есть устройство. "
          L"Связаться с ним никто не сможет.\n\nРазрешённое устройство проходит только этот фильтр: "
          L"соединения, о которых компьютер не просил, по-прежнему не пускает брандмауэр Windows. "
          L"Разрешаются адреса IPv4, по IPv6 локальная сеть закрыта целиком. DNS роутера закрыт и для "
          L"клиента: вместо системного DNS — зашифрованный.\n\nДействует, пока подключение включено.";
      if (seen != nullptr) {
        hint += L"\n\n" + seen->title + L": проверяется после каждого подключения, ↻ — ещё раз.";
        if (seen->status != UiCheck::Status::NotRun && seen->status != UiCheck::Status::Running) {
          hint += L"\nСейчас: " + seen->summary;
          if (!seen->detail.empty()) {
            hint += L"\n" + seen->detail;
          }
        }
      }
      const float after = headHint(at, std::move(hint));
      if (seen != nullptr) {
        headRefresh(at, after, L"Проверить снова", UiCommand::RunChecks, static_cast<int>(kCheckExposure),
                    !c.checksRunning, L"Проверить ещё раз, что видно этот компьютер в локальной сети");
      }
      if (c.lanClosed) {
        for (std::size_t i = 0; i < c.lanHosts.size(); ++i) {
          const UiLanHost& h = c.lanHosts[i];
          if (!h.allowed) {
            continue;
          }
          line(top);
          const std::wstring title = h.router ? L"Роутер" : h.name.empty() ? h.address : h.name;
          Item host = CommandItem(Kind::ValueRow, row(top), title, nullptr, UiCommand::LanChipMenu,
                                  static_cast<int>(i));
          host.detail = !h.seen ? L"не в сети" : title != h.address ? h.address : L"";
          host.logo = "lan";
          top += kSettingsRow + put(std::move(host));
        }
        // The rest around, counted: "Ещё" only under the ones let in, one said as a word.
        std::wstring rest;
        if (others == 0) {
          rest = L"Других устройств в сети пока не видно";
        } else if (others == 1) {
          rest = allowed > 0 ? L"Ещё одно устройство в сети — вход закрыт" : L"Одно устройство в сети — вход закрыт";
        } else {
          rest = (allowed > 0 ? L"Ещё " : L"") + std::to_wstring(others) + L" " +
                 Plural(others, L"устройство", L"устройства", L"устройств") + L" в сети — вход закрыт";
        }
        line(top);
        Item add = CommandItem(Kind::ValueRow, row(top), std::move(rest), kGlyphAdd, UiCommand::LanAdd);
        add.detail = others == 0 ? L"по адресу" : L"разрешить";
        add.tip = L"Разрешить устройство: из тех, что в сети, или по адресу";
        top += kSettingsRow + put(std::move(add));
      }
      y = close(card, top);
    }
    if (seen != nullptr && seen->status == UiCheck::Status::Fail) {
      l.items.push_back(CommandItem(Kind::Banner, {x0, y, x1, y + 40}, L"Брандмауэр: " + seen->summary + L" — открыть его",
                                    kGlyphWarning, UiCommand::OpenFirewall));
      y += 40 + kSettingsGap;
    }

    // What's cut on the way: ads, trackers, cookie windows, the dangerous - dots in things, a click turns
    // one; how many were cut since the start and the last hour of it by minutes; what was cut lately
    // (a click lets one through), all of the last two minutes at once, and who asks for it.
    {
      const std::size_t card = open(y);
      const UiRouting& r = c.routing;
      // The Russian sites' block cuts off too (and its sites come in "Сайт сломался только что?"), so it is "any".
      const bool any = r.blockAds || r.trackers || r.cookieWindows || r.malware || r.russia == kBucketBlock;
      const bool counting = any && connected;
      // Only the minutes counted: the first hour, less of them - the trace starts where they do.
      const std::size_t known = std::clamp<std::size_t>(c.blockedKnown, 1, c.blockedMinutes.size());
      const std::size_t skipped = c.blockedMinutes.size() - known;
      std::uint64_t hour = 0;
      std::uint32_t busiest = 0;
      for (std::size_t i = skipped; i < c.blockedMinutes.size(); ++i) {
        hour += c.blockedMinutes[i];
        busiest = std::max(busiest, c.blockedMinutes[i]);
      }
      const bool wholeHour = skipped == 0;
      // Counted from 0 within the hour: the readout's number is the same - the subtitle says how
      // long, not "7" twice (11.11.2026, review).
      const bool sameNumber = !wholeHour && !c.blockedSince.empty();
      const std::wstring minutes = std::to_wstring(known) + L" " + Plural(known, L"минуту", L"минуты", L"минут");
      std::wstring said = !any         ? L"ничего не закрывается: включи нужное ниже"
                          : !connected ? L"считает, пока подключение включено"
                          : wholeHour  ? Grouped(hour) + L" за последний час"
                          : sameNumber ? L"за " + minutes
                          : known == 1 ? Grouped(hour) + L" за последнюю минуту"
                                       : Grouped(hour) + L" за последние " + minutes;
      const std::size_t at = head(y, L"Блокировка", std::move(said), 0, counting ? 3 : 0);
      const float headTop = l.items[at].rect.top;
      float top = foot(at);
      headHint(at, L"Что закрывается по пути, по спискам. Точка в названии — закрывается; нажми, чтобы "
                   L"переключить, наведи — расскажет, что это.\n\nСломалось что-то нужное — «Недавно» ниже: "
                   L"нажми имя, и оно пойдёт дальше, или «Пропустить последние» — покажет, что закрыто за "
                   L"последние 2 минуты: пропусти одно или всё сразу. Закрыты «Российские сайты» — их имена "
                   L"там же, отдельной строкой: пойдут напрямую. Ошибся — «Вернуть» закроет снова.");
      if (counting) {
        Item count = CommandItem(Kind::Readout, {x1 - 20 - kReadoutWidth, headTop + 6, x1 - 20, headTop + 48},
                                 Grouped(c.blockedTotal), nullptr, UiCommand::BlockedMenu);
        count.detail = c.blockedSince.empty() ? L"закрыто с запуска"
                       : sameNumber           ? L"закрыто"  // since when: under the trace
                                              : L"закрыто с " + c.blockedSince;
        count.tip = c.blockedSince.empty() ? L"Столько адресов закрыто с запуска Sovereign"
                                           : L"Столько адресов закрыто с запуска Sovereign в " + c.blockedSince;
        l.items.push_back(std::move(count));
        // The hour, a bar a minute: its share of the busiest, in thousandths.
        Item trace = Make(Kind::Trace, {in, top, x1 - 20, top + kTraceHeight});
        trace.text = wholeHour ? std::wstring() : c.blockedFrom;  // under where it starts
        for (std::size_t i = skipped; i < c.blockedMinutes.size(); ++i) {
          const std::uint32_t minute = c.blockedMinutes[i];
          const std::uint64_t share =
              minute == 0 || busiest == 0 ? 0 : std::max<std::uint64_t>(1, std::uint64_t{minute} * 1000 / busiest);
          trace.text2 += static_cast<wchar_t>(share);
        }
        l.items.push_back(std::move(trace));
        top += kTraceHeight + 8;
      }
      struct Category {
        const wchar_t* name;
        bool on;
        UiCommand command;
        const wchar_t* tip;
      };
      std::vector<Category> categories = {
          {L"Реклама", r.blockAds, UiCommand::ToggleBlockAds,
           L"Запросы к рекламным серверам по спискам никуда не уходят: страницы грузятся без рекламы. "
           L"Сломалось что-то нужное — «Недавно» ниже: нажми имя, и оно пойдёт дальше, или «Пропустить "
           L"последние» — выбери из закрытого за последние 2 минуты."},
          {L"Трекеры", r.trackers, UiCommand::ToggleTrackers,
           L"Серверы аналитики и слежки (счётчики, пиксели, профилирование): сайт открывается, но не узнаёт, "
           L"куда ты ходил до него. Обычно ничего не ломает."},
          {L"Окна cookie", r.cookieWindows, UiCommand::ToggleCookieWindows,
           L"Убирает окна «принять cookie» OneTrust, Cookiebot и подобные. Сами cookie не трогает: входы на "
           L"сайты работают. Своё окно сайта (TikTok, DeepSeek) — нет: это не скрипт со стороны."},
          {L"Опасные", r.malware, UiCommand::ToggleMalware,
           L"Адреса с вирусами, фишингом и мошенничеством по спискам: поддельный банк или «выигрыш» не "
           L"откроется."}};
      if (any) {
        categories.push_back(
            {L"Обход DoH", r.closeDoh, UiCommand::ToggleCloseDoh,
             L"Браузер со своим «безопасным DNS» (DoH, DoT) спрашивает адреса мимо клиента, и списки выше "
             L"для него не работают. Включено: известные DNS-сервисы закрыты, браузер спрашивает через "
             L"клиент. Если в браузере выбран конкретный сервис безопасного DNS и сайты перестали "
             L"открываться — выключи его там или здесь."});
      }
      // They flow from the rows' line and wrap at the card's.
      float x = in;
      for (const Category& category : categories) {
        const float w = kDotInk + std::ceil(TextWidth(category.name, caption_.get()));
        if (x > in && x + w > x1 - 20) {
          x = in;
          top += kDotHeight + 6;
        }
        Item dot = CommandItem(Kind::Dot, {x, top, x + w, top + kDotHeight}, category.name, nullptr, category.command);
        dot.checked = category.on;
        dot.tip = category.tip;
        l.items.push_back(std::move(dot));
        x += w + 6;
      }
      top += kDotHeight + 10;
      if (any) {
        // What was cut lately, as many names as fit beside the title.
        line(top);
        Item recent = CommandItem(Kind::ValueRow, row(top), L"Недавно", nullptr, UiCommand::BlockedMenu);
        const float room = (x1 - 4) - (x0 + 4) - 16 - 30 - kInstrumentIndent - TextWidth(recent.text, body_.get()) - 12;
        std::wstring names;
        for (const std::wstring& name : c.blockedLately) {
          std::wstring longer = names;
          if (!longer.empty()) {
            longer += L", ";
          }
          longer += name;
          if (!names.empty() && TextWidth(longer, trailing_.get()) > room) {
            break;
          }
          names = std::move(longer);
        }
        recent.detail = names.empty() ? std::wstring(L"пока пусто") : names;
        recent.tip = L"Что закрыто недавно: нажми имя, и оно пойдёт дальше";
        top += kSettingsRow + put(std::move(recent));
        // A site broke just now: what was cut in the last two minutes, in a menu - one of them let
        // through or all (main.cpp's PassJustNow). A click passed them all at once, by chance too.
        line(top);
        Item broke = Make(Kind::ValueRow, row(top), L"Сайт сломался только что?");
        broke.indent = kInstrumentIndent;
        l.items.push_back(std::move(broke));
        const std::size_t n = c.blockedJustNow;
        const std::wstring pass = n > 0 ? L"Пропустить последние " + std::to_wstring(n) + L"…" : L"Пропускать нечего";
        const float bw = std::ceil(TextWidth(pass, body_.get())) + 28;
        Item button = CommandItem(Kind::Button, {x1 - 20 - bw, top + 4, x1 - 20, top + kSettingsRow - 4}, pass, nullptr,
                                  UiCommand::PassJustNow);
        button.quiet = true;
        button.enabled = n > 0;
        if (n > 0) {
          button.tip = L"Покажет " + std::to_wstring(n) + L" " + Plural(n, L"адрес", L"адреса", L"адресов") + L", " +
                       Plural(n, L"закрытый", L"закрытых", L"закрытых") +
                       L" за последние 2 минуты: пропусти нужный или все сразу";
        }
        l.items.push_back(std::move(button));
        top += kSettingsRow;
        if (!c.passedNames.empty()) {
          // What it let through, a few minutes after: "Вернуть" closes it again (main.cpp's LetBack).
          const std::size_t p = c.passedNames.size();
          line(top);
          Item passed = Make(Kind::ValueRow, row(top),
                             L"Пропущено в " + c.passedAt + L": " + std::to_wstring(p) + L" " +
                                 Plural(p, L"адрес", L"адреса", L"адресов"));
          passed.indent = kInstrumentIndent;
          std::wstring list;
          for (const std::wstring& name : c.passedNames) {
            list += L"\n";
            list += name;
          }
          passed.tip = L"Идут дальше по правилу на доске «Маршруты»:" + list;
          l.items.push_back(std::move(passed));
          const std::wstring back = L"Вернуть";
          const float backWidth = std::ceil(TextWidth(back, body_.get())) + 28;
          Item undo = CommandItem(Kind::Button, {x1 - 20 - backWidth, top + 4, x1 - 20, top + kSettingsRow - 4}, back,
                                  nullptr, UiCommand::LetBack);
          undo.quiet = true;
          undo.tip = L"Снова закрыть, убрать из правил:" + list;
          l.items.push_back(std::move(undo));
          top += kSettingsRow;
        }
        line(top);
        Item who = CommandItem(Kind::ValueRow, row(top), L"Кто обращается к закрытому", nullptr, UiCommand::BlockedApps);
        who.detail = L"по программам";
        who.tip = L"Какие программы обращались к закрытым адресам и через какие CNAME";
        top += kSettingsRow + put(std::move(who));
      }
      y = close(card, top);
    }

    // The fine settings, folded: QUIC, IPv4, DNS - auto by default, seldom touched (the routing page's
    // till 17.10.2026). The head says what's set, to know one's own, not to read it all.
    {
      const std::size_t card = open(y);
      const float fold = FoldOf(c.routingMore, c.routingFold);
      const UiRouting& r = c.routing;
      std::wstring dns = r.remoteDns;  // "авто · Cloudflare", "Google (DoH)", "свой · …": the server's name
      constexpr std::wstring_view kAuto = L"авто · ";
      if (dns.starts_with(kAuto)) {
        dns.erase(0, kAuto.size());
      } else if (dns.starts_with(L"свой")) {
        dns = L"свой";
      }
      if (const std::size_t paren = dns.find(L" ("); paren != std::wstring::npos) {
        dns.erase(paren);
      }
      std::wstring said = L"DNS " + dns + L" · QUIC " + (r.quic == 1 ? L"выкл." : r.quic == 2 ? L"вкл." : L"авто");
      if (r.ipv4Only) {
        said += L" · только IPv4";
      }
      const std::size_t at = head(y, L"Тонкая настройка", std::move(said), 0, 2);
      l.items[at].action = ItemAction::Command;
      l.items[at].command = UiCommand::ToggleRoutingMore;
      l.items[at].fold = fold;
      float top = foot(at);
      if (fold > 0) {
        const std::size_t first = l.items.size();
        top = FoldBody(l, first, top, FineRows(l, c, x0, x1, top), fold);
      }
      y = close(card, top);
    }

    // The app: its version and the sing-box inside, whether there's newer; how it starts and updates,
    // the data folder. A version out, its button in the head.
    // (A card of rows - the versions, "Проверить сейчас" - from 30.10.2026 till 11.11.2026: the check is
    // the head's ↻ now.)
    {
      const std::size_t card = open(y);
      const bool checking = c.update == UiUpdate::Checking || c.update == UiUpdate::Downloading;
      const bool boxLatest = !c.boxIsLatest || *c.boxIsLatest;
      std::wstring state;
      switch (c.update) {
        case UiUpdate::Checking: state = L"проверяю…"; break;
        case UiUpdate::Downloading:
          state = c.updateInstalling        ? L"ставлю " + c.updateVersion + L"…"
                  : c.updateProgress >= 0 ? L"скачиваю " + std::to_wstring(c.updateProgress) + L"%"
                                          : L"скачиваю " + c.updateVersion + L"…";
          break;
        case UiUpdate::Available: state = L"вышла " + c.updateVersion; break;
        case UiUpdate::Waiting: state = L"вышла " + c.updateVersion + L", скачана"; break;
        case UiUpdate::Failed: state = L"проверить не вышло"; break;
        case UiUpdate::Off: state = L"обновления не в этой сборке"; break;
        case UiUpdate::UpToDate:
          // When: "обновлений нет" alone read as now, said hours before a release (11.11.2026).
          state = !boxLatest                   ? L"вышел sing-box " + c.boxLatest
                  : c.updateCheckedAt.empty() ? std::wstring(L"обновлений нет")
                                              : L"обновлений нет, проверено в " + c.updateCheckedAt;
          break;
        case UiUpdate::Idle: break;
      }
      std::wstring said = L"sing-box " + c.boxVersion + (state.empty() ? std::wstring() : L" · " + state);
      const int lamp = c.update == UiUpdate::UpToDate && boxLatest ? 1 : 0;
      const std::size_t at = head(y, L"Sovereign " + c.version, std::move(said), lamp, 4);
      float top = foot(at);
      headHint(at, L"Версия клиента и sing-box внутри него. Новые версии ищутся сами, раз в 12 часов, если в "
                   L"«Обновлениях» не выбрано «не проверять»; «Проверить» — поискать сейчас, «Обновить» — "
                   L"поставить вышедшую.");
      // Seen always, a button with its word: the shy ↻ of the other heads, nobody found (11.11.2026).
      // A version out, the same place is the update's own button, white: the wide bar under the card
      // and the title's arrow said it twice (11.11.2026).
      {
        const D2D1_RECT_F h = l.items[at].rect;
        const float cy = (h.top + h.bottom) / 2;
        const D2D1_RECT_F box{h.right - 16 - kHeadButtonWidth, cy - 14, h.right - 16, cy + 14};
        if (c.update == UiUpdate::Available || c.update == UiUpdate::Waiting) {
          Item install = CommandItem(Kind::AccentButton, box, L"Обновить", kGlyphDownload, UiCommand::InstallUpdate);
          install.tip = c.update == UiUpdate::Waiting
                            ? L"Поставить " + c.updateVersion + L" сейчас. VPN пропадёт примерно на полминуты"
                            : L"Скачать и поставить " + c.updateVersion + L". VPN пропадёт примерно на полминуты";
          l.items.push_back(std::move(install));
        } else {
          const bool downloading = c.update == UiUpdate::Downloading;
          Item check = CommandItem(Kind::Button, box,
                                   downloading ? L"Обновляю…" : checking ? L"Проверяю…" : L"Проверить", nullptr,
                                   UiCommand::CheckUpdate);
          check.quiet = true;
          check.enabled = !checking && c.update != UiUpdate::Off;
          check.tip = L"Поискать новую версию сейчас";
          l.items.push_back(std::move(check));
        }
      }

      line(top);
      Item startup = CommandItem(Kind::ValueRow, row(top), L"Автозапуск", nullptr, UiCommand::ChooseStartup);
      startup.detail = !c.autostart ? L"выключен" : c.connectAtStartup ? L"с подключением" : L"без подключения";
      startup.hinted = true;
      const float startupGrown = put(std::move(startup));
      HintAfter(l, L"Автозапуск", body_.get(), in, x1 - 8, top + kSettingsRow / 2,
                L"При входе в Windows клиент запускается в области уведомлений, без окна. «С подключением» — "
                L"подключается сам, если был включён; «без подключения» — ждёт, пока включишь.");
      top += kSettingsRow + startupGrown;

      line(top);
      Item updates = CommandItem(Kind::ValueRow, row(top), L"Обновления", nullptr, UiCommand::ChooseUpdates);
      updates.detail = c.updateMode == UpdateMode::Off       ? L"не проверять"
                       : c.updateMode == UpdateMode::Install ? L"ставить сами"
                                                             : L"только проверять";
      updates.hinted = true;
      const float updatesGrown = put(std::move(updates));
      HintAfter(l, L"Обновления", body_.get(), in, x1 - 8, top + kSettingsRow / 2,
                L"«Только проверять» — раз в 12 часов; новая версия появится значком в заголовке окна, "
                L"обновляешь нажатием на него. «Ставить сами» — новая версия скачивается сама и "
                L"ставится без вопросов, когда компьютер свободен: никто не трогал мышь и клавиатуру столько минут, сколько выбрано строкой ниже, "
                L"ничего не открыто на весь экран, ничего не качается. VPN пропадает примерно на полминуты. «Не "
                L"проверять» — только ↻ у названия Sovereign выше.");
      top += kSettingsRow + updatesGrown;

      if (c.updateMode == UpdateMode::Install) {  // how long the computer must be left alone before it installs
        line(top);
        Item quiet = CommandItem(Kind::ValueRow, row(top), L"Ставить после тишины", nullptr, UiCommand::ChooseUpdateQuiet);
        quiet.detail = std::to_wstring(c.updateQuietMinutes) + L" мин";
        top += kSettingsRow + put(std::move(quiet));
      }

      line(top);
      Item folder = CommandItem(Kind::ValueRow, row(top), L"Папка данных", kGlyphFolder, UiCommand::OpenFolder);
      folder.detail = L"настройки и конфиги";
      folder.tip = L"Открыть папку данных: настройки, конфиги, их история";
      top += kSettingsRow + put(std::move(folder));
      y = close(card, top);
    }
    if (c.update == UiUpdate::Failed || (c.update == UiUpdate::Available && !c.updateError.empty())) {
      y = Paragraph(l, Kind::ErrorText, UpdateStatus(c), x0, x1, y) + kSettingsGap;
    }
    return y;
  }

  /* ---- drawing ---- */

  // `hot`: the way (rail) whose route the pointer is on - a chip, a heading or a "+" of it (Draw);
  // the routing scheme's lines lit for it, the others put back. -1: none.
  void DrawItem(const Canvas& k, const Item& it, const UiContent& c, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    const D2D1_COLOR_F primary = FromColorRef(ui::kPrimaryText);
    const D2D1_COLOR_F secondary = FromColorRef(ui::kSecondaryText);
    const D2D1_COLOR_F lit = FromColorRef(ui::kLit);
    const float hoverAlpha = pressed ? 0.04f : (hovered ? 0.08f : 0.0f);
    switch (it.kind) {
      case Kind::Card:
        // Glass: a breath of white over what's behind, a brighter edge; else the card's own colour.
        if (!it.quiet) {
          k.Round(r, kRadius, it.glass ? Rgb(255, 255, 255, 0.035f) : FromColorRef(ui::kCardColor));
          if (hoverAlpha > 0 && it.action != ItemAction::None) {  // a card that is a link
            k.Round(r, kRadius, Rgb(255, 255, 255, hoverAlpha));
          }
          k.Outline(r, kRadius, Rgb(255, 255, 255, it.glass ? 0.10f : 0.08f));
          // An instrument's verdict: its card's edge in the colour of its fate - red, it went off; amber, worth fixing.
          if (it.verdict >= 2) {
            k.Outline(r, kRadius, it.verdict >= 3 ? FromColorRef(ui::kDanger, 0.6f) : FromColorRef(ui::kWarning, 0.5f), 1.25f);
          }
        }
        if (it.rail >= 0) {  // a way's card: the settings' card, and the rail down its edge - no tint, no other geometry
          // An empty way: the same rail, short and at full tone (a pale one fails 3:1), at the head's middle.
          const float mid = (r.top + r.bottom) / 2;
          const float top = it.quiet ? mid - kRailQuiet / 2 : r.top + 8;
          const float bottom = it.quiet ? mid + kRailQuiet / 2 : r.bottom - 8;
          k.Round({r.left, top, r.left + 3, bottom}, 1.5f, FromColorRef(BucketColor(it.rail), kRailAlpha));
        }
        break;
      case Kind::Divider:  // a groove cut in the panel: dark, and the lit lip under it
        k.Line({r.left, r.top + 0.5f}, {r.right, r.top + 0.5f}, Rgb(0, 0, 0, 0.35f));
        k.Line({r.left, r.top + 1.5f}, {r.right, r.top + 1.5f}, Rgb(255, 255, 255, 0.05f));
        break;
      case Kind::Title:  // the page's name in the title bar; a configuration's renames on a click
        k.Text(it.text, heading_.get(), r, hovered && it.action != ItemAction::None ? lit : primary);
        break;
      case Kind::Heading: k.Text(it.text, heading_.get(), r, primary); break;
      case Kind::Lead: k.Text(it.text, lead_.get(), r, primary); break;
      case Kind::ModePill: {
        // A key in the sentence: the card's plate, a hairline brightened while open, the
        // chevron turned over then (the mockup's .pill).
        const float radius = (r.bottom - r.top) / 2;
        k.Round(r, radius, Rgb(255, 255, 255, hovered || it.checked ? 0.10f : 0.06f));
        if (pressed) {
          k.Round(r, radius, Rgb(0, 0, 0, 0.12f));
        }
        k.Outline(r, radius, Rgb(255, 255, 255, it.checked ? 0.38f : hovered ? 0.30f : 0.20f));
        k.Text(it.text, lead_.get(), {r.left + kPillSide, r.top, r.right - kPillSide - kChevron - 8, r.bottom}, primary);
        const float cx = r.right - kPillSide - kChevron / 2;
        const float cy = (r.top + r.bottom) / 2;
        const float turn = it.checked ? -1.0f : 1.0f;
        Polyline(k, {{cx - 3.5f, cy - 1.75f * turn}, {cx, cy + 1.75f * turn}, {cx + 3.5f, cy - 1.75f * turn}}, primary,
                 1.75f);
        break;
      }
      case Kind::Route: DrawRoute(k, it); break;
      case Kind::GhostAdd: {
        // A dashed place for one more: its way's colour round it under the pointer.
        const COLORREF tone = BucketColor(it.rail);
        const D2D1_RECT_F in{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f};
        if (hovered || pressed) {
          k.Round(r, kRadius, FromColorRef(tone, pressed ? 0.05f : 0.08f));
        }
        k.t->DrawRoundedRectangle(D2D1::RoundedRect(in, kRadius - 0.5f, kRadius - 0.5f),
                                  k.Color(hovered ? FromColorRef(tone, 0.9f) : Rgb(255, 255, 255, 0.2f)), 1,
                                  dashed_.get());
        const D2D1_COLOR_F ink = hovered ? primary : secondary;
        if (it.text.empty()) {  // the square: a "+" of two strokes on its middle
          const float cx = (r.left + r.right) / 2;
          const float cy = (r.top + r.bottom) / 2;
          Polyline(k, {{cx - 5, cy}, {cx + 5, cy}}, ink, 1.5f);
          Polyline(k, {{cx, cy - 5}, {cx, cy + 5}}, ink, 1.5f);
        } else {
          k.Text(it.text, wrap_.get(), {r.left + 12, r.top + 6, r.right - 12, r.bottom - 6}, ink);
        }
        break;
      }
      case Kind::Menu: {
        // Raised over the page: a soft shadow under it, its own lighter plate, a hairline.
        for (int n = 1; n <= 4; ++n) {
          const float grow = static_cast<float>(n) * 4;
          k.Round({r.left - grow / 2, r.top + grow / 2, r.right + grow / 2, r.bottom + grow},
                  kRadius + grow / 2, Rgb(0, 0, 0, 0.14f));
        }
        k.Round(r, kRadius, FromColorRef(ui::kRaisedColor));
        k.Outline(r, kRadius, Rgb(255, 255, 255, 0.13f));
        break;
      }
      case Kind::MenuItem: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, pressed ? 0.04f : 0.06f));
        }
        const float textLeft = r.left + kMenuDot;
        const float textRight = r.right - kMenuCheck;
        k.t->FillEllipse(D2D1::Ellipse({r.left + 8 + 4, r.top + 6 + 10}, 4, 4), k.Color(FromColorRef(BucketColor(it.rail))));
        k.Text(it.text, strong_.get(), {textLeft, r.top + 6, textRight, r.top + 6 + 20}, primary);
        k.Text(it.detail, captionWrap_.get(), {textLeft, r.top + 6 + 20 + 2, textRight, r.bottom - 6}, secondary);
        if (it.checked) {
          const float x = r.right - 8 - 16;
          const float y = r.top + 6 + 2;
          Polyline(k, {{x + 3, y + 8.5f}, {x + 6.2f, y + 11.5f}, {x + 13, y + 4.5f}}, lit, 1.75f);
        }
        break;
      }
      case Kind::Text: k.Text(it.text, body_.get(), r, it.warn ? FromColorRef(ui::kDanger) : primary); break;
      case Kind::Muted: {  // one that picks something (a Command): brighter under the mouse
        D2D1_RECT_F at = r;
        if (it.lamp > 0) {  // a dot before it (WARP: works, or doesn't answer)
          const COLORREF tone = it.lamp >= 3 ? ui::kDanger : it.lamp == 2 ? ui::kWarning : ui::kUpload;
          k.t->FillEllipse(D2D1::Ellipse({r.left + 3.5f, (r.top + r.bottom) / 2}, 3.5f, 3.5f), k.Color(FromColorRef(tone)));
          at.left += kMutedLamp;
        }
        k.Text(it.engraved ? Spaced(it.text) : it.text, it.engraved ? engraved_.get() : caption_.get(), at,
               it.warn ? FromColorRef(ui::kDanger)
                       : (hovered && it.action == ItemAction::Command ? primary : secondary));
        break;
      }
      case Kind::Meter: {
        // A thin track and how much of it is used: amber from three
        // quarters, red from nine tenths.
        const float share = std::clamp(static_cast<float>(it.index) / 1000, 0.0f, 1.0f);
        k.Round(r, 1.5f, Rgb(255, 255, 255, 0.08f));
        const COLORREF used = share >= 0.9f ? ui::kDanger : share >= 0.75f ? ui::kWarning : ui::kPrimaryText;
        k.Round({r.left, r.top, r.left + (r.right - r.left) * share, r.bottom}, 1.5f,
                FromColorRef(used, used == ui::kPrimaryText ? 0.75f : 1.0f));
        break;
      }
      case Kind::Wrap: k.Text(it.text, wrap_.get(), r, secondary); break;
      case Kind::Caption:  // `warn`: a caution, amber (the routing's "nothing goes through the VPN yet")
        k.Text(it.text, captionWrap_.get(), r, it.warn ? FromColorRef(ui::kWarning) : secondary);
        break;
      case Kind::ErrorText: {
        const D2D1_COLOR_F danger = FromColorRef(ui::kDanger);
        k.Text(it.glyph, glyph_.get(), {r.left, r.top, r.left + 20, r.top + 20}, danger);
        k.Text(it.text, wrap_.get(), {r.left + 28, r.top, r.right, r.bottom}, danger);
        break;
      }
      case Kind::Field:
        k.Text(it.text, body_.get(), {r.left, r.top, r.left + 110, r.bottom}, secondary);
        k.Text(it.detail, body_.get(), {r.left + 110, r.top, r.right, r.bottom}, primary);
        break;
      case Kind::ServerColumns: {
        // Over a Way's figures, right-aligned as they are; a head wider than its column reaches left.
        const auto cells = WayCells(r.right, r.top, r.bottom);
        const D2D1_COLOR_F ink = FromColorRef(ui::kSecondaryText, 0.75f);
        k.Text(L"ОТКРЫТИЕ", trailing_.get(), {cells[0].right - 90, r.top, cells[0].right, r.bottom}, ink);
        k.Text(L"ИГРЫ", trailing_.get(), {cells[1].right - 60, r.top, cells[1].right, r.bottom}, ink);
        k.Text(L"СКОРОСТЬ", trailing_.get(), {cells[2].right - 80, r.top, cells[2].right, r.bottom}, ink);
        break;
      }
      case Kind::Way: DrawWay(k, it, c, hoverAlpha); break;
      case Kind::Link:
        k.Text(it.text, trailing_.get(), r,
               it.sub == 1 && !hovered && !pressed ? FromColorRef(ui::kPrimaryText)
                                                   : FromColorRef(ui::kVpn, pressed ? 0.7f : (hovered ? 1.0f : 0.85f)));
        break;
      case Kind::Power: DrawPower(k, it, c, hovered, pressed); break;
      case Kind::Question: DrawQuestion(k, c); break;
      case Kind::Day: DrawDay(k, it, hovered, pressed); break;
      case Kind::ServerCard: {
        if (hoverAlpha > 0 && it.enabled) {
          k.Round(r, kRadius - 1, Rgb(255, 255, 255, hoverAlpha));
        }
        const float mid = (r.top + r.bottom) / 2;
        k.Text(it.text, heading_.get(), {r.left + 16, r.top + 6, r.right - 34, mid + 2},
               it.enabled ? primary : secondary);
        k.Text(it.detail, caption_.get(), {r.left + 16, mid + 2, r.right - 34, r.bottom - 6}, secondary);
        if (it.enabled) {
          k.Text(it.glyph, glyph_.get(), {r.right - 30, r.top, r.right - 6, r.bottom}, secondary);
        }
        break;
      }
      case Kind::ValueRow: {
        if (hoverAlpha > 0 && it.enabled) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        const float cy = (r.top + r.bottom) / 2;
        DrawTick(k, it, cy);
        float textLeft = r.left + 16 + it.indent;
        if (ID2D1Bitmap* logo = it.logo.empty() ? nullptr : Logo(k.t, it.logo); logo != nullptr) {
          k.t->DrawBitmap(logo, {textLeft, cy - 8, textLeft + 16, cy + 8});
          textLeft += 22;
        }
        // The value starts after the title and the "?" that follows it.
        const float valueLeft = textLeft + TextWidth(it.text, body_.get()) + 12 + (it.hinted ? kHint + 6 : 0.0f);
        // `warn`: a row that ends something (quit) - in red, its glyph too.
        const D2D1_COLOR_F ink = it.warn ? FromColorRef(ui::kDanger) : it.enabled ? primary : secondary;
        // A row that only tells (a version): its value runs to the edge, no arrow.
        const bool tells = it.action == ItemAction::None && it.glyph.empty();
        if (it.stacked) {  // no room beside the title: the value under it, from the title's left
          const float line = r.bottom - kStackLine;  // the title's line is the row's own height, kStackLine above the foot
          k.Text(it.text, body_.get(), {textLeft, r.top, valueLeft, line}, ink);
          k.Text(it.detail, trailing_.get(), {textLeft, line - 6, r.right - (tells ? 16.0f : 30.0f), r.bottom}, secondary);
        } else {
          k.Text(it.text, body_.get(), {textLeft, r.top, valueLeft, r.bottom}, ink);
          k.Text(it.detail, trailing_.get(),
                 {valueLeft, r.top, r.right - (tells ? 16.0f : 30.0f), r.bottom}, secondary);
        }
        // Its own glyph if it has one (a fold's), else "goes on to a choice".
        if (!tells) {
          DrawIcon(k, it.glyph.empty() ? std::wstring_view(kGlyphChevron) : std::wstring_view(it.glyph),
                   {r.right - 28, r.top, r.right - 8, r.bottom}, it.warn ? ink : secondary, std::max(it.fold, 0.0f));
        }
        break;
      }
      case Kind::Banner: {
        // The hand's plate, lit .08, for every banner. Checked: what's wrong (Overview's Issues) - the
        // warning's dot stands where the glyph would, the plate stays; the verdict is never a fill or a glyph.
        k.Round(r, kRadius, FromColorRef(ui::kLit, pressed ? 0.05f : (hovered ? 0.12f : 0.08f)));
        const D2D1_COLOR_F ink = it.enabled ? primary : secondary;
        if (it.checked) {
          k.t->FillEllipse(D2D1::Ellipse({r.left + 24, (r.top + r.bottom) / 2}, 4.5f, 4.5f),
                           k.Color(FromColorRef(ui::kWarning)));
        } else {
          k.Text(it.glyph, glyph_.get(), {r.left + 10, r.top, r.left + 38, r.bottom}, ink);
        }
        k.Text(it.text, body_.get(), {r.left + 44, r.top, r.right - 36, r.bottom}, ink);
        if (it.enabled) {
          k.Text(kGlyphChevron, glyph_.get(), {r.right - 34, r.top, r.right - 8, r.bottom}, ink);
        }
        break;
      }
      case Kind::Button:
      case Kind::AccentButton:
      case Kind::DangerButton: DrawButton(k, it, hovered, pressed); break;
      case Kind::IconButton:
        if (it.key) {
          DrawKey(k, it, hovered && it.enabled, pressed && it.enabled);
          break;
        }
        if (it.chrome) {
          DrawChrome(k, it, hovered && it.enabled, pressed && it.enabled);
          break;
        }
        if (hoverAlpha > 0 && it.enabled) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        DrawIcon(k, it.glyph, r, !it.enabled ? FromColorRef(ui::kSecondaryText, 0.4f) : (hovered ? primary : secondary));
        if (it.checked) {
          k.t->FillEllipse(D2D1::Ellipse({r.right - 9, r.top + 9}, 4, 4), k.Color(lit));
        }
        break;
      case Kind::Switch: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        const float right = r.right - 16;
        const D2D1_COLOR_F ink = it.enabled ? primary : secondary;
        DrawTick(k, it, (r.top + r.bottom) / 2);
        const float textLeft = r.left + 16 + it.indent;
        if (it.detail.empty()) {
          k.Text(it.text, body_.get(), {textLeft, r.top, right - 60, r.bottom}, ink);
        } else {
          k.Text(it.text, body_.get(), {textLeft, r.top + 10, right - 60, r.top + 34}, ink);
          k.Text(it.detail, caption_.get(), {textLeft, r.top + 34, right - 60, r.top + 56}, secondary);
        }
        DrawSwitch(k, {right - 40, (r.top + r.bottom) / 2 - 10, right, (r.top + r.bottom) / 2 + 10}, it.checked);
        break;
      }
      case Kind::Choice: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        const D2D1_POINT_2F dot{r.left + 22, it.stacked ? r.top + (kLatencyRow - 6) / 2 : (r.top + r.bottom) / 2};
        if (it.checked) {
          k.t->FillEllipse(D2D1::Ellipse(dot, 9, 9), k.Color(lit));
          k.t->FillEllipse(D2D1::Ellipse(dot, 4, 4), k.Color(FromColorRef(ui::kLitInk)));
        } else {
          k.t->DrawEllipse(D2D1::Ellipse(dot, 8.5f, 8.5f), k.Color(secondary), kIconStroke);
        }
        // With a detail: the name and the latency on top, the detail under them.
        // A detail that wrapped (stacked) makes the row taller: the first line stays where it is.
        const float mid = it.stacked ? r.top + (kLatencyRow - 6) / 2 : (r.top + r.bottom) / 2;
        const D2D1_RECT_F line = it.detail.empty() ? r : D2D1_RECT_F{r.left, r.top + 4, r.right, mid + 2};
        // Which sites open through the server, their marks right to left before the latency:
        // the logo in its own colours opens, faded and struck out in red answers "not from here", faded
        // doesn't answer (the head's tip says it in words).
        const bool isServer = it.command == UiCommand::SetProtocol && it.index >= 0 &&
                              static_cast<std::size_t>(it.index) < c.delays.size();
        // The failures' column went with the ways' figures (06.11.2026); a server's chevron takes the edge.
        const float delayRight = line.right - kColGap - (it.fold >= 0 ? kServerFold : 0.0f);
        const float delayLeft = delayRight - (c.delaysDetailed ? kDelayColFull : kDelayColNarrow);
        float nameRight = delayLeft;  // "48 ±3 мс" and "нет ответа" fit between here and the failures
        if (it.command == UiCommand::SetProtocol && it.index >= 0 && static_cast<std::size_t>(it.index) < c.sites.size()) {
          const auto& sites = c.sites[static_cast<std::size_t>(it.index)];
          const float iconY = std::floor((line.top + line.bottom) / 2) + 0.5f;
          for (auto site = sites.rbegin(); site != sites.rend(); ++site) {
            DrawSiteIcon(k, *site, std::floor(nameRight - kSiteIcon / 2), iconY, secondary);
            nameRight -= kSiteIcon + 6;
          }
        }
        k.Text(it.text, body_.get(), {line.left + 42, line.top, nameRight - 8, line.bottom}, primary);
        if (!it.detail.empty()) {
          k.Text(it.detail, it.stacked ? captionWrap_.get() : caption_.get(),
                 {r.left + 42, mid + 2, r.right - 12, r.bottom - 4}, secondary);
        }
        if (isServer) {
          const UiDelay& delay = c.delays[static_cast<std::size_t>(it.index)];
          const std::wstring label = DelayLabel(delay, c.delaysDetailed);
          const D2D1_RECT_F cell{delayLeft, line.top, delayRight, line.bottom};
          const int dimDigits = c.delaysDetailed ? DelayDimDigits(delay) : 0;
          if (dimDigits > 0) {
            // "81 ±11 мс": the 1 of the 81 isn't proven - dim, the verdict's colour keeps the rest.
            const auto digits = static_cast<UINT32>(std::to_wstring(delay.ms).size());
            k.TextDimmed(label, delay_.get(), cell, FromColorRef(ui::DelayColor(delay)), digits - static_cast<UINT32>(dimDigits),
                         static_cast<UINT32>(dimDigits), secondary);
          } else {
            k.Text(label, delay_.get(), cell, FromColorRef(ui::DelayColor(delay)));
          }
        }
        break;
      }
      case Kind::ProfileRow: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        const float mid = (r.top + r.bottom) / 2;
        k.Text(it.text, body_.get(), {r.left + 14, r.top + 2, r.right - 28, mid + 1}, it.checked ? primary : secondary);
        k.Text(it.detail, caption_.get(), {r.left + 14, mid + 1, r.right - 28, r.bottom - 2},
               it.warn ? FromColorRef(ui::kDanger) : secondary);
        k.Text(it.glyph, glyph_.get(), {r.right - 26, r.top, r.right - 4, r.bottom}, secondary);
        break;
      }
      case Kind::CheckRow: {
        const std::array<D2D1_COLOR_F, 5> colors = {secondary, lit, FromColorRef(ui::kUpload),
                                                    FromColorRef(ui::kWarning), FromColorRef(ui::kDanger)};
        const D2D1_COLOR_F status = colors[static_cast<std::size_t>(std::clamp(it.index, 0, 4))];
        // The title's line: the whole row, or - the result stands under it - the row less kStackLine.
        const float lineBottom = it.stacked ? r.bottom - kStackLine : r.bottom;
        const float cy = (r.top + lineBottom) / 2;
        k.t->FillEllipse(D2D1::Ellipse({r.left + 20.5f, cy}, 4.5f, 4.5f), k.Color(status));  // its ink from the line 20
        // The title and its "?" (a Hint item after it), the result in what's
        // left on the right, or under the title when that's too little.
        const float titleRight = r.left + 34 + TextWidth(it.text, body_.get());
        k.Text(it.text, body_.get(), {r.left + 34, r.top, titleRight + 2, lineBottom}, primary);
        const bool quiet = it.index <= 1;  // not run, running
        const D2D1_COLOR_F ink = quiet ? secondary : (it.index >= 3 ? status : primary);
        if (it.stacked) {
          k.Text(it.detail, trailing_.get(), {r.left + 34, lineBottom - 6, r.right - 16, r.bottom}, ink);
        } else {
          const float resultLeft = titleRight + 6 + kHint + 8;
          const float resultRight = r.right - CheckButtonRoom(it);
          k.Text(it.detail, trailing_.get(), {resultLeft, r.top, resultRight, r.bottom}, ink);
        }
        break;
      }
      case Kind::Hint: {
        const D2D1_POINT_2F center{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        const D2D1_COLOR_F ink = hovered ? primary : FromColorRef(ui::kSecondaryText, 0.85f);
        k.t->DrawEllipse(D2D1::Ellipse(center, 7.5f, 7.5f), k.Color(ink), kIconStroke);
        k.Text(L"?", hint_.get(), {center.x - 8, center.y - 8.5f, center.x + 8, center.y + 7.5f}, ink);
        break;
      }
      case Kind::Toggle: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        const float cx = (r.left + r.right) / 2;
        const float cy = (r.top + r.bottom) / 2;
        DrawSwitch(k, {cx - 20, cy - 10, cx + 20, cy + 10}, it.checked);
        break;
      }
      case Kind::Segment: {
        // Picked: a key pressed in and lit - the light's white, not the VPN's
        // blue (ui_style.h); the other a groove, its word dimmer.
        if (it.checked) {
          k.Round(r, 6, FromColorRef(ui::kLit, pressed ? 0.08f : (hovered ? 0.16f : 0.12f)));
          k.Outline(r, 6, FromColorRef(ui::kLit, 0.40f));
        } else {
          k.Round(r, 6, Rgb(0, 0, 0, 0.18f));
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
          k.Outline(r, 6, Rgb(255, 255, 255, 0.08f));
        }
        k.Text(it.text, button_.get(), r, it.checked ? primary : (hovered ? primary : secondary));
        break;
      }
      case Kind::Chip:
        if (it.checked) {
          k.Round(r, 15, FromColorRef(ui::kLit, pressed ? 0.08f : (hovered ? 0.16f : 0.12f)));
          k.Outline(r, 15, FromColorRef(ui::kLit, 0.40f));
        } else {
          k.Round(r, 15, Rgb(255, 255, 255, hoverAlpha));
          k.Outline(r, 15, Rgb(255, 255, 255, 0.10f));
        }
        k.Text(it.text, chip_.get(), r, it.checked ? primary : secondary);
        break;
      case Kind::Instrument: {
        // A card's head (settings-instruments.html): the lamp, the name, what it does now under it; its
        // switch or fold on the right. The lamp says how it is (green works, amber worth fixing, red it went
        // off), none where there's nothing to check.
        if (hoverAlpha > 0 && it.action != ItemAction::None && it.enabled) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        const float nameMid = r.top + 18;
        if (it.lamp > 0) {
          const COLORREF tone = it.lamp >= 3 ? ui::kDanger : it.lamp == 2 ? ui::kWarning : ui::kUpload;
          k.t->FillEllipse(D2D1::Ellipse({r.left + kInstrumentLamp, nameMid}, 4, 4), k.Color(FromColorRef(tone)));
        }
        const float textLeft = r.left + kInstrumentText;
        const float textRight = r.right - InstrumentControl(it);
        k.Text(it.text, heading_.get(), {textLeft, r.top + 6, textRight, r.top + 30}, primary);
        if (!it.detail.empty()) {
          k.Text(it.detail, captionWrap_.get(), {textLeft, r.top + 30, textRight, r.bottom}, it.loud ? primary : secondary);
        }
        const float cy = (r.top + r.bottom) / 2;
        if (it.sub == 1) {
          const float right = r.right - 16;
          const D2D1_RECT_F track{right - 40, cy - 10, right, cy + 10};
          DrawSwitch(k, track, it.checked);
          if (it.lid) {
            DrawLid(k, track);
          } else if (it.lifted) {  // the lid up: only its hinge, over the switch
            k.Round({track.left - 2, track.top - 9, track.right + 2, track.top - 6}, 1.5f, secondary);
          }
        } else if (it.sub == 2) {
          DrawIcon(k, kGlyphChevronDown, {r.right - 36, cy - 10, r.right - 8, cy + 10}, secondary, std::max(it.fold, 0.0f));
        }
        break;
      }
      case Kind::Dot: {
        // A category as a thing with its state in it: a hairline rectangle, a dot - filled white when on.
        k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        k.Outline(r, 6, Rgb(255, 255, 255, it.checked ? 0.16f : 0.08f));
        const D2D1_POINT_2F dot{r.left + 12, (r.top + r.bottom) / 2};
        if (it.checked) {
          k.t->FillEllipse(D2D1::Ellipse(dot, 3, 3), k.Color(lit));
        } else {
          k.t->DrawEllipse(D2D1::Ellipse(dot, 2.375f, 2.375f), k.Color(secondary), 1.25f);
        }
        k.Text(it.text, caption_.get(), {r.left + 21, r.top, r.right - 6, r.bottom}, it.checked ? primary : secondary);
        break;
      }
      case Kind::Trace: DrawTrace(k, it); break;
      case Kind::Readout: {
        // A count read off: the number large and right-aligned, its unit under it.
        if (hoverAlpha > 0 && it.action != ItemAction::None) {
          k.Round(Inflate(r, 4), 6, Rgb(255, 255, 255, hoverAlpha));
        }
        const float numberWidth = std::ceil(TextWidth(it.text, state_.get())) + 2;
        k.Text(it.text, state_.get(), {r.right - numberWidth, r.top, r.right, r.top + 26}, primary);
        k.Text(it.detail, trailing_.get(), {r.left, r.top + 26, r.right, r.bottom}, secondary);
        break;
      }
      case Kind::Pill: {
        // A window cut into the panel: dark, a hairline, a shadow along its top edge.
        if (it.rail >= 0) {  // a way's label: tinted in its colour, a dot before the name
          const COLORREF tone = BucketColor(it.rail);
          const float cy = (r.top + r.bottom) / 2;
          k.Round(r, (r.bottom - r.top) / 2, FromColorRef(tone, 0.12f));
          k.Outline(r, (r.bottom - r.top) / 2, FromColorRef(tone, 0.40f));
          k.t->FillEllipse(D2D1::Ellipse({r.left + 11, cy}, 3, 3), k.Color(FromColorRef(tone)));
          k.Text(it.text, caption_.get(), {r.left + 22, r.top, r.right, r.bottom}, primary);
          break;
        }
        // `warn`: the verdict's lamp - glow and rim in the warning's hue, the word stays white.
        const D2D1_COLOR_F ink = it.warn ? primary : secondary;
        k.Round(r, 5, Rgb(0, 0, 0, 0.30f));
        if (it.warn) {
          k.Round(r, 5, FromColorRef(ui::kWarning, 0.12f));
        }
        k.Outline(r, 5, it.warn ? FromColorRef(ui::kWarning, 0.40f) : Rgb(255, 255, 255, 0.08f));
        k.Text(it.text, caption_.get(), {r.left + kPillPad, r.top, r.right, r.bottom}, ink);
        break;
      }
      case Kind::AppTitle: {
        const float cy = (r.top + r.bottom) / 2;
        float left = r.left;
        if (ID2D1Bitmap* icon = ProgramIcon(k.t, it.text2); icon != nullptr) {
          k.t->DrawBitmap(icon, {left, cy - 8, left + 16, cy + 8});
          left += 24;
        }
        k.Text(it.text, caption_.get(), {left, r.top, r.right, r.bottom}, secondary);
        break;
      }
      case Kind::AppRow: {
        const float cy = (r.top + r.bottom) / 2;
        if (ID2D1Bitmap* icon = ProgramIcon(k.t, it.detail); icon != nullptr) {
          k.t->DrawBitmap(icon, {r.left + 12, cy - kAppIcon / 2, r.left + 12 + kAppIcon, cy + kAppIcon / 2});
        } else {
          k.Text(it.glyph, glyph_.get(), {r.left + 8, r.top, r.left + 36, r.bottom}, secondary);
        }
        k.Text(it.text, body_.get(), {r.left + 48, r.top, r.right, r.bottom}, primary);
        break;
      }
      case Kind::Tag: {
        // A thing on a way: a plate a breath lighter than the page, a hairline, its icon 8 in
        // (a program's, a service's logo, else a globe: a site) and its name 8 after it.
        k.Round(r, kRadius, Rgb(255, 255, 255, 0.045f + (pressed ? 0.02f : (hovered ? 0.035f : 0.0f))));
        k.Outline(r, kRadius, Rgb(255, 255, 255, hovered ? 0.2f : 0.13f));
        const float iconY = it.stacked ? r.top + kTag / 2 : (r.top + r.bottom) / 2;
        const float iconLeft = r.left + 8;
        if (ID2D1Bitmap* icon = it.logo.empty() ? ProgramIcon(k.t, it.text2) : Logo(k.t, it.logo); icon != nullptr) {
          k.t->DrawBitmap(icon, {iconLeft, iconY - 8, iconLeft + 16, iconY + 8});
        } else {  // a globe of lines: its rim, a meridian, the equator
          const D2D1_POINT_2F mid{iconLeft + 8, iconY};
          const D2D1_COLOR_F ink = FromColorRef(ui::kSecondaryText);
          k.t->DrawEllipse(D2D1::Ellipse(mid, 6.4f, 6.4f), k.Color(ink), 1.2f);
          k.t->DrawEllipse(D2D1::Ellipse(mid, 2.8f, 6.4f), k.Color(ink), 1.2f);
          k.t->DrawLine({mid.x - 6.4f, mid.y}, {mid.x + 6.4f, mid.y}, k.Color(ink), 1.2f);
        }
        const float left = r.left + 32;
        const float right = r.right - 12;
        // A thing that goes here anyway (the rest's way, UiChip::idle) is set quieter: named, no exception.
        const D2D1_COLOR_F ink = it.quiet ? secondary : primary;
        if (it.stacked) {  // wider than the page: the text wraps, the detail stands under it (ChipWrappedHeight)
          const float room = right - left;
          const float detailHeight = it.detail.empty() ? 0.0f : TextHeight(it.detail, captionWrap_.get(), room);
          k.Text(it.text, wrap_.get(), {left, r.top + 6, right, r.bottom - 6 - detailHeight}, ink);
          if (!it.detail.empty()) {
            k.Text(it.detail, captionWrap_.get(), {left, r.bottom - 6 - detailHeight, right, r.bottom - 6}, secondary);
          }
          break;
        }
        k.Text(it.text, body_.get(), {left, r.top, right + 2, r.bottom}, ink);  // 2 of slack: see Board
        if (!it.detail.empty()) {
          const float after = left + std::ceil(TextWidth(it.text, body_.get())) + 8;
          k.Text(it.detail, caption_.get(), {after, r.top, right + 2, r.bottom}, secondary);
        }
        break;
      }
      case Kind::Find: {
        // The log's search: a dark well, a magnifier, what's sought or what to seek in it.
        k.Round(r, 6, Rgb(0, 0, 0, 0.18f));
        k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        k.Outline(r, 6, Rgb(255, 255, 255, it.text.empty() ? 0.08f : 0.22f));
        const float cy = (r.top + r.bottom) / 2;
        k.Text(kGlyphSearch, glyph_.get(), {r.left + 6, cy - 10, r.left + 30, cy + 10}, secondary);
        // (text2: what to seek in, where it isn't the log - the routing page's address)
        const std::wstring_view placeholder = it.text2.empty() ? std::wstring_view(L"Поиск: сайт, сервер, ошибка") : std::wstring_view(it.text2);
        k.Text(it.text.empty() ? placeholder : std::wstring_view(it.text),
               body_.get(), {r.left + 36, cy - 10, r.right - 8, cy + 10}, it.text.empty() ? secondary : primary);
        break;
      }
      case Kind::LogBox: break;  // DrawLog
    }
  }

  // A tip over everything, as AIControls has it (24.10.2026, he: "аккуратненько,
  // как в айконтролс"): a dark box hugging its words, a nose pointing at the
  // middle of what it's about - over it, under it where the window ends
  // above (the title bar's keys), inside the window. A "?"'s paragraphs wrap
  // and get more room round them.
  void DrawTip(const Canvas& k, const Item& it, D2D1_SIZE_F size) const {
    const std::wstring_view text = TipText(it);
    if (text.empty()) {
      return;
    }
    constexpr float kNose = 6;     // the triangle's height, and half its base
    constexpr float kTipGap = 6;   // between its point and the item
    constexpr float kTipRound = 6;
    constexpr float kEdge = 6;     // the window's edge it keeps off
    // A "?"'s paragraphs narrow enough to read; a key's line rather unbroken.
    const float maxWidth = std::min(it.kind == Kind::Hint ? 300.0f : 360.0f, size.width - 2 * kEdge - 20);
    const auto m = Metrics(text, tip_.get(), maxWidth, 10000);
    if (!m) {
      return;
    }
    const bool wrapped = m->lineCount > 1;
    const float padX = wrapped ? 10.0f : 8.0f;
    const float padY = wrapped ? 8.0f : 4.0f;
    const float textWidth = std::min(maxWidth, std::ceil(m->widthIncludingTrailingWhitespace) + 1);
    const float w = textWidth + 2 * padX;
    const float h = std::ceil(m->height) + 2 * padY;

    // What it points at: a key's pill, a title's words - not the cell, the row.
    D2D1_RECT_F at = it.chrome ? KeyPill(it) : it.rect;
    if (it.kind == Kind::Title) {
      at.right = std::min(at.right, at.left + TextWidth(it.text, heading_.get()));
    }
    const float middle = (at.left + at.right) / 2;
    const float left = std::clamp(middle - w / 2, kEdge, std::max(kEdge, size.width - kEdge - w));
    const float above = at.top - kTipGap - kNose - kEdge;
    const float under = size.height - kEdge - (at.bottom + kTipGap + kNose);
    const bool below = h > above && (h <= under || under > above);
    const float top = below ? std::min(at.bottom + kTipGap + kNose, std::max(kEdge, size.height - kEdge - h))
                            : std::max(kEdge, at.top - kTipGap - kNose - h);

    const D2D1_COLOR_F fill = FromColorRef(ui::kRaisedColor);
    k.Round({left, top, left + w, top + h}, kTipRound, fill);
    const float noseX = std::clamp(middle, left + kTipRound + kNose, left + w - kTipRound - kNose);
    const float baseY = below ? top + 0.5f : top + h - 0.5f;  // a hair into the box: no seam
    const float pointY = below ? top - kNose : top + h + kNose;
    wil::com_ptr<ID2D1PathGeometry> nose;
    wil::com_ptr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(d2d_->CreatePathGeometry(nose.put())) && SUCCEEDED(nose->Open(sink.put()))) {
      sink->BeginFigure({noseX - kNose, baseY}, D2D1_FIGURE_BEGIN_FILLED);
      sink->AddLine({noseX, pointY});
      sink->AddLine({noseX + kNose, baseY});
      sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      if (SUCCEEDED(sink->Close())) {
        k.t->FillGeometry(nose.get(), k.Color(fill));
      }
    }
    k.Text(text, tip_.get(), {left + padX, top + padY, left + padX + textWidth, top + h - padY + 2},
           FromColorRef(ui::kPrimaryText));
  }

  static D2D1_COLOR_F ToneColor(UiQuestion::Tone tone, float alpha = 1) {
    switch (tone) {
      case UiQuestion::Tone::Warning: return FromColorRef(ui::kWarning, alpha);
      case UiQuestion::Tone::Danger: return FromColorRef(ui::kDanger, alpha);
      case UiQuestion::Tone::Accent: break;
    }
    return FromColorRef(ui::kLit, alpha);
  }

  // MainWindow::Ask's card (AskBox): a soft shadow under it, lighter than
  // the cards of the page, its edge catching the light at the top, the
  // buttons' band darker below a line.
  void DrawQuestion(const Canvas& k, const UiContent& c) const {
    if (!c.question) {
      return;
    }
    const UiQuestion& q = *c.question;
    const D2D1_SIZE_F size = k.t->GetSize();
    const AskBoxes b = AskBox(q, size.width, size.height);
    const D2D1_RECT_F r = b.card;
    constexpr float kAskRadius = 12;
    for (int i = 6; i >= 1; --i) {
      const auto f = static_cast<float>(i);
      k.Round({r.left - f * 2.5f, r.top - f * 1.5f, r.right + f * 2.5f, r.bottom + f * 4}, kAskRadius + f * 2.5f,
              Rgb(0, 0, 0, 0.07f));
    }
    k.Round(r, kAskRadius, FromColorRef(ui::kCardColor));  // the band's
    k.Round({r.left, r.top, r.right, b.band}, kAskRadius, FromColorRef(ui::kDialogColor));
    k.Fill({r.left, b.band - kAskRadius, r.right, b.band}, FromColorRef(ui::kDialogColor));
    k.Line({r.left, b.band + 0.5f}, {r.right, b.band + 0.5f}, Rgb(255, 255, 255, 0.06f));
    k.Outline(r, kAskRadius, Rgb(255, 255, 255, 0.09f));
    k.Line({r.left + kAskRadius, r.top + 1.5f}, {r.right - kAskRadius, r.top + 1.5f}, Rgb(255, 255, 255, 0.07f));

    const D2D1_POINT_2F mid{(b.circle.left + b.circle.right) / 2, (b.circle.top + b.circle.bottom) / 2};
    const float radius = (b.circle.right - b.circle.left) / 2;
    k.t->FillEllipse(D2D1::Ellipse(mid, radius, radius), k.Color(ToneColor(q.tone, 0.15f)));
    k.t->DrawEllipse(D2D1::Ellipse(mid, radius - 0.5f, radius - 0.5f), k.Color(ToneColor(q.tone, 0.28f)), 1);
    k.Text(q.glyph.empty() ? std::wstring_view(kGlyphWarning) : std::wstring_view(q.glyph), glyph_.get(),
           b.circle, ToneColor(q.tone));
    k.Text(q.title, ask_.get(), b.title, FromColorRef(ui::kPrimaryText));
    k.Text(q.text, wrap_.get(), b.text, FromColorRef(ui::kSecondaryText));
    if (q.calendar) {
      // The month and the year between their steps (the keys are items, DrawDay);
      // the weekdays over the grid, a hairline under them.
      const UiCalendar& cal = *q.calendar;
      const CalendarKeys keys = CalendarHead(b.calHead);
      k.Text(kMonthNames.at(static_cast<std::size_t>(std::clamp(cal.month, 1, 12) - 1)), center_.get(), keys.monthName,
             FromColorRef(ui::kPrimaryText));
      k.Text(std::to_wstring(cal.year), center_.get(), keys.yearName, FromColorRef(ui::kPrimaryText));
      const float width = (b.calGrid.right - b.calGrid.left) / 7;
      for (std::size_t i = 0; i < kWeekdayNames.size(); ++i) {
        const float left = b.calGrid.left + width * static_cast<float>(i);
        k.Text(kWeekdayNames[i], hint_.get(), {left, b.calWeek.top, left + width, b.calWeek.bottom},
               FromColorRef(ui::kSecondaryText, i >= 5 ? 0.6f : 1.0f));
      }
      const float line = std::floor(b.calGrid.top) - 1.5f;
      k.Line({b.calGrid.left, line}, {b.calGrid.right, line}, Rgb(255, 255, 255, 0.07f));
    }
  }

  // A calendar key on a question's card: a step (a chevron) or a day.
  void DrawDay(const Canvas& k, const Item& it, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    if (!it.glyph.empty()) {
      if (hovered || pressed) {
        k.Round(r, 6, Rgb(255, 255, 255, pressed ? 0.05f : 0.10f));
      }
      const float cx = std::floor((r.left + r.right) / 2) + 0.5f;
      const float cy = std::floor((r.top + r.bottom) / 2) + 0.5f;
      const float direction = static_cast<float>(it.sub);
      const D2D1_COLOR_F ink = FromColorRef(hovered ? ui::kPrimaryText : ui::kSecondaryText);
      k.t->DrawLine({cx - direction * 2, cy - 4}, {cx + direction * 2, cy}, k.Color(ink), 1.3f, roundCaps_.get());
      k.t->DrawLine({cx + direction * 2, cy}, {cx - direction * 2, cy + 4}, k.Color(ink), 1.3f, roundCaps_.get());
      return;
    }
    D2D1_COLOR_F ink = FromColorRef(ui::kPrimaryText);
    if (it.checked) {
      k.Round(r, 6, FromColorRef(ui::kLit, pressed ? 0.8f : 1.0f));
      ink = FromColorRef(ui::kLitInk);
    } else {
      if (hovered || pressed) {
        k.Round(r, 6, Rgb(255, 255, 255, pressed ? 0.05f : 0.10f));
      }
      if (it.sub == 1) {  // today
        k.Outline(r, 6, FromColorRef(ui::kLit, 0.45f));
      }
    }
    k.Text(it.text, button_.get(), r, ink);
  }

  // The log's visible rows in `box`: the time, a dot of how it went, what
  // happened and with what under it, how many times on the right; selected
  // rows highlighted, a thumb when there is more to scroll.
  void DrawLog(const Canvas& k, D2D1_RECT_F box, const LogView& log) const {
    const D2D1_COLOR_F primary = Rgb(222, 226, 234);
    const D2D1_COLOR_F secondary = FromColorRef(ui::kSecondaryText);
    if (log.shown.empty()) {
      const wchar_t* empty = log.lines.empty()    ? L"Пока пусто: служба ещё ничего не написала."
                             : !log.search.empty() ? L"Ничего не нашлось."
                             : log.filter == 1     ? L"Проблем нет."
                                                   : L"Здесь пока ничего.";
      k.Text(empty, button_.get(), box, secondary);
      return;
    }
    k.t->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
    const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor(log.scroll / kLogRow)));
    const float textEnd = box.right - kLogBar;  // the scrollbar's strip on the right
    for (auto i = static_cast<int>(first); i < log.Count(); ++i) {
      const float top = box.top + static_cast<float>(i) * kLogRow - log.scroll;
      if (top > box.bottom) {
        break;
      }
      const D2D1_RECT_F row{box.left, top, box.right, top + kLogRow};
      if (log.Selected(i)) {
        k.Fill(row, FromColorRef(ui::kLit, 0.12f));
      } else if (i > 0 && !log.Selected(i - 1)) {
        k.t->DrawLine({box.left + 12, top}, {textEnd, top}, k.Color(Rgb(255, 255, 255, 0.05f)), 1.0f);
      }
      const LogEvent& event = log.Event(i);
      const bool two = !event.detail.empty();
      const float mid = two ? top + 15 : top + kLogRow / 2;
      const float x = box.left + 12;
      k.Text(event.time.empty() ? std::wstring_view(L"--:--") : std::wstring_view(event.time), caption_.get(),
             {x, mid - 10, x + 40, mid + 10}, secondary);
      const COLORREF tone = event.tone == LogTone::Bad    ? ui::kDanger
                            : event.tone == LogTone::Warn ? ui::kWarning
                            : event.tone == LogTone::Ok   ? ui::kUpload
                                                          : ui::kSecondaryText;
      k.t->FillEllipse(D2D1::Ellipse({x + 48, mid}, 4, 4),
                       k.Color(FromColorRef(tone, event.tone == LogTone::Neutral ? 0.6f : 1.0f)));
      const float left = x + 60;
      float end = textEnd;
      if (const int times = log.Repeats(i); times > 1) {
        const std::wstring count = std::format(L"×{}", times);
        const float w = TextWidth(count, chip_.get()) + 14;
        const D2D1_RECT_F badge{end - w, mid - 9, end, mid + 9};
        k.Round(badge, 9, Rgb(255, 255, 255, 0.08f));
        k.Text(count, chip_.get(), badge, secondary);
        end = badge.left - 8;
      }
      k.Text(event.title.empty() ? std::wstring_view(log.Line(i)) : std::wstring_view(event.title), body_.get(),
             {left, mid - 10, end, mid + 10}, primary);
      if (two) {
        k.Text(event.detail, caption_.get(), {left, top + 25, textEnd, top + 41}, secondary);
      }
    }
    if (const auto thumb = LogThumb(box, log)) {
      // Thin at rest; wider and brighter under the pointer, to be taken.
      const float width = log.barHot ? 8.0f : 3.0f;
      k.Round({box.right - 4 - width, thumb->top + 2, box.right - 4, thumb->bottom - 2}, width / 2,
              Rgb(255, 255, 255, log.barHot ? 0.45f : 0.22f));
    }
    if (log.pan) {
      // Where the middle button went down: the farther from it, the faster.
      const D2D1_POINT_2F at = *log.pan;
      k.t->FillEllipse(D2D1::Ellipse(at, 12, 12), k.Color(Rgb(20, 24, 32, 0.85f)));
      k.t->DrawEllipse(D2D1::Ellipse(at, 12, 12), k.Color(Rgb(255, 255, 255, 0.6f)), 1.2f);
      k.t->FillEllipse(D2D1::Ellipse(at, 2.5f, 2.5f), k.Color(Rgb(255, 255, 255, 0.85f)));
    }
    k.t->PopAxisAlignedClip();
  }

  void DrawSwitch(const Canvas& k, D2D1_RECT_F track, bool on) const {
    const float mid = (track.top + track.bottom) / 2;
    if (on) {
      k.Round(track, 10, FromColorRef(ui::kLit));
      k.t->FillEllipse(D2D1::Ellipse({track.right - 10, mid}, 6, 6), k.Color(FromColorRef(ui::kLitInk)));
    } else {
      k.Outline(track, 10, FromColorRef(ui::kSecondaryText), kIconStroke);
      k.t->FillEllipse(D2D1::Ellipse({track.left + 10, mid}, 5, 5), k.Color(FromColorRef(ui::kSecondaryText)));
    }
  }

  // A guard's lid over a switch (`track`): see-through, told by its rim, its hinge on top and a sparse
  // hatching - one click lifts it, the next turns the switch (an off kill switch has none).
  void DrawLid(const Canvas& k, D2D1_RECT_F track) const {
    const D2D1_RECT_F lid{track.left - 7, track.top - 5, track.right + 7, track.bottom + 5};
    const D2D1_COLOR_F ink = FromColorRef(ui::kSecondaryText);
    k.t->PushAxisAlignedClip(Inflate(lid, -1.25f), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float h = lid.bottom - lid.top;
    const int stripes = static_cast<int>(std::ceil((lid.right + h - lid.left) / 6));
    for (int i = 0; i < stripes; ++i) {  // CSS's 135°: stripes up to the right, every 6
      const float x = lid.left + 6.0f * static_cast<float>(i);
      k.Line({x, lid.top}, {x - h, lid.bottom}, FromColorRef(ui::kSecondaryText, 0.28f));
    }
    k.t->PopAxisAlignedClip();
    k.Outline(lid, 5, FromColorRef(ui::kSecondaryText, 0.85f), 1.25f);
    k.Round({lid.left + 5, lid.top - 1.5f, lid.right - 5, lid.top + 1.5f}, 1.5f, ink);
  }

  // The last hour of blocks (Kind::Trace): a bar a minute, its height the minute's share of the
  // busiest, the older ones paler; a baseline under them; "час назад" and "сейчас" under it.
  // text2 holds the minutes known, the last ones of the hour: less than 60 - the trace starts
  // where the counting did, under it its time (text), not an hour of nothing (11.11.2026, review).
  void DrawTrace(const Canvas& k, const Item& it) const {
    constexpr std::size_t kMinutes = 60;
    const D2D1_RECT_F r = it.rect;
    const std::size_t n = std::min(it.text2.size(), kMinutes);
    const std::size_t from = kMinutes - n;  // the first known minute's place in the hour
    const float base = r.top + kTraceBars;
    const float step = (r.right - r.left + 2) / static_cast<float>(kMinutes);
    const float start = n == kMinutes ? r.left : r.left + step * static_cast<float>(from);
    for (std::size_t i = 0; i < n; ++i) {
      const float share = static_cast<float>(it.text2[i]) / 1000.0f;
      if (share <= 0) {
        continue;
      }
      const std::size_t place = from + i;
      const float alpha = place < kMinutes / 2 ? 0.3f : (place < kMinutes * 11 / 12 ? 0.55f : 1.0f);
      const float x = r.left + step * static_cast<float>(place);
      const float top = base - std::max(1.0f, (kTraceBars - 1) * std::min(share, 1.0f));
      k.Round({x, top, x + step - 2, base}, 1, FromColorRef(ui::kDanger, 0.85f * alpha));
    }
    const D2D1_COLOR_F secondary = FromColorRef(ui::kSecondaryText);
    if (n < kMinutes) {  // the hour not counted yet: a faint dotted stretch, the line from where it began
      const int dots = static_cast<int>((start - r.left) / 6);
      for (int i = 0; i < dots; ++i) {
        const float x = r.left + 6.0f * static_cast<float>(i);
        k.Line({x, base + 0.5f}, {x + 2, base + 0.5f}, FromColorRef(ui::kSecondaryText, 0.15f));
      }
    }
    k.Line({start, base + 0.5f}, {r.right, base + 0.5f}, FromColorRef(ui::kSecondaryText, 0.3f));
    const D2D1_RECT_F under{r.left, base + 2, r.right, r.bottom};
    k.Text(L"сейчас", trailing_.get(), under, secondary);
    if (n == kMinutes || it.text.empty()) {
      k.Text(L"час назад", caption_.get(), under, secondary);
      return;
    }
    // The start's time under the start, as long as it keeps off "сейчас".
    const float room = r.right - TextWidth(L"сейчас", trailing_.get()) - 8 - TextWidth(it.text, caption_.get());
    const float left = std::max(r.left, std::min(start, room));
    k.Text(it.text, caption_.get(), {left, base + 2, r.right, r.bottom}, secondary);
  }

  // The fate's mark on the card's edge (3x12, the rail's tone): a row whose fate is known puts it
  // at its middle - while its switch is on, or always for a way (a DNS). The row sits 4 in from
  // the card's edge, so the mark stands on the edge itself. The switch stays white. A row in by
  // an instrument's name (indent) has it just before its text: on the edge it stood under no one
  // (11.11.2026, review).
  void DrawTick(const Canvas& k, const Item& it, float cy) const {
    if (it.tick < 0 || !(it.tickAlways || it.checked)) {
      return;
    }
    const float x = it.indent > 0 ? it.rect.left + it.indent + 6 : it.rect.left - 4;
    k.Round({x, cy - 6, x + 3, cy + 6}, 1.5f, FromColorRef(BucketColor(it.tick), kRailAlpha));
  }

  void DrawButton(const Canvas& k, const Item& it, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    D2D1_COLOR_F fill = Rgb(255, 255, 255, 0.06f);
    D2D1_COLOR_F ink = FromColorRef(ui::kPrimaryText);
    if (it.kind == Kind::AccentButton) {
      fill = FromColorRef(ui::kLit, pressed ? 0.8f : (hovered ? 0.9f : 1.0f));
      ink = FromColorRef(ui::kLitInk);
    } else if (it.kind == Kind::DangerButton) {
      // The block's lamp: a glow and a rim, the word stays white - no red fill (law.md, Button).
      fill = FromColorRef(ui::kDanger, pressed ? 0.08f : (hovered ? 0.18f : 0.12f));
    } else if (hovered) {
      fill = Rgb(255, 255, 255, pressed ? 0.05f : 0.10f);
    }
    if (it.action == ItemAction::Answer && it.index == 1) {
      // A question's yes is always the hand's: white. The tone burns as the lamp in the question's head.
      fill = FromColorRef(ui::kLit, pressed ? 0.8f : (hovered ? 0.9f : 1.0f));
      ink = FromColorRef(ui::kLitInk);
    }
    const bool lamp = it.kind == Kind::DangerButton && !(it.action == ItemAction::Answer && it.index == 1);
    // Quiet: a hairline, no plate - an action at hand that isn't the card's point ("Пропустить последние 7").
    const bool quiet = it.kind == Kind::Button && it.quiet;
    if (quiet) {
      fill = Rgb(255, 255, 255, hovered ? (pressed ? 0.05f : 0.08f) : 0.0f);
    }
    if (!it.enabled) {
      fill = Rgb(255, 255, 255, quiet ? 0.0f : 0.03f);
      ink = FromColorRef(ui::kSecondaryText, 0.5f);
    }
    k.Round(r, 6, fill);
    if (quiet) {
      k.Outline(r, 6, Rgb(255, 255, 255, it.enabled ? 0.16f : 0.08f));
    } else if (lamp && it.enabled) {
      k.Outline(r, 6, FromColorRef(ui::kDanger, 0.40f));
    } else if (it.kind == Kind::Button || lamp) {
      k.Outline(r, 6, Rgb(255, 255, 255, 0.08f));
    }
    // Glyph and text centered together.
    float textWidth = 0;
    if (const auto m = Metrics(it.text, button_.get(), r.right - r.left, r.bottom - r.top)) {
      textWidth = m->width;
    }
    const float glyphWidth = it.glyph.empty() ? 0.0f : 26.0f;
    const float left = (r.left + r.right - textWidth - glyphWidth) / 2;
    if (!it.glyph.empty()) {
      k.Text(it.glyph, glyph_.get(), {left, r.top, left + 18, r.bottom}, ink);
    }
    const D2D1_RECT_F textBox{left + glyphWidth, r.top, left + glyphWidth + textWidth + 2, r.bottom};
    k.Text(it.text, body_.get(), textBox, ink);
  }

  // The live traffic's dots round the switch's square `cube`: each with a
  // soft halo and a short trail behind it, fading in and out along its path;
  // sparks fade as they fly.
  void DrawLive(const Canvas& k, const LiveTraffic& live, D2D1_RECT_F cube) const {
    for (const Particle& p : live.particles) {
      D2D1_COLOR_F color = FromColorRef(ui::kVpn);
      float alpha = std::sin(std::numbers::pi_v<float> * std::clamp(p.t, 0.0f, 1.0f));
      switch (p.stream) {
        case Stream::Vpn: break;
        case Stream::Warp: color = FromColorRef(ui::kWarp); break;
        case Stream::Direct: color = FromColorRef(ui::kDirect); alpha *= 0.7f; break;
        case Stream::Blocked:
          color = FromColorRef(ui::kDanger);
          alpha = std::min(1.0f, p.t * 4);
          break;
        case Stream::Spark:
          color = FromColorRef(ui::kDanger);
          alpha = 1 - p.t / kSparkLife;
          break;
      }
      if (alpha <= 0.01f) {
        continue;
      }
      const D2D1_POINT_2F at = DotAt(p, cube);
      // The trail: where it was a moment ago, fainter.
      if (p.stream != Stream::Spark) {
        for (int i = 1; i <= 3; ++i) {
          Particle back = p;
          back.t -= 0.018f * static_cast<float>(i);
          if (back.t > 0) {
            const D2D1_POINT_2F was = DotAt(back, cube);
            color.a = alpha * 0.45f / static_cast<float>(i);
            k.t->FillEllipse(D2D1::Ellipse(was, p.size * 0.8f, p.size * 0.8f), k.Color(color));
          }
        }
      }
      color.a = alpha * 0.16f;
      k.t->FillEllipse(D2D1::Ellipse(at, p.size * 2.8f, p.size * 2.8f), k.Color(color));
      color.a = alpha * 0.95f;
      k.t->FillEllipse(D2D1::Ellipse(at, p.size, p.size), k.Color(color));
    }
  }

  // The main screen's backdrop: the dark, and a light from behind the switch
  // (`at`, its rect) - the VPN's blue while connected, a faint grey otherwise.
  // A stage for the live traffic to come.
  void DrawBackdrop(ID2D1RenderTarget* t, D2D1_SIZE_F size, D2D1_RECT_F at, bool on) const {
    const D2D1_POINT_2F center{(at.left + at.right) / 2, (at.top + at.bottom) / 2};
    const float radius = std::max(size.width, at.bottom - at.top) * 0.75f;
    const D2D1_COLOR_F light = on ? FromColorRef(ui::kVpn, 0.22f) : Rgb(255, 255, 255, 0.05f);
    const std::array<D2D1_GRADIENT_STOP, 3> stops{
        {{0, light}, {0.45f, D2D1_COLOR_F{light.r, light.g, light.b, light.a * 0.35f}}, {1, D2D1_COLOR_F{light.r, light.g, light.b, 0}}}};
    wil::com_ptr<ID2D1GradientStopCollection> collection;
    wil::com_ptr<ID2D1RadialGradientBrush> brush;
    if (SUCCEEDED(t->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put())) &&
        SUCCEEDED(t->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(center, {0, 0}, radius, radius),
                                               collection.get(), brush.put()))) {
      t->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), brush.get());
    }
  }
  // An icon button of the main screen's glass card: as flat as the title bar's
  // (a ring round the plus looked like a button sewn on - he, 22.10.2026) - the
  // glyph alone, a soft square under it with the pointer. Its text is
  // grayscale: ClearType on the translucent card fringes the glyph red and blue.
  void DrawKey(const Canvas& k, const Item& it, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    if (hovered || pressed) {
      k.Round(r, 8, Rgb(255, 255, 255, pressed ? 0.14f : 0.09f));
    }
    const D2D1_TEXT_ANTIALIAS_MODE mode = k.t->GetTextAntialiasMode();
    k.t->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    k.Text(it.glyph, glyph_.get(), r, Rgb(242, 244, 248, hovered ? 1.0f : 0.8f));
    k.t->SetTextAntialiasMode(mode);
  }
  // A title bar key's pill: inset from the bar's top and the window's edge,
  // not the whole cell up to them.
  // A key's pill; a new version's is thinner (30.10.2026, his: the button was
  // big, its top edge flush with the window's), with its number on it as wide
  // as its cell.
  static D2D1_RECT_F KeyPill(const Item& it) {
    if (it.loud) {  // the report's key on a bad day: its words beside the bubble, a quiet button's outline
      const float cy = (it.rect.top + it.rect.bottom) / 2;
      return {it.rect.left + 2, cy - 13, it.rect.right - 2, cy + 13};
    }
    if (it.checked) {
      const float cy = (it.rect.top + it.rect.bottom) / 2;
      if (it.detail.empty()) {
        const float cx = (it.rect.left + it.rect.right) / 2;
        return {cx - 16, cy - 10, cx + 16, cy + 10};
      }
      return {it.rect.left + 2, cy - 10, it.rect.right - 2, cy + 10};
    }
    return ChromePill(it.rect);
  }
  // Capitals cut into a panel breathe: a hair space between the letters (a
  // text format has no letter spacing; only a layout has).
  static std::wstring Spaced(std::wstring_view s) {
    std::wstring out;
    for (const wchar_t ch : s) {
      if (!out.empty() && ch != L' ' && out.back() != L' ') {
        out += L'\x200A';
      }
      out += ch;
    }
    return out;
  }
  // A polyline through `points`, round at its ends (the routing page's chevron, check and "+").
  void Polyline(const Canvas& k, std::initializer_list<D2D1_POINT_2F> points, D2D1_COLOR_F ink, float width) const {
    wil::com_ptr<ID2D1PathGeometry> path;
    wil::com_ptr<ID2D1GeometrySink> sink;
    if (points.size() < 2 || FAILED(d2d_->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) {
      return;
    }
    const auto* p = points.begin();
    sink->BeginFigure(*p, D2D1_FIGURE_BEGIN_HOLLOW);
    for (++p; p != points.end(); ++p) {
      sink->AddLine(*p);
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (SUCCEEDED(sink->Close())) {
      k.t->DrawGeometry(path.get(), k.Color(ink), width, roundCaps_.get());
    }
  }

  // A site's mark on a server's head, 16 px round (cx, cy): the service's own logo in its own
  // colours (icons.h ServiceLogo) - green outlines were hard to tell apart (12.11.2026, his: "реальные
  // цвета а то слабо узнаются"). Opens: as it is. Doesn't answer: faded. Answers "not from here":
  // faded and struck out in red. No logo: its label in `ink`.
  void DrawSiteIcon(const Canvas& k, const UiSite& site, float cx, float cy, D2D1_COLOR_F ink) const {
    const float x = std::round(cx);
    const float y = std::round(cy);
    const bool ok = site.state == SiteCheck::State::Ok;
    const bool blocked = site.state == SiteCheck::State::Blocked;
    if (ID2D1Bitmap* logo = site.logo.empty() ? nullptr : Logo(k.t, site.logo); logo != nullptr) {
      k.t->DrawBitmap(logo, {x - 8, y - 8, x + 8, y + 8}, ok ? 1.0f : 0.3f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      k.Text(site.label, caption_.get(), {x - 12, y - 9, x + 12, y + 9}, ok ? FromColorRef(ui::kPrimaryText) : ink);
    }
    if (blocked) {  // the slash, on a dark edge so it reads over any logo
      k.Line({x - 7, y + 7}, {x + 7, y - 7}, D2D1::ColorF(0, 0, 0, 0.6f), 3.4f);
      k.Line({x - 7, y + 7}, {x + 7, y - 7}, FromColorRef(ui::kDanger), 1.8f);
    }
  }

  // A line of the routing scheme (Kind::Route): the mode's spine from its node down to the way
  // everything else goes, or a way's branch off it to its station (the block's: a bar, the line
  // ends). Steady: the pointer does not light it.
  void DrawRoute(const Canvas& k, const Item& it) const {
    const D2D1_RECT_F r = it.rect;
    const D2D1_COLOR_F ink = FromColorRef(BucketColor(it.rail));
    const D2D1_COLOR_F hole = FromColorRef(ui::kWindowColor);
    const float x = r.left + kLineX;
    const float end = r.left + kStationX;
    const bool spine = it.sub == kRouteSpine;
    const float width = spine ? 3.0f : 2.0f;
    const float y = spine ? r.bottom : (r.top + r.bottom) / 2;  // where it turns into its way
    const float from = spine ? r.top : y - kLineTurn;
    wil::com_ptr<ID2D1PathGeometry> path;
    wil::com_ptr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(d2d_->CreatePathGeometry(path.put())) && SUCCEEDED(path->Open(sink.put()))) {
      sink->BeginFigure({x, from}, D2D1_FIGURE_BEGIN_HOLLOW);
      sink->AddLine({x, y - kLineTurn});
      sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment({x, y}, {x + kLineTurn, y}));
      sink->AddLine({end, y});
      sink->EndFigure(D2D1_FIGURE_END_OPEN);
      if (SUCCEEDED(sink->Close())) {
        k.t->DrawGeometry(path.get(), k.Color(ink), width, roundCaps_.get());
        if (spine) {  // the traffic on the main line: dots going down it (still, with the motion off)
          k.t->DrawGeometry(path.get(), k.Color(Rgb(255, 255, 255, 0.55f)), 1.5f, dots_.get());
        }
      }
    }
    if (it.sub == kRouteEnd) {  // the block: the line ends in a bar
      k.t->DrawLine({end, y - 6}, {end, y + 6}, k.Color(ink), width, roundCaps_.get());
    } else {  // a station, hollow: the window's colour inside its ring
      const float radius = spine ? 5.0f : 4.0f;
      k.t->FillEllipse(D2D1::Ellipse({end, y}, radius, radius), k.Color(hole));
      k.t->DrawEllipse(D2D1::Ellipse({end, y}, radius, radius), k.Color(ink), spine ? 3.0f : 2.0f);
    }
    if (spine) {  // the mode's node at the top: a faint ring, a disc, a hole
      const D2D1_POINT_2F node{x, r.top};
      k.t->DrawEllipse(D2D1::Ellipse(node, 10, 10), k.Color(FromColorRef(BucketColor(it.rail), 0.25f)), 1.5f);
      k.t->FillEllipse(D2D1::Ellipse(node, 6, 6), k.Color(FromColorRef(BucketColor(it.rail))));
      k.t->FillEllipse(D2D1::Ellipse(node, 2.5f, 2.5f), k.Color(hole));
    }
  }

  // The title bar's own icons, drawn as lines like the cross and the dash -
  // one stroke, round ends, 14 DIPs across, nothing filled - instead of
  // Segoe's glyphs, which differ in weight and detail from each other
  // (30.10.2026, his: "слишком пестро ... я хотел что ты сам рисунок кнопок
  // переделал"). False: not one of ours, the font's glyph is drawn.
  // A glyph in `box`: ours drawn in lines where there is one, else the font's.
  // `turn`: a fold's chevron, 0 pointing down (folded) .. 1 up (open), turned half a circle by it.
  void DrawIcon(const Canvas& k, std::wstring_view glyph, D2D1_RECT_F box, D2D1_COLOR_F ink, float turn = 0) const {
    // Through pixels' middles: at 100 % lines a pixel thick, not two halves.
    const float cx = std::floor((box.left + box.right) / 2) + 0.5f;
    const float cy = std::floor((box.top + box.bottom) / 2) + 0.5f;
    if (!DrawLineIcon(k, glyph, cx, cy, ink, turn)) {
      k.Text(glyph, glyph_.get(), box, ink);
    }
  }
  bool DrawLineIcon(const Canvas& k, std::wstring_view glyph, float cx, float cy, D2D1_COLOR_F ink,
                    float turn = 0) const {
    const auto line = [&](float x0, float y0, float x1, float y1) {
      k.t->DrawLine({cx + x0, cy + y0}, {cx + x1, cy + y1}, k.Color(ink), kIconStroke, roundCaps_.get());
    };
    const auto ring = [&](float x, float y, float rx, float ry) {
      k.t->DrawEllipse(D2D1::Ellipse({cx + x, cy + y}, rx, ry), k.Color(ink), kIconStroke);
    };
    if (glyph == kGlyphSettings) {
      // Three sliders: a line each, a ring on it where it's set.
      const std::array<std::pair<float, float>, 3> sliders{{{-4.5f, -2.5f}, {0.0f, 2.5f}, {4.5f, -0.5f}}};
      for (const auto& [y, x] : sliders) {
        line(-7, y, x - 2.9f, y);
        line(x + 2.9f, y, 7, y);
        ring(x, y, 1.9f, 1.9f);
      }
    } else if (glyph == kGlyphLog) {
      // A sheet with its lines.
      k.t->DrawRoundedRectangle(D2D1::RoundedRect({cx - 5.5f, cy - 7, cx + 5.5f, cy + 7}, 2, 2), k.Color(ink),
                                kIconStroke);
      line(-2.5f, -2.5f, 2.5f, -2.5f);
      line(-2.5f, 0.5f, 2.5f, 0.5f);
      line(-2.5f, 3.5f, 0.5f, 3.5f);
    } else if (glyph == kGlyphRoute) {
      // A way from one point to another: a step up, with a ring at its start and a dot at its end.
      ring(-5, 5, 1.8f, 1.8f);
      line(-5, 3.2f, -5, -0.5f);
      line(-5, -0.5f, 5, -0.5f);
      line(5, -0.5f, 5, -3.2f);
      k.t->FillEllipse(D2D1::Ellipse({cx + 5, cy - 5}, 1.8f, 1.8f), k.Color(ink));
    } else if (glyph == kGlyphReport) {
      // A speech bubble with a "!" in it: to say what's wrong (11.11.2026 - the bug read as "debug", not as
      // "tell us"). Its body a rounded rectangle, the tail down left, one figure.
      wil::com_ptr<ID2D1PathGeometry> bubble;
      wil::com_ptr<ID2D1GeometrySink> sink;
      if (SUCCEEDED(d2d_->CreatePathGeometry(bubble.put())) && SUCCEEDED(bubble->Open(sink.put()))) {
        constexpr float kL = -6.25f;
        constexpr float kR = 6.25f;
        constexpr float kT = -6.0f;
        constexpr float kB = 3.5f;
        constexpr float kC = 1.5f;  // the corners
        const auto p = [&](float x, float y) { return D2D1_POINT_2F{cx + x, cy + y}; };
        const auto corner = [&](float x, float y) {
          sink->AddArc(D2D1::ArcSegment(p(x, y), {kC, kC}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        };
        sink->BeginFigure(p(kL + kC, kT), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(p(kR - kC, kT));
        corner(kR, kT + kC);
        sink->AddLine(p(kR, kB - kC));
        corner(kR - kC, kB);
        sink->AddLine(p(-0.25f, kB));
        sink->AddLine(p(-3.5f, 6.25f));
        sink->AddLine(p(-3.5f, kB));
        sink->AddLine(p(kL + kC, kB));
        corner(kL, kB - kC);
        sink->AddLine(p(kL, kT + kC));
        corner(kL + kC, kT);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (SUCCEEDED(sink->Close())) {
          k.t->DrawGeometry(bubble.get(), k.Color(ink), kIconStroke, roundCaps_.get());
        }
      }
      line(0, -3.5f, 0, -0.5f);
      k.t->FillEllipse(D2D1::Ellipse({cx, cy + 1.4f}, 0.85f, 0.85f), k.Color(ink));
    } else if (glyph == kGlyphBack) {  // back: an arrow to the left, the title bar's first key
      line(-6, 0, 6, 0);
      line(-6, 0, -1, -5);
      line(-6, 0, -1, 5);
    } else if (glyph == kGlyphChevron) {  // goes on to a choice
      line(-1.8f, -4, 2.2f, 0);
      line(2.2f, 0, -1.8f, 4);
    } else if (glyph == kGlyphChevronDown || glyph == kGlyphChevronUp) {  // a fold: opens, closes
      // Down (folded) turned half a circle round its middle to up (open) by `turn`.
      const float angle = (glyph == kGlyphChevronUp ? 1.0f : turn) * 3.14159265f;
      const float sine = std::sin(angle);
      const float cosine = std::cos(angle);
      const auto at = [&](float x, float y) { return D2D1_POINT_2F{x * cosine - y * sine, x * sine + y * cosine}; };
      const D2D1_POINT_2F a = at(-4, -2);
      const D2D1_POINT_2F b = at(0, 2);
      const D2D1_POINT_2F c = at(4, -2);
      line(a.x, a.y, b.x, b.y);
      line(b.x, b.y, c.x, c.y);
    } else if (glyph == kGlyphAdd) {
      line(-5.5f, 0, 5.5f, 0);
      line(0, -5.5f, 0, 5.5f);
    } else if (glyph == kGlyphRefresh) {
      // Three quarters of a ring and its arrowhead at the open end, up right.
      wil::com_ptr<ID2D1PathGeometry> arc;
      wil::com_ptr<ID2D1GeometrySink> sink;
      constexpr float kR = 5.5f;
      if (SUCCEEDED(d2d_->CreatePathGeometry(arc.put())) && SUCCEEDED(arc->Open(sink.put()))) {
        sink->BeginFigure({cx + kR, cy}, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment({cx, cy - kR}, {kR, kR}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (SUCCEEDED(sink->Close())) {
          k.t->DrawGeometry(arc.get(), k.Color(ink), kIconStroke, roundCaps_.get());
        }
      }
      line(0, -kR, -2.6f, -kR - 2.4f);
      line(0, -kR, -2.6f, -kR + 2.4f);
    } else if (glyph == kGlyphFolder) {
      // Its outline: the tab up left, the body.
      const std::array<D2D1_POINT_2F, 7> outline{
          {{-7, -4.5f}, {-2.2f, -4.5f}, {-0.6f, -2.8f}, {7, -2.8f}, {7, 5}, {-7, 5}, {-7, -4.5f}}};
      for (std::size_t i = 1; i < outline.size(); ++i) {
        line(outline[i - 1].x, outline[i - 1].y, outline[i].x, outline[i].y);
      }
    } else {
      return false;
    }
    return true;
  }
  static D2D1_RECT_F ChromePill(D2D1_RECT_F r) {
    const float cx = (r.left + r.right) / 2;
    const float cy = (r.top + r.bottom) / 2;
    return {cx - 16, cy - 14, cx + 16, cy + 14};
  }
  // A key of the title bar (Caption): the pill under the pointer; the cross
  // and the dash drawn as lines of Windows' own size - ChromeClose at the
  // glyphs' 16 DIPs was the biggest thing in the bar, "it doesn't fit in"
  // (24.10.2026). The cross's pill is a soft red, the cross on it red.
  void DrawChrome(const Canvas& k, const Item& it, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    const bool cross = it.glyph == kGlyphClose;
    if (it.loud) {
      // A bad day (the kill switch closed the internet): "Сообщить о проблеме" said, not only drawn - a
      // quiet button, a hairline and no fill, the bubble before the words (settings-instruments.html, frame 5).
      const D2D1_RECT_F pill = KeyPill(it);
      if (hovered || pressed) {
        k.Round(pill, 6, Rgb(255, 255, 255, pressed ? 0.05f : 0.08f));
      }
      k.Outline(pill, 6, Rgb(255, 255, 255, 0.16f));
      const D2D1_COLOR_F ink = FromColorRef(ui::kPrimaryText);
      DrawLineIcon(k, kGlyphReport, std::floor(pill.left + 16) + 0.5f, std::floor((pill.top + pill.bottom) / 2) + 0.5f,
                   ink);
      k.Text(it.detail, caption_.get(), {pill.left + 30, r.top, pill.right - 8, r.bottom}, ink);
      return;
    }
    if (!it.checked && (hovered || pressed)) {
      k.Round(ChromePill(r), 8,
              cross ? FromColorRef(ui::kDanger, pressed ? 0.14f : 0.22f)
                    : Rgb(255, 255, 255, pressed ? 0.05f : 0.08f));
    }
    // Checked (a new version's key, Caption): a solid accent pill with a
    // halo, its number on it where it fits - the one thing on the bar that's
    // news (30.10.2026, his: "ахуенный значок"; it was the arrow in the
    // accent alone, lost among the keys).
    if (it.checked) {
      // An indicator window: a dark recess with a lit hairline and a pilot
      // lamp lit in it, the version in white (30.10.2026: the solid blue pill
      // was a button; this is a reading). White since: blue is the VPN's.
      const D2D1_RECT_F pill = KeyPill(it);
      const D2D1_COLOR_F lamp = FromColorRef(ui::kLit);
      k.Round(pill, 7, Rgb(0, 0, 0, 0.38f));
      k.Round(pill, 7, FromColorRef(ui::kLit, pressed ? 0.03f : (hovered ? 0.10f : 0.05f)));
      k.Outline(pill, 7, FromColorRef(ui::kLit, hovered ? 0.60f : 0.35f));
      if (it.detail.empty()) {
        k.Text(it.glyph, glyph_.get(), r, FromColorRef(ui::kPrimaryText));
        k.Led({pill.right - 5, pill.top + 5}, 2.0f, lamp);
      } else {
        k.Led({pill.left + 12, (pill.top + pill.bottom) / 2}, 2.5f, lamp);
        k.Text(it.detail, pill_.get(), {pill.left + 22, r.top, pill.right, r.bottom}, FromColorRef(ui::kPrimaryText));
      }
      return;
    }
    const D2D1_COLOR_F ink = !it.enabled ? FromColorRef(ui::kSecondaryText, 0.4f)
                             : !hovered   ? FromColorRef(ui::kSecondaryText)
                             : cross      ? FromColorRef(ui::kDanger)
                                          : FromColorRef(ui::kPrimaryText);
    // Through pixels' middles: at 100 % lines a pixel thick, not two halves.
    const float cx = std::floor((r.left + r.right) / 2) + 0.5f;
    const float cy = std::floor((r.top + r.bottom) / 2) + 0.5f;
    constexpr float kHalf = 4.5f;  // with the round ends 10 DIPs across
    if (cross) {
      k.t->DrawLine({cx - kHalf, cy - kHalf}, {cx + kHalf, cy + kHalf}, k.Color(ink), kIconStroke, roundCaps_.get());
      k.t->DrawLine({cx - kHalf, cy + kHalf}, {cx + kHalf, cy - kHalf}, k.Color(ink), kIconStroke, roundCaps_.get());
    } else if (it.glyph == kGlyphMinimize) {
      k.t->DrawLine({cx - kHalf, cy}, {cx + kHalf, cy}, k.Color(ink), kIconStroke, roundCaps_.get());
    } else if (!DrawLineIcon(k, it.glyph, cx, cy, ink)) {
      k.Text(it.glyph, glyph_.get(), r, ink);
    }
  }
  // The switch is a cube (cube.h): in the pose the window animates it
  // through, or - a still picture, a snapshot - the state's own.
  void DrawPower(const Canvas& k, const Item& it, const UiContent& c, bool hovered, bool pressed) const {
    DrawCube(k.t, k.cube != nullptr ? *k.cube : CubePoseFor(c.display), it.rect, hovered, pressed);
  }

  wil::com_ptr<ID2D1Factory> d2d_;
  wil::com_ptr<IDWriteFactory> dwrite_;
  wil::com_ptr<IDWriteFontCollection> fonts_;  // Geist; null if missing
  wil::com_ptr<IWICImagingFactory> wic_;
  wil::com_ptr<ID2D1StrokeStyle> roundCaps_;  // the title bar's cross and dash
  wil::com_ptr<ID2D1StrokeStyle> dashed_;     // a ghost key's outline
  wil::com_ptr<ID2D1StrokeStyle> dots_;       // the routing scheme's main line, its traffic
  wil::com_ptr<IDWriteInlineObject> ellipsis_;
  wil::com_ptr<IDWriteTextFormat> lead_;  // the routing page's opening words and its mode (17.75)
  wil::com_ptr<IDWriteTextFormat> state_;
  wil::com_ptr<IDWriteTextFormat> delay_;
  wil::com_ptr<IDWriteTextFormat> heading_;
  wil::com_ptr<IDWriteTextFormat> body_;
  wil::com_ptr<IDWriteTextFormat> caption_;
  wil::com_ptr<IDWriteTextFormat> button_;
  wil::com_ptr<IDWriteTextFormat> center_;
  wil::com_ptr<IDWriteTextFormat> strong_;  // 14 semibold from the left: a menu choice's title
  wil::com_ptr<IDWriteTextFormat> chip_;
  wil::com_ptr<IDWriteTextFormat> wrap_;
  wil::com_ptr<IDWriteTextFormat> captionWrap_;
  wil::com_ptr<IDWriteTextFormat> mono_;
  wil::com_ptr<IDWriteTextFormat> trailing_;  // a result on a row's right, cut with "…"
  wil::com_ptr<IDWriteTextFormat> hint_;      // the "?" of a hint
  wil::com_ptr<IDWriteTextFormat> pill_;      // a new version's number on its pill in the title bar
  wil::com_ptr<IDWriteTextFormat> engraved_;  // a panel's label: small capitals, spaced
  wil::com_ptr<IDWriteTextFormat> tip_;       // a tip's text, wrapped
  wil::com_ptr<IDWriteTextFormat> ask_;       // a question's title, wrapped
  wil::com_ptr<IDWriteTextFormat> glyph_;
  // ProgramIcon's, by exe path; tied to one render target (ForgetIcons).
  mutable std::map<std::wstring, wil::com_ptr<ID2D1Bitmap>> icons_;
  mutable std::map<std::string, wil::com_ptr<ID2D1Bitmap>> logos_;  // Logo's, by service; ForgetIcons too
  mutable wil::com_ptr<ID2D1Bitmap> flags_;  // Flags'
  mutable bool flagsLoaded_ = false;
};

int Scale(float dip, UINT dpi) { return static_cast<int>(std::lround(dip * static_cast<float>(dpi) / 96.0f)); }

// `text` onto the clipboard; false if the clipboard wouldn't take it.
bool CopyToClipboard(HWND owner, const std::wstring& text) {
  if (!OpenClipboard(owner)) {
    return false;
  }
  bool copied = false;
  if (EmptyClipboard()) {
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    wil::unique_hglobal memory(GlobalAlloc(GMEM_MOVEABLE, bytes));
    if (memory) {
      if (void* locked = GlobalLock(memory.get()); locked != nullptr) {
        std::memcpy(locked, text.c_str(), bytes);
        GlobalUnlock(memory.get());
        if (SetClipboardData(CF_UNICODETEXT, memory.get()) != nullptr) {
          memory.release();  // the clipboard owns it now
          copied = true;
        }
      }
    }
  }
  CloseClipboard();
  return copied;
}

}  // namespace

struct MainWindow::Impl {
  HINSTANCE instance;
  CommandHandler onCommand;
  Painter painter;
  wil::unique_hwnd window;
  HWND hwnd = nullptr;  // window's handle, known from WM_NCCREATE on (window is set when creation returns)
  wil::unique_hicon bigIcon;
  wil::unique_hicon smallIcon;

  UiContent content;
  UiPage page = UiPage::Overview;
  std::string openProfile;  // the id of the configuration UiPage::Profile shows
  float scroll = 0;
  Layout layout;
  Interaction in;
  LogView log;
  bool selecting = false;      // dragging a selection over the log
  bool settingsMore = false;   // the configurations' relay fold open (UiContent::settingsMore)
  int profileTab = 0;          // the configurations' tab (UiContent::profileTab)
  bool routeMenu = false;      // the routing mode's menu dropped (UiContent::routeMenu)
  bool lidUp = false;          // the kill switch's lid lifted, for kLidUpMs (UiContent::lidUp)
  // The servers' order on their page (UiContent::serverOrder): kept through
  // a latency test - every figure "…" there, the list would fall back to the
  // subscription's order and jump again as the results came in.
  std::vector<std::size_t> serverOrder;
  std::vector<std::pair<std::wstring, bool>> serverFolds;  // servers' cards opened or folded by hand (UiContent::serverFolds)
  // MainWindow::Ask's question while it's open (UiContent::question), what
  // its yes does, and when it came (Painter::questionShown).
  std::optional<UiQuestion> question;
  std::function<void()> onYes;
  std::function<void()> onOther;
  std::function<void(const UiCalendar&)> onDate;  // a calendar's yes (AskDate)
  std::chrono::steady_clock::time_point questionAt{};
  // When the pointer's wait for a tip is over. The timer alone isn't enough:
  // while the live traffic keeps a WM_PAINT waiting, WM_TIMER is never
  // handed out (a paint outranks it) and no tip showed on the main screen
  // (30.10.2026, he: 'при наводке на таракана подсказку бы'). The paint
  // looks at the clock too.
  std::optional<std::chrono::steady_clock::time_point> tipDue;
  std::optional<float> barGrab;  // dragging the log's thumb: where in it it was taken, DIPs below its top
  ULONGLONG panSince = 0;        // the middle button went down (log.pan)
  bool panMoved = false;         // the pointer left the still zone since
  bool panEndedByRight = false;  // a right press ended it: no menu on its release
  std::optional<POINT> mouse;  // the cursor over the client area, in pixels
  bool placed = false;  // sized and put by the tray once, on the first show

  wil::com_ptr<ID2D1HwndRenderTarget> target;
  ULONGLONG deactivatedAt = 0;  // GetTickCount64 when the window last lost the focus

  // The connect cube (cube.h): from the pose it was in to its state's since
  // cubeStart, turning while it connects. A timer only while it moves and
  // the window shows.
  CubePose cubeFrom;
  CubePose cubeTo;
  ULONGLONG cubeStart = 0;
  Display cubeDisplay = Display::Starting;
  bool cubeKnown = false;

  // Windows' "animation effects" (Accessibility > Visual effects): off, the
  // cube takes each pose at once.
  static bool Motion() {
    BOOL on = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) == FALSE || on != FALSE;
  }

  // Where the turn has got to, without the idle life - what a new turn starts from.
  CubePose CubeBase(ULONGLONG now) const {
    if (!Motion()) {
      return cubeTo;
    }
    const auto since = static_cast<float>(now - cubeStart);
    CubePose pose = BlendPose(cubeFrom, cubeTo, EaseOut(since / static_cast<float>(kCubeTurnMs)));
    if (cubeDisplay == Display::Starting) {
      pose.yaw = cubeFrom.yaw + since * kCubeSpin;
    }
    return pose;
  }

  // What's drawn: the turn, and over it the small idle life (Idle).
  CubePose CubeAt(ULONGLONG now) const {
    const CubePose base = CubeBase(now);
    return Motion() ? Idle(base, cubeDisplay, static_cast<double>(now) / 1000.0) : base;
  }

  bool CubeMoving(ULONGLONG now) const {
    return Motion() && (cubeDisplay == Display::Starting || now - cubeStart < kCubeTurnMs);
  }

  // Alive at rest too (Idle): while the overview shows the cube, on screen.
  bool CubeIdling() const {
    return Motion() && cubeKnown && cubeDisplay != Display::ServiceDown && page == UiPage::Overview &&
           hwnd != nullptr && IsWindowVisible(hwnd) != FALSE && IsIconic(hwnd) == FALSE && !Blocked();
  }


  // The window's edge in the colour of the main way (ui::WindowBorder). One
  // call when it changes. Windows 10 has no such attribute (22000+): the call
  // fails and the edge stays the system's - nothing to undo.
  std::optional<COLORREF> border;
  bool borderKnown = false;
  void ApplyBorder(const UiContent& c) {
    const std::optional<COLORREF> want = ui::WindowBorder(c);
    if (hwnd == nullptr || (borderKnown && want == border)) {
      return;
    }
    borderKnown = true;
    border = want;
    constexpr DWORD kBorderColor = 34;                  // DWMWA_BORDER_COLOR
    constexpr COLORREF kDefaultEdge = 0xFFFFFFFF;       // DWMWA_COLOR_DEFAULT
    const COLORREF value = want.value_or(kDefaultEdge);
    DwmSetWindowAttribute(hwnd, kBorderColor, &value, sizeof value);
  }

  // A new state: a turn from wherever the cube is into its pose - forward,
  // to the next square-on yaw (askew by the pose's own offset).
  void CubeTo(Display display) {
    const ULONGLONG now = GetTickCount64();
    if (cubeKnown && display == cubeDisplay) {
      return;
    }
    CubePose to = CubePoseFor(display);
    if (!cubeKnown) {
      cubeKnown = true;
      cubeFrom = to;
      cubeStart = display == Display::Starting ? now : now - kCubeTurnMs;
    } else {
      cubeFrom = CubeBase(now);
      cubeFrom.yaw = std::fmod(cubeFrom.yaw, 360.0f);
      to.yaw = NextRestYaw(cubeFrom.yaw) + (to.yaw - 45);
      cubeStart = now;
    }
    cubeTo = to;
    cubeDisplay = display;
    KickCube();
  }

  void KickCube() {
    if (hwnd != nullptr && IsWindowVisible(hwnd) != FALSE && (CubeMoving(GetTickCount64()) || CubeIdling())) {
      SetTimer(hwnd, kCubeTimer, CubeMoving(GetTickCount64()) ? kCubeFrameMs : kCubeIdleFrameMs, nullptr);
    }
  }

  // The live traffic (LiveTraffic): its timer runs while the overview shows
  // and there's traffic to draw or dots still on their way; it stops itself.
  LiveTraffic live;
  // Frames as often as the screen shows them - 60, 144, whatever it runs at:
  // each paint ends waiting for the screen's refresh (the render target's
  // EndDraw) and asks for the next one, so no timer (Windows' can't go past
  // 64 a second, and its 15.6 ms ticks made the dots jerk). The last step,
  // by the precise clock; empty: not running.
  std::optional<std::chrono::steady_clock::time_point> liveAt;

  // A fold opening or closing (the fine settings', the relay's): how open it is, eased over
  // kFoldMs from where it was; its flag in the content says where it goes.
  struct FoldMove {
    bool known = false;
    float from = 0;
    float to = 0;
    std::chrono::steady_clock::time_point at{};
    float Progress() const {
      const float t = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - at).count() /
                      static_cast<float>(kFoldMs);
      return std::clamp(t, 0.0f, 1.0f);
    }
    float Now() const { return from + (to - from) * EaseOut(Progress()); }
    bool Moving() const { return known && from != to && Progress() < 1.0f; }
  };
  FoldMove routingMove;
  FoldMove relayMove;

  // The flag says where the fold goes; if that's new, it moves from where it is - at once with the
  // window away, a modal box up or Windows' animations off.
  void Aim(FoldMove& move, bool open) {
    const float to = open ? 1.0f : 0.0f;
    const bool still = !Motion() || hwnd == nullptr || IsWindowVisible(hwnd) == FALSE || Blocked();
    if (!move.known || still) {
      move = {true, to, to, std::chrono::steady_clock::now()};
    } else if (move.to != to) {
      move = {true, move.Now(), to, std::chrono::steady_clock::now()};
    }
  }

  // The content's folds as they stand now: at rest -1, the flag says all.
  void SetFolds() {
    content.routingFold = routingMove.Moving() ? routingMove.Now() : -1.0f;
    content.relayFold = relayMove.Moving() ? relayMove.Now() : -1.0f;
  }

  // New flags (from the host, or the window's own): the folds aimed anew.
  void NoteFolds() {
    Aim(routingMove, content.routingMore);
    Aim(relayMove, settingsMore);
    SetFolds();
  }

  // A frame of a fold moving: the page laid out again at its height of now; true while one still does.
  bool StepFold() {
    if (!routingMove.Moving() && !relayMove.Moving()) {
      if (content.routingFold >= 0 || content.relayFold >= 0) {  // the last frame: at rest
        SetFolds();
        Relayout();
      }
      return false;
    }
    SetFolds();
    Relayout();
    return true;
  }

  // A frame of a question coming in (Painter::questionShown); true while it still is.
  bool StepQuestion() {
    if (!question || IsWindowVisible(hwnd) == FALSE || !Motion() || Blocked()) {
      painter.questionShown = 1;
      return false;
    }
    const float t = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - questionAt).count() /
                    static_cast<float>(kQuestionInMs);
    painter.questionShown = std::min(1.0f, t);
    return t < 1.0f;
  }

  // A modal box over the window (MessageBox, a file dialog) disables it. Such
  // a box shows itself only once its loop finds the queue empty, and a frame
  // asking for the next one keeps a paint there: the box stayed invisible and
  // the disabled window looked hung (22.10.2026, the cross's question). No
  // frames are asked for meanwhile; WM_ENABLE starts them again.
  bool Blocked() const { return IsWindowEnabled(hwnd) == FALSE; }

  void KickLive() {
    if (!liveAt && hwnd != nullptr && IsWindowVisible(hwnd) != FALSE && page == UiPage::Overview && Motion() &&
        (content.display == Display::On || !live.particles.empty())) {
      liveAt = std::chrono::steady_clock::now();
      InvalidateRect(hwnd, nullptr, FALSE);
    }
  }

  // A step before a paint: whether to go on (the next frame asked for after it).
  bool StepLiveTraffic() {
    auto now = std::chrono::steady_clock::now();
    if (liveAt && now - *liveAt < std::chrono::milliseconds(2)) {
      // Faster than any screen: nothing waited for its refresh (a remote
      // session, say) - wait for the compositor's, not spin.
      DwmFlush();
      now = std::chrono::steady_clock::now();
    }
    const float dt = liveAt ? std::min(0.1f, std::chrono::duration<float>(now - *liveAt).count()) : 0.0f;
    liveAt = now;
    const bool on = content.display == Display::On;
    if (!layout.glow || page != UiPage::Overview || IsWindowVisible(hwnd) == FALSE || !Motion() || Blocked() ||
        (!on && live.particles.empty())) {
      liveAt.reset();
      if (page != UiPage::Overview || IsWindowVisible(hwnd) == FALSE) {
        live.particles.clear();  // shown again, it starts afresh
      }
      return false;
    }
    StepLive(live, content, on, dt, *layout.glow);
    return true;
  }

  Impl(HINSTANCE inst, CommandHandler handler) : instance(inst), onCommand(std::move(handler)) {
    WNDCLASSW wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Impl::WindowProc;
    wc.hInstance = instance;
    // IDC_ARROW (32512) spelled out: the macro goes through the UNICODE-dependent
    // MAKEINTRESOURCE, which the CI's clang won't match with LoadCursorW.
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);  // already registered is fine
    // WS_CLIPCHILDREN: a repaint (a hover anywhere repaints it all) leaves an
    // edit box in place over its row alone - drawn over, it flickered as the
    // mouse moved (15.10.2026).
    // No minimize or maximize: a window of the tray's - the cross hides it
    // there; its edges still size it (a long log).
    window.reset(CreateWindowExW(0, kClassName, L"Sovereign",
                                 (WS_OVERLAPPEDWINDOW & ~(WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, this));
    THROW_LAST_ERROR_IF(!window);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    // The frame again, now with the title bar ours (WM_NCCALCSIZE).
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    const UINT dpi = GetDpiForWindow(hwnd);
    bigIcon = LoadAppIcon(GetSystemMetricsForDpi(SM_CXICON, dpi));
    smallIcon = LoadAppIcon(GetSystemMetricsForDpi(SM_CXSMICON, dpi));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon.get()));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon.get()));
  }

  ~Impl() {
    // The window goes before the members its procedure uses.
    window.reset();
  }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
      // The creation parameter arrives as an LPARAM - the cast is the API's shape.
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);  // NOLINT(performance-no-int-to-ptr)
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
      static_cast<Impl*>(create->lpCreateParams)->hwnd = hwnd;
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));  // NOLINT(performance-no-int-to-ptr)
    if (message == WM_NCDESTROY) {
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      if (self != nullptr) {
        self->hwnd = nullptr;
      }
    }
    return self != nullptr ? self->Handle(hwnd, message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
  }

  UINT Dpi() const { return GetDpiForWindow(hwnd); }
  float ToDip(int px) const { return static_cast<float>(px) * 96.0f / static_cast<float>(Dpi()); }

  D2D1_SIZE_F ClientDip() const {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return {ToDip(rc.right), ToDip(rc.bottom)};
  }

  // The item under a point: the last laid out wins - drawn last, it's on top
  // (a "?" over its row, a button in a check's line).
  int HitAt(POINT px, bool anyItem = false) const {
    const float x = ToDip(px.x);
    const float y = ToDip(px.y);
    for (std::size_t i = layout.items.size(); i-- > 0;) {
      const Item& item = layout.items[i];
      if (question && item.action != ItemAction::Answer) {
        continue;  // a question open: the page under it takes nothing
      }
      if (routeMenu && !InMenu(item)) {
        continue;  // the mode's menu dropped: a press elsewhere only puts it away (WM_LBUTTONDOWN)
      }
      if ((!anyItem && !Interactive(item)) || !Contains(Seen(item), x, y)) {
        continue;
      }
      if (item.scrolls && (y < layout.viewTop || y >= layout.viewBottom)) {
        continue;
      }
      return static_cast<int>(i);
    }
    return -1;
  }

  // The settings' card head (Kind::Instrument) under the pointer, clickable or not: its shy "?" and ↻
  // show while the pointer is anywhere on it. -1: none.
  int HeadAt(POINT px) const {
    if (question) {
      return -1;
    }
    const float x = ToDip(px.x);
    const float y = ToDip(px.y);
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
      const Item& item = layout.items[i];
      if (item.kind == Kind::Instrument && Contains(Seen(item), x, y) &&
          (!item.scrolls || (y >= layout.viewTop && y < layout.viewBottom))) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  // The routing mode's menu and the pill it drops from: all that takes a press while it's open.
  static bool InMenu(const Item& item) {
    return item.kind == Kind::Menu || item.kind == Kind::MenuItem || item.kind == Kind::ModePill;
  }

  // What shows a tip: a "?" at once, an icon button (its name) after a while.
  bool IsKind(int index, Kind kind) const {
    return index >= 0 && static_cast<std::size_t>(index) < layout.items.size() &&
           layout.items[static_cast<std::size_t>(index)].kind == kind;
  }

  // Shows its words after a while: an icon button its name, anything with a
  // tip of its own that. A "?" isn't one: its shows at once.
  bool HasTip(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < layout.items.size() && !IsKind(index, Kind::Hint) &&
           !TipText(layout.items[static_cast<std::size_t>(index)]).empty();
  }

  // The pointer is over `h` now: a "?" shows its tip, anything else with one
  // (HasTip) starts the wait for it - none if it comes straight from another
  // tip (along the title bar's keys: the names one after another).
  void Hover(int h) {
    if (h == in.hover) {
      return;
    }
    const bool tipShown = in.tip >= 0 && !IsKind(in.tip, Kind::Hint);
    in.hover = h;
    KillTimer(hwnd, kTipTimer);
    tipDue.reset();
    in.tip = IsKind(h, Kind::Hint) || (tipShown && HasTip(h)) ? h : -1;
    if (in.tip < 0 && HasTip(h)) {
      SetTimer(hwnd, kTipTimer, kTipDelayMs, nullptr);
      tipDue = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTipDelayMs);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  bool InLog(POINT px) const { return layout.logBox && Contains(*layout.logBox, ToDip(px.x), ToDip(px.y)); }

  // The log line under a point, clamped to the lines there are; -1 if none.
  int LogLineAt(POINT px) const {
    if (!layout.logBox || log.shown.empty()) {
      return -1;
    }
    const float y = ToDip(px.y) - layout.logBox->top + log.scroll;
    const auto line = static_cast<int>(std::floor(y / kLogRow));
    return std::clamp(line, 0, log.Count() - 1);
  }

  // On the log's scrollbar strip, with something to scroll.
  bool OnLogBar(POINT px) const {
    if (!layout.logBox || !InLog(px)) {
      return false;
    }
    return LogThumb(*layout.logBox, log) && ToDip(px.x) >= layout.logBox->right - kLogBar;
  }

  // The thumb's top dragged to `top` (DIPs): the lines scrolled to match.
  void DragLogThumb(float top) {
    if (!layout.logBox) {
      return;
    }
    const D2D1_RECT_F box = *layout.logBox;
    const auto thumb = LogThumb(box, log);
    if (!thumb) {
      return;
    }
    const float view = box.bottom - box.top;
    const float room = view - (thumb->bottom - thumb->top);
    const float at = room > 0 ? std::clamp((top - box.top) / room, 0.0f, 1.0f) : 0.0f;
    ScrollLog(at * (log.Height() - view) - log.scroll);
  }

  static HCURSOR PanCursor() { return LoadCursorW(nullptr, MAKEINTRESOURCEW(32645)); }  // IDC_SIZENS

  // The middle button's scrolling, as browsers have it: held and moved, it
  // ends on release; clicked, it goes on until the next press, a key or the wheel.
  void StartPan(POINT px) {
    SetCapture(hwnd);  // first: taken from another, WM_CAPTURECHANGED would end it
    log.pan = D2D1::Point2F(ToDip(px.x), ToDip(px.y));
    panSince = GetTickCount64();
    panMoved = false;
    SetTimer(hwnd, kPanTimer, kPanFrameMs, nullptr);
    SetCursor(PanCursor());
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  void StopPan() {
    if (!log.pan) {
      return;
    }
    log.pan.reset();
    KillTimer(hwnd, kPanTimer);
    if (GetCapture() == hwnd) {
      ReleaseCapture();
    }
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  // A frame of it: the farther the pointer from where it was pressed, the
  // faster - a few lines a second near, thousands far.
  void PanStep() {
    POINT px{};
    GetCursorPos(&px);
    ScreenToClient(hwnd, &px);
    if (!log.pan) {
      return;
    }
    const float off = ToDip(px.y) - log.pan->y;
    const float beyond = std::abs(off) - kPanStill;
    if (beyond <= 0) {
      return;
    }
    panMoved = true;
    ScrollLog(std::copysign(beyond * 0.12f + beyond * beyond * 0.003f, off));
  }

  // The log's scroll within its lines, or at the bottom while following.
  void ClampLog() {
    if (!layout.logBox) {
      return;
    }
    const float view = layout.logBox->bottom - layout.logBox->top;
    const float max = std::max(0.0f, log.Height() - view);
    log.scroll = log.follow ? max : std::clamp(log.scroll, 0.0f, max);
  }

  void ScrollLog(float dip) {
    if (!layout.logBox) {
      return;
    }
    const float view = layout.logBox->bottom - layout.logBox->top;
    const float max = std::max(0.0f, log.Height() - view);
    log.scroll = std::clamp(log.scroll + dip, 0.0f, max);
    log.follow = log.scroll >= max - 1;
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  // The log scrolled just enough for `row` to be in view.
  void ShowLogRow(int row) {
    if (!layout.logBox || row < 0) {
      return;
    }
    const float view = layout.logBox->bottom - layout.logBox->top;
    const float top = static_cast<float>(row) * kLogRow;
    if (top < log.scroll) {
      ScrollLog(top - log.scroll);
    } else if (top + kLogRow > log.scroll + view) {
      ScrollLog(top + kLogRow - view - log.scroll);
    }
  }

  // The routing board's "сейчас: …" (UiContent::routeNow), off the log's lines of the last ten
  // minutes: the sites that went each way, the newest first, a few by name and the rest counted.
  // A line has its time as "HH:MM" only: read against the local clock, round midnight too.
  void NoteRouteNow() {
    constexpr std::size_t kNamed = 3;
    constexpr int kRecent = 10;       // minutes
    constexpr std::size_t kName = 26;  // a longer host is cut
    for (std::vector<std::wstring>& way : content.routeNow) {
      way.clear();
    }
    content.routeNowMore.fill(0);
    SYSTEMTIME local{};
    GetLocalTime(&local);
    const int minutes = local.wHour * 60 + local.wMinute;
    std::array<std::unordered_set<std::wstring>, 4> seen;
    for (std::size_t i = log.events.size(); i-- > 0;) {
      const LogEvent& event = log.events[i];
      if (event.hidden || event.route == LogRoute::None) {
        continue;
      }
      const std::wstring& stamp = event.time;
      if (stamp.size() != 5 || stamp[2] != L':') {
        continue;
      }
      const int at = (stamp[0] - L'0') * 600 + (stamp[1] - L'0') * 60 + (stamp[3] - L'0') * 10 + (stamp[4] - L'0');
      if ((minutes - at + 24 * 60) % (24 * 60) > kRecent) {
        break;  // the lines run in time: the rest is older
      }
      const std::wstring& host = event.title;
      // An address of numbers says nothing at a glance.
      const bool numbers = std::all_of(host.begin(), host.end(), [](wchar_t ch) { return (ch >= L'0' && ch <= L'9') || ch == L'.'; });
      if (host.empty() || numbers || host.find(L':') != std::wstring::npos) {
        continue;
      }
      // The site it's registered under says more at a glance than the host: "datadoghq.com", not
      // "http-intake.logs.us5.datadoghq.com" (a zone of two parts, co.uk or com.ru, counts as one).
      std::wstring name = host;
      while (!name.empty() && name.back() == L'.') {
        name.pop_back();
      }
      if (const std::size_t last = name.rfind(L'.'); last != std::wstring::npos && last > 0) {
        static constexpr std::array<std::wstring_view, 9> kZones = {L"co", L"com", L"net", L"org", L"gov", L"edu", L"ac", L"msk", L"spb"};
        std::size_t start = name.rfind(L'.', last - 1);
        if (start != std::wstring::npos && start > 0 && name.size() - last == 3 &&
            std::find(kZones.begin(), kZones.end(), std::wstring_view(name).substr(start + 1, last - start - 1)) != kZones.end()) {
          start = name.rfind(L'.', start - 1);
        }
        if (start != std::wstring::npos) {
          name.erase(0, start + 1);
        }
      }
      const std::size_t way = event.route == LogRoute::Direct  ? 0
                              : event.route == LogRoute::Proxy ? 1
                              : event.route == LogRoute::Block ? 2
                                                               : 3;
      if (!seen[way].insert(name).second) {
        continue;
      }
      if (content.routeNow[way].size() < kNamed) {
        content.routeNow[way].push_back(name.size() > kName ? name.substr(0, kName - 1) + L"…" : name);
      } else {
        ++content.routeNowMore[way];
      }
    }
  }

  void Relayout() {
    const D2D1_SIZE_F size = ClientDip();
    LogState state;
    state.paused = log.paused;
    state.held = log.held.size();
    state.counts = log.counts;
    state.filter = log.filter;
    state.search = log.search;
    state.only = !log.only.empty();
    state.onlyTitle = log.onlyTitle;
    state.route = log.route == LogRoute::Direct  ? L"сайты напрямую"
                  : log.route == LogRoute::Proxy ? L"сайты через VPN"
                  : log.route == LogRoute::Block ? L"заблокированные сайты"
                  : log.route == LogRoute::Warp  ? L"сайты через WARP"
                                                 : L"";
    if (page == UiPage::Routing) {
      NoteRouteNow();
    }
    if (log.HasSelection()) {
      state.selected = true;
      state.detail = log.Line(log.caret);
      state.server = log.AboutWay(log.caret);
    }
    content.settingsMore = settingsMore;
    content.profileTab = profileTab;
    content.routeMenu = routeMenu;
    content.lidUp = lidUp;
    content.serverOrder = serverOrder;
    content.serverFolds = serverFolds;
    content.question = question;
    layout = painter.Build(content, page, openProfile, size.width, size.height, scroll, state);
    ClampLog();
    // The same spot may hold another item now (the content refreshes every
    // second under a resting pointer): it is pointed at anew, its tip timed
    // as if the pointer had just come; the same item keeps its tip.
    const int over = mouse ? HitAt(*mouse) : -1;
    in.head = mouse ? HeadAt(*mouse) : -1;
    if (over != in.hover) {
      Hover(over);
    } else {
      in.tip = IsKind(over, Kind::Hint) || (in.tip >= 0 && HasTip(over)) ? over : -1;
    }
    if (in.focus >= static_cast<int>(layout.items.size()) ||
        (in.focus >= 0 && (!Interactive(layout.items[static_cast<std::size_t>(in.focus)]) ||
                           (question && layout.items[static_cast<std::size_t>(in.focus)].action != ItemAction::Answer)))) {
      in.focus = -1;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  // A question over the page (MainWindow::Ask): whatever was going on in it
  // stops, the card comes in.
  void Ask(UiQuestion asked, std::function<void()> yes, std::function<void()> other = {}) {
    EndEdit(true);
    StopPan();
    selecting = false;
    barGrab.reset();
    question = std::move(asked);
    onYes = std::move(yes);
    onOther = std::move(other);
    onDate = nullptr;
    in.focus = in.pressed = in.tip = -1;
    in.focusVisible = false;
    questionAt = std::chrono::steady_clock::now();
    painter.questionShown = Motion() ? 0.0f : 1.0f;
    Relayout();
  }

  // The question answered (1 yes, 2 the other way, 0 no) or dropped: the page takes clicks again.
  void Answer(int which) {
    if (!question) {
      return;
    }
    if (which >= kAnswerMonthBack && question->calendar) {
      // A calendar's key: the date moves, the card stays.
      UiCalendar& cal = *question->calendar;
      switch (which) {
        case kAnswerMonthBack: CalendarShiftMonths(cal, -1); break;
        case kAnswerMonthOn: CalendarShiftMonths(cal, 1); break;
        case kAnswerYearBack: CalendarShiftMonths(cal, -12); break;
        case kAnswerYearOn: CalendarShiftMonths(cal, 12); break;
        default:
          if (which > kAnswerDay) {
            cal.day = std::clamp(which - kAnswerDay, 1, DaysInMonth(cal.year, cal.month));
          }
          break;
      }
      Relayout();
      return;
    }
    const std::function<void()> yes = std::exchange(onYes, nullptr);
    const std::function<void()> other = std::exchange(onOther, nullptr);
    const std::function<void(const UiCalendar&)> picked = std::exchange(onDate, nullptr);
    const std::optional<UiCalendar> date = question->calendar;
    question.reset();
    in.focus = in.pressed = -1;
    Relayout();
    if (which == 1 && date && picked) {
      picked(*date);
      return;
    }
    const std::function<void()>& then = which == 1 ? yes : other;
    if (which != 0 && then) {
      then();
    }
  }

  // A date asked on a calendar (MainWindow::AskDate): yes hands over what it
  // holds, the arrows, Page Up/Down (with Ctrl: the year) and Enter work it.
  void AskDate(UiQuestion asked, std::function<void(const UiCalendar&)> picked, std::function<void()> other) {
    Ask(std::move(asked), nullptr, std::move(other));
    onDate = std::move(picked);
  }

  // A key on a calendar's card: false if the question has none or the key
  // isn't one of its.
  bool CalendarKey(WPARAM key, bool ctrl) {
    if (!question || !question->calendar) {
      return false;
    }
    UiCalendar& cal = *question->calendar;
    switch (key) {
      case VK_LEFT: CalendarShiftDays(cal, -1); break;
      case VK_RIGHT: CalendarShiftDays(cal, 1); break;
      case VK_UP: CalendarShiftDays(cal, -7); break;
      case VK_DOWN: CalendarShiftDays(cal, 7); break;
      case VK_PRIOR: CalendarShiftMonths(cal, ctrl ? -12 : -1); break;
      case VK_NEXT: CalendarShiftMonths(cal, ctrl ? 12 : 1); break;
      case VK_HOME: cal.day = 1; break;
      case VK_END: cal.day = DaysInMonth(cal.year, cal.month); break;
      default: return false;
    }
    Relayout();
    return true;
  }

  // An edit box over the item for `anchor` and `index` on the page shown
  // (RenameProfile: over the title).
  void EditOver(UiCommand anchor, int index, UiCommand onEnter, const std::wstring& initial, bool digitsOnly) {
    for (const Item& it : layout.items) {
      const bool title = anchor == UiCommand::RenameProfile && it.kind == Kind::Title;
      const bool button = it.action == ItemAction::Command && it.command == anchor && it.index == index;
      if (title || button) {
        D2D1_RECT_F rect = it.rect;
        if (anchor == UiCommand::BoardAdd) {
          // A way's "+": a site is typed from it to the scheme's right, from the scheme's left
          // where that leaves too little for an address.
          constexpr float kRoom = 160;
          rect = {layout.boardRight - it.rect.left >= kRoom ? it.rect.left : layout.boardLeft, it.rect.top,
                  layout.boardRight, it.rect.top + kTag};
        } else if (anchor == UiCommand::ChipMenu) {
          // A chip: what's in it, or its name, across the scheme on its line.
          rect = {layout.boardLeft, it.rect.top, layout.boardRight, it.rect.top + kTag};
        }
        if (anchor == UiCommand::AddOwnServer && it.kind == Kind::IconButton) {
          // The last of the add keys: the address is typed left of them all, over the label or the title.
          constexpr float kRoom = 240;
          float keysLeft = it.rect.left;
          for (const Item& key : layout.items) {
            const bool addKey = std::any_of(kAddWays.begin(), kAddWays.end(),
                                            [&](const AddWay& way) { return way.command == key.command; });
            if (addKey && key.action == ItemAction::Command && key.rect.top < it.rect.bottom &&
                key.rect.bottom > it.rect.top) {
              keysLeft = std::min(keysLeft, key.rect.left);
            }
          }
          rect = {std::max(8.0F, keysLeft - kRoom), it.rect.top, keysLeft - 4, it.rect.bottom};
        }
        if (anchor == UiCommand::CheckRoute) {
          // Its button is an icon: the address is typed over the line's result.
          for (const Item& row : layout.items) {
            if (row.kind == Kind::CheckRow && row.rect.top < it.rect.bottom && row.rect.bottom > it.rect.top) {
              rect = {row.rect.left + 30 + TitleWidth(row) + kHint + 16, it.rect.top, it.rect.left - 4,
                      it.rect.bottom};
            }
          }
        }
        const bool onScheme = anchor == UiCommand::BoardAdd || anchor == UiCommand::ChipMenu;
        if (onScheme) {  // the box sits 4 inside a plate of its own (Interaction::edit)
          BeginEdit({rect.left + 4, rect.top, rect.right - 4, rect.bottom}, onEnter, index, initial, digitsOnly);
          if (edit.box) {
            in.edit = rect;
            in.editWay = anchor == UiCommand::BoardAdd ? index : it.rail;
            InvalidateRect(hwnd, nullptr, FALSE);
          }
        } else {
          BeginEdit(rect, onEnter, index, initial, digitsOnly);
        }
        return;
      }
    }
  }

  float TitleWidth(const Item& row) const { return painter.BodyWidth(row.text); }

  // Right-click on an item: what can be done with it - a server, a
  // configuration, a check, a rule, a program, the exit, a field. The row
  // under a "?" or a button counts; nothing for the rest.
  void ItemMenu(POINT screen) {
    POINT client = screen;
    ScreenToClient(hwnd, &client);
    const float x = ToDip(client.x);
    const float y = ToDip(client.y);
    const Item* found = nullptr;
    for (std::size_t i = layout.items.size(); i-- > 0;) {
      const Item& it = layout.items[i];
      const bool rowKind = it.kind == Kind::Choice || it.kind == Kind::ProfileRow || it.kind == Kind::CheckRow ||
                           it.kind == Kind::Switch || it.kind == Kind::AppRow ||
                           it.kind == Kind::Field || it.kind == Kind::ServerCard;
      if (rowKind && Contains(it.rect, x, y) && (!it.scrolls || (y >= layout.viewTop && y < layout.viewBottom))) {
        found = &it;
        break;
      }
    }
    if (found == nullptr) {
      return;
    }
    const Item item = *found;  // a copy: a command may relayout
    UiArgs args;
    args.index = item.index;
    args.sub = item.sub;
    args.owner = hwnd;
    args.anchor = screen;
    if (item.kind == Kind::ProfileRow && item.action == ItemAction::Command && item.command == UiCommand::RuleMenu) {
      onCommand(UiCommand::RuleMenu, args);  // a rule's menu is its click's
      return;
    }

    wil::unique_hmenu menu(CreatePopupMenu());
    if (!menu) {
      return;
    }
    const auto add = [&](UINT id, const std::wstring& text, bool enabled = true) {
      AppendMenuW(menu.get(), enabled ? MF_STRING : MF_STRING | MF_GRAYED, id, text.c_str());
    };
    const auto separator = [&] { AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr); };
    const auto at = [](const auto& list, int i) { return i >= 0 && static_cast<std::size_t>(i) < list.size(); };
    std::wstring name = item.text;  // what "copy the name" copies
    std::wstring ip;
    const UiProfile* profile = nullptr;
    switch (item.kind) {
      case Kind::Choice: {
        // Only a Hysteria 2 server has a menu: its speed - every Hysteria 2
        // server's (RoutingSettings::hysteriaMbps), the one set ticked. The
        // rest went (17.10.2026, he saw no use): picking is the row's click,
        // the latency the stopwatch's, copying its name or exit nobody's.
        const auto i = static_cast<std::size_t>(item.index);
        if (item.command != UiCommand::SetProtocol || i >= content.protocolTitles.size() ||
            !content.protocolTitles[i].starts_with(L"Hysteria 2")) {
          return;
        }
        AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, L"Скорость Hysteria 2 (на все её серверы)");
        separator();
        for (std::size_t s = 0; s < kHysteriaSpeeds.size(); ++s) {
          const int speed = kHysteriaSpeeds.at(s);
          const std::wstring text = speed == 0 ? std::wstring(L"Авто (BBR) — сервер подбирает сам")
                                               : std::format(L"{} Мбит/с (Brutal)", speed);
          AppendMenuW(menu.get(), MF_STRING | (speed == content.routing.hysteriaMbps ? MF_CHECKED : 0U),
                      kMenuHysteriaSpeed + static_cast<UINT>(s), text.c_str());
        }
        separator();
        AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, L"Brutal шлёт ровно с этой скоростью и не сбавляет");
        AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, L"при потерях: не ставь больше скорости своего канала.");
        break;
      }
      case Kind::ProfileRow:
        if (item.action != ItemAction::OpenProfile || !at(content.profiles, item.index)) {
          return;
        }
        profile = &content.profiles[static_cast<std::size_t>(item.index)];
        add(kMenuOpen, L"Открыть");
        add(kMenuToggle, profile->enabled ? L"Выключить" : L"Включить");
        if (profile->subscription) {
          add(kMenuRefresh, L"Обновить");
          if (profile->heldBack) {
            add(kMenuRefreshDirect, L"Скачать напрямую (сервер увидит IP)…");
          }
          add(kMenuCopyLink, L"Копировать ссылку");
        }
        add(kMenuRename, L"Переименовать");
        separator();
        add(kMenuRemove, L"Удалить…");
        break;
      case Kind::CheckRow: {
        add(kMenuRun, item.sub == static_cast<int>(kCheckRoute) ? L"Проверить адрес…" : L"Проверить снова",
            !content.checksRunning);
        separator();
        add(kMenuCopyResult, L"Копировать результат", item.index > 1);
        add(kMenuCopyAll, L"Копировать все результаты");
        break;
      }
      case Kind::AppRow:
        add(kMenuRemove, L"Убрать из списка");
        add(kMenuCopyName, L"Копировать имя");
        break;
      case Kind::ServerCard:
        add(kMenuOpen, L"Выбрать сервер…", item.enabled);
        add(kMenuTestDelays, L"Проверить задержку", true);
        add(kMenuCheckProtection, L"Проверить защиту от утечек", content.on && !content.checksRunning);
        separator();
        add(kMenuCopyIp, content.exitIp.empty() ? std::wstring(L"Копировать IP выхода")
                                                : L"Копировать IP выхода: " + content.exitIp,
            !content.exitIp.empty());
        add(kMenuToggleExitIp, content.hideExitIp ? L"Показывать IP" : L"Скрыть IP");
        ip = content.exitIp;
        break;
      case Kind::Field:
        add(kMenuCopyValue, L"Копировать «" + item.detail + L"»", !item.detail.empty());
        break;
      default: return;
    }
    const auto command = static_cast<UINT>(TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                                          screen.x, screen.y, 0, hwnd, nullptr));
    if (command >= kMenuHysteriaSpeed && command < kMenuHysteriaSpeed + kHysteriaSpeeds.size()) {
      args.index = kHysteriaSpeeds.at(command - kMenuHysteriaSpeed);
      onCommand(UiCommand::SetHysteriaMbps, args);
      return;
    }
    switch (command) {
      case kMenuTestDelays: onCommand(UiCommand::TestDelays, args); break;
      case kMenuCheckProtection:
        args.index = static_cast<int>(kCheckExit);
        onCommand(UiCommand::RunChecks, args);
        break;
      case kMenuCopyName: CopyToClipboard(hwnd, name); break;
      case kMenuCopyIp: CopyToClipboard(hwnd, ip); break;
      case kMenuCopyValue: CopyToClipboard(hwnd, item.detail); break;
      case kMenuOpen:
        if (item.kind == Kind::ServerCard) {
          Go(UiPage::Servers);
        } else if (profile != nullptr) {
          openProfile = profile->id;
          Go(UiPage::Profile);
        }
        break;
      case kMenuToggle:
        onCommand(UiCommand::ToggleProfile, args);
        break;
      case kMenuRefresh: onCommand(UiCommand::RefreshProfile, args); break;
      case kMenuRefreshDirect: onCommand(UiCommand::RefreshDirect, args); break;
      case kMenuCopyLink: onCommand(UiCommand::CopyProfileLink, args); break;
      case kMenuRename:
        if (profile != nullptr) {
          const std::wstring current = profile->name;
          openProfile = profile->id;
          Go(UiPage::Profile);
          EditOver(UiCommand::RenameProfile, args.index, UiCommand::RenameProfile, current, false);
        }
        break;
      case kMenuRemove:
        onCommand(item.kind == Kind::AppRow ? UiCommand::RemoveApp : UiCommand::RemoveProfile, args);
        break;
      case kMenuRun:
        args.index = item.sub == static_cast<int>(kCheckRoute) ? 0 : item.sub;
        onCommand(item.sub == static_cast<int>(kCheckRoute) ? UiCommand::CheckRoute : UiCommand::RunChecks, args);
        break;
      case kMenuCopyResult:
        if (at(content.checks, item.sub)) {
          CopyToClipboard(hwnd, CheckText(content.checks[static_cast<std::size_t>(item.sub)]));
        }
        break;
      case kMenuCopyAll: {
        std::wstring all;
        for (const UiCheck& check : content.checks) {
          all += CheckText(check) + L"\r\n";
        }
        CopyToClipboard(hwnd, all);
        break;
      }
      case kMenuToggleExitIp: onCommand(UiCommand::ToggleExitIp, args); break;
      default: break;
    }
  }

  // A check as text: "Утечка DNS: i3D.net (DE) — резолверы не в России."
  static std::wstring CheckText(const UiCheck& check) {
    std::wstring text = check.title + L": " + (check.status == UiCheck::Status::NotRun ? L"не проверялось" : check.summary);
    if (!check.detail.empty()) {
      text += L" — " + check.detail;
    }
    return text;
  }

  void Go(UiPage p) {
    EndEdit(true);
    // A configuration opened from anywhere: the list's tab is the one it is on, so "Назад" lands by it;
    // back from it, the one it is on now (its "Мой сервер" switched there).
    if (p == UiPage::Profile || (p == UiPage::Subscription && page == UiPage::Profile)) {
      const auto opened = std::find_if(content.profiles.begin(), content.profiles.end(),
                                       [&](const UiProfile& it) { return it.id == openProfile; });
      if (opened != content.profiles.end()) {
        profileTab = opened->ownHost.empty() ? 0 : 1;
      }
    }
    if (p != page) {
      page = p;
      scroll = 0;
      in.focus = -1;
      in.pressed = -1;
      routeMenu = false;
    }
    Relayout();
    KickLive();
    KickCube();  // back on the overview, the cube's idle life starts again
  }

  // The routing mode's menu dropped or put away; the focus, on an item of the page, would
  // be on nothing it can reach.
  void SetRouteMenu(bool open) {
    if (routeMenu == open) {
      return;
    }
    routeMenu = open;
    in.pressed = -1;
    if (open && in.focusVisible) {
      in.focus = -1;  // Tab goes into the menu: MoveFocus below
    }
    Relayout();
    if (open && in.focusVisible) {
      MoveFocus(false);
    }
  }

  void Activate(const Item& item) {
    switch (item.action) {
      case ItemAction::None:
      case ItemAction::Hint: return;
      case ItemAction::Page: Go(static_cast<UiPage>(item.index)); return;
      case ItemAction::EditName:
        if (item.index >= 0 && static_cast<std::size_t>(item.index) < content.profiles.size()) {
          EditOver(UiCommand::RenameProfile, item.index, UiCommand::RenameProfile,
                   content.profiles[static_cast<std::size_t>(item.index)].name, false);
        }
        return;
      case ItemAction::OpenProfile:
        if (item.index >= 0 && static_cast<std::size_t>(item.index) < content.profiles.size()) {
          openProfile = content.profiles[static_cast<std::size_t>(item.index)].id;
          Go(UiPage::Profile);
        }
        return;
      case ItemAction::ProfileTab:
        if (std::clamp(item.index, 0, 1) != profileTab) {
          profileTab = std::clamp(item.index, 0, 1);
          scroll = 0;
          in.pressed = -1;
          Relayout();
        }
        return;
      case ItemAction::LogFilter:
        log.filter = std::clamp(item.index, 0, kLogFilters - 1);
        log.only.clear();
        log.onlyTitle.clear();
        log.Rebuild();
        log.follow = true;
        ClampLog();
        Relayout();
        return;
      case ItemAction::CopyLogs:
        CopyToClipboard(hwnd, LogText(item.index != 1 && log.HasSelection()));
        return;
      case ItemAction::LogMore: LogMoreMenu(item.rect); return;
      case ItemAction::FindLog:
        BeginEdit({item.rect.left + 32, item.rect.top, item.rect.right - 4, item.rect.bottom}, UiCommand::RenameProfile,
                  -1, log.search, false);
        edit.search = true;
        return;
      case ItemAction::RouteLog:  // the routing board's "ещё N": the log, the sites of that way only
        log.route = item.index == kBucketDirect  ? LogRoute::Direct
                    : item.index == kBucketBlock ? LogRoute::Block
                    : item.index == kBucketWarp  ? LogRoute::Warp
                                                 : LogRoute::Proxy;
        log.only.clear();
        log.onlyTitle.clear();
        log.search.clear();
        log.filter = 0;
        log.anchor = log.caret = -1;
        log.follow = true;
        log.Rebuild();
        ClampLog();
        Go(UiPage::Logs);
        return;
      case ItemAction::LogOnly:
        if (!log.only.empty() || item.index == 1) {  // (index 1: the header's way back, with a way's sites too)
          log.only.clear();
          log.onlyTitle.clear();
          log.route = LogRoute::None;
          log.follow = true;
        } else if (log.HasSelection()) {
          const LogEvent& event = log.Event(log.caret);
          log.only = event.key;
          log.onlyTitle = event.title;
          log.follow = true;
        }
        log.Rebuild();
        ClampLog();
        Relayout();
        return;
      case ItemAction::LogServer: Go(UiPage::Servers); return;
      case ItemAction::PauseLogs:
        log.paused = !log.paused;
        if (!log.paused) {
          AddLogs(std::exchange(log.held, {}), false);
        }
        Relayout();
        return;
      case ItemAction::SaveLogs: SaveLogs(); return;
      case ItemAction::ToggleMore:
        settingsMore = !settingsMore;
        NoteFolds();
        Relayout();
        return;
      case ItemAction::ToggleServer: {
        const auto at = std::find_if(serverFolds.begin(), serverFolds.end(),
                                     [&](const auto& entry) { return entry.first == item.text2; });
        if (at != serverFolds.end()) {
          at->second = item.index == 0;
        } else {
          serverFolds.emplace_back(item.text2, item.index == 0);
        }
        Relayout();
        return;
      }
      case ItemAction::RouteMenu: SetRouteMenu(!routeMenu); return;
      case ItemAction::Hide: PostMessageW(hwnd, WM_CLOSE, 0, 0); return;
      case ItemAction::Answer: Answer(item.index); return;
      case ItemAction::LiftLid:  // the switch under it takes the next click, for a while
        lidUp = true;
        SetTimer(hwnd, kLidTimer, kLidUpMs, nullptr);
        Relayout();
        return;
      case ItemAction::Command: break;
    }
    if (item.command == UiCommand::ToggleKillSwitch && lidUp) {  // turned: the lid shuts over it again
      lidUp = false;
      KillTimer(hwnd, kLidTimer);
    }
    if (item.kind == Kind::MenuItem) {
      SetRouteMenu(false);  // a mode picked: the menu goes, the command relayouts with it gone
    }
    UiArgs args;
    args.index = item.index;
    args.sub = item.sub;
    args.owner = hwnd;
    const float scale = static_cast<float>(Dpi()) / 96.0f;
    // Under the row's text, in by an instrument's name too: the DNS's menu opened on the card's edge.
    POINT anchor{static_cast<LONG>((item.rect.left + item.indent) * scale), static_cast<LONG>(item.rect.bottom * scale)};
    ClientToScreen(hwnd, &anchor);
    args.anchor = anchor;
    onCommand(item.command, args);
  }

  // As the core wrote them (CRLF): the selected rows' lines, or every line the window keeps.
  std::wstring LogText(bool selectedOnly) const {
    std::wstring text;
    if (selectedOnly) {
      for (int i = 0; i < log.Count(); ++i) {
        if (log.Selected(i)) {
          text += log.Line(i);
          text += L"\r\n";
        }
      }
      return text;
    }
    for (const std::wstring& line : log.lines) {
      text += line;
      text += L"\r\n";
    }
    return text;
  }

  // The window's copy only: the service keeps its own, the next lines come.
  void ClearLogs() {
    log.Clear();
    log.held.clear();
    log.only.clear();
    log.onlyTitle.clear();
    log.anchor = log.caret = -1;
    log.follow = true;
    log.Rebuild();
    ClampLog();
    Relayout();
  }

  // The "⋯" key's menu, under it: save, how much the core writes, clear.
  void LogMoreMenu(D2D1_RECT_F key) {
    wil::unique_hmenu menu(CreatePopupMenu());
    if (!menu) {
      return;
    }
    const float scale = static_cast<float>(Dpi()) / 96.0f;
    POINT at{static_cast<LONG>(key.right * scale), static_cast<LONG>(key.bottom * scale)};
    ClientToScreen(hwnd, &at);
    const std::wstring level =
        L"Подробность записи: " + (content.logLevel.empty() ? std::wstring(L"авто") : content.logLevel) + L"…";
    AppendMenuW(menu.get(), log.lines.empty() ? MF_STRING | MF_GRAYED : MF_STRING, kLogMenuSave, L"Сохранить в файл…");
    AppendMenuW(menu.get(), MF_STRING, kLogMenuLevel, level.c_str());
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), log.lines.empty() ? MF_STRING | MF_GRAYED : MF_STRING, kLogMenuClear, L"Очистить");
    const auto command = static_cast<UINT>(
        TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN | TPM_TOPALIGN, at.x, at.y, 0, hwnd, nullptr));
    switch (command) {
      case kLogMenuSave: SaveLogs(); break;
      case kLogMenuLevel: {
        UiArgs args;
        args.owner = hwnd;
        args.anchor = at;
        onCommand(UiCommand::ChooseLogLevel, args);
        break;
      }
      case kLogMenuClear: ClearLogs(); break;
      default: break;
    }
  }

  // Ctrl+C: the selected lines, or all shown if none are.
  void CopyLogs() { CopyToClipboard(hwnd, LogText(log.HasSelection())); }

  void SelectAllLogs() {
    if (log.Count() > 0) {
      log.anchor = 0;
      log.caret = log.Count() - 1;
      Relayout();
    }
  }

  // The shown lines into a text file the user picks (UTF-8, CRLF).
  void SaveLogs() {
    std::wstring path;
    try {
      const auto dialog = wil::CoCreateInstance<IFileSaveDialog>(CLSID_FileSaveDialog);
      const COMDLG_FILTERSPEC filter{L"Текст", L"*.txt"};
      THROW_IF_FAILED(dialog->SetFileTypes(1, &filter));
      THROW_IF_FAILED(dialog->SetDefaultExtension(L"txt"));
      SYSTEMTIME now{};
      GetLocalTime(&now);
      const std::wstring name = std::format(L"sovereign-log-{:04}{:02}{:02}-{:02}{:02}.txt", now.wYear, now.wMonth,
                                            now.wDay, now.wHour, now.wMinute);
      THROW_IF_FAILED(dialog->SetFileName(name.c_str()));
      const HRESULT shown = dialog->Show(hwnd);
      if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return;
      }
      THROW_IF_FAILED(shown);
      wil::com_ptr<IShellItem> item;
      THROW_IF_FAILED(dialog->GetResult(&item));
      wil::unique_cotaskmem_string chosen;
      THROW_IF_FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen));
      path = chosen.get();
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("the save dialog failed");
      return;
    }
    const std::wstring text = LogText(false);
    std::string utf8;
    if (!text.empty()) {
      const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                        nullptr);
      utf8.resize(static_cast<std::size_t>(n));
      WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), n, nullptr, nullptr);
    }
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out << utf8;
    if (!out) {
      UiQuestion note;
      note.title = L"Не удалось записать файл";
      note.text = L"Может быть, папка только для чтения или файл открыт в другой программе. Попробуй сохранить в другое место.";
      note.yes = L"Понятно";
      note.no.clear();
      note.tone = UiQuestion::Tone::Warning;
      Ask(std::move(note), nullptr);
    }
  }

  // Right-click on the log: what can be done with its lines. A click on a
  // line outside the selection selects it first.
  void LogMenu(POINT screen) {
    POINT client = screen;
    ScreenToClient(hwnd, &client);
    if (const int line = LogLineAt(client); line >= 0 && InLog(client) && !log.Selected(line)) {
      log.anchor = log.caret = line;
      Relayout();
    }
    wil::unique_hmenu menu(CreatePopupMenu());
    if (!menu) {
      return;
    }
    const UINT any = log.Count() > 0 ? MF_STRING : MF_STRING | MF_GRAYED;
    AppendMenuW(menu.get(), log.HasSelection() ? MF_STRING : MF_STRING | MF_GRAYED, kLogMenuCopy,
                L"Копировать\tCtrl+C");
    AppendMenuW(menu.get(), any, kLogMenuCopyAll, L"Копировать всё");
    AppendMenuW(menu.get(), any, kLogMenuSelectAll, L"Выделить всё\tCtrl+A");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), any, kLogMenuSave, L"Сохранить в файл…");
    AppendMenuW(menu.get(), log.lines.empty() ? MF_STRING | MF_GRAYED : MF_STRING, kLogMenuClear, L"Очистить");
    const auto command = static_cast<UINT>(TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                                          screen.x, screen.y, 0, hwnd, nullptr));
    switch (command) {
      case kLogMenuCopy: CopyToClipboard(hwnd, LogText(true)); break;
      case kLogMenuCopyAll: CopyToClipboard(hwnd, LogText(false)); break;
      case kLogMenuSelectAll: SelectAllLogs(); break;
      case kLogMenuSave: SaveLogs(); break;
      case kLogMenuClear: ClearLogs(); break;
      default: break;
    }
  }

  void AddLogs(const std::vector<std::wstring>& lines, bool replace) {
    if (replace) {
      log.Clear();
      log.held.clear();
      log.anchor = log.caret = -1;
      log.follow = true;
    } else if (log.paused) {
      // Waits for the resume; the oldest go past what the log keeps.
      log.held.insert(log.held.end(), lines.begin(), lines.end());
      if (log.held.size() > kLogKeep) {
        log.held.erase(log.held.begin(), log.held.end() - static_cast<std::ptrdiff_t>(kLogKeep));
      }
      if (page == UiPage::Logs && IsWindowVisible(hwnd)) {
        Relayout();  // the "+N"
      }
      return;
    }
    for (const std::wstring& line : lines) {
      log.Push(line);
    }
    // Rows above what the user looks at: as many fewer, the view kept on the same ones.
    const auto first = static_cast<int>(std::max(0.0f, std::floor(log.scroll / kLogRow)));
    int firstLine = first < log.Count() ? log.shown[static_cast<std::size_t>(first)] : -1;
    if (log.lines.size() > kLogKeep) {
      const std::size_t cut = log.lines.size() - kLogKeep;
      log.Cut(cut);  // the oldest go
      firstLine -= static_cast<int>(cut);
    }
    log.Rebuild();
    if (!log.follow && firstLine >= 0) {
      const auto at = std::find(log.shown.begin(), log.shown.end(), firstLine);
      if (at != log.shown.end()) {  // still its row's newest: the same row on top
        log.scroll = std::max(0.0f, log.scroll + static_cast<float>(static_cast<int>(at - log.shown.begin()) - first) * kLogRow);
      }
    }
    ClampLog();
    if (page == UiPage::Logs && IsWindowVisible(hwnd)) {
      Relayout();
    }
  }

  // Tab order: the interactive items as laid out, rail first.
  void MoveFocus(bool back) {
    std::vector<int> order;
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
      // A tip's own ("?", a way's name) takes no focus: there's nothing to press.
      const Item& it = layout.items[i];
      if (Interactive(it) && it.action != ItemAction::Hint && it.kind != Kind::Day &&
          (!question || it.action == ItemAction::Answer) && (!routeMenu || InMenu(it))) {
        order.push_back(static_cast<int>(i));
      }
    }
    if (order.empty()) {
      return;
    }
    auto at = std::find(order.begin(), order.end(), in.focus);
    if (at == order.end()) {
      in.focus = back ? order.back() : order.front();
    } else if (back) {
      in.focus = at == order.begin() ? order.back() : *(at - 1);
    } else {
      in.focus = (at + 1) == order.end() ? order.front() : *(at + 1);
    }
    in.focusVisible = true;
    ScrollIntoView(layout.items[static_cast<std::size_t>(in.focus)]);
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  void ScrollIntoView(const Item& item) {
    if (!item.scrolls) {
      return;
    }
    const float margin = 16;
    if (item.rect.top < layout.viewTop + margin) {
      scroll -= layout.viewTop + margin - item.rect.top;
    } else if (item.rect.bottom > layout.viewBottom - margin) {
      scroll += item.rect.bottom - (layout.viewBottom - margin);
    } else {
      return;
    }
    const int focus = in.focus;
    Relayout();
    in.focus = focus;
  }

  void ScrollBy(float dip) {
    EndEdit(true);
    scroll += dip;
    Relayout();
  }

  /* ---- typing in place ---- */

  struct InlineEdit {
    wil::unique_hwnd box;
    UiCommand onEnter = UiCommand::RenameProfile;
    int index = 0;
    bool search = false;  // the log's: what's typed filters the rows as it's typed, nothing goes to the tray
  };
  InlineEdit edit;
  wil::unique_hfont editFont;
  wil::unique_hbrush editBrush;

  void BeginEdit(D2D1_RECT_F rect, UiCommand onEnter, int index, const std::wstring& initial, bool digitsOnly) {
    EndEdit(false);
    in.edit.reset();
    const UINT dpi = Dpi();
    const int fontHeight = Scale(17, dpi);
    editFont.reset(CreateFontW(-fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI"));
    if (!editBrush) {
      editBrush.reset(CreateSolidBrush(ui::kCardColor));
    }
    const int left = Scale(rect.left, dpi);
    const int right = Scale(rect.right, dpi);
    const int height = fontHeight + Scale(10, dpi);
    const int top = Scale((rect.top + rect.bottom) / 2, dpi) - height / 2;
    edit.box.reset(CreateWindowExW(0, L"EDIT", initial.c_str(),
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | (digitsOnly ? ES_NUMBER : 0), left, top,
                                   right - left, height, hwnd, nullptr, instance, nullptr));
    if (!edit.box) {
      return;
    }
    edit.onEnter = onEnter;
    edit.index = index;
    edit.search = false;
    SendMessageW(edit.box.get(), WM_SETFONT, reinterpret_cast<WPARAM>(editFont.get()), TRUE);
    SendMessageW(edit.box.get(), EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Scale(8, dpi), Scale(8, dpi)));
    // A name is short; a rule's sites, a DNS server's URL may be long (64 cut
    // them silently before 0.4.85).
    const bool name = onEnter == UiCommand::RenameProfile || onEnter == UiCommand::ChipRuleName;
    SendMessageW(edit.box.get(), EM_LIMITTEXT, digitsOnly || name ? 64 : 300, 0);
    SendMessageW(edit.box.get(), EM_SETSEL, 0, -1);
    SetWindowSubclass(edit.box.get(), EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SetFocus(edit.box.get());
  }

  // The box gone; with keep, what was typed goes to the tray.
  void EndEdit(bool keep) {
    if (!edit.box) {
      return;
    }
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(edit.box.get())) + 1, L'\0');
    text.resize(static_cast<std::size_t>(GetWindowTextW(edit.box.get(), text.data(), static_cast<int>(text.size()))));
    const UiCommand command = edit.onEnter;
    const int index = edit.index;
    const bool search = std::exchange(edit.search, false);
    edit.box.reset();  // its WM_KILLFOCUS finds no box: nothing twice
    in.edit.reset();
    InvalidateRect(hwnd, nullptr, FALSE);
    SetFocus(hwnd);
    if (search) {
      if (keep) {
        SetLogSearch(text);  // as typed already (EN_CHANGE); Esc emptied it
      }
      return;
    }
    const auto first = text.find_first_not_of(L" \t");
    if (!keep || first == std::wstring::npos) {
      return;
    }
    UiArgs args;
    args.index = index;
    args.owner = hwnd;
    args.text = text.substr(first, text.find_last_not_of(L" \t") - first + 1);
    onCommand(command, args);
  }

  // The log's rows with `text` in them (case aside), the newest in view.
  void SetLogSearch(std::wstring_view text) {
    const auto first = text.find_first_not_of(L" \t");
    std::wstring sought(first == std::wstring_view::npos ? std::wstring_view()
                                                         : text.substr(first, text.find_last_not_of(L" \t") - first + 1));
    if (sought == log.search) {
      return;
    }
    log.search = std::move(sought);
    log.Rebuild();
    log.follow = true;
    ClampLog();
    Relayout();
  }

  static LRESULT CALLBACK EditProc(HWND box, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<Impl*>(data);  // NOLINT(performance-no-int-to-ptr) - the subclass's reference data
    switch (message) {
      case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
          if (wParam == VK_ESCAPE && self->edit.search) {
            SetWindowTextW(box, L"");  // the log's search: Esc takes it away
          }
          self->EndEdit(wParam == VK_RETURN);
          return 0;
        }
        break;
      case WM_CHAR:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
          return 0;  // no beep
        }
        break;
      case WM_KILLFOCUS:
        PostMessageW(self->hwnd, kEndEditMessage, 0, 0);
        break;
      case WM_NCDESTROY:
        RemoveWindowSubclass(box, EditProc, 1);
        break;
      default:
        break;
    }
    return DefSubclassProc(box, message, wParam, lParam);
  }

  LRESULT Handle(HWND w, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
      case WM_PAINT: {
        if (tipDue && std::chrono::steady_clock::now() >= *tipDue) {
          tipDue.reset();
          KillTimer(w, kTipTimer);
          if (HasTip(in.hover)) {
            in.tip = in.hover;
          }
        }
        const bool going = liveAt && StepLiveTraffic();
        const bool coming = StepQuestion();
        const bool folding = StepFold();
        PAINTSTRUCT ps;
        BeginPaint(w, &ps);
        Paint();
        EndPaint(w, &ps);
        if (going || coming || folding) {
          InvalidateRect(w, nullptr, FALSE);  // the next frame: this one waited for the screen's refresh
        }
        return 0;
      }
      case WM_ENABLE:
        if (wParam != FALSE) {
          KickLive();  // the modal box is gone (Blocked)
        }
        return 0;
      case WM_ERASEBKGND: return 1;  // Direct2D paints everything
      case WM_NCCALCSIZE:
        if (wParam == TRUE) {
          // No system title bar: ours is in the client (Painter::Caption);
          // the sides and the bottom keep their sizing borders.
          auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);  // NOLINT(performance-no-int-to-ptr)
          const LONG top = params->rgrc[0].top;
          const LRESULT result = DefWindowProcW(w, message, wParam, lParam);
          params->rgrc[0].top = top;
          return result;
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case WM_NCHITTEST: {
        // The sides, the bottom, the corners - the system's; the top edge
        // sizes it; anything not taking a click drags it - the title bar but
        // for its buttons, the cards, the empty space (he asked, 22.10.2026).
        // The log keeps its press: it selects lines, its bar scrolls.
        const LRESULT hit = DefWindowProcW(w, message, wParam, lParam);
        if (hit != HTCLIENT) {
          return hit;
        }
        POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(w, &at);
        const UINT dpi = GetDpiForWindow(w);
        const int frame = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        if (at.y < frame) {
          RECT rc{};
          GetClientRect(w, &rc);
          return at.x < frame * 2 ? HTTOPLEFT : at.x >= rc.right - frame * 2 ? HTTOPRIGHT : HTTOP;
        }
        // The mode's menu dropped: a press anywhere is the window's, to put it away.
        if (HitAt(at) >= 0 || routeMenu ||
            (!question && (InLog(at) || log.pan.has_value() || barGrab.has_value() || selecting))) {
          return HTCLIENT;
        }
        return HTCAPTION;
      }
      case WM_SIZE:
        EndEdit(true);
        if (target) {
          target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        Relayout();
        if (wParam != SIZE_MINIMIZED) {
          KickCube();  // restored from the taskbar: the idle timer stopped while it was minimized
        }
        return 0;
      case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);  // NOLINT(performance-no-int-to-ptr)
        const UINT dpi = GetDpiForWindow(w);
        info->ptMinTrackSize = {Scale(kMinWidth, dpi), Scale(kMinHeight, dpi)};
        return 0;
      }
      case WM_DPICHANGED: {
        // The system's suggestion keeps the window the same physical size.
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);  // NOLINT(performance-no-int-to-ptr)
        target.reset();
      painter.ForgetIcons();
        SetWindowPos(w, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Relayout();
        return 0;
      }
      case WM_MOUSEMOVE: {
        const POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        mouse = at;
        if (log.pan) {
          SetCursor(PanCursor());  // captured: no WM_SETCURSOR comes
          return 0;
        }
        if (barGrab) {
          DragLogThumb(ToDip(at.y) - *barGrab);
          return 0;
        }
        if (const bool hot = page == UiPage::Logs && !question && OnLogBar(at); hot != log.barHot) {
          log.barHot = hot;
          InvalidateRect(w, nullptr, FALSE);
        }
        if (selecting && layout.logBox) {
          const D2D1_RECT_F box = *layout.logBox;
          if (const int line = LogLineAt(at); line >= 0 && line != log.caret) {
            log.caret = line;
            // Past the box's edge: the lines follow the mouse.
            if (ToDip(at.y) < box.top) {
              ScrollLog(-kLogRow);
            } else if (ToDip(at.y) > box.bottom) {
              ScrollLog(kLogRow);
            }
            Relayout();  // the card under the list: the row's line
          }
          return 0;
        }
        if (const int head = HeadAt(at); head != in.head) {
          in.head = head;
          InvalidateRect(w, nullptr, FALSE);
        }
        Hover(HitAt(at));
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, w, 0};
        TrackMouseEvent(&tme);
        return 0;
      }
      case WM_MOUSELEAVE:
        mouse.reset();
        log.barHot = barGrab.has_value();
        in.head = -1;
        Hover(-1);
        return 0;
      case WM_TIMER:
        if (wParam == kCubeTimer) {
          // One more frame after the last move: it lands on the pose exactly.
          // At rest the timer slows to the idle life's pace (and a turn
          // begun meanwhile is KickCube's, back at the quick one).
          if (IsWindowVisible(w) == FALSE || (!CubeMoving(GetTickCount64()) && !CubeIdling())) {
            KillTimer(w, kCubeTimer);
          } else if (!CubeMoving(GetTickCount64())) {
            SetTimer(w, kCubeTimer, kCubeIdleFrameMs, nullptr);
          }
          InvalidateRect(w, nullptr, FALSE);
          return 0;
        }
        if (wParam == kPanTimer) {
          if (log.pan) {
            PanStep();
          }
          return 0;
        }
        if (wParam == kLidTimer) {
          KillTimer(w, kLidTimer);
          lidUp = false;
          Relayout();
          return 0;
        }
        if (wParam == kTipTimer) {
          KillTimer(w, kTipTimer);
          tipDue.reset();
          if (HasTip(in.hover)) {
            in.tip = in.hover;
            InvalidateRect(w, nullptr, FALSE);
          }
          return 0;
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case WM_LBUTTONDOWN: {
        const POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (log.pan) {
          StopPan();  // a press ends it, and does nothing else
          return 0;
        }
        SetFocus(w);
        SetCapture(w);
        in.focusVisible = false;
        // A press puts a tip away (a "?"'s stays: it's what it's for), and it
        // stays away until the pointer comes to something else.
        KillTimer(w, kTipTimer);
        tipDue.reset();
        if (!IsKind(in.tip, Kind::Hint)) {
          in.tip = -1;
        }
        if (question) {
          in.pressed = HitAt(at);  // its buttons only: the log under it keeps its lines
          InvalidateRect(w, nullptr, FALSE);
          return 0;
        }
        if (routeMenu && HitAt(at) < 0) {
          ReleaseCapture();
          SetRouteMenu(false);  // a press beside the mode's menu puts it away, and does nothing else
          return 0;
        }
        if (const auto thumb = page == UiPage::Logs && OnLogBar(at) && layout.logBox ? LogThumb(*layout.logBox, log)
                                                                                     : std::nullopt) {
          // The thumb taken where pressed; a press beside it brings it there.
          const float y = ToDip(at.y);
          const float grab = y >= thumb->top && y < thumb->bottom ? y - thumb->top : (thumb->bottom - thumb->top) / 2;
          barGrab = grab;
          log.barHot = true;
          DragLogThumb(y - grab);
          return 0;
        }
        if (InLog(at)) {
          const int line = LogLineAt(at);
          if ((wParam & MK_SHIFT) != 0 && log.HasSelection() && line >= 0) {
            log.caret = line;
          } else {
            log.anchor = log.caret = line;
          }
          selecting = line >= 0;
          Relayout();  // the row's card under the list
          ShowLogRow(log.caret);  // which the card's coming may have covered
          return 0;
        }
        in.pressed = HitAt(at);
        InvalidateRect(w, nullptr, FALSE);
        return 0;
      }
      case WM_LBUTTONUP: {
        if (barGrab) {
          barGrab.reset();
          ReleaseCapture();
          log.barHot = mouse && OnLogBar(*mouse);
          InvalidateRect(w, nullptr, FALSE);
          return 0;
        }
        ReleaseCapture();
        if (std::exchange(selecting, false)) {
          return 0;
        }
        const int h = HitAt({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        const int pressed = std::exchange(in.pressed, -1);
        InvalidateRect(w, nullptr, FALSE);
        if (h >= 0 && h == pressed) {
          in.focus = h;
          const Item item = layout.items[static_cast<std::size_t>(h)];  // a copy: the command may relayout
          Activate(item);
        }
        return 0;
      }
      case WM_CAPTURECHANGED:
        selecting = false;
        barGrab.reset();
        StopPan();
        return 0;
      case WM_MBUTTONDOWN: {
        // The middle button on the log: scrolling by where the pointer goes.
        const POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (log.pan) {
          StopPan();
        } else if (!question && page == UiPage::Logs && layout.logBox && InLog(at) && LogThumb(*layout.logBox, log)) {
          StartPan(at);
        }
        return 0;
      }
      case WM_MBUTTONUP:
        if (log.pan && (panMoved || GetTickCount64() - panSince >= kPanHoldMs)) {
          StopPan();
        }
        return 0;
      case WM_RBUTTONDOWN:
        if (log.pan) {
          StopPan();
          panEndedByRight = true;
          return 0;
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case WM_MOUSEWHEEL: {
        StopPan();
        if (question) {
          return 0;  // the page stays where the question found it
        }
        POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(w, &at);
        const float step = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        if (InLog(at)) {
          ScrollLog(step * 2 * kLogRow);
        } else {
          ScrollBy(step * kWheelStep);
        }
        return 0;
      }
      case WM_KEYDOWN: return Key(wParam) ? 0 : DefWindowProcW(w, message, wParam, lParam);
      case WM_CONTEXTMENU:
        if (std::exchange(panEndedByRight, false) || question) {
          return 0;
        }
        if (page == UiPage::Logs && layout.logBox) {
          POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
          if (at.x == -1 && at.y == -1) {
            // From the keyboard (Shift+F10): at the log's corner.
            const float scale = static_cast<float>(Dpi()) / 96.0f;
            at = {static_cast<LONG>(layout.logBox->left * scale) + 16, static_cast<LONG>(layout.logBox->top * scale) + 16};
            ClientToScreen(w, &at);
          }
          LogMenu(at);
          return 0;
        }
        if (POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; at.x != -1 || at.y != -1) {
          ItemMenu(at);
          return 0;
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
          StopPan();
          deactivatedAt = GetTickCount64();
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case WM_COMMAND:
        // The log's search box: the rows follow what's typed.
        if (HIWORD(wParam) == EN_CHANGE && edit.box && edit.search &&
            reinterpret_cast<HWND>(lParam) == edit.box.get()) {  // NOLINT(performance-no-int-to-ptr) - the control's window
          std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(edit.box.get())) + 1, L'\0');
          text.resize(static_cast<std::size_t>(GetWindowTextW(edit.box.get(), text.data(), static_cast<int>(text.size()))));
          SetLogSearch(text);
          return 0;
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case kEndEditMessage:
        if (edit.box && GetFocus() != edit.box.get()) {
          EndEdit(true);
        }
        return 0;
      case WM_CTLCOLOREDIT:
        if (edit.box && reinterpret_cast<HWND>(lParam) == edit.box.get()) {  // NOLINT(performance-no-int-to-ptr)
          const auto dc = reinterpret_cast<HDC>(wParam);  // NOLINT(performance-no-int-to-ptr)
          SetTextColor(dc, ui::kPrimaryText);
          SetBkColor(dc, ui::kCardColor);
          return reinterpret_cast<LRESULT>(editBrush.get());
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case WM_CLOSE:
        // Only the view closes: the tray keeps running.
        EndEdit(true);
        ShowWindow(w, SW_HIDE);
        return 0;
      case WM_SHOWWINDOW:
        if (wParam == FALSE) {
          Answer(0);  // hidden, a question goes unanswered: no
          // Hidden, the window hears no more of the pointer: where it was is
          // no longer where it is, and the window shown lights nothing up.
          mouse.reset();
          Hover(-1);
        }
        return DefWindowProcW(w, message, wParam, lParam);
      default:
        return DefWindowProcW(w, message, wParam, lParam);
    }
  }

  bool Key(WPARAM key) {
    if (log.pan) {
      StopPan();  // a key ends the middle button's scrolling, and does nothing else
      return true;
    }
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (question) {
      // Only its buttons: the page under it waits for the answer. Enter
      // with neither in focus is yes.
      if (CalendarKey(key, ctrl)) {
        return true;
      }
      switch (key) {
        case VK_ESCAPE: Answer(0); return true;
        case VK_TAB:
        case VK_LEFT:
        case VK_RIGHT: MoveFocus(shift || key == VK_LEFT); return true;
        case VK_RETURN:
        case VK_SPACE:
          if (in.focus >= 0 && static_cast<std::size_t>(in.focus) < layout.items.size() &&
              layout.items[static_cast<std::size_t>(in.focus)].action == ItemAction::Answer) {
            const Item item = layout.items[static_cast<std::size_t>(in.focus)];
            Activate(item);
          } else if (key == VK_RETURN) {
            Answer(1);
          }
          return true;
        default: return true;
      }
    }
    if (ctrl && key >= '1' && key < '1' + kUiPageCount) {
      Go(static_cast<UiPage>(key - '1'));
      return true;
    }
    const bool onLog = page == UiPage::Logs && layout.logBox;
    if (onLog && ctrl && key == 'A') {
      SelectAllLogs();
      return true;
    }
    if (onLog && ctrl && key == 'C') {
      CopyLogs();
      return true;
    }
    if (onLog && key == VK_ESCAPE && log.HasSelection()) {
      log.anchor = log.caret = -1;
      Relayout();
      return true;
    }
    if (key == VK_ESCAPE) {
      // The mode's menu away first; else back (BackOf); from the overview, away.
      if (routeMenu) {
        SetRouteMenu(false);
      } else if (page != UiPage::Overview) {
        Go(BackOf(page));
      } else {
        ShowWindow(hwnd, SW_HIDE);
      }
      return true;
    }
    const float screen = onLog ? layout.logBox->bottom - layout.logBox->top - kLogRow : ClientDip().height * 0.8f;
    const auto scrollBy = [&](float dip) {
      if (onLog) {
        ScrollLog(dip);
      } else {
        ScrollBy(dip);
      }
    };
    const float total = onLog ? log.Height() : layout.contentHeight;
    switch (key) {
      case VK_TAB: MoveFocus(shift); return true;
      case VK_RETURN:
      case VK_SPACE:
        if (in.focus >= 0 && in.focus < static_cast<int>(layout.items.size())) {
          const Item item = layout.items[static_cast<std::size_t>(in.focus)];
          Activate(item);
        }
        return true;
      case VK_DOWN: scrollBy(onLog ? kLogRow : kWheelStep / 2); return true;
      case VK_UP: scrollBy(onLog ? -kLogRow : -kWheelStep / 2); return true;
      case VK_NEXT: scrollBy(screen); return true;
      case VK_PRIOR: scrollBy(-screen); return true;
      case VK_HOME: scrollBy(-total); return true;
      case VK_END: scrollBy(total); return true;
      default: return false;
    }
  }

  void Paint() {
    if (!target) {
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const auto dpi = static_cast<float>(Dpi());
      const D2D1_RENDER_TARGET_PROPERTIES props =
          D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi);
      if (FAILED(painter.D2d()->CreateHwndRenderTarget(
              props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), target.put()))) {
        return;
      }
      target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    }
    in.cube = cubeKnown ? std::optional(CubeAt(GetTickCount64())) : std::nullopt;
    target->BeginDraw();
    painter.Draw(target.get(), layout, content, in, log, &live);
    if (target->EndDraw() == D2DERR_RECREATE_TARGET) {
      target.reset();
      painter.ForgetIcons();
    }
  }

  // Sized for the monitor under the cursor and put in the corner of its work
  // area - where the notification area usually is, and the tray is where
  // it's opened from.
  void Place() {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(monitor, &mi);
    UINT dpiX = 96;
    UINT dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const RECT work = mi.rcWork;
    const int w = std::min(Scale(kDefaultWidth, dpiX), static_cast<int>(work.right - work.left));
    const int h = std::min(Scale(kDefaultHeight, dpiX), static_cast<int>(work.bottom - work.top));
    const int margin = Scale(12, dpiX);
    SetWindowPos(hwnd, nullptr, std::max(work.left, work.right - w - margin), std::max(work.top, work.bottom - h - margin),
                 w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  }
};

MainWindow::MainWindow(HINSTANCE instance, CommandHandler onCommand)
    : impl_(std::make_unique<Impl>(instance, std::move(onCommand))) {}

MainWindow::~MainWindow() = default;

void MainWindow::Show(UiPage page) {
  Impl& m = *impl_;
  if (!m.placed) {
    m.placed = true;
    m.Place();
  }
  m.Go(page);
  ShowWindow(m.hwnd, IsIconic(m.hwnd) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(m.hwnd);
  m.KickCube();  // turning while it connects: the timer stopped while hidden
  m.KickLive();
}

void MainWindow::Hide() { ShowWindow(impl_->hwnd, SW_HIDE); }

void MainWindow::Toggle() {
  Impl& m = *impl_;
  // A click on the tray icon takes the focus from the window before the
  // click arrives: a window that just lost it was in front.
  const bool inFront = GetForegroundWindow() == m.hwnd || GetTickCount64() - m.deactivatedAt < kToggleGraceMs;
  if (IsVisible() && inFront) {
    Hide();
  } else {
    Show(IsVisible() ? m.page : UiPage::Overview);
  }
}

bool MainWindow::IsVisible() const { return impl_->hwnd != nullptr && IsWindowVisible(impl_->hwnd) != FALSE; }

void MainWindow::Update(const UiContent& content) {
  Impl& m = *impl_;
  m.CubeTo(content.display);
  m.ApplyBorder(content);
  const bool sameList = content.protocols == m.content.protocols && content.autoOption == m.content.autoOption &&
                        content.serversByDelay == m.content.serversByDelay;
  // While the servers' page is in sight with figures on it, its rows stay where they are: a new
  // measurement that shuffles 179 and 180 ms made one look for the server again (06.11.2026). It's
  // sorted again when the list changes, the order key is pressed, or the page is out of sight.
  const bool watching = IsVisible() && m.page == UiPage::Servers && AnyDelayAnswered(m.content);
  if (!sameList || m.serverOrder.empty() || (!watching && !content.delaysTesting)) {
    m.serverOrder = ServerOrder(content);
  }
  m.content = content;
  m.NoteFolds();
  if (m.log.SetWays(WaysOf(content))) {
    m.log.Rebuild();  // a server's name or place came or changed: the rows say it now
  }
  if (IsVisible()) {
    m.Relayout();
    m.KickLive();
  }
}

void MainWindow::EditInPlace(UiCommand anchor, int index, UiCommand onEnter, const std::wstring& initial,
                             bool digitsOnly) {
  impl_->EditOver(anchor, index, onEnter, initial, digitsOnly);
}

void MainWindow::Ask(UiQuestion question, std::function<void()> onYes, std::function<void()> onOther) {
  if (!IsVisible()) {
    Show(impl_->page);
  }
  impl_->Ask(std::move(question), std::move(onYes), std::move(onOther));
}

void MainWindow::AskDate(UiQuestion question, std::function<void(const UiCalendar&)> onDate,
                         std::function<void()> onOther) {
  if (!IsVisible()) {
    Show(impl_->page);
  }
  impl_->AskDate(std::move(question), std::move(onDate), std::move(onOther));
}

void MainWindow::SetLogs(const std::vector<std::wstring>& lines) { impl_->AddLogs(lines, true); }

void MainWindow::AppendLogs(const std::vector<std::wstring>& lines) {
  if (!lines.empty()) {
    impl_->AddLogs(lines, false);
  }
}

std::wstring DescribeMainWindowLayout(const UiContent& content, UiPage page, float width, float height) {
  const Painter painter;
  float scroll = 0;
  const std::string profile = content.profiles.empty() ? std::string() : content.profiles.front().id;
  const Layout layout = painter.Build(content, page, profile, width, height, scroll, LogState{});
  return painter.Describe(layout, width);
}

void RenderMainWindowSnapshot(const UiContent& content, UiPage page, const std::vector<std::wstring>& logs, UINT width,
                              UINT height, float dpi, const std::wstring& pngPath, std::wstring_view tipFor) {
  const Painter painter;
  IWICImagingFactory* wic = painter.Wic();
  THROW_HR_IF_NULL(E_NOINTERFACE, wic);
  wil::com_ptr<IWICBitmap> bitmap;
  THROW_IF_FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.put()));
  wil::com_ptr<ID2D1RenderTarget> target;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi,
      dpi);
  THROW_IF_FAILED(painter.D2d()->CreateWicBitmapRenderTarget(bitmap.get(), props, target.put()));
  target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

  LogView log;
  log.SetWays(WaysOf(content));
  for (const std::wstring& line : logs) {
    log.Push(line);
  }
  log.Rebuild();
  LogState state;
  state.counts = log.counts;
  // Shows what a selected row looks like: a server's trouble, its line in full under the list.
  for (int i = log.Count(); i-- > 0;) {
    if (log.Event(i).topic == LogTopic::Server) {
      log.anchor = log.caret = i;
      state.selected = true;
      state.detail = log.Line(i);
      state.server = log.AboutWay(i);
      break;
    }
  }
  float scroll = 0;
  const float scale = 96.0f / dpi;
  const std::string profile = content.profiles.empty() ? std::string() : content.profiles.front().id;
  const Layout layout = painter.Build(content, page, profile, static_cast<float>(width) * scale,
                                      static_cast<float>(height) * scale, scroll, state);
  if (layout.logBox) {  // at the newest, as the window follows them
    log.scroll = std::max(0.0f, log.Height() - (layout.logBox->bottom - layout.logBox->top));
  }
  // The live traffic as it would be a few seconds in, an ad broken on the cube among it.
  LiveTraffic live;
  if (layout.glow) {
    for (int i = 0; i < 100; ++i) {
      UiContent now = content;
      now.blockedCount = i < 60 ? 0 : content.blockedCount;
      StepLive(live, now, content.display == Display::On, 1.0f / 30, *layout.glow);
    }
  }
  Interaction in;
  constexpr std::wstring_view kEditPrefix = L"edit:";
  if (tipFor.starts_with(kEditPrefix)) {
    // A way's line typed over (the box itself is a window: only its plate shows here).
    const std::wstring_view words = tipFor.substr(kEditPrefix.size());
    const auto it = std::find_if(layout.items.begin(), layout.items.end(), [&](const Item& item) {
      return item.kind == Kind::GhostAdd && item.text.starts_with(words);
    });
    THROW_HR_IF(E_INVALIDARG, it == layout.items.end());
    in.edit = D2D1_RECT_F{layout.boardRight - it->rect.left >= 160 ? it->rect.left : layout.boardLeft, it->rect.top,
                          layout.boardRight, it->rect.top + kTag};
    in.editWay = it->rail;
  } else if (!tipFor.empty()) {
    // Its tip, or its words where it has none: pointed at, without a tip then.
    const auto it = std::find_if(layout.items.begin(), layout.items.end(), [&](const Item& item) {
      return TipText(item).starts_with(tipFor) || (TipText(item).empty() && item.text.starts_with(tipFor));
    });
    THROW_HR_IF(E_INVALIDARG, it == layout.items.end());
    in.hover = static_cast<int>(it - layout.items.begin());
    in.tip = TipText(*it).empty() ? -1 : in.hover;
  }
  target->BeginDraw();
  painter.Draw(target.get(), layout, content, in, log, &live);
  THROW_IF_FAILED(target->EndDraw());

  wil::com_ptr<IWICStream> stream;
  THROW_IF_FAILED(wic->CreateStream(stream.put()));
  THROW_IF_FAILED(stream->InitializeFromFilename(pngPath.c_str(), GENERIC_WRITE));
  wil::com_ptr<IWICBitmapEncoder> encoder;
  THROW_IF_FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
  THROW_IF_FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
  wil::com_ptr<IWICBitmapFrameEncode> frame;
  THROW_IF_FAILED(encoder->CreateNewFrame(frame.put(), nullptr));
  THROW_IF_FAILED(frame->Initialize(nullptr));
  THROW_IF_FAILED(frame->SetSize(width, height));
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  THROW_IF_FAILED(frame->SetPixelFormat(&format));
  THROW_IF_FAILED(frame->WriteSource(bitmap.get(), nullptr));
  THROW_IF_FAILED(frame->Commit());
  THROW_IF_FAILED(encoder->Commit());
}

}  // namespace sovereign::tray
