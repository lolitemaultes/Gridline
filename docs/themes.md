# Themes

Gridline's colours are set in **Settings > Theme** (also **View > Theme settings...**). **View > Dark theme** switches between the dark and the light theme.

## Appearance

- **Theme**: Dark, Light or Follow system.
- **Accent**, **Lit glyphs**, **LEDs** and **Song position**: the colours used for selection and focus, active tools and toggles, lamps, and the playback line.

## Adjust

- **Contrast** moves text and lines away from the panels, or closer to them.
- **Brightness** makes the panels, fields and grids lighter or darker. At -100% they are pitch black, in both the dark and the light theme. On the way down the text stays readable: in the light theme, once the panels are darker than its dark text can read on, the text, knobs and glyphs switch to their light versions.
- **White text and glyphs** draws text, browser folders and the icons of tools that are not in use in pure white, with dim and faint text as steps of the same white. It works on dark panels: with the light theme at its normal brightness the text stays dark, because white would not read on light panels. Take the brightness down and it turns white.

Every change asks **Keep** or **Revert** after a few seconds, as other display changes do, and **Reset colours** puts colours, contrast, brightness and white text back to the defaults.

## What follows the theme

The whole main window follows these settings: the top bar, the browser with its folders and icons, the playlist with its audio waveforms and clips, the channel rack, the mixer, the piano roll, menus and dialogs, and the bars of plugin windows. The title bar the desktop draws above Gridline's windows follows too: it is dark when Gridline's panels are dark and light when they are light (Windows 10 and 11, macOS, and X11 desktops that read the GTK theme variant, such as GNOME). On a native Wayland session the desktop draws that bar in its own colours.

What does not change:

- **Plugin faces.** Each built-in instrument and effect keeps its own panel design, as plugins from other makers keep theirs.
- **The control styles** Graphite, Hairline and Lumen (Settings > Theme > Controls) keep their own materials. The default style, Slate, is drawn from the theme.
- **The Gridline logo** in the About window and the splash screen.
- **Your colours.** Channel, pattern, track and clip colours stay the colours you picked; waveforms and notes are drawn in a lighter (dark theme) or deeper (light theme) shade of them, and lighter still with white text.

Window layouts never change the theme or its colours.
