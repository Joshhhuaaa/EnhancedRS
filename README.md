# Enhanced RS

A patch for the original Rainbow Six, fixing bugs and adding gameplay improvements.

If you'd like to donate, all contributions are appreciated.
<div align="left">
  <a href="https://www.paypal.com/donate/?hosted_button_id=UB67N4GNTCEZ6">
    <img src="https://github.com/user-attachments/assets/6a8878e8-3ae8-48e5-8d2a-ae367c71df10" width="256" alt="PayPal"/>
  </a>
</div>

## Installation
The latest version of Enhanced RS can be found on the [Releases](https://github.com/Joshhhuaaa/EnhancedRS/releases) page.

### Game Setup
- After downloading Enhanced RS, extract the contents to your Rainbow Six directory and overwrite all existing files when prompted.
- You can adjust additional settings in `EnhancedRS.ini` located in the `plugins` folder.

## Uninstallation
- Navigate to the game folder, delete the `plugins` folder, `ddraw.dll`, `dxwrapper.dll`, and `dxwrapper.ini`.

## Features
### Widescreen Support
In the stock game, menus and cutscenes are hardcoded to render at 640x480, while the HUD stretches at widescreen aspect ratios. Enhanced RS renders menus and cutscenes at the in-game resolution and dynamically scales HUD elements to maintain their original proportions at any resolution.

Field of view is calculated automatically based on the aspect ratio, widening the horizontal FOV while preserving the vertical FOV from 4:3.

<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src=""></td>
      <td width="50%"><img style="width:100%" src=""></td>
    </tr>
    <tr>
      <td align="center">Stock</td>
      <td align="center">Enhanced</td>
    </tr>
  </table>
</div>

### Increased Character LOD Distance
Significantly increases the distance at which higher detail character models are rendered, improving character quality at greater distances.

### Raw Input
Mouse input is read directly, reducing overhead at high polling rates and helping prevent stuttering at extreme rates such as 8000 Hz. Adds support for Mouse 4 and 5 side buttons.

### Mouse Sensitivity Multiplier
Separate sensitivity multiplier for in-game aiming and the menu cursor to allow more control than the game's setting.

### Task Switching
The original game does not support switching to other applications while playing. Enhanced RS adds support for <key>Alt</key> + <key>Tab</key> and other Windows application-switching shortcuts.

<key>Alt</key> + <key>F4</key> can also be used to exit the game.

### Portable Configuration
The original game stores its settings in the Windows Registry and uses fixed installation paths. This makes settings difficult to transfer between installations and can cause the game to break when its directory is moved.

Enhanced RS removes the game's dependency on the Windows Registry and stores settings locally in `Sherman.ini`. Installation paths are also derived from the game's executable directory, allowing the entire installation to be moved without having to update any paths.

### Eagle Watch
The GOG release does not include the Eagle Watch mission pack. Enhanced RS includes the necessary files to run the expansion.

You can play the expansion by launching `RainbowSixMP.exe`.