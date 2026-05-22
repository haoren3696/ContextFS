# quickstart.tape — drives demo/agentvfs-quickstart.sh under VHS to render
# demo/quickstart.gif for the README.
#
# All the actual visual logic lives in agentvfs-quickstart.sh; this tape
# only types the launch command and holds long enough for the script to
# finish. Re-render with:
#
#   ./scripts/render-quickstart.sh           # convenience wrapper
#   vhs demo/quickstart.tape                 # raw

Output demo/quickstart.gif

Set Shell bash
Set FontSize 16
Set Width 1200
Set Height 760
Set Theme "Catppuccin Mocha"
Set TypingSpeed 50ms
Set PlaybackSpeed 1.0

# Quiet the prompt so the GIF doesn't open with hostname/cwd noise.
Hide
Type "export PS1='$ '"
Enter
Type "clear"
Enter
Show

# One command — that's the whole user-facing surface.
Type "./demo/agentvfs-quickstart.sh"
Sleep 300ms
Enter

# The script paces its own beats (~22s total). Hold the recording open
# long enough for it to finish, plus a small breath at the end so the
# final frame isn't cut mid-fade.
Sleep 24s

Hide
Type "exit"
Enter
Show
