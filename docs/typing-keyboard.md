# Typing keyboard

The typing keyboard plays the selected channel from your computer keyboard. Turn it on with the
keyboard button in the top bar, **Options > Typing keyboard to piano** or Ctrl+T (Cmd+T on macOS).
Right-click the button for its options: the octave, the velocity, chord mode and **My keys say**,
what is printed on your keys.

Gridline opens with the typing keyboard on if it was on when you last closed it. To choose instead,
go to **Settings > MIDI > Typing keyboard > When Gridline starts**: **As it was left** (the default),
**On** or **Off**. While it is on, shortcuts on single letters and digits wait until it is off.

## Keys

Keys play by where they sit on the keyboard, whatever layout your computer uses. The names below are
the US positions; the options menu and the hint in the status bar name them as your keys say.

The names follow **My keys say** in **Settings > Shortcuts**, the same setting that names every
shortcut (see [keyboard-shortcuts.md](keyboard-shortcuts.md#my-keys-say)): **Same as my computer**
(the default) names the keys as your computer's layout types them; **QWERTY**, **QWERTZ**,
**AZERTY**, **Dvorak** and **Colemak** name them as printed on that keyboard. It used to be the
typing keyboard's own Keyboard layout choice: a layout you chose there carries over, and Standard
(QWERTY) becomes Same as my computer, which names the keys the same way on a QWERTY computer.

- **Z** to **/** (the bottom two rows) play from the base octave, **Q** to **P** (the top two rows)
  one octave higher.
- **[** and **]** move the octave down and up, one step per press. Keypad **-** and **+** do the same.
- **-** and **=** lower and raise the velocity. Keypad **/** and **\*** do the same.
- **\\** turns chord mode on and off, **'** picks the next chord.

Holding a key never repeats it: holding an octave key moves the octave once. A note you hold keeps
sounding while you click between Gridline's windows, and stops when you switch to another
application.

## One octave

The typing keyboard has one octave for all of Gridline: every channel, every instrument and every
layout. Only its octave keys and the octave menu change it. Selecting another channel, picking a
preset, replacing an instrument or clicking into a plugin's window leaves it where it was, and the
same key plays the same note.

The first note key plays C of the octave picked. In Studio, at octave 4, **Z** plays 48. On the A
row of Clips and Smooth, **A** plays 60. The Tracker counts octaves as its pattern editor does: at
octave 4, **Z** plays C-4 (60), wherever the keyboard is (the pattern, the instrument list, a plugin
window or the Sample editor), and F1 to F8 pick the octave there.

A preset can still sound higher or lower than the one before: many presets set their own octave or
transpose, as part of the sound. The typing keyboard sends the same notes; check the instrument's
own octave or transpose control if one preset sounds out of place.

## Shortcuts while it is on

While the typing keyboard is on, the keys it plays only play: shortcuts on those keys alone wait
until it is off. Keypad **-** and **+** change the octave and do not switch the pattern, **-** and
**=** change the velocity and do not zoom, and shortcut sets that put commands on single letters
play notes instead. In Plasma's editor **[** and **]** step the octave, not its layer, envelope or LFO tabs. Shortcuts with Ctrl, Cmd or Alt, and keys such as Space, the arrows, Page Up,
Page Down and the function keys, work as usual. The key that turns the typing keyboard off always
works.
