===========================================================================
 SOVIETKIT 1.5 - Anarchy, Skins, Better Trackbuilders and Walking Distance
 for Workers & Resources: Soviet Republic  (1.1.1.9)
 Requires TesmioLoader
===========================================================================

Three things in this file:

   1.  INSTALLING              two steps, and Steam cannot do them for you
   2.  THE KEYS                what each one does
   3.  CHANGING THINGS         every switch in this mod is text you can edit

And one thing worth knowing before any of it: ANARCHY IS A MODE, NOT A
SETTING. Alt+F turns it on, the word ANARCHY appears on screen, and Alt+F
turns it off again and puts all 367 bytes back. Turn it on for the bit you
need it for and off again afterwards. That is what the key is for.


===========================================================================
 1.  INSTALLING
===========================================================================

STEP ONE. Copy the "tesmioloader" folder that sits beside this file into:

    ...\steamapps\common\SovietRepublic\

Say YES to merging. The files land where every other loader plugin's do:

    tesmioloader\build\plugins\sovietkit.dll
    tesmioloader\build\plugins\sovietkit.ini

Steam cannot do this for you. TesmioLoader loads plugins out of its own
folder, which Steam does not write to. Every loader plugin is installed by
hand; this one is no different.

STEP TWO. Open

    tesmioloader\build\tesmioloader.ini

and add one line:

    sovietkit=1

STEP THREE, which is not really a step. Start the game through

    tesmiolauncher.exe

Steam's Play button starts SOVIET64.exe directly, without the loader, and
then nothing in this mod exists.


DID IT WORK?

    tesmioloader\build\sovietkit.log

should end with

    ready: anarchy on, skins on, trackcrew on, walking on

If it says something else, it says why on the line above. The most likely
answer is that the game has updated: every offset in this plugin was read
out of SOVIET64.exe v1.1.1.9, and on any other build it refuses to hook or
patch anything at all rather than guess.


IF YOU HAD THE SEPARATE PLUGINS

If skins, anarchy, railfree or trackcrew are still in your plugins folder,
take them out. They hook the same functions and only the first one to get
there wins.


===========================================================================
 2.  THE KEYS
===========================================================================

    Alt+F            anarchy on / off. BOTH halves of it. The word ANARCHY
                     appears while it is on.

    Ctrl+Alt+T       anarchy's layer 1 on its own - the panic key. It takes
                     anarchy down with it.

    Ctrl+Alt+G       better trackbuilders on / off
    Ctrl+Alt+D       one-way signals read as two-way, FOR ROUTING ONLY.
                     Read the warning in the ini first.

    Ctrl+Alt+R       the full report, into the log
    Ctrl+Alt+K       zero every counter

    Ctrl+Alt+C       copy the selected building's skin
    Ctrl+Alt+V       paste it onto another building
    Ctrl+Alt+A       put it on EVERY building of that type
    Ctrl+Alt+Z       undo that
    Ctrl+Alt+]       step the selected building's skin forwards
    Ctrl+Alt+[       ...and backwards, which the game's own button cannot do
    Ctrl+Alt+P       retain-on-pick on / off
    Ctrl+Alt+S       the self test: read a skin, write a different one, read
                     it back, put the original back. One keypress tells you
                     whether finding, reading and writing each work.
    Ctrl+Alt+H       hex dump of the selected building

Modifiers are matched EXACTLY. Alt+F means Alt down with Ctrl and Shift UP,
so none of the Ctrl+Alt bindings can reach the anarchy toggle on the way
past. Every key and every modifier is rebindable in the [keys] section.


===========================================================================
 3.  CHANGING THINGS
===========================================================================

Everything is in

    tesmioloader\build\plugins\sovietkit.ini

with a comment above every setting explaining what it does and, where it
matters, what it costs. The first block is the one most people want:

    [sovietkit]
    anarchy   = 1
    skins     = 1
    trackcrew = 1
    walking   = 1

Setting one of those to 0 means that part installs nothing at all: no hooks,
no patches, no hotkeys, no per-frame work. The others are unaffected. It
is the first thing to try if you suspect one of them of anything.

SAVE THE INI AS UTF-8 WITHOUT A BOM. PowerShell's -Encoding UTF8 writes a
BOM, the first section header stops matching, and every setting silently
falls back to its default. Nothing is logged. Nothing looks wrong.

A few worth knowing about:

    [anarchy] allow_outside_map
        The one refusal anarchy leaves on. It is not the game being fussy -
        it is the last check between the build tools and memory the game
        does not own. Turning it on is a crash, not a feature, and the log
        will tell you so every time anarchy comes on.

    [anarchy] messages
        With anarchy on there is no refusal, so the game is stopped from
        recording one - which is what silences the warning text and the red
        preview. Set it to 0 and everything still builds, but the game tells
        you what it WOULD have refused, which is occasionally useful.

    [skins] spread_on_change
        Off by default. On, changing a building's skin with the spray-can
        also repaints every one of that type already standing. Ctrl+Alt+Z
        undoes it.

    [trackcrew] ignore_signal_direction
        Off by default, and read its comment before turning it on. While it
        is on it is NOT limited to track builders: any train can plan a
        route the wrong way down a one-way section.

    [walking] distance, car_distance, regen_on_load
        How far a citizen will walk, in metres of path, and how far a citizen
        with a car will drive. The game's own values are 480 and 2500;
        distance ships at 1000. regen_on_load makes a city you already have
        pick the new number up when you load it. Needs a build the plugin has
        an address table for - the log says whether it has.

    [debug] trace  and  each part's  debug
        Both ship off. Turn them on together when something looks wrong,
        reproduce it, then press Ctrl+Alt+R. At trace 2 the report prints
        which addresses each hook was called from, which is usually the
        whole answer.


===========================================================================
 WHAT THIS CANNOT PROMISE
===========================================================================

A refusal usually means the simulation was not written for the thing you
are about to do. Track through a building has a building inside it. A
sewage pipe that runs uphill will be built and will not flow. A power line
through a hill is a power line through a hill. The game will not crash on
these - the checks are about placement, not memory - but it will not do
anything clever with them either.

KEEP A BACKUP SAVE.


===========================================================================
 CREDITS AND SOURCE
===========================================================================

TesmioLoader by Tesmio. This is a plugin and does not run without it.

Source, the reverse-engineering notes, and the full table of every byte this
plugin changes and why:

    https://github.com/daniepe/WRSR-1.1.19-WRSR-Anarchy-and-QOL
