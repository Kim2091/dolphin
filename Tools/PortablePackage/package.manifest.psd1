#
# Layout manifest for the Dolphin + RTX Remix portable package.
#
# Package-Portable.ps1 reads this and nothing else to decide what ends up in a
# release. When the build output grows a new file, or a new kind of dev droppings
# starts appearing next to Dolphin.exe, this is the file to edit -- not the script.
#

@{
    # ------------------------------------------------------------------
    # Which git remote names the fork these binaries came from.
    #
    # This repo's 'origin' is upstream dolphin-emu, which did not build any of
    # this and would send a bug reporter to the wrong issue tracker. Remotes are
    # tried in this order and the first that exists wins.
    # ------------------------------------------------------------------
    PreferredRemotes = @('personal', 'fork', 'kim2091', 'origin')

    # ------------------------------------------------------------------
    # Where the Remix runtime is built, relative to this repo. Used to record
    # the runtime's branch and commit in VERSION.txt when -RuntimeRepo is not
    # given explicitly.
    #
    # There are a dozen dxvk-remix checkouts on this machine, most of them for
    # other games. This is the one on the Dolphin line. Getting it wrong is
    # safe: the commit is cross-checked against the version string embedded in
    # the shipped DLL, and a mismatch drops the details rather than recording a
    # commit that never built this binary.
    # ------------------------------------------------------------------
    DefaultRuntimeRepo = '..\dxvk-remix-dolphin-atmos'

    # ------------------------------------------------------------------
    # What comes out of the Dolphin build (build\release\x64\Binaries).
    #
    # Everything in the build output is copied EXCEPT what the exclusion rules
    # below reject. That direction is deliberate: a new Qt DLL or Sys\ subfolder
    # that the build starts emitting should ship automatically, whereas a new
    # kind of dev droppings is something a human should notice and name.
    # ------------------------------------------------------------------
    Emulator = @{
        # Refuse to package if any of these are missing from the build output.
        Required = @(
            'Dolphin.exe'
            'DolphinNoGUI.exe'
            'DolphinTool.exe'
            'Updater.exe'
            'qt.conf'
            'build_info.txt'
            'COPYING'
            'Qt6Core.dll'
            'Qt6Gui.dll'
            'Qt6Svg.dll'
            'Qt6Widgets.dll'
            'QtPlugins'
            'Sys'
            'Languages'
            'Licenses'
        )

        # Top-level entries dropped wholesale. Matched case-insensitively against
        # the entry name, wildcards allowed.
        ExcludeNames = @(
            # Per-game Remix state the developer accumulated while testing.
            # Shipping it would push someone else's tuning onto every user.
            'Remix'

            # Unit-test binary and its fixtures. Not part of a user build.
            'Tests'

            # Debug symbols: 123 MB and no use to anyone without the source.
            '*.pdb'

            # Hand-rolled backups. One of these (a 242 MB copy of the runtime)
            # shipped in v0.0.6 by accident.
            #
            # These patterns are deliberately over-broad on the suffix side.
            # The first version of this list had '*.bak' and '*.bak-*' and still
            # let 'rtx.conf.gamesky-bak' through, because a human naming a
            # backup does not consult a pattern list first.
            '*.bak'
            '*.bak*'
            '*-bak'
            '*-bak*'
            '*.backup'
            '*-backup'
            '*.before-*'
            '*-before-*'
            '*.orig'
            '*.old'
            '*-old'
            '*.save'
            '*.tmp'
            '*.temp'
            '*~'

            # Runtime state written next to the executable at run time. Users
            # get these generated fresh; ours carry local paths and tuning.
            #
            # Trailing wildcards catch hand-edited variants ('rtx.conf.gamesky',
            # 'user.conf.test'). qt.conf is NOT matched by these and must ship.
            'rtx.conf*'
            'user.conf*'
            'dxvk.conf*'
            'imgui.ini'
            'metrics.txt'
            'nrc_session_log.txt'
            '*.dxvk-cache'

            # Crash dumps and logs from local sessions.
            '*.dmp'
            '*.log'

            # A local game library. Ours points at ROMs we cannot redistribute.
            'Games'
            'User'
        )

        # Dropped anywhere in the tree, at any depth, by leaf name.
        ExcludeLeafNames = @(
            'Thumbs.db'
            'desktop.ini'
            '.DS_Store'
        )
    }

    # ------------------------------------------------------------------
    # What comes out of the Remix runtime drop (dolphin-remix-runtime).
    # ------------------------------------------------------------------
    Runtime = @{
        # The path tracer itself. Renamed on the way in -- see README section 2
        # of the shipped docs: the name is load-bearing, because Dolphin loads
        # it explicitly and a plain d3d9.dll would also be picked up by the
        # system loader for unrelated processes in the same folder.
        PrimaryDll       = 'd3d9.dll'
        PrimaryDllAs     = 'd3d9-remix.dll'

        # Refuse to package if these are missing from the runtime drop.
        Required = @(
            'd3d9.dll'
            'usd'
            'NRD.dll'
            'NRC_Vulkan.dll'
            'nvngx_dlss.dll'
            'nvngx_dlssd.dll'
            'rtxio.dll'
        )

        # The runtime drop is a hand-maintained folder, so it collects the same
        # sort of dust as the build output.
        ExcludeNames = @(
            '*.pdb'
            '*.bak'
            '*.bak*'
            '*-bak'
            '*-bak*'
            '*.backup'
            '*-backup'
            '*.before-*'
            '*-before-*'
            '*.orig'
            '*.old'
            '*-old'
            '*.tmp'
            '*~'
            '*.lib'
            '*.exp'
            '*.lnk'
            'rtx.conf*'
            'user.conf*'
            'dxvk.conf*'
            'imgui.ini'
            'metrics.txt'
            'nrc_session_log.txt'
            '*.dxvk-cache'
            '*.log'

            # Runtime's own install notes -- superseded by the shipped README,
            # and it describes copying files into a build tree, which is wrong
            # advice for someone holding a finished package.
            'README.md'
            'GFX-settings-sample.ini'

            # 32-bit bridge launcher and its payload. Dolphin is x64 and uses
            # the native runtime; shipping these invites the wrong install.
            'NvRemixLauncher32.exe'
            '.trex'
            '*.7z'
        )

        # The runtime must identify itself as a build of this fork's ABI line.
        # Package-Portable.ps1 scans the DLL for this pattern and records the
        # match in VERSION.txt. A stock RTX Remix release will not match, and
        # would fail at load with INCOMPATIBLE_VERSION anyway.
        VersionStringPattern = 'remix-[a-zA-Z0-9._/-]*\+[0-9a-f]{7,}'
    }

    # ------------------------------------------------------------------
    # Directories that ship empty. Git will not track these, so they are
    # named here and created at staging time. Dolphin creates most of them
    # on first run, but a user poking around a fresh extract should see the
    # shape of the thing.
    # ------------------------------------------------------------------
    EmptyDirs = @(
        'rtx-remix/mods'
        'User/Cache/GameCovers'
        'User/Cache/RetroAchievements'
        'User/Config/GraphicMods'
        'User/Dump/Audio'
        'User/Dump/Debug/BranchWatch'
        'User/Dump/Debug/JitBlocks'
        'User/Dump/DSP'
        'User/Dump/Frames'
        'User/Dump/Objects'
        'User/Dump/SSL'
        'User/Dump/Textures'
        'User/GameSettings'
        'User/GBA/Saves'
        'User/GC/EUR'
        'User/GC/JAP'
        'User/GC/USA/Card A'
        'User/Load/DynamicInputTextures'
        'User/Load/GraphicMods'
        'User/Load/Riivolution'
        'User/Load/Textures'
        'User/Load/WiiSDSync'
        'User/Logs'
        'User/Maps'
        'User/SavedAssembly'
        'User/ScreenShots'
        'User/Shaders/Anaglyph'
        'User/StateSaves'
        'User/Styles'
        'User/Themes'
        'User/Triforce'
        'User/WFS'
        'User/Wii/import'
        'User/Wii/meta'
        'User/Wii/shared1'
        'User/Wii/sys'
        'User/Wii/ticket'
        'User/Wii/title'
        'User/Wii/tmp'
        'User/Wii/wfs'
    )

    # ------------------------------------------------------------------
    # Documents rendered from docs\ with {{PLACEHOLDER}} substitution.
    # Source is relative to Tools\PortablePackage\docs.
    # ------------------------------------------------------------------
    Documents = @(
        @{ Source = 'README.md';                 Dest = 'README.md' }
        @{ Source = 'CHANGELOG.md';              Dest = 'CHANGELOG.md' }
        @{ Source = 'VERSION.txt';               Dest = 'VERSION.txt' }
        @{ Source = 'GFX-settings-reference.ini'; Dest = 'GFX-settings-reference.ini' }
    )

    # ------------------------------------------------------------------
    # Static files copied verbatim, no placeholder substitution.
    # Source is relative to Tools\PortablePackage.
    #
    # Dolphin's COPYING and Licenses\ cover Dolphin. They say nothing about the
    # ~44 runtime DLLs that make up most of this download -- dxvk-remix itself,
    # NVIDIA DLSS/NRD/NRC/RTXIO, Intel XeSS, AMD FidelityFX, OpenUSD, Python.
    # Shipping those binaries without their license texts is not something to
    # leave to a follow-up, so they are vendored here and always included.
    # ------------------------------------------------------------------
    ExtraFiles = @(
        @{ Source = 'legal\Remix-Runtime\README.txt';             Dest = 'Licenses\Remix-Runtime\README.txt' }
        @{ Source = 'legal\Remix-Runtime\LICENSE-dxvk.txt';       Dest = 'Licenses\Remix-Runtime\LICENSE-dxvk.txt' }
        @{ Source = 'legal\Remix-Runtime\LICENSE-MIT.txt';        Dest = 'Licenses\Remix-Runtime\LICENSE-MIT.txt' }
        @{ Source = 'legal\Remix-Runtime\ThirdPartyLicenses.txt'; Dest = 'Licenses\Remix-Runtime\ThirdPartyLicenses.txt' }
    )

    # ------------------------------------------------------------------
    # Post-staging assertions. The package is rejected if any fire.
    # These are the last line of defence: the exclusion rules above are a
    # blocklist, and a blocklist is only ever as current as the last thing
    # that got past it.
    # ------------------------------------------------------------------
    Verify = @{
        # Must exist in the staged tree, at these exact paths.
        MustExist = @(
            'Dolphin.exe'
            'd3d9-remix.dll'
            'portable.txt'
            'qt.conf'
            'README.md'
            'CHANGELOG.md'
            'VERSION.txt'
            'GFX-settings-reference.ini'
            'User/Config/GFX.ini'
            'Games/PUT-YOUR-GAMES-HERE.txt'
            'usd/usdGeom'
            'Sys/GameSettings'
            'QtPlugins/platforms'
            'COPYING'
            'Licenses/GPL-2.0-or-later.txt'
            'Licenses/Remix-Runtime/ThirdPartyLicenses.txt'
            'Licenses/Remix-Runtime/LICENSE-dxvk.txt'
        )

        # Must NOT exist anywhere in the staged tree. Wildcards match leaf names.
        MustNotExist = @(
            '*.pdb'
            '*.bak'
            '*.bak*'
            '*-bak'
            '*-bak*'
            '*.backup'
            '*-backup'
            '*.before-*'
            '*-before-*'
            '*.orig'
            '*.old'
            '*-old'
            '*.tmp'
            '*~'
            '*.ilk'
            '*.exp'
            '*.lib'
            'rtx.conf*'
            'user.conf*'
            'dxvk.conf*'
            'imgui.ini'
            'metrics.txt'
            'nrc_session_log.txt'
            '*.dxvk-cache'
            '*.dmp'
            'd3d9.dll'
            'tests.exe'
            'Thumbs.db'
            'desktop.ini'
        )

        # No single file above this may ship without -AllowLargeFiles. The
        # runtime DLL is ~242 MB and expected; anything else this big is a
        # symbol file or a stray backup that slipped the net.
        MaxFileSizeMB = 260

        # Whole package bounds. A package far outside these means something is
        # either missing or duplicated -- both worth stopping for.
        MinTotalSizeMB = 600
        MaxTotalSizeMB = 1200
    }
}
