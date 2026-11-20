Call to Arms Gates of hell - 4K UI Fix (AI) — a large interface for Call to Arms – Gates of Hell
Version: {version}
https://github.com/kuladun404/Call-to-Arms-Gates-of-hell-4K-UI-Fix-AI


WHAT IT DOES

On 2K, 4K and ultrawide (21:9) monitors the game's interface is small. The mod enlarges it
(1.25 to 2 times) and renders the 3D world at the resolution you choose. Interface text is sharp:
letters are drawn at high resolution instead of being stretched. It does not change any game files.


REQUIREMENTS

- Windows 10 or 11, 64-bit.
- Tested on NVIDIA graphics cards. It should work on AMD and Intel, but it has not been tested there yet.
  If something goes wrong, see "If something goes wrong".


INSTALLING

1. Close the game.
2. Open the game folder: Steam → Library → right-click the game → Manage → Browse local files.
   The folder "Call to Arms - Gates of Hell" opens.
3. Drag the binaries folder from this archive into it. Windows merges it with the game's folder,
   and d3d11.dll, ui4k.ini, ui4k_fonts2.dat and ui4k_fonts4.dat end up in binaries\x64, next to
   call_to_arms.exe. If Windows asks whether to replace files, say yes.

You can also do it by hand: put these four files into the game's binaries\x64 folder.

If binaries\x64 already has another d3d11.dll (ReShade, for example), move it somewhere else:
it will not work together with this mod.


USING IT

Start the game and open the video settings, the Resolution list. The mod replaces that list with its own entries:

    2560×1440  ×2

  - the first part is the resolution of the 3D world;
  - "×2" is how many times the interface is enlarged.

Pick an entry and press Apply.

- A smaller world (2560×1440 on a 4K monitor, for example) gives more frames per second, and the
  interface stays sharp.
- The entry with your monitor's resolution and "×1" is the game as without the mod.
- Entries labelled "UI like Full HD" have an interface of the same size as on a regular
  1920×1080 monitor (×2 on 4K, ×1.33 on 1440-high monitors).
- Two monitors: the list has the entries of the monitor the game is on, plus an entry labelled
  "other monitor" that moves the game to the second monitor. That monitor's entries appear
  the next time the game starts.


SETTINGS

The file ui4k.ini in binaries\x64 describes every setting. Usually nothing needs changing.
- To turn the mod off without removing it: enabled=0.
- The game's letters instead of the sharp ones: fonts=0.
- The scale can be changed right in the game: windows, 3D views and item icons are redrawn by themselves.


UPDATING

Close the game and drag the binaries folder from the new archive in the same way as when installing,
replacing the files. If you changed ui4k.ini and want to keep your settings, do not replace it.
From 2.0 beta: replace everything in the archive; the letter files ui4k_fonts2.dat and ui4k_fonts4.dat are new.


REMOVING

1. Close the game.
2. Delete d3d11.dll, ui4k.ini, ui4k_fonts2.dat, ui4k_fonts4.dat, ui4k_labels.pak and ui4k.log
   (and ui4k.2.log and the like, if any) from binaries\x64.
3. If the folder %LOCALAPPDATA%\ui4k exists, delete it too: the mod keeps the entry labels there
   when it cannot write into the game folder.

There is no need to set the resolution back first: without the mod the game switches to its
desktop resolution setting by itself.


MULTIPLAYER

The mod changes neither call_to_arms.exe nor the game files that multiplayer checks.
It only changes how the picture is put on your screen.


IF SOMETHING GOES WRONG

The mod writes ui4k.log next to itself (binaries\x64). Send it to whoever gave you the mod and say
what you did, what you saw, and which graphics card and monitor you have.
To play without the mod but keep it installed: enabled=0 in ui4k.ini.
