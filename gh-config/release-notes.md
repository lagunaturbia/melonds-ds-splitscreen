First public Windows x64 test build of **melonDS DS Splitscreen**, a Libretro core based on Jesse Talavera's melonDS DS. It runs two to four Nintendo DS instances side by side with an emulated local wireless link on one PC.

- Supports Multi-Card and DS Download Play. Guests boot into the Download Play search screen when using native DS or DS Lite firmware.
- The attached ZIP includes the Windows x64 DLL, matching core info, RetroBat 8.3 system and Carbon theme files, and the corresponding source code with build instructions.
- Confirmed on a Lenovo Legion Go S that the second player boots directly into the Download Play search in 1.0. Earlier testing on the Alpha 3 base included Mario Party DS via Download Play and linked sessions in Diddy Kong Racing DS, Shrek Smash n' Crash Racing and Burnout Legends.
- Mario Kart DS linked multiplayer can stall. Separate single-player races can be played side by side.

To install, extract the ZIP into the RetroBat root, place a DS game in `roms/ndsmultiplayer` and launch it from **Nintendo DS Multiplayer**. See the included README for native BIOS and firmware requirements.
