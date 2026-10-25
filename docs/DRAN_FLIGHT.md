# Dran free flight

The native port lets Toan summon and ride Dran in the procedural Norune overworld. It is available immediately for exploring the mod world; it is not gated by the retail boss fight. Dungeons and interiors keep their normal controls.

| Action | Keyboard / mouse default | Controller |
| --- | --- | --- |
| Summon / request landing / cancel landing | B or middle mouse | Right-stick click |
| Fly forward / brake | W / S | Left stick forward / back |
| Turn Dran | A / D | Left stick left / right |
| Climb | Space or left mouse | Cross |
| Descend | Right mouse | Circle |
| Boost while moving forward | F or X | R1 |
| Orbit the camera | Mouse or I / J / K / L | Right stick |
| Pause | Enter | Start |

Flight hovers when forward input is released. A landing request stops horizontal travel, descends, prepares detailed terrain, and returns to normal walking. A second request cancels descent. Water, steep slopes, buildings, and nearby trees prevent landing; the HUD asks for a clearing. Ground menus and Georama are unavailable while mounted.

The implementation uses extracted retail assets, so the usual legally supplied game data is required. Dran is loaded separately from enemy AI, and Toan uses the original riding animation. No boss combat or story flags are modified.

Toan follows Dran's animated `to_c` socket using the same `CFrame::SetReference` mechanism as the retail `SYNC_CHARA_CHARA` cutscene command. The s30 event script explicitly pairs that socket with Toan's e411cc01d riding clip. The reference is scoped to animation/cloth updates and rendering, preserving world coordinates for flight, saves, and origin rebasing; it is restored before returning to normal gameplay. Dran's original motion is unchanged.

The flight camera orbits independently from steering, keeps the chosen orientation when input stops, and supports a full horizontal orbit and vertical elevation from -89 to +89 degrees. The one-degree margin avoids a singular look-at basis. Its distance stays constant as it tilts.

Flight terrain uses asynchronous aerial detail instead of the walking teleport reload. Airborne clearance samples the same terrain triangles without river carving, a conservative upper bound that avoids constructing drainage networks on the game thread. Exact terrain remains in use for landing and walking. Music uses the regional biome in flight, avoiding a second river query for riverbank classification.

The HUD uses the native Georama-style outlined message font and the shared input glyph renderer. Symbols follow the selected/active device and configured button bindings.

For a direct, isolated overworld preview, run `Play-Dran.cmd` from this worktree. It uses `port/run/dran-preview-save`, separate from existing saves.

Build with `build.bat windows-x64-release`. Run from this worktree with `run.bat windows-x64-release`. The normal build driver expects the Windows dependencies in this checkout; tools/windows/configure.ps1 also accepts an existing dependency root through -Root and a separate -BuildDirectory.

Set `DC_DRAN_TRACE=1` for input, mounting, flight, and landing diagnostics. Test runs should use separate --save directories, as in tools/test_town_combat.py.

A save made while mounted resumes at the takeoff position. Land before saving to retain the destination. Flight is not restored into a walking character high above the ground.

Run `python tools/test_dran_flight.py` for the integration checks (or pass a case name: lifecycle, travel, dungeon, combat, camera, performance). Each case has its own save folder and keeps its log and screenshot under `port/run/mod-tests/dran`.
