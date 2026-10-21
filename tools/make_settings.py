"""Builds settings/SETTINGS.cfg from the decoded DUMMYS.DAT with the mod's overrides.

The game loads COMMON:/SETTINGS/SETTINGS.cfg (plain text) in preference to the
obfuscated DUMMYS.DAT, so dropping this file into Common/Settings changes the
engine settings without touching any original game file.
"""
import pathlib, re, sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from dummydat import xor

root = pathlib.Path(__file__).resolve().parent.parent
src = pathlib.Path(sys.argv[1])  # path to the game's Common/Settings/DUMMYS.DAT
text = xor(src.read_bytes()).decode('latin1')
# The last few bytes of DUMMYS.DAT are not covered by the XOR key (they decode to
# garbage); they hold the closing brace of the final table. Drop them and close
# any table left open.
text = text[:text.rfind('}') + 1]


def open_braces(lua: str) -> int:
    lua = re.sub(r'--[^\n]*', '', lua)        # comments
    lua = re.sub(r'"(?:\\.|[^"\\])*"', '', lua)  # strings
    return lua.count('{') - lua.count('}')


text += '\r\n}' * open_braces(text) + '\r\n'
if open_braces(text) != 0:
    sys.exit('unbalanced braces after repair')

common = [
    # Visible proof that SETTINGS.cfg is used instead of DUMMYS.DAT.
    ('\t-- ShowFPS = "Advanced Maximum",', '\tShowFPS = "Advanced Maximum", -- wrc10mp: load marker'),
]
# LAN mode (RakNet broadcast search, used over Tailscale) or the built-in Steam
# mode (Steam lobbies + Steam P2P), selected in RakNetNetworkManager at
# 0x140f8c610: UseLanNetwork wins, else KtOS if UseKtOS and UseKtOSMultiplayer
# (default true), else Steam.
variants = {
    'SETTINGS.cfg': common + [('\tUseLanNetwork = false,', '\tUseLanNetwork = true, -- wrc10mp')],
    'SETTINGS_steam.cfg': common + [('\tUseKtOS = true,\r\n}', '\tUseKtOS = true,\r\n\tUseKtOSMultiplayer = false, -- wrc10mp\r\n}')],
}
outdir = root / 'settings'
outdir.mkdir(exist_ok=True)
for name, overrides in variants.items():
    result = text
    for old, new in overrides:
        if result.count(old) != 1:
            sys.exit(f'{name}: override anchor not found exactly once: {old!r}')
        result = result.replace(old, new)
    out = outdir / name
    out.write_bytes(result.encode('latin1'))
    print(f'wrote {out}')
