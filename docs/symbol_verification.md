# Symbol verification ledger

`config/game.toml` was seeded from 5849 signatures matched against this 5659 image. An entry counts as verified only
when listed here or in FINDINGS.md with the instruction that proves it.

| Symbol | Address | Status | Proof |
|---|---|---|---|
| Direct3D_CreateDevice | 0x30A510 | verified (+ [abi]) | FINDINGS.md "XDK 5659 values" |
| IDirectSound_DownloadEffectsImage | 0x246207 | verified | .XTLID 0xB003C; calls CDirectSound 0x244D20 |
| XPP_GamepadType (data) | 0x31823C | verified | .XTLID 0x1007C |
| D3D_Device / D3D_pDevice (data) | 0x314B40 / 0x316FE8 | verified | 0x30A551 |
| Everything found "by .XTLID" in analysis/xbe_symbols_experiment.log | — | exact by construction | the image's own export table |
| 15 LTCG D3D functions | — | parked at 0, pending [abi] proof | candidates in config comments |

Pending: analysis/abi_5659.md, analysis/d3d_layout_5659.md, analysis/missing_symbols_5659.md (in progress).
