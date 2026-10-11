# BlueShift

BlueShift is a light-cycle arena game. Ride a bike, leave a solid trail, and be the last one still on the grid.

One person hosts. Everyone else joins that game on the local network. The host decides crashes and who wins the round. Your own bike steers immediately on your machine.

## Play a packaged build

Unzip the Windows build and run:

`Windows\MyProject\Binaries\Win64\NeonGrid.exe`

Use that file. The smaller `NeonGrid.exe` at the top of the folder is only a launcher. Windows may show a SmartScreen warning the first time you open an unsigned build.

Everyone in the match needs the same build. Allow the game on private networks when Windows asks.

## Solo

1. Click **PLAY**.
2. Set the round options.
3. Click **ENGAGE**.

## Multiplayer

Up to four people. One computer hosts.

**Host**

1. Click **MULTIPLAYER**.
2. Click **OPEN LINK**.
3. Tell the other players the address on screen. The game listens on port **7777**.
4. When everyone is in the lobby, click **ENGAGE**.

**Join**

1. Click **MULTIPLAYER**.
2. Wait for the host to appear, then click **JOIN**.
3. If the scan stays empty, type the host address and click **CONNECT BY ADDRESS**.

On one computer, open the game twice. The second window will not see the scan. Type `127.0.0.1` and connect. Only the first window should open the link.

Closing the host ends the match.

## Controls

Menus use the mouse.

| Action | Keyboard | Gamepad |
| --- | --- | --- |
| Steer | A / D or Left / Right | Left stick |
| Sprint | Left Shift | Left bumper |
| Jump | Left Ctrl or F | Right bumper |
| Use powerup | Space | Right trigger or A |
| Leave the match | Esc | Menu |

The bike keeps moving forward. Steering turns it. Your trail is solid, and hitting a trail or the wall eliminates you.

## Project

Unreal Engine 5.8. The game module is `NeonGrid`, and the project file is `MyProject/MyProject.uproject`.
