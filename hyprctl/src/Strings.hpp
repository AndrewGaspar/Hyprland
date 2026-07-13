#pragma once

#include <string_view>

const std::string_view USAGE = R"#(usage: hyprctl [flags] <command> [args...|--help]

commands:
    activewindow        → Gets the active window name and its properties
    activeworkspace     → Gets the active workspace and its properties
    animations          → Gets the current config'd info about animations
                          and beziers
    binds               → Lists all registered binds
    clients             → Lists all windows with their properties
    configerrors        → Lists all current config parsing errors
    cursorpos           → Gets the current cursor position in global layout
                          coordinates
    decorations <window_regex> → Lists all decorations and their info
    devices             → Lists all connected keyboards and mice
    dismissnotify [amount] → Dismisses all or up to AMOUNT notifications
    dispatch <dispatcher> [args] → Issue a dispatch to call a keybind
                          dispatcher with arguments
    eval <code>         → Issue a Lua string to execute
    getoption <option>  → Gets the config option status (values)
    globalshortcuts     → Lists all global shortcuts
    hyprpaper ...       → Issue a hyprpaper request
    hyprsunset ...      → Issue a hyprsunset request
    instances           → Lists all running instances of Hyprland with
                          their info
    keyword <name> <value> → Issue a keyword to call a config keyword
                          dynamically
    kill                → Issue a kill to get into a kill mode, where you can
                          kill an app by clicking on it. You can exit it
                          with ESCAPE
    layers              → Lists all the surface layers
    layouts             → Lists all layouts available (including plugin'd ones)
    monitors            → Lists active outputs with their properties,
                          'monitors all' lists active and inactive outputs
    notify ...          → Sends a notification using the built-in Hyprland
                          notification system
    openxr ...          → Controls the OpenXR extension: session state, XR
                          monitor lifecycle, pose verbs, adaptive anchoring
    output ...          → Allows you to add and remove fake outputs to your
                          preferred backend
    plugin ...          → Issue a plugin request
    reload [config-only] → Issue a reload to force reload the config. Pass
                          'config-only' to disable monitor reload
    repl [code]         → Enter interactive Lua REPL mode (^D to exit)
                          or issue a Lua string and print the result
    rollinglog          → Prints tail of the log. Also supports -f/--follow
                          option
    setcursor <theme> <size> → Sets the cursor theme and reloads the cursor
                          manager
    seterror <color> <message...> → Sets the hyprctl error string. Color has
                          the same format as in colors in config. Will reset
                          when Hyprland's config is reloaded
    setprop ...         → Sets a window property
    getprop ...         → Gets a window property
    splash              → Get the current splash
    status              → Get internal status information
    switchxkblayout ... → Sets the xkb layout index for a keyboard
    systeminfo          → Get system info
    version             → Prints the hyprland version, meaning flags, commit
                          and branch of build.
    workspacerules      → Lists all workspace rules
    workspaces          → Lists all workspaces with their properties

flags:
    -j                  → Output in JSON
    -r                  → Refresh state after issuing command (e.g. for
                          updating variables)
    --batch             → Execute a batch of commands, separated by ';'
    --instance (-i)     → use a specific instance. Can be either signature or
                          index in hyprctl instances (0, 1, etc)
    --quiet (-q)        → Disable the output of hyprctl

--help:
    Can be used to print command's arguments that did not fit into this page
    (three dots))#";

const std::string_view HYPRPAPER_HELP = R"#(usage: hyprctl [flags] hyprpaper <request>

requests:
    wallpaper       → Issue a wallpaper to call a config wallpaper dynamically.
                      Arguments are [mon],[path],[fit_mode]. Fit mode is optional.

flags:
    See 'hyprctl --help')#";

const std::string_view HYPRSUNSET_HELP = R"#(usage: hyprctl [flags] hyprsunset <request>

requests:
    temperature <temp> → Enable blue-light filter
    identity           → Disable blue-light filter
    gamma <gamma>      → Enable gamma filter

flags:
    See 'hyprctl --help')#";

const std::string_view NOTIFY_HELP = R"#(usage: hyprctl [flags] notify <icon> <time_ms> <color> <message...>

icon:
    Integer of value:
        0       → Warning
        1       → Info
        2       → Hint
        3       → Error
        4       → Confused
        5       → Ok
        6 or -1 → No icon

time_ms:
    Time to display notification in milliseconds

color:
    Notification color. Format is the same as for colors in hyprland.lua. Use
    0 for default color for icon

message:
    Notification message

flags:
    See 'hyprctl --help')#";

const std::string_view OPENXR_HELP = R"#(usage: hyprctl [flags] openxr [request]

requests:
    status (default) → Session/runtime state and all XR monitors (supports -j)
    enable           → Start the OpenXR session (connect to the runtime)
    disable          → Stop the OpenXR session

    create <name> <WxH[@Hz]> <anchor-spec...> [size:M]
                     → Create an XR monitor (same grammar as the xrmonitor
                       config keyword, space-separated; see example/openxr.conf)
    destroy <name>   → Destroy an XR monitor
    select <name>    → Select the monitor subsequent verbs act on
    layout           → Dump paste-ready xrmonitor config lines with the LIVE
                       poses (persist an in-headset arrangement)

    anchor <spec>    → Re-anchor the selected monitor (local/head/body/device)
    move <dx dy dz>  → Nudge the selected monitor (metres)
    rotate <yaw [pitch]> → Rotate (degrees)
    scale <factor|size:M> → Resize the quad
    distance <±M|M>  → Push/pull along the anchor ray
    center           → Recenter in front of the current view
    place <name> at <x,y,z>
                     → Re-anchor <name> to local, MOVING its center to the given
                       LOCAL_FLOOR point (metres), facing the headset. The point
                       is what 'openxr gaze' returns — drop a monitor where the
                       user was looking (voice: 'place it here')

    adaptive <on|off|toggle> → Adaptive dock<->follow on the selected monitor
    dock [here]      → Force-dock (optionally at the current pose)
    undock           → Force-undock into the roam leash
    roam <head|body> → Roam-follow mode while undocked

    gazegrab         → Toggle a gaze carry on the dwell-stable gazed-at monitor
    gazegrab <name>  → Begin a gaze carry on the NAMED monitor (voice-targeted,
                       ignores the live dwell candidate)
    gazerelease      → Release the active gaze carry
    gaze             → Current head ray + dwell-stable gaze candidate (-j)
    gaze at <ms>     → Same, resolved from the rolling head-pose ring at a
    gaze --at-ms <ms>  monotonic-clock timestamp (CLOCK_MONOTONIC ms); adds
                       matchedTimestampMs + ageMs. Lets a voice daemon target
                       where the head pointed AT SPEECH ONSET, not at parse time

flags:
    See 'hyprctl --help')#";

const std::string_view OUTPUT_HELP = R"#(usage: hyprctl [flags] output <create <backend> | remove <name>>

create <backend>:
    Creates new virtual output. Possible values for backend: wayland, x11,
    headless or auto.

remove <name>:
    Removes virtual output. Pass the output's name, as found in
    'hyprctl monitors'

flags:
    See 'hyprctl --help')#";

const std::string_view PLUGIN_HELP = R"#(usage: hyprctl [flags] plugin <request>

requests:
    load <path>     → Loads a plugin. Path must be absolute
    unload <path>   → Unloads a plugin. Path must be absolute
    list            → Lists all loaded plugins

flags:
    See 'hyprctl --help')#";

const std::string_view SETPROP_HELP = R"#(usage: hyprctl [flags] setprop <regex> <property> <value> [lock]

regex:
    Regular expression by which a window will be searched

property:
    See https://wiki.hypr.land/Configuring/Using-hyprctl/#setprop for list
    of properties

value:
    Property value

lock:
    Optional argument. If lock is not added, will be unlocked. Locking means a
    dynamic windowrule cannot override this setting.

flags:
    See 'hyprctl --help')#";

const std::string_view GETPROP_HELP = R"#(usage: hyprctl [flags] getprop <regex> <property>

regex:
    Regular expression by which a window will be searched

property:
    See https://wiki.hypr.land/Configuring/Using-hyprctl/#setprop for list
    of properties

flags:
    See 'hyprctl --help')#";

const std::string_view SWITCHXKBLAYOUT_HELP = R"#(usage: [flags] switchxkblayout <device> <cmd>

device:
    You can find the device using 'hyprctl devices' command

cmd:
    'next' for next, 'prev' for previous, or ID for a specific one. IDs are
    assigned based on their order in config file (keyboard_layout),
    starting from 0

flags:
    See 'hyprctl --help')#";
