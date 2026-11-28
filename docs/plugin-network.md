# Block internet access for a plugin

Gridline can keep a single installed plugin (VST3, VST2, CLAP, AU or LV2) off the internet, for
privacy. It is set per plugin, for every format of it, and saved with your plugin settings.

## Where to set it

- **The plugin's window:** click **OFFLINE** on the bar above the plugin. The bar's panel opens on
  its **Internet** page, with **Allowed** and **Blocked**. OFFLINE is lit while the plugin is
  blocked. The Gridline panel of a plugin window has the same page as its **INTERNET** tab.
- **The Plugin Manager:** the **Internet** column shows a globe for each plugin: struck through
  while it is blocked. Click it to block or allow the plugin. Pick several plugins with Ctrl (Cmd on
  macOS) or Shift and a bar under the list blocks or allows them all at once. The **Offline** view
  lists the blocked plugins, and a plugin's details have an **INTERNET ACCESS** section.
- **Settings > Plugins > INTERNET ACCESS:** whether plugins found from now on start **Allowed**
  (the default) or **Blocked**. A plugin that starts blocked is blocked from its first scan on.

Changing it reloads the plugin's open instances with their state, the same way switching the
sandbox does.

The choice belongs to this computer, not to the project. It is not saved in projects and never
reaches the people you work with in a shared session: they keep their own choices, and opening
someone else's project does not change yours. A plugin counts as new only the first time this
computer finds it, also when the plugin list is rebuilt.

## Allow until Gridline closes

Some plugins sign in or check their licence online the first time. **Allow until Gridline closes**
lets a blocked plugin connect until you quit Gridline; the next time it is blocked again. **Block it
again now** ends it early.

## How it works

A blocked plugin always runs in its own process (as with **Make bridged (run in sandbox)**), and that
process asks the system to keep it off the internet before any of the plugin's code is loaded. The
scan that reads the plugin's name and parameters is restricted the same way. Programs the plugin
starts inherit the restriction. Gridline's own process is not restricted: updates, collaboration
and **Import from URL** keep working.

- **macOS:** the system sandbox's no-internet profile. Local connections the plugin needs (to
  Gridline, the window server and audio) keep working.
- **Linux:** a private network space with no route out. Where the system does not allow one (for
  example where user namespaces are switched off), a system call filter that refuses internet
  sockets. Because the private network space has no network devices at all, a plugin whose licence
  is tied to the computer's network card may not find its licence while blocked.
- **Windows, FreeBSD, NetBSD and OpenBSD:** not available. Blocking one program there needs
  administrator rights, so the option is shown switched off with the reason.

If the restriction cannot be applied, the plugin is not loaded: it never runs unblocked by mistake.
Its window says why and offers **Allow until Gridline closes**.

## What it does not cover

- Licence managers, updaters and other programs that run on their own, not started by the plugin.
- Other plugins, and Gridline itself.
- What the plugin writes to files.
- Connection attempts are not counted or listed: the system does not report them.

It is a privacy setting, not protection against harmful plugins. To be safe from a plugin you do
not trust, do not install it.
