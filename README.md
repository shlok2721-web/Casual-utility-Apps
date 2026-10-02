# Abyssal Battery Monitor (v2)

Two small programs:

| Program | Toolkit | Job |
|---|---|---|
| `battery-monitord` | GLib only (no GTK) | Always running in the background. Polls the battery, rings the alarm, runs sleep/shutdown. ~8 MB RSS, wakes once per poll interval. |
| `battery-monitor-panel` | GTK4 (+ layer-shell) | The window. Spawned automatically by the daemon when a ceiling is reached; also launchable by hand. Closing it exits the process, so it costs nothing when not shown. |

They talk over the session D-Bus (`com.local.BatteryMonitor`). Settings live in
`~/.config/battery-monitor/config.conf`.

## Build

```sh
sudo pacman -S gtk4 gtk4-layer-shell alsa-lib mpg123 pipewire-alsa base-devel
make
sudo make install        # optional; installs both into /usr/local/bin
```

`mpv` is no longer used.

## Run

Hyprland autostart (only the daemon):

```
exec-once = /path/to/battery-monitord
```

Optional key to open the settings panel any time:

```
bind = $mainMod, B, exec, /path/to/battery-monitor-panel
```

If you don't `make install`, keep both binaries in the same folder - the daemon
looks next to itself first when it spawns the panel.

## What changed

### Thresholds: 0-100 for everything
High, Sleep and Shutdown sliders all go 0-100 and are independent (no forced
ordering), so you can set e.g. Sleep = 100 to test that it fires. If you set
Shutdown >= Sleep, shutdown wins and the panel shows a warning.
Saving settings (or `Reload`) re-arms every ceiling and checks immediately.

### Sound: ALSA direct, infinite loop (no mpv)
`libmpg123` decodes the MP3, `libasound` plays it to the `default` ALSA device
(= PipeWire when `pipewire-alsa` is installed; falls back to `pipewire`, then
`plughw:0,0`). It loops forever until you turn it off. The audio thread and all
buffers exist only while an alarm rings - nothing is allocated while idle.
If the audio server isn't ready yet (e.g. just after resume) it keeps retrying.

Sound files: set in the config file, defaults are
`~/Music/snorcon-high-battery-charge-421821.mp3` and
`~/Music/snorcon-low-battery-charge-421814.mp3`:

```ini
[sound]
high_file=/path/to/high.mp3
low_file=/path/to/low.mp3
```
MP3 only. Each alarm sound can also be switched off in the panel.

### Panel appears by itself
When a ceiling is reached the daemon sends a notification **and** opens the
panel on top of everything. A panel opened that way closes itself ~6 s after
the alarm ends, unless you touched anything in it.

### The alarm button
While an alarm is active the panel shows, at the top, a black button with a
red/blue glow: **TURN OFF ALARM**. It is not visible otherwise.
- High alarm: the button stops the alarm completely.
- Sleep/Shutdown alarm: the button silences the sound; the countdown keeps
  running. Below it, **Cancel sleep / Cancel shutdown** aborts the action.

### Sleep / shutdown: 1 minute of alarm first
At the Sleep or Shutdown ceiling (only while **not** on AC) the alarm plays for
60 s (`grace_seconds` in the config, 10-600), then the daemon runs
`systemctl suspend` / `poweroff`. The battery is re-read right before the
action; if the charger is connected the action is cancelled (and plugging in
during the countdown cancels it automatically).

**After waking from sleep** the daemon re-checks after 3 s and, if the battery
is still below the ceiling and unplugged, starts the 60 s alarm again - so you
always get time to plug in or keep going. This uses logind's `PrepareForSleep`
signal (no polling), with a clock-drift check as fallback.

### Check interval
Panel slider, **60-1200 s** in steps of 10 s (default 300). Applied as soon as
you press *Save & apply*.

### Low overhead in the background
- GLib main loop, no GTK, no polling except one coalesced timer per interval.
- Battery is read straight from `/sys/class/power_supply` (auto-detects the
  system battery, ignores mouse/headset batteries, uses the AC `online` flag).
- Log is a 100-line ring buffer; nothing is written to disk except the config.
- Faster 1 s ticking happens only during an alarm.

## Panel buttons
- **Save & apply** - write config, daemon reloads and re-checks.
- **Check now** - immediate battery check.
- **Test High / Sleep / Shutdown** - the full alarm (sound, notification,
  panel, countdown) but nothing is actually suspended or powered off.

## Resource-leak testing (done)
- Daemon: ASan + LeakSanitizer + UBSan, several hundred alarm start/stop
  cycles (sound thread, notifications, panel spawns, reloads). No leaks or
  memory errors; fds and thread count constant; release build stays ~7.4 MB.
- Panel: 150 alarm cycles. Found and fixed unbounded growth caused by a CSS
  `@keyframes` pulse animation (the blurred glow was re-rendered at 60 fps);
  the glow now blinks by toggling a class twice a second, and only while the
  alarm sound is ringing. Text-buffer undo history is disabled. RSS is flat
  after warm-up.
- Found and fixed: if the audio thread died on its own the daemon kept
  reporting "sound on" and never joined it. It is now detected and reaped.

## Troubleshooting
- No sound: run `./battery-monitord` in a terminal and read the `audio:` lines.
  Check that `pipewire-alsa` is installed and the sound file exists.
- `SIGHUP` also makes the daemon reload the config: `pkill -HUP battery-monitord`.
- Test without touching your battery: point the daemon at a fake sysfs tree
  with `BM_PS_DIR=/tmp/fake ./battery-monitord`
  (`/tmp/fake/BAT0/{type,capacity,status}`, `/tmp/fake/AC/{type,online}`).
