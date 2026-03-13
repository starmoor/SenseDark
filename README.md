# SenseDark（光感深色）

Switches the system Light/Dark appearance automatically based on the ambient light sensor. A tweak for roothide jailbroken iPhones.

The configuration lives right in the native appearance settings: **Settings → Display & Brightness → Automatic → Options** gains a third choice, *Ambient Light*, next to the system's *Sunset to Sunrise* / *Custom Schedule*. Selecting it expands an inline group with the light/dark thresholds, switch delay, sampling interval, manual-override behavior and a live lux readout. There is also a small guide pane under Settings that jumps straight to Display & Brightness.

中文说明见[下方](#中文说明)。

## How it works

- Subscribes to the ambient light sensor via `IOHIDEventSystemClient` (vendor page `0xff00`, usage `4`; `ReportInterval` controls the sampling rate). Only SpringBoard touches the sensor; the options page reads the value SpringBoard publishes
- Switches appearance through UIKit's private `UISUserInterfaceStyleMode`. When the native *Automatic* mode is on it goes through `setOverride:`, which keeps the system toggle on and the *Options* row in place
- Two thresholds with a gap between them prevent flicker at the boundary; light also has to stay past the threshold for a configurable number of seconds before anything switches, so a passing shadow doesn't count
- Changing appearance by hand (Control Center or Settings) pauses the automation until the next unlock
- Sensor stops while the screen is off and comes back on unlock

The core pipeline (ALS subscription, hysteresis, manual-switch pause, switching via `UISUserInterfaceStyleMode`) follows skittyblock/LightsOut (GPLv3). The Options-page integration, coexistence with the system schedule, debounce, live readout and roothide support are written from scratch.

## Install

Add `https://repo.rusti.cc/ios/` in Sileo or Zebra — the source lists `SenseDark (roothide)` and `SenseDark (rootless)`, pick the one matching your jailbreak. Or download the `.deb` from the releases page and install with `dpkg -i`. Requires mobilesubstrate (ElleKit) and PreferenceLoader.

## Build

Two package schemes: **roothide** (verified on device) and **rootless** (built by CI, not yet tested on a rootless device).

```bash
git clone https://github.com/starmoor/SenseDark && cd SenseDark
# roothide — needs the roothide theos fork (mainline theos has no roothide scheme)
THEOS=~/theos-roothide make package THEOS_PACKAGE_SCHEME=roothide FINALPACKAGE=1
# rootless — mainline theos or the roothide fork
THEOS=~/theos make package THEOS_PACKAGE_SCHEME=rootless FINALPACKAGE=1
```

CI (`.github/workflows/release.yml`) builds both with the roothide fork and attaches the two `.deb`s to the GitHub release whenever a `v*` tag is pushed.

## Compatibility

Developed and verified on iOS 17.2 (roothide Bootstrap). The rootless package builds but hasn't been tried on a rootless device. The private classes used here exist on iOS 15–17; only 17.2 has been tested.

## Code layout

| File | Role |
| --- | --- |
| `Tweak.x` | SpringBoard side — ALS sampling, hysteresis/debounce, appearance switching, prefs hot-reload |
| `SenseDarkSettings.x` | Preferences side — hooks that inject the third radio option into the native Options page |
| `SDCommon.h` | Cross-process contract: prefs domain, darwin notification names, keys, defaults (single source) |
| `SDAmbientPanel.h` | The inline ambient config group — localization, prefs I/O, live lux readout, specifier construction |
| `SDPSSpec.h` | Private `PSSpecifier`/`PSListController` runtime helpers (dynamically probed, no private headers) |
| `SDALSClient.h` | ALS HID client — `IOHIDEventSystemClient` via `dlsym`, vendor page `0xff00`/usage 4 |
| `SDLog.h` | Async diagnostics (CFPreferences channel + optional socket probe), fully compiled out when `SD_DIAG=0` |
| `SenseDarkPrefs.m` | Guide pane bundle (PreferenceLoader entry) — one-tap jump into Display & Brightness |

Both `SD_DIAG` switches default to 0; set them to 1 to turn on the diagnostics channel while debugging.

## 中文说明

**光感深色（SenseDark）**，环境光自动切换系统深浅色外观的 roothide 越狱插件。

配置直接做在原生外观设置里：**设置 → 显示与亮度 → 自动 → 选项** 在系统的“日落到日出/自定义时段”之外多出第三项**「根据环境光」**，选中后在原地展开配置组，可以调浅色/深色阈值、切换延迟、采样间隔、手动切换暂停，还有实时环境光读数。设置里另有一个指引页，一键跳到显示与亮度。

- lux ≥ 浅色阈值切浅色，≤ 深色阈值切深色，两阈值之间的滞回区防临界抖动
- 越限要持续满足设定的秒数才真正切换，瞬时阴影不会误触
- 在控制中心或设置里手动切外观会暂停自动切换，下次解锁恢复
- 灭屏停采样，解锁恢复
- 核心管线（ALS 订阅、滞回、手动暂停、切换原语）参考 skittyblock/LightsOut（GPLv3），选项页集成和其余实现全部原创

Sileo / Zebra 添加源 `https://repo.rusti.cc/ios/`——源内并列 `SenseDark (roothide)` 与 `SenseDark (rootless)` 两个包，按越狱类型装对应的；或从 GitHub Releases 下载 deb 手动安装。在 iOS 17.2（roothide Bootstrap）上实测开发。支持 roothide 与 rootless 两种打包 scheme（roothide 实测，rootless 由 CI 构建但未上机）；CI 在推送 `v*` 标签时自动出两种包并发 GitHub Release。
